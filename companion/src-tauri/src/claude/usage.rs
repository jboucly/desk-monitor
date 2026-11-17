//! Token usage read from the Claude Code transcripts (`~/.claude/projects/**/*.jsonl`).
//!
//! Each answer of the model is an `assistant` line with `message.usage`. A streamed answer is written
//! on several lines with the same `message.id`: the last one wins. Files are read incrementally (an
//! offset per file), so that a scan only parses the lines added since the previous one.

use chrono::{DateTime, Local, NaiveDate};
use serde_json::Value;
use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

/// Days kept: the week shown on the screen, plus a margin for the time zone.
const KEEP_DAYS: i64 = 8;

/// One answer of the model.
struct Answer {
    session: String,
    day: NaiveDate,
    time: i64,
    tokens: u64,
    cost: f64,
    /// Tokens in the context window (input + cache), for the main conversation only
    context: Option<u64>,
    model: String,
}

/// Tokens and estimated cost of a period.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Total {
    pub tokens: u64,
    pub cost: f64,
}

/// What the screen shows about one session.
#[derive(Clone, Debug, Default)]
pub struct SessionUsage {
    pub tokens: u64,
    pub context: u64,
    pub model: String,
    /// Time of the last answer (Unix seconds)
    pub last: i64,
}

#[derive(Default)]
struct FileState {
    offset: u64,
    modified: Option<SystemTime>,
}

/// Incremental reader of the transcripts.
#[derive(Default)]
pub struct Usage {
    files: HashMap<PathBuf, FileState>,
    answers: HashMap<String, Answer>,
    /// Conversation titles written by Claude Code (`ai-title` lines), by session
    titles: HashMap<String, String>,
    /// Totals by day and by session, rebuilt after a scan that found new answers
    days: HashMap<NaiveDate, Total>,
    sessions: HashMap<String, SessionUsage>,
}

impl Usage {
    /// Reads what was added to the transcripts since the last scan.
    pub fn scan(&mut self, root: &Path) {
        let cutoff = SystemTime::now() - Duration::from_secs(KEEP_DAYS as u64 * 86400);
        let mut paths = Vec::new();
        collect_jsonl(root, cutoff, &mut paths, 0);
        let mut changed = false;
        for (path, modified) in paths {
            let state = self.files.entry(path.clone()).or_default();
            if state.modified == Some(modified) {
                continue;
            }
            state.modified = Some(modified);
            let Some(text) = read_from(&path, &mut state.offset) else { continue };
            for line in text.lines() {
                changed |= self.read_line(line);
            }
        }
        if changed {
            self.prune();
            self.aggregate();
        }
    }

    /// Parses one transcript line. Returns true when it was an answer.
    fn read_line(&mut self, line: &str) -> bool {
        let usage_line = line.contains("\"usage\"");
        if !usage_line && !line.contains("\"ai-title\"") {
            return false;
        }
        let Ok(entry) = serde_json::from_str::<Value>(line) else { return false };
        let session = entry["sessionId"].as_str().unwrap_or("").to_string();
        if entry["type"] == "ai-title" {
            if let Some(title) = entry["aiTitle"].as_str() {
                self.titles.insert(session, title.to_string());
            }
            return false;
        }
        if entry["type"] != "assistant" {
            return false;
        }
        let message = &entry["message"];
        let (Some(id), Some(usage)) = (message["id"].as_str(), message.get("usage")) else { return false };
        let Some(time) = entry["timestamp"].as_str().and_then(|t| DateTime::parse_from_rfc3339(t).ok()) else {
            return false;
        };
        let model = message["model"].as_str().unwrap_or("").to_string();
        // Synthetic messages (e.g. an API error shown in the transcript) cost nothing
        if model.starts_with('<') {
            return false;
        }
        let count = |key: &str| usage[key].as_u64().unwrap_or(0);
        let (input, output) = (count("input_tokens"), count("output_tokens"));
        let (read, written) = (count("cache_read_input_tokens"), count("cache_creation_input_tokens"));
        // Cache writes cost more for one hour than for five minutes; older transcripts lack the split
        let written_1h = usage["cache_creation"]["ephemeral_1h_input_tokens"].as_u64().unwrap_or(0).min(written);
        let price = price_of(&model);
        let cost = (input as f64 * price.input
            + output as f64 * price.output
            + (written - written_1h) as f64 * price.input * 1.25
            + written_1h as f64 * price.input * 2.0
            + read as f64 * price.cache_read)
            / 1e6;
        let sidechain = entry["isSidechain"].as_bool().unwrap_or(false);
        self.answers.insert(
            id.to_string(),
            Answer {
                session,
                day: time.with_timezone(&Local).date_naive(),
                time: time.timestamp(),
                tokens: input + output + read + written,
                cost,
                context: (!sidechain).then_some(input + read + written),
                model,
            },
        );
        true
    }

    /// Forgets the answers older than the kept days.
    fn prune(&mut self) {
        let oldest = Local::now().date_naive() - chrono::Days::new(KEEP_DAYS as u64);
        self.answers.retain(|_, a| a.day >= oldest);
    }

    fn aggregate(&mut self) {
        self.days.clear();
        self.sessions.clear();
        for answer in self.answers.values() {
            let day = self.days.entry(answer.day).or_default();
            day.tokens += answer.tokens;
            day.cost += answer.cost;
            let session = self.sessions.entry(answer.session.clone()).or_default();
            session.tokens += answer.tokens;
            if let Some(context) = answer.context {
                if answer.time >= session.last {
                    session.last = answer.time;
                    session.context = context;
                    session.model = answer.model.clone();
                }
            }
        }
    }

    /// Totals of the last 7 days, oldest first, today last.
    pub fn days(&self, today: NaiveDate) -> Vec<Total> {
        (0..7u64)
            .rev()
            .map(|back| self.days.get(&(today - chrono::Days::new(back))).copied().unwrap_or_default())
            .collect()
    }

    pub fn session(&self, id: &str) -> Option<&SessionUsage> {
        self.sessions.get(id)
    }

    pub fn title(&self, id: &str) -> Option<&str> {
        self.titles.get(id).map(String::as_str)
    }
}

/// Transcripts modified after `cutoff`, with their modification time. Subagent transcripts live in
/// subfolders of the project folders.
fn collect_jsonl(dir: &Path, cutoff: SystemTime, out: &mut Vec<(PathBuf, SystemTime)>, depth: u32) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(meta) = entry.metadata() else { continue };
        if meta.is_dir() && depth < 4 {
            collect_jsonl(&path, cutoff, out, depth + 1);
        } else if path.extension().is_some_and(|e| e == "jsonl") {
            if let Ok(modified) = meta.modified() {
                if modified >= cutoff {
                    out.push((path, modified));
                }
            }
        }
    }
}

/// Reads the complete lines written after `offset` and moves the offset after them. A file that got
/// shorter was rewritten: it is read again from the start.
fn read_from(path: &Path, offset: &mut u64) -> Option<String> {
    let mut file = std::fs::File::open(path).ok()?;
    let len = file.metadata().ok()?.len();
    if len < *offset {
        *offset = 0;
    }
    file.seek(SeekFrom::Start(*offset)).ok()?;
    let mut bytes = Vec::new();
    file.take(len - *offset).read_to_end(&mut bytes).ok()?;
    // A line still being written stays for the next scan
    let end = bytes.iter().rposition(|&b| b == b'\n').map_or(0, |i| i + 1);
    bytes.truncate(end);
    *offset += end as u64;
    Some(String::from_utf8_lossy(&bytes).into_owned())
}

/// API prices of a model, in US dollars per million tokens. Cache writes are derived from `input`
/// (x1.25 for five minutes, x2 for one hour).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Price {
    pub input: f64,
    pub output: f64,
    pub cache_read: f64,
}

/// Prices of the Claude models (Anthropic API, November 2026), matched on the model id.
pub fn price_of(model: &str) -> Price {
    const fn p(input: f64, output: f64, cache_read: f64) -> Price {
        Price { input, output, cache_read }
    }
    const TABLE: &[(&str, Price)] = &[
        ("fable-5-2", p(8.0, 40.0, 0.20)),
        ("mythos-5-2", p(8.0, 40.0, 0.20)),
        ("fable-5-1", p(10.0, 50.0, 0.25)),
        ("mythos-5-1", p(10.0, 50.0, 0.25)),
        ("fable-5", p(10.0, 50.0, 1.0)),
        ("mythos", p(10.0, 50.0, 1.0)),
        ("opus-5-5", p(4.0, 20.0, 0.20)),
        ("opus-5", p(5.0, 25.0, 0.50)),
        ("opus-4-8", p(5.0, 25.0, 0.50)),
        ("opus-4-7", p(5.0, 25.0, 0.50)),
        ("opus-4-6", p(5.0, 25.0, 0.50)),
        ("opus-4-5", p(5.0, 25.0, 0.50)),
        ("opus", p(15.0, 75.0, 1.50)),
        ("sonnet-5-5", p(2.0, 10.0, 0.10)),
        ("sonnet-5", p(2.0, 10.0, 0.20)),
        ("sonnet", p(3.0, 15.0, 0.30)),
        ("haiku-5-5", p(0.10, 0.50, 0.01)),
        ("haiku-4-5", p(1.0, 5.0, 0.10)),
        ("haiku", p(0.80, 4.0, 0.08)),
    ];
    TABLE
        .iter()
        .find(|(key, _)| model.contains(key))
        .map(|(_, price)| *price)
        // Unknown model: priced like the current Opus
        .unwrap_or(p(4.0, 20.0, 0.20))
}

/// Short display name of a model id: "claude-opus-5-5" gives "Opus 5.5".
pub fn model_name(model: &str) -> String {
    let rest = model.trim_start_matches("claude-");
    let mut parts = rest.split('-').filter(|p| !p.is_empty() && !(p.len() == 8 && p.chars().all(|c| c.is_ascii_digit())));
    let Some(family) = parts.next() else { return String::new() };
    let mut name: String = family[..1].to_uppercase() + &family[1..];
    let version: Vec<&str> = parts.take(2).collect();
    if !version.is_empty() {
        name.push(' ');
        name.push_str(&version.join("."));
    }
    name
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(id: &str, session: &str, time: &str, input: u64, output: u64, read: u64, written: u64, sidechain: bool) -> String {
        serde_json::json!({
            "type": "assistant", "sessionId": session, "timestamp": time, "isSidechain": sidechain,
            "message": { "id": id, "model": "claude-opus-5-5", "usage": {
                "input_tokens": input, "output_tokens": output,
                "cache_read_input_tokens": read, "cache_creation_input_tokens": written,
            }},
        })
        .to_string()
    }

    #[test]
    fn keeps_the_last_line_of_a_streamed_answer() {
        let mut usage = Usage::default();
        let now = Local::now().to_rfc3339();
        assert!(usage.read_line(&line("m1", "s1", &now, 10, 5, 100, 0, false)));
        assert!(usage.read_line(&line("m1", "s1", &now, 10, 50, 100, 0, false)));
        assert!(usage.read_line(&line("m2", "s1", &now, 2, 3, 1000, 20, true)));
        usage.aggregate();
        let session = usage.session("s1").unwrap();
        assert_eq!(session.tokens, 160 + 1025);
        // The context comes from the main conversation, not from the subagent
        assert_eq!(session.context, 110);
        assert_eq!(usage.days(Local::now().date_naive())[6].tokens, 1185);
    }

    #[test]
    fn reads_titles_and_ignores_other_lines() {
        let mut usage = Usage::default();
        assert!(!usage.read_line(r#"{"type":"ai-title","sessionId":"s1","aiTitle":"Fix the build"}"#));
        assert!(!usage.read_line(r#"{"type":"user","sessionId":"s1","message":{"content":"hi"}}"#));
        assert_eq!(usage.title("s1"), Some("Fix the build"));
    }

    #[test]
    fn prices_and_names_models() {
        assert_eq!(price_of("claude-opus-5-5").input, 4.0);
        assert_eq!(price_of("claude-opus-4-1-20250805").input, 15.0);
        assert_eq!(price_of("claude-sonnet-4-6").output, 15.0);
        assert_eq!(model_name("claude-opus-5-5"), "Opus 5.5");
        assert_eq!(model_name("claude-sonnet-4-5-20250929"), "Sonnet 4.5");
        assert_eq!(model_name("claude-fable-5"), "Fable 5");
    }

    #[test]
    fn reads_complete_lines_only() {
        let dir = std::env::temp_dir().join(format!("dm-usage-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("t.jsonl");
        std::fs::write(&path, "a\nb\npartial").unwrap();
        let mut offset = 0;
        assert_eq!(read_from(&path, &mut offset).unwrap(), "a\nb\n");
        assert_eq!(offset, 4);
        std::fs::write(&path, "a\nb\npartial line\n").unwrap();
        assert_eq!(read_from(&path, &mut offset).unwrap(), "partial line\n");
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
