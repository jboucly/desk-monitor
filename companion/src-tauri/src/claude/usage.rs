//! Token usage read from the Claude Code transcripts (`~/.claude/projects/**/*.jsonl`).
//!
//! Each answer of the model is an `assistant` line with `message.usage`. A streamed answer is written
//! on several lines with the same `message.id`: the last one wins. Files are read incrementally (an
//! offset per file), so that a scan only parses the lines added since the previous one.
//!
//! Subagents write their own transcripts in `<project>/<session>/subagents/agent-<id>.jsonl`, next to
//! an `agent-<id>.meta.json` (type, description, finished).

use chrono::{DateTime, Local, NaiveDate, Timelike};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

/// Days kept: the 30 days of the usage view, plus a margin for the time zone.
const KEEP_DAYS: i64 = 31;

/// Days of the usage view.
const DETAIL_DAYS: u64 = 30;

/// Model families with their own color on the screen; the others are summed as "Autres".
const MAX_FAMILIES: usize = 4;

/// A subagent counts as running when its transcript moved in the last minutes.
const SUBAGENT_IDLE: Duration = Duration::from_secs(10 * 60);

/// One answer of the model.
struct Answer {
    session: String,
    day: NaiveDate,
    time: i64,
    input: u64,
    output: u64,
    cache_read: u64,
    cache_write: u64,
    cost: f64,
    /// Tokens in the context window (input + cache), for the main conversation only
    context: Option<u64>,
    model: String,
    project: String,
    /// Subagent that made the answer
    agent: Option<String>,
}

impl Answer {
    fn tokens(&self) -> u64 {
        self.input + self.output + self.cache_read + self.cache_write
    }
}

/// Tokens of one session by kind, for its detail view.
#[derive(Clone, Debug, Default)]
pub struct SessionDetail {
    pub input: u64,
    pub output: u64,
    pub cache_read: u64,
    pub cache_write: u64,
    pub cost: f64,
    /// Time of the first answer (Unix seconds)
    pub started: i64,
    /// Tokens by subagent id
    pub agents: HashMap<String, u64>,
}

/// A subagent of a session, from its meta file.
#[derive(Clone, Debug)]
pub struct Subagent {
    pub id: String,
    pub kind: String,
    pub description: String,
    pub running: bool,
    /// Last write of its transcript (Unix seconds)
    pub active: i64,
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
    /// Project folder of each session's transcript, where its subagents are
    folders: HashMap<String, PathBuf>,
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
            // Main transcript: <project folder>/<session>.jsonl
            if path.parent().and_then(Path::file_name).is_some_and(|n| n != "subagents") {
                if let (Some(session), Some(folder)) = (path.file_stem(), path.parent()) {
                    self.folders.insert(session.to_string_lossy().into_owned(), folder.to_path_buf());
                }
            }
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
        let project = entry["cwd"].as_str().map(super::sessions::project_of).unwrap_or_default();
        self.answers.insert(
            id.to_string(),
            Answer {
                session,
                day: time.with_timezone(&Local).date_naive(),
                time: time.timestamp(),
                input,
                output,
                cache_read: read,
                cache_write: written,
                cost,
                context: (!sidechain).then_some(input + read + written),
                model,
                project,
                agent: entry["agentId"].as_str().filter(|_| sidechain).map(str::to_string),
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
            day.tokens += answer.tokens();
            day.cost += answer.cost;
            let session = self.sessions.entry(answer.session.clone()).or_default();
            session.tokens += answer.tokens();
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

    /// Tokens of one session by kind, and by subagent.
    pub fn session_detail(&self, id: &str) -> SessionDetail {
        let mut detail = SessionDetail { started: i64::MAX, ..Default::default() };
        for answer in self.answers.values().filter(|a| a.session == id) {
            detail.input += answer.input;
            detail.output += answer.output;
            detail.cache_read += answer.cache_read;
            detail.cache_write += answer.cache_write;
            detail.cost += answer.cost;
            detail.started = detail.started.min(answer.time);
            if let Some(agent) = &answer.agent {
                *detail.agents.entry(agent.clone()).or_default() += answer.tokens();
            }
        }
        if detail.started == i64::MAX {
            detail.started = 0;
        }
        detail
    }

    /// The subagents of a session, newest first, from their meta files.
    pub fn subagents(&self, id: &str) -> Vec<Subagent> {
        let Some(folder) = self.folders.get(id) else { return Vec::new() };
        let Ok(entries) = std::fs::read_dir(folder.join(id).join("subagents")) else { return Vec::new() };
        let mut found = Vec::new();
        for entry in entries.flatten() {
            let path = entry.path();
            let Some(name) = path.file_name().and_then(|n| n.to_str()) else { continue };
            let Some(agent) = name.strip_suffix(".meta.json").and_then(|n| n.strip_prefix("agent-")) else { continue };
            let Some(meta) = std::fs::read_to_string(&path).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()) else {
                continue;
            };
            let transcript = path.with_file_name(format!("agent-{agent}.jsonl"));
            let modified = std::fs::metadata(&transcript).and_then(|m| m.modified()).or_else(|_| entry.metadata().and_then(|m| m.modified()));
            let recent = modified.as_ref().is_ok_and(|m| m.elapsed().map_or(true, |age| age < SUBAGENT_IDLE));
            found.push(Subagent {
                id: agent.to_string(),
                kind: meta["agentType"].as_str().unwrap_or("Agent").to_string(),
                description: meta["description"].as_str().unwrap_or("").to_string(),
                running: !meta["finished"].as_bool().unwrap_or(false) && recent,
                active: modified.ok().and_then(|m| m.duration_since(SystemTime::UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs() as i64),
            });
        }
        found.sort_by(|a, b| b.running.cmp(&a.running).then(b.active.cmp(&a.active)));
        found
    }

    /// The usage view: 30 days by model family, today by hour, the busiest projects, and the totals of
    /// today, 7 days and 30 days.
    pub fn usage_detail(&self, today: NaiveDate) -> Value {
        let first = today - chrono::Days::new(DETAIL_DAYS - 1);
        let week_start = today - chrono::Days::new(6);
        let family = |model: &str| model_name(model).split(' ').next().unwrap_or("").to_string();

        // A session belongs to the project it started in: its working folder moves with `cd`
        let mut started_in: HashMap<&str, (i64, &str)> = HashMap::new();
        for answer in self.answers.values().filter(|a| !a.project.is_empty()) {
            let first = started_in.entry(answer.session.as_str()).or_insert((answer.time, answer.project.as_str()));
            if answer.time < first.0 {
                *first = (answer.time, answer.project.as_str());
            }
        }

        // Families ranked by tokens over the 30 days
        let mut by_family: HashMap<String, u64> = HashMap::new();
        let mut by_project: HashMap<String, Total> = HashMap::new();
        let mut hours = [0u64; 24];
        let (mut today_total, mut week, mut month) = (Total::default(), Total::default(), Total::default());
        for answer in self.answers.values().filter(|a| a.day >= first && a.day <= today) {
            let tokens = answer.tokens();
            *by_family.entry(family(&answer.model)).or_default() += tokens;
            if let Some((_, name)) = started_in.get(answer.session.as_str()) {
                let project = by_project.entry(name.to_string()).or_default();
                project.tokens += tokens;
                project.cost += answer.cost;
            }
            let add = |total: &mut Total| {
                total.tokens += tokens;
                total.cost += answer.cost;
            };
            add(&mut month);
            if answer.day >= week_start {
                add(&mut week);
            }
            if answer.day == today {
                add(&mut today_total);
                if let Some(time) = DateTime::from_timestamp(answer.time, 0) {
                    hours[time.with_timezone(&Local).hour() as usize] += tokens;
                }
            }
        }
        let mut families: Vec<(String, u64)> = by_family.into_iter().filter(|(name, _)| !name.is_empty()).collect();
        families.sort_by_key(|a| std::cmp::Reverse(a.1));
        families.truncate(MAX_FAMILIES);
        let names: Vec<String> = families.into_iter().map(|(name, _)| name).collect();

        // One entry per day: total, cost, and tokens by family (the named ones, then the others)
        let mut days: Vec<(Total, Vec<u64>)> = vec![(Total::default(), vec![0; names.len() + 1]); DETAIL_DAYS as usize];
        for answer in self.answers.values().filter(|a| a.day >= first && a.day <= today) {
            let index = (answer.day - first).num_days() as usize;
            let (total, split) = &mut days[index];
            total.tokens += answer.tokens();
            total.cost += answer.cost;
            let slot = names.iter().position(|n| *n == family(&answer.model)).unwrap_or(names.len());
            split[slot] += answer.tokens();
        }

        let mut projects: Vec<(String, Total)> = by_project.into_iter().collect();
        projects.sort_by_key(|a| std::cmp::Reverse(a.1.tokens));
        projects.truncate(5);

        let round = |cost: f64| (cost * 100.0).round() / 100.0;
        let total = |t: Total| json!({ "tokens": t.tokens, "cost": round(t.cost) });
        json!({
            "models": names,
            "days": days.iter().map(|(t, split)| json!({ "tokens": t.tokens, "cost": round(t.cost), "m": split })).collect::<Vec<_>>(),
            "hours": hours,
            "projects": projects.iter().map(|(name, t)| json!({
                "name": super::screen_text(name, 40), "tokens": t.tokens, "cost": round(t.cost)
            })).collect::<Vec<_>>(),
            "today": total(today_total),
            "week": total(week),
            "month": total(month),
        })
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
