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
//! `claude_asks` message. The link task sends both to the screen when they change.

pub mod install;
mod desktop;
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

    /// The link tells whether a screen is connected. Without one, waiting approvals go back to the
    /// terminal.
    pub fn set_screen(&self, connected: bool) {
        self.shared.screen.store(connected, Ordering::Relaxed);
        if !connected {
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

/// Starts the hook server and the collector. `token` is the secret the hooks send.
pub fn start(token: String) -> Claude {
    let (claude, wake_rx, snapshot_tx) = create(token);
    tauri::async_runtime::spawn(server::serve(claude.shared.clone(), PORT));
    let collector = claude.shared.clone();
    std::thread::Builder::new()
        .name("claude-collector".into())
        .spawn(move || collect(collector, wake_rx, snapshot_tx))
        .expect("cannot start the Claude collector");
    claude
}

/// The shared state and its channels, without the tasks.
fn create(token: String) -> (Claude, mpsc::Receiver<()>, watch::Sender<Value>) {
    let (asks_tx, asks) = watch::channel(json!({ "type": "claude_asks", "asks": [] }));
    let (snapshot_tx, snapshot) = watch::channel(json!({ "type": "claude", "sessions": [] }));
    let (wake_tx, wake_rx) = mpsc::channel();
    let shared = Arc::new(Shared {
        token,
        hooks: Mutex::new(Hooks::default()),
        asks: Mutex::new(Vec::new()),
        statusline_limits: Mutex::new(None),
        next_ask: AtomicU64::new(1),
        screen: AtomicBool::new(false),
        asks_tx,
        wake_tx: Mutex::new(wake_tx),
    });
    (Claude { shared, snapshot, asks }, wake_rx, snapshot_tx)
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
fn collect(shared: Arc<Shared>, wake: mpsc::Receiver<()>, snapshot_tx: watch::Sender<Value>) {
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
        let snapshot = {
            let mut hooks = shared.hooks.lock().unwrap();
            hooks.prune(now());
            let waiting: Vec<String> = shared.asks.lock().unwrap().iter().map(|a| a.session.clone()).collect();
            let statusline = shared.statusline_limits.lock().unwrap().clone();
            build_snapshot(&live, &hooks, &waiting, &usage, &desktop, statusline, now())
        };
        snapshot_tx.send_if_modified(|current| {
            let changed = *current != snapshot;
            if changed {
                *current = snapshot;
            }
            changed
        });
    }
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
        let (claude, _wake, _snapshot) = create("secret".into());
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
        let (claude, _wake, _snapshot) = create("secret".into());
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
        let (claude, _wake, _snapshot) = create("secret".into());
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
        let snapshot = build_snapshot(&live, &Hooks::default(), &[], &usage, &desktop, None, now());
        println!("{}", serde_json::to_string_pretty(&snapshot).unwrap());
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
