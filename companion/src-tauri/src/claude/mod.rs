//! Claude Code on the screen (see protocol/README.md, "Claude Code"): running sessions, token usage,
//! plan limits, and approvals answered from the screen.
//!
//! Sources:
//! - the live session registry of Claude Code (`~/.claude/sessions`), read every second;
//! - the hooks installed in `~/.claude/settings.json`, posted to a local HTTP server (`server.rs`);
//! - the transcripts, for the tokens (`usage.rs`);
//! - the desktop app's plan limits (`desktop.rs`) and the status line's (`rate_limits`).
//!
//! A collector thread merges them into the `claude` snapshot, and the pending approvals make the
//! `claude_asks` message. While the screen shows a detail view (`claude_view`), the collector also
//! builds its `claude_detail`. The link task sends them to the screen when they change.

pub mod install;
mod desktop;
mod limits;
mod server;
mod sessions;
mod usage;

use serde_json::{json, Value};
use sessions::{Hooks, State};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::sync::{oneshot, watch};

/// Port of the hook server (127.0.0.1 only).
pub const PORT: u16 = 47823;

/// Sessions sent to the screen, at most.
const MAX_SESSIONS: usize = 6;

/// Answer of the screen to an approval.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Decision {
    Allow,
    Deny,
    /// Let Claude Code ask in the terminal
    Ask,
}

/// An approval waiting for the screen.
pub struct Ask {
    id: String,
    session: String,
    project: String,
    tool: String,
    detail: String,
    /// The tool call, to recognize it when it is settled in the terminal
    tool_name: String,
    tool_input: Value,
    reply: Option<oneshot::Sender<Decision>>,
}

impl Ask {
    /// Builds an approval from a PermissionRequest hook event.
    fn from_event(event: &Value, reply: oneshot::Sender<Decision>) -> Ask {
        let tool_name = event["tool_name"].as_str().unwrap_or("").to_string();
        let cwd = event["cwd"].as_str().unwrap_or("");
        Ask {
            id: String::new(),
            session: event["session_id"].as_str().unwrap_or("").to_string(),
            project: screen_text(&sessions::project_of(cwd), 60),
            tool: screen_text(&tool_label(&tool_name), 40),
            detail: screen_text(&tool_detail(&tool_name, &event["tool_input"], cwd), 200),
            tool_name,
            tool_input: event["tool_input"].clone(),
            reply: Some(reply),
        }
    }

    /// Answers the waiting hook request, once.
    fn resolve(&mut self, decision: Decision) {
        if let Some(reply) = self.reply.take() {
            let _ = reply.send(decision);
        }
    }
}

/// State shared by the hook server, the collector thread and the link.
pub struct Shared {
    token: String,
    hooks: Mutex<Hooks>,
    asks: Mutex<Vec<Ask>>,
    statusline_limits: Mutex<Option<Value>>,
    /// Status line readings of the plan limits, for the limits view
    limits_history: Mutex<limits::History>,
    /// Detail view open on the screen: "session" (with the session id), "usage" or "limits"
    view: Mutex<Option<(String, String)>>,
    next_ask: AtomicU64,
    screen: AtomicBool,
    asks_tx: watch::Sender<Value>,
    /// Wakes the collector up when a hook changed something
    wake_tx: Mutex<mpsc::Sender<()>>,
}

impl Shared {
    fn wake(&self) {
        let _ = self.wake_tx.lock().unwrap().send(());
    }

    fn screen_connected(&self) -> bool {
        self.screen.load(Ordering::Relaxed)
    }

    fn apply_hook(&self, event: &Value) {
        self.hooks.lock().unwrap().apply(event, now());
        self.settle_in_terminal(event);
        self.wake();
    }

    /// An event showing that an approval was settled in the terminal (the tool ran or was refused,
    /// the turn ended) releases it.
    fn settle_in_terminal(&self, event: &Value) {
        let name = event["hook_event_name"].as_str().unwrap_or("");
        let session = event["session_id"].as_str().unwrap_or("");
        let same_call = |ask: &Ask| ask.tool_name == event["tool_name"].as_str().unwrap_or("") && ask.tool_input == event["tool_input"];
        let mut asks = self.asks.lock().unwrap();
        let mut changed = false;
        for ask in asks.iter_mut().filter(|a| a.session == session && a.reply.is_some()) {
            let settled = match name {
                "PostToolUse" | "PostToolUseFailure" | "PermissionDenied" => same_call(ask),
                "UserPromptSubmit" | "Stop" | "StopFailure" | "SessionEnd" => true,
                _ => false,
            };
            if settled {
                ask.resolve(Decision::Ask);
                changed = true;
            }
        }
        if changed {
            asks.retain(|a| a.reply.is_some());
            self.publish_asks(&asks);
        }
    }

    fn add_ask(&self, mut ask: Ask) -> String {
        ask.id = format!("a{}", self.next_ask.fetch_add(1, Ordering::Relaxed));
        let id = ask.id.clone();
        let mut asks = self.asks.lock().unwrap();
        asks.push(ask);
        self.publish_asks(&asks);
        drop(asks);
        self.wake();
        id
    }

    fn remove_ask(&self, id: &str) {
        let mut asks = self.asks.lock().unwrap();
        let before = asks.len();
        asks.retain(|a| a.id != id);
        if asks.len() != before {
            self.publish_asks(&asks);
            drop(asks);
            self.wake();
        }
    }

    fn publish_asks(&self, asks: &[Ask]) {
        let list: Vec<Value> = asks
            .iter()
            .filter(|a| a.reply.is_some())
            .map(|a| json!({ "id": a.id, "project": a.project, "tool": a.tool, "detail": a.detail }))
            .collect();
        self.asks_tx.send_replace(json!({ "type": "claude_asks", "asks": list }));
    }
}

/// Handle used by the link and the window.
#[derive(Clone)]
pub struct Claude {
    shared: Arc<Shared>,
    /// `claude` message, rebuilt when something changes
    pub snapshot: watch::Receiver<Value>,
    /// `claude_asks` message
    pub asks: watch::Receiver<Value>,
    /// `claude_detail` message of the open view; without a `view` key when none is open
    pub detail: watch::Receiver<Value>,
}

impl Claude {
    /// Answer of the screen (`claude_answer`).
    pub fn answer(&self, id: &str, decision: &str) {
        let decision = match decision {
            "allow" => Decision::Allow,
            "deny" => Decision::Deny,
            _ => Decision::Ask,
        };
        let mut asks = self.shared.asks.lock().unwrap();
        if let Some(ask) = asks.iter_mut().find(|a| a.id == id) {
            ask.resolve(decision);
        }
        asks.retain(|a| a.reply.is_some());
        self.shared.publish_asks(&asks);
    }

    /// Detail view the screen opened (`claude_view`): "session", "usage", "limits", or None when it
    /// closed. Its data comes on `detail`.
    pub fn set_view(&self, view: Option<&str>, id: &str) {
        let view = view.filter(|v| matches!(*v, "session" | "usage" | "limits")).map(|v| (v.to_string(), id.to_string()));
        *self.shared.view.lock().unwrap() = view;
        self.shared.wake();
    }

    /// The link tells whether a screen is connected. Without one, waiting approvals go back to the
    /// terminal.
    pub fn set_screen(&self, connected: bool) {
        self.shared.screen.store(connected, Ordering::Relaxed);
        if !connected {
            *self.shared.view.lock().unwrap() = None;
            let mut asks = self.shared.asks.lock().unwrap();
            asks.iter_mut().for_each(|a| a.resolve(Decision::Ask));
            asks.clear();
            self.shared.publish_asks(&asks);
        }
    }

    /// Number of sessions in the last snapshot, for the window.
    pub fn session_count(&self) -> usize {
        self.snapshot.borrow()["sessions"].as_array().map_or(0, Vec::len)
    }

    pub fn token(&self) -> &str {
        &self.shared.token
    }
}

/// Starts the hook server and the collector. `token` is the secret the hooks send; `data_dir` is
/// the companion's config folder, where the limits history is kept.
pub fn start(token: String, data_dir: Option<PathBuf>) -> Claude {
    let (claude, wake_rx, snapshot_tx, detail_tx) = create(token, data_dir.map(|dir| dir.join("claude-limits.json")));
    tauri::async_runtime::spawn(server::serve(claude.shared.clone(), PORT));
    let collector = claude.shared.clone();
    std::thread::Builder::new()
        .name("claude-collector".into())
        .spawn(move || collect(collector, wake_rx, snapshot_tx, detail_tx))
        .expect("cannot start the Claude collector");
    claude
}

/// The shared state and its channels, without the tasks.
fn create(token: String, history: Option<PathBuf>) -> (Claude, mpsc::Receiver<()>, watch::Sender<Value>, watch::Sender<Value>) {
    let (asks_tx, asks) = watch::channel(json!({ "type": "claude_asks", "asks": [] }));
    let (snapshot_tx, snapshot) = watch::channel(json!({ "type": "claude", "sessions": [] }));
    let (detail_tx, detail) = watch::channel(json!({ "type": "claude_detail" }));
    let (wake_tx, wake_rx) = mpsc::channel();
    let shared = Arc::new(Shared {
        token,
        hooks: Mutex::new(Hooks::default()),
        asks: Mutex::new(Vec::new()),
        statusline_limits: Mutex::new(None),
        limits_history: Mutex::new(limits::History::load(history)),
        view: Mutex::new(None),
        next_ask: AtomicU64::new(1),
        screen: AtomicBool::new(false),
        asks_tx,
        wake_tx: Mutex::new(wake_tx),
    });
    (Claude { shared, snapshot, asks, detail }, wake_rx, snapshot_tx, detail_tx)
}

/// Folder of the Claude Code user data (`~/.claude`).
pub fn claude_dir() -> Option<PathBuf> {
    std::env::var_os("CLAUDE_CONFIG_DIR")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")).map(|h| PathBuf::from(h).join(".claude")))
}

fn now() -> i64 {
    chrono::Utc::now().timestamp()
}

/// Collector loop: every second, or sooner after a hook event.
fn collect(shared: Arc<Shared>, wake: mpsc::Receiver<()>, snapshot_tx: watch::Sender<Value>, detail_tx: watch::Sender<Value>) {
    let Some(root) = claude_dir() else {
        log::warn!("no home folder: Claude Code sessions are not followed");
        return;
    };
    let mut system = sysinfo::System::new();
    let mut usage = usage::Usage::default();
    let mut desktop = desktop::Desktop::default();
    let (mut last_scan, mut last_desktop) = (None::<Instant>, None::<Instant>);
    loop {
        // A burst of hook events makes one snapshot
        if wake.recv_timeout(Duration::from_secs(1)).is_ok() {
            std::thread::sleep(Duration::from_millis(200));
            while wake.try_recv().is_ok() {}
        }
        if last_scan.map_or(true, |t| t.elapsed() >= Duration::from_secs(5)) {
            usage.scan(&root.join("projects"));
            last_scan = Some(Instant::now());
        }
        if last_desktop.map_or(true, |t| t.elapsed() >= Duration::from_secs(30)) {
            desktop.refresh(&mut system);
            last_desktop = Some(Instant::now());
        }
        let live = sessions::read_registry(&root.join("sessions"), &mut system);
        let view = shared.view.lock().unwrap().clone();
        let (snapshot, detail) = {
            let mut hooks = shared.hooks.lock().unwrap();
            hooks.prune(now());
            let waiting: Vec<String> = shared.asks.lock().unwrap().iter().map(|a| a.session.clone()).collect();
            let statusline = shared.statusline_limits.lock().unwrap().clone();
            let snapshot = build_snapshot(&live, &hooks, &waiting, &usage, &desktop, statusline, now());
            let detail = match view.as_ref().map(|(v, id)| (v.as_str(), id.as_str())) {
                Some(("session", id)) => session_detail(&snapshot, id, &live, &hooks, &usage),
                Some(("usage", _)) => Some(json!({ "view": "usage" }).merge(usage.usage_detail(chrono::Local::now().date_naive()))),
                Some(("limits", _)) => {
                    let history = shared.limits_history.lock().unwrap();
                    Some(limits::detail(snapshot.get("limits"), history.samples(), &desktop.history, now()))
                }
                _ => None,
            };
            (snapshot, detail)
        };
        let mut detail = detail.unwrap_or_else(|| json!({}));
        detail["type"] = json!("claude_detail");
        for (tx, value) in [(&snapshot_tx, snapshot), (&detail_tx, detail)] {
            tx.send_if_modified(|current| {
                let changed = *current != value;
                if changed {
                    *current = value;
                }
                changed
            });
        }
    }
}

/// Adds the keys of another JSON object.
trait Merge {
    fn merge(self, other: Value) -> Value;
}

impl Merge for Value {
    fn merge(mut self, other: Value) -> Value {
        if let (Some(target), Value::Object(source)) = (self.as_object_mut(), other) {
            target.extend(source);
        }
        self
    }
}

/// The session view: the session as in the snapshot (found by its short id), with its tokens by
/// kind, its subagents and its last tool calls.
fn session_detail(snapshot: &Value, short_id: &str, live: &[sessions::Live], hooks: &Hooks, usage: &usage::Usage) -> Option<Value> {
    let item = snapshot["sessions"].as_array()?.iter().find(|s| s["id"] == short_id)?.clone();
    let id = live.iter().map(|l| &l.id).chain(hooks.sessions.keys()).find(|id| id.starts_with(short_id))?.clone();
    let tokens = usage.session_detail(&id);
    let total = tokens.input + tokens.output + tokens.cache_read + tokens.cache_write;
    let context = item["context"].as_u64().unwrap_or(0);
    let subagents: Vec<Value> = usage
        .subagents(&id)
        .iter()
        .take(6)
        .map(|agent| {
            json!({
                "type": screen_text(&agent.kind, 30),
                "description": screen_text(&agent.description, 80),
                "running": agent.running,
                "tokens": tokens.agents.get(&agent.id).copied().unwrap_or(0),
            })
        })
        .collect();
    let tools: Vec<Value> = hooks.sessions.get(&id).map_or_else(Vec::new, |hook| {
        hook.tools
            .iter()
            .rev()
            .map(|call| {
                json!({
                    "name": screen_text(&tool_label(&call.tool), 24),
                    "detail": screen_text(&call.detail, 80),
                    "status": call.status,
                    "at": call.at,
                })
            })
            .collect()
    });
    let round = |cost: f64| (cost * 100.0).round() / 100.0;
    Some(item.merge(json!({
        "view": "session",
        // A context past 200k tokens means the 1M window
        "context_max": if context > 200_000 { 1_000_000 } else { 200_000 },
        "tokens": {
            "in": tokens.input, "out": tokens.output,
            "cache_read": tokens.cache_read, "cache_write": tokens.cache_write,
        },
        "total": total,
        "cost": round(tokens.cost),
        "started": tokens.started,
        "subagents": subagents,
        "tools": tools,
    })))
}

/// One session as the screen shows it, before sorting.
struct Row {
    id: String,
    cwd: String,
    entrypoint: String,
    state: State,
    since: i64,
    tool: String,
}

/// Merges the sources into the `claude` message.
fn build_snapshot(
    live: &[sessions::Live],
    hooks: &Hooks,
    waiting: &[String],
    usage: &usage::Usage,
    desktop: &desktop::Desktop,
    statusline: Option<Value>,
    now: i64,
) -> Value {
    let mut rows: HashMap<String, Row> = HashMap::new();
    for session in live {
        rows.insert(
            session.id.clone(),
            Row {
                id: session.id.clone(),
                cwd: session.cwd.clone(),
                entrypoint: session.entrypoint.clone(),
                state: session.state,
                since: session.since,
                tool: String::new(),
            },
        );
    }
    for (id, hook) in &hooks.sessions {
        match rows.get_mut(id) {
            // The hooks are more precise than the registry when they are as recent
            Some(row) => {
                if hook.since + 1 >= row.since {
                    row.state = hook.state;
                    row.since = hook.since;
                }
                if row.state == State::Working {
                    row.tool = hook.tool.clone();
                }
            }
            // Session unknown to the registry (older Claude Code, SDK): kept while it is recent
            None if hook.state != State::Idle || now - hook.seen < 10 * 60 => {
                rows.insert(
                    id.clone(),
                    Row {
                        id: id.clone(),
                        cwd: hook.cwd.clone(),
                        entrypoint: String::new(),
                        state: hook.state,
                        since: hook.since,
                        tool: hook.tool.clone(),
                    },
                );
            }
            None => {}
        }
    }
    for id in waiting {
        if let Some(row) = rows.get_mut(id) {
            row.state = State::Waiting;
        }
    }

    let mut rows: Vec<Row> = rows.into_values().collect();
    let rank = |s: State| match s {
        State::Waiting => 0,
        State::Working => 1,
        State::Idle => 2,
    };
    rows.sort_by(|a, b| rank(a.state).cmp(&rank(b.state)).then(b.since.cmp(&a.since)));
    let sessions: Vec<Value> = rows
        .iter()
        .take(MAX_SESSIONS)
        .map(|row| {
            let project = sessions::project_of(&row.cwd);
            let stats = usage.session(&row.id);
            let mut item = json!({
                "id": row.id.chars().take(8).collect::<String>(),
                "name": screen_text(usage.title(&row.id).unwrap_or(&project), 60),
                "project": screen_text(&project, 40),
                "surface": sessions::surface(&row.entrypoint),
                "state": row.state.as_str(),
                "since": row.since,
                "context": stats.map_or(0, |s| s.context),
                "tokens": stats.map_or(0, |s| s.tokens),
            });
            if !row.tool.is_empty() {
                item["tool"] = json!(screen_text(&tool_label(&row.tool), 30));
            }
            if let Some(model) = stats.map(|s| usage::model_name(&s.model)).filter(|m| !m.is_empty()) {
                item["model"] = json!(model);
            }
            let running = usage.subagents(&row.id).iter().filter(|a| a.running).count();
            if running > 0 {
                item["subagents"] = json!(running);
            }
            item
        })
        .collect();

    let days = usage.days(chrono::Local::now().date_naive());
    let week = days.iter().fold(usage::Total::default(), |sum, d| usage::Total { tokens: sum.tokens + d.tokens, cost: sum.cost + d.cost });
    let today = days.last().copied().unwrap_or_default();
    let round = |cost: f64| (cost * 100.0).round() / 100.0;
    let mut snapshot = json!({
        "type": "claude",
        "desktop": desktop.running,
        "sessions": sessions,
        "today": { "tokens": today.tokens, "cost": round(today.cost) },
        "week": { "tokens": week.tokens, "cost": round(week.cost) },
        "days": days.iter().map(|d| d.tokens).collect::<Vec<_>>(),
    });
    // The most recent reading of the plan limits
    let at = |v: &Option<Value>| v.as_ref().and_then(|v| v["at"].as_i64()).unwrap_or(i64::MIN);
    let limits = if at(&statusline) >= at(&desktop.limits) { statusline } else { desktop.limits.clone() };
    if let Some(limits) = limits {
        snapshot["limits"] = limits;
    }
    snapshot
}

/// Plan limits from the status line input:
/// `{"five_hour": {"used_percentage", "resets_at"}, "seven_day": {…}}`.
fn statusline_limits(rate_limits: &Value) -> Option<Value> {
    let window = |w: &Value| -> Option<Value> {
        let pct = w["used_percentage"].as_f64().or_else(|| w["utilization"].as_f64())?;
        let mut out = json!({ "pct": (pct * 10.0).round() / 10.0 });
        if let Some(reset) = reset_time(&w["resets_at"]) {
            out["resets_at"] = json!(reset);
        }
        Some(out)
    };
    let five_hour = window(&rate_limits["five_hour"])?;
    let week = window(&rate_limits["seven_day"]).or_else(|| window(&rate_limits["week"]))?;
    Some(json!({ "five_hour": five_hour, "week": week, "at": now(), "source": "statusline" }))
}

/// A reset time given as Unix seconds, milliseconds or an ISO 8601 string.
fn reset_time(value: &Value) -> Option<i64> {
    if let Some(n) = value.as_f64() {
        return Some(if n > 1e11 { (n / 1000.0) as i64 } else { n as i64 });
    }
    chrono::DateTime::parse_from_rfc3339(value.as_str()?).ok().map(|t| t.timestamp())
}

/// Name of a tool for the screen: MCP tools ("mcp__server__tool") keep their last part.
fn tool_label(name: &str) -> String {
    match name.strip_prefix("mcp__") {
        Some(rest) => rest.rsplit("__").next().unwrap_or(rest).replace('_', " "),
        None => name.to_string(),
    }
}

/// What a tool call will run or touch: the command, the file, the URL…
fn tool_detail(tool: &str, input: &Value, cwd: &str) -> String {
    let text = |key: &str| input[key].as_str().map(str::to_string);
    let relative = |path: String| path.strip_prefix(cwd).map(|p| p.trim_start_matches('/').to_string()).unwrap_or(path);
    let found = match tool {
        "Bash" | "PowerShell" => text("command"),
        "Edit" | "MultiEdit" | "Write" | "Read" | "NotebookEdit" => text("file_path").or_else(|| text("notebook_path")).map(relative),
        "WebFetch" => text("url"),
        "WebSearch" => text("query"),
        "Glob" | "Grep" => text("pattern"),
        "Task" | "Agent" => text("description"),
        _ => None,
    };
    found.unwrap_or_else(|| match input {
        Value::Null => String::new(),
        Value::Object(map) if map.is_empty() => String::new(),
        other => other.to_string(),
    })
}

/// Text the screen fonts can draw (Latin-1 plus typographic punctuation), on one line, cut to
/// `max` characters.
fn screen_text(text: &str, max: usize) -> String {
    let mut out = String::new();
    for c in text.chars() {
        let c = match c {
            '\n' | '\r' | '\t' => ' ',
            '\u{2018}' | '\u{2019}' => '\'',
            '\u{201C}' | '\u{201D}' => '"',
            '\u{2013}' | '\u{2014}' | '\u{2026}' | '\u{2022}' | '\u{20AC}' => c,
            c if (c as u32) < 0x20 => continue,
            c if (c as u32) > 0xFF => '?',
            c => c,
        };
        if c == ' ' && out.ends_with(' ') {
            continue;
        }
        out.push(c);
    }
    let out = out.trim();
    if out.chars().count() > max {
        out.chars().take(max - 1).collect::<String>() + "\u{2026}"
    } else {
        out.to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Posts JSON to the hook server and returns the response body.
    async fn post(port: u16, path: &str, token: &str, body: &Value) -> String {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        let mut stream = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
        let body = body.to_string();
        let request = format!(
            "POST {path} HTTP/1.1\r\nHost: x\r\nX-DM-Token: {token}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        );
        stream.write_all(request.as_bytes()).await.unwrap();
        let mut response = String::new();
        stream.read_to_string(&mut response).await.unwrap();
        response
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn answers_an_approval_from_the_screen() {
        let (claude, _wake, _snapshot, _detail) = create("secret".into(), None);
        let port = 47900 + (std::process::id() % 90) as u16;
        tokio::spawn(server::serve(claude.shared.clone(), port));
        tokio::time::sleep(Duration::from_millis(200)).await;
        let event = json!({
            "hook_event_name": "PermissionRequest", "session_id": "s1", "cwd": "/p/desk",
            "tool_name": "Bash", "tool_input": { "command": "rm -rf build" }
        });

        // Wrong secret, then no screen: answered at once
        assert!(post(port, "/permission", "nope", &event).await.starts_with("HTTP/1.1 401"));
        assert!(post(port, "/permission", "secret", &event).await.ends_with("{}"));

        // With a screen, the request waits for its answer
        claude.set_screen(true);
        let pending = tokio::spawn(async move { post(port, "/permission", "secret", &event).await });
        let mut asks = claude.asks.clone();
        asks.wait_for(|a| a["asks"].as_array().is_some_and(|l| !l.is_empty())).await.unwrap();
        let ask = asks.borrow()["asks"][0].clone();
        assert_eq!(ask["project"], "desk");
        assert_eq!(ask["detail"], "rm -rf build");
        claude.answer(ask["id"].as_str().unwrap(), "deny");
        let response = pending.await.unwrap();
        assert!(response.contains(r#""behavior":"deny""#), "{response}");
        assert!(claude.asks.borrow()["asks"].as_array().unwrap().is_empty());
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn releases_an_approval_settled_in_the_terminal() {
        let (claude, _wake, _snapshot, _detail) = create("secret".into(), None);
        let port = 47900 + (std::process::id() % 90) as u16 + 1;
        tokio::spawn(server::serve(claude.shared.clone(), port));
        tokio::time::sleep(Duration::from_millis(200)).await;
        claude.set_screen(true);
        let event = json!({
            "hook_event_name": "PermissionRequest", "session_id": "s1", "cwd": "/p",
            "tool_name": "Bash", "tool_input": { "command": "ls" }
        });
        let pending = tokio::spawn(async move { post(port, "/permission", "secret", &event).await });
        let mut asks = claude.asks.clone();
        asks.wait_for(|a| a["asks"].as_array().is_some_and(|l| !l.is_empty())).await.unwrap();
        let done = json!({ "hook_event_name": "PostToolUse", "session_id": "s1", "tool_name": "Bash", "tool_input": { "command": "ls" } });
        post(port, "/hook", "secret", &done).await;
        assert!(pending.await.unwrap().ends_with("{}"));
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn removes_an_approval_whose_request_was_dropped() {
        let (claude, _wake, _snapshot, _detail) = create("secret".into(), None);
        let port = 47900 + (std::process::id() % 90) as u16 + 2;
        tokio::spawn(server::serve(claude.shared.clone(), port));
        tokio::time::sleep(Duration::from_millis(200)).await;
        claude.set_screen(true);
        let event = json!({
            "hook_event_name": "PermissionRequest", "session_id": "s1", "cwd": "/p",
            "tool_name": "Bash", "tool_input": { "command": "touch x" }
        });
        // Refused in Claude Code's own dialog: the hook request is dropped, no other hook comes
        let pending = tokio::spawn(async move { post(port, "/permission", "secret", &event).await });
        let mut asks = claude.asks.clone();
        asks.wait_for(|a| a["asks"].as_array().is_some_and(|l| !l.is_empty())).await.unwrap();
        pending.abort();
        let cleared = tokio::time::timeout(
            Duration::from_secs(3),
            asks.wait_for(|a| a["asks"].as_array().is_some_and(|l| l.is_empty())),
        )
        .await;
        assert!(cleared.is_ok(), "the approval stayed on the screen");
    }

    #[test]
    fn builds_the_session_view_from_the_hooks() {
        let mut hooks = Hooks::default();
        let at = now();
        let event = |name: &str, command: &str| {
            json!({ "hook_event_name": name, "session_id": "0123456789abcdef", "cwd": "/p/desk", "tool_name": "Bash", "tool_input": { "command": command } })
        };
        hooks.apply(&event("UserPromptSubmit", ""), at);
        hooks.apply(&event("PreToolUse", "ls"), at);
        hooks.apply(&event("PostToolUse", "ls"), at);
        hooks.apply(&event("PreToolUse", "cargo test"), at);
        let usage = usage::Usage::default();
        let snapshot = build_snapshot(&[], &hooks, &[], &usage, &desktop::Desktop::default(), None, at);
        assert_eq!(snapshot["sessions"][0]["id"], "01234567");

        let view = session_detail(&snapshot, "01234567", &[], &hooks, &usage).unwrap();
        assert_eq!(view["view"], "session");
        assert_eq!(view["state"], "working");
        assert_eq!(view["project"], "desk");
        let tools = view["tools"].as_array().unwrap();
        assert_eq!((tools[0]["detail"].as_str(), tools[0]["status"].as_str()), (Some("cargo test"), Some("run")));
        assert_eq!(tools[1]["status"], "ok");
        assert!(session_detail(&snapshot, "nope", &[], &hooks, &usage).is_none());
    }

    /// Manual check against this computer's Claude Code data:
    /// `cargo test local_snapshot -- --ignored --nocapture`.
    #[test]
    #[ignore]
    fn local_snapshot() {
        let root = claude_dir().unwrap();
        let mut system = sysinfo::System::new();
        let mut usage = usage::Usage::default();
        let started = Instant::now();
        usage.scan(&root.join("projects"));
        println!("scan: {:?}", started.elapsed());
        let mut desktop = desktop::Desktop::default();
        desktop.refresh(&mut system);
        let live = sessions::read_registry(&root.join("sessions"), &mut system);
        let hooks = Hooks::default();
        let snapshot = build_snapshot(&live, &hooks, &[], &usage, &desktop, None, now());
        println!("{}", serde_json::to_string_pretty(&snapshot).unwrap());

        // The three detail views, with their size in the WebSocket frame
        let usage_view = usage.usage_detail(chrono::Local::now().date_naive());
        let limits_view = limits::detail(snapshot.get("limits"), &[], &desktop.history, now());
        let first = snapshot["sessions"][0]["id"].as_str().unwrap_or("");
        let session_view = session_detail(&snapshot, first, &live, &hooks, &usage).unwrap_or_default();
        for (name, view) in [("usage", &usage_view), ("limits", &limits_view), ("session", &session_view)] {
            println!("{name} view: {} bytes\n{}", view.to_string().len(), serde_json::to_string_pretty(view).unwrap());
        }
    }

    #[test]
    fn describes_tool_calls() {
        assert_eq!(tool_detail("Bash", &json!({ "command": "ls -la" }), "/p"), "ls -la");
        assert_eq!(tool_detail("Edit", &json!({ "file_path": "/p/src/main.rs" }), "/p"), "src/main.rs");
        assert_eq!(tool_label("mcp__github__create_issue"), "create issue");
        assert_eq!(tool_detail("Other", &json!({}), "/p"), "");
    }

    #[test]
    fn makes_screen_text() {
        assert_eq!(screen_text("l’été\n  🚀 ok", 40), "l'été ? ok");
        assert_eq!(screen_text("abcdef", 4), "abc\u{2026}");
    }

    #[test]
    fn reads_status_line_limits() {
        let limits = statusline_limits(&json!({
            "five_hour": { "used_percentage": 42.0, "resets_at": "2026-11-17T12:00:00Z" },
            "seven_day": { "used_percentage": 78.0, "resets_at": 1795000000 }
        }))
        .unwrap();
        assert_eq!(limits["five_hour"]["pct"], 42.0);
        assert_eq!(limits["five_hour"]["resets_at"], 1794916800);
        assert_eq!(limits["week"]["resets_at"], 1795000000);
        assert!(statusline_limits(&Value::Null).is_none());
    }

    #[test]
    fn waiting_sessions_come_first() {
        let live = vec![
            sessions::Live { id: "a".into(), cwd: "/x/one".into(), entrypoint: "cli".into(), state: State::Working, since: 50 },
            sessions::Live { id: "b".into(), cwd: "/x/two".into(), entrypoint: "claude-vscode".into(), state: State::Idle, since: 10 },
        ];
        let snapshot = build_snapshot(&live, &Hooks::default(), &["b".into()], &usage::Usage::default(), &desktop::Desktop::default(), None, 100);
        assert_eq!(snapshot["sessions"][0]["project"], "two");
        assert_eq!(snapshot["sessions"][0]["state"], "waiting");
        assert_eq!(snapshot["sessions"][1]["surface"], "cli");
        assert_eq!(snapshot["days"].as_array().unwrap().len(), 7);
        assert!(snapshot.get("limits").is_none());
    }
}
