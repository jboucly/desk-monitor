//! Running Claude Code sessions: the live registry Claude Code keeps in `~/.claude/sessions/<pid>.json`,
//! refined by the events of the hooks (state, tool in use).

use serde_json::Value;
use std::collections::HashMap;
use std::path::Path;
use sysinfo::{Pid, ProcessRefreshKind, ProcessesToUpdate, System};

/// State shown on the screen.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Working,
    Waiting,
    Idle,
}

impl State {
    pub fn as_str(self) -> &'static str {
        match self {
            State::Working => "working",
            State::Waiting => "waiting",
            State::Idle => "idle",
        }
    }
}

/// A session of the registry, whose process is alive.
#[derive(Clone, Debug)]
pub struct Live {
    pub id: String,
    pub cwd: String,
    pub entrypoint: String,
    pub state: State,
    /// Unix seconds of the last state change
    pub since: i64,
}

/// Reads the registry and keeps the sessions whose process still runs.
pub fn read_registry(dir: &Path, system: &mut System) -> Vec<Live> {
    let Ok(entries) = std::fs::read_dir(dir) else { return Vec::new() };
    let mut found = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().map_or(true, |e| e != "json") {
            continue;
        }
        let Some(value) = std::fs::read_to_string(&path).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()) else {
            continue;
        };
        if let (Some(pid), Some(live)) = (value["pid"].as_u64(), parse_registry(&value)) {
            found.push((Pid::from_u32(pid as u32), live));
        }
    }
    let pids: Vec<Pid> = found.iter().map(|(pid, _)| *pid).collect();
    system.refresh_processes_specifics(ProcessesToUpdate::Some(&pids), true, ProcessRefreshKind::nothing());
    found.into_iter().filter(|(pid, _)| system.process(*pid).is_some()).map(|(_, live)| live).collect()
}

/// One registry file. Unknown statuses count as idle, except the ones that wait for the user.
fn parse_registry(value: &Value) -> Option<Live> {
    let id = value["sessionId"].as_str()?.to_string();
    let status = value["status"].as_str().unwrap_or("idle");
    let state = match status {
        "busy" | "working" | "running" => State::Working,
        s if s.contains("wait") || s.contains("input") || s.contains("permission") => State::Waiting,
        _ => State::Idle,
    };
    let millis = value["statusUpdatedAt"].as_i64().or_else(|| value["startedAt"].as_i64()).unwrap_or(0);
    Some(Live {
        id,
        cwd: value["cwd"].as_str().unwrap_or("").to_string(),
        entrypoint: value["entrypoint"].as_str().unwrap_or("").to_string(),
        state,
        since: millis / 1000,
    })
}

/// What the hooks told about a session.
#[derive(Clone, Debug)]
pub struct HookState {
    pub state: State,
    pub since: i64,
    pub tool: String,
    pub cwd: String,
    /// Unix seconds of the last event, to forget sessions whose end was missed
    pub seen: i64,
}

/// States of the sessions, from their hook events.
#[derive(Default)]
pub struct Hooks {
    pub sessions: HashMap<String, HookState>,
}

impl Hooks {
    /// Applies one hook event (the JSON Claude Code posts). `now` is in Unix seconds.
    pub fn apply(&mut self, event: &Value, now: i64) {
        let Some(id) = event["session_id"].as_str() else { return };
        let name = event["hook_event_name"].as_str().unwrap_or("");
        if name == "SessionEnd" {
            self.sessions.remove(id);
            return;
        }
        let entry = self.sessions.entry(id.to_string()).or_insert_with(|| HookState {
            state: State::Idle,
            since: now,
            tool: String::new(),
            cwd: String::new(),
            seen: now,
        });
        entry.seen = now;
        if let Some(cwd) = event["cwd"].as_str() {
            entry.cwd = cwd.to_string();
        }
        let next = match name {
            "SessionStart" | "Stop" | "StopFailure" => Some(State::Idle),
            "UserPromptSubmit" | "PostToolUse" | "PostToolUseFailure" | "PermissionDenied" => Some(State::Working),
            "PreToolUse" => {
                entry.tool = event["tool_name"].as_str().unwrap_or("").to_string();
                Some(State::Working)
            }
            "PermissionRequest" => Some(State::Waiting),
            // idle_prompt: Claude waits for the next prompt; permission_prompt is handled by PermissionRequest
            "Notification" if event["notification_type"] == "idle_prompt" => Some(State::Idle),
            _ => None,
        };
        if let Some(state) = next {
            if state != entry.state {
                entry.state = state;
                entry.since = now;
            }
            if state == State::Idle {
                entry.tool.clear();
            }
        }
    }

    /// Forgets the sessions silent for more than two hours.
    pub fn prune(&mut self, now: i64) {
        self.sessions.retain(|_, s| now - s.seen < 2 * 3600);
    }
}

/// Where the session runs, from Claude Code's entrypoint ("cli", "claude-vscode", "claude-desktop"…).
pub fn surface(entrypoint: &str) -> &'static str {
    match entrypoint {
        "" => "other",
        e if e.contains("vscode") || e.contains("cursor") => "vscode",
        e if e.contains("jetbrains") || e.contains("intellij") => "jetbrains",
        e if e.contains("desktop") => "desktop",
        e if e.contains("cli") => "cli",
        _ => "other",
    }
}

/// Last folder of a path: the project name.
pub fn project_of(cwd: &str) -> String {
    Path::new(cwd).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn follows_hook_events() {
        let mut hooks = Hooks::default();
        let event = |name: &str| json!({ "session_id": "s1", "hook_event_name": name, "cwd": "/p/desk", "tool_name": "Bash" });
        hooks.apply(&event("UserPromptSubmit"), 10);
        assert_eq!(hooks.sessions["s1"].state, State::Working);
        hooks.apply(&event("PreToolUse"), 11);
        assert_eq!(hooks.sessions["s1"].tool, "Bash");
        assert_eq!(hooks.sessions["s1"].since, 10);
        hooks.apply(&event("PermissionRequest"), 12);
        assert_eq!(hooks.sessions["s1"].state, State::Waiting);
        hooks.apply(&event("Stop"), 20);
        assert_eq!(hooks.sessions["s1"].state, State::Idle);
        assert!(hooks.sessions["s1"].tool.is_empty());
        hooks.apply(&event("SessionEnd"), 30);
        assert!(hooks.sessions.is_empty());
    }

    #[test]
    fn reads_registry_entries() {
        let live = parse_registry(&json!({
            "pid": 1, "sessionId": "s1", "cwd": "/a/desk-monitor", "status": "busy",
            "statusUpdatedAt": 1794905411285_i64, "entrypoint": "claude-vscode"
        }))
        .unwrap();
        assert_eq!(live.state, State::Working);
        assert_eq!(live.since, 1794905411);
        assert_eq!(surface(&live.entrypoint), "vscode");
        assert_eq!(project_of(&live.cwd), "desk-monitor");
    }
}
