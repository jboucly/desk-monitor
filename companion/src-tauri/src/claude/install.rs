//! Installs the companion's hooks (and, as an option, a status line relay) in the Claude Code user
//! settings, `~/.claude/settings.json`. Our entries are recognized by the hook server's URL, so that
//! installing again replaces them and uninstalling removes only them. The file is backed up once,
//! then written atomically; key order and the user's other settings are kept.

use super::{claude_dir, PORT};
use serde::Serialize;
use serde_json::{json, Map, Value};
use std::path::{Path, PathBuf};

/// Hook events that only update the session state.
const STATE_EVENTS: &[&str] = &[
    "SessionStart",
    "SessionEnd",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PostToolUseFailure",
    "PermissionDenied",
    "Notification",
    "Stop",
    "StopFailure",
];

/// Seconds Claude Code waits for an approval from the screen; the server answers before (100 s).
const PERMISSION_TIMEOUT: u64 = 110;

/// Script of the status line relay, next to the settings.
const RELAY_SCRIPT: &str = "desk-monitor-statusline.sh";
/// The status line that was there before the relay, restored when it is removed.
const RELAY_BACKUP: &str = "desk-monitor-statusline.json";

/// What is installed, for the window.
#[derive(Clone, Debug, Default, Serialize)]
pub struct InstallState {
    pub hooks: bool,
    pub statusline: bool,
    /// Error while reading the settings, in French
    pub error: String,
}

fn base_url() -> String {
    format!("http://127.0.0.1:{PORT}/")
}

fn settings_path() -> Result<PathBuf, String> {
    claude_dir().map(|dir| dir.join("settings.json")).ok_or_else(|| "Dossier personnel introuvable".to_string())
}

fn read_settings(path: &Path) -> Result<Value, String> {
    match std::fs::read_to_string(path) {
        Ok(text) if text.trim().is_empty() => Ok(json!({})),
        Ok(text) => match serde_json::from_str::<Value>(&text) {
            Ok(value) if value.is_object() => Ok(value),
            _ => Err(format!("{} n'est pas un JSON valide : corrige-le d'abord", path.display())),
        },
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(json!({})),
        Err(e) => Err(format!("Lecture de {} impossible : {e}", path.display())),
    }
}

fn write_settings(path: &Path, settings: &Value) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    // First change: keep the user's file as it was
    let backup = path.with_extension("json.desk-monitor.bak");
    if path.exists() && !backup.exists() {
        let _ = std::fs::copy(path, &backup);
    }
    let text = serde_json::to_string_pretty(settings).map_err(|e| e.to_string())? + "\n";
    let temp = path.with_extension("json.desk-monitor.tmp");
    std::fs::write(&temp, text).map_err(|e| format!("Écriture impossible : {e}"))?;
    std::fs::rename(&temp, path).map_err(|e| format!("Écriture impossible : {e}"))
}

/// True for a hook that points to our server.
fn is_ours(hook: &Value) -> bool {
    hook["url"].as_str().is_some_and(|url| url.starts_with(&base_url()))
}

/// Removes our hooks, and the groups they leave empty.
fn remove_hooks(settings: &mut Value) {
    let Some(events) = settings.get_mut("hooks").and_then(Value::as_object_mut) else { return };
    for groups in events.values_mut() {
        let Some(groups) = groups.as_array_mut() else { continue };
        for group in groups.iter_mut() {
            if let Some(hooks) = group.get_mut("hooks").and_then(Value::as_array_mut) {
                hooks.retain(|h| !is_ours(h));
            }
        }
        groups.retain(|g| g["hooks"].as_array().map_or(true, |h| !h.is_empty()));
    }
    events.retain(|_, groups| groups.as_array().map_or(true, |g| !g.is_empty()));
    if events.is_empty() {
        settings.as_object_mut().unwrap().remove("hooks");
    }
}

/// Adds our hooks (after removing older copies).
fn add_hooks(settings: &mut Value, token: &str) {
    remove_hooks(settings);
    let root = settings.as_object_mut().unwrap();
    let events = root.entry("hooks").or_insert_with(|| Value::Object(Map::new()));
    if !events.is_object() {
        *events = Value::Object(Map::new());
    }
    let events = events.as_object_mut().unwrap();
    let hook = |path: &str, timeout: u64| {
        json!({ "hooks": [{
            "type": "http",
            "url": format!("{}{path}", base_url()),
            "timeout": timeout,
            "headers": { "X-DM-Token": token },
        }]})
    };
    let mut push = |event: &str, group: Value| {
        let groups = events.entry(event).or_insert_with(|| json!([]));
        if !groups.is_array() {
            *groups = json!([]);
        }
        groups.as_array_mut().unwrap().push(group);
    };
    for event in STATE_EVENTS {
        push(event, hook("hook", 2));
    }
    push("PermissionRequest", hook("permission", PERMISSION_TIMEOUT));
}

fn has_hooks(settings: &Value) -> bool {
    settings["hooks"]["PermissionRequest"]
        .as_array()
        .is_some_and(|groups| groups.iter().any(|g| g["hooks"].as_array().is_some_and(|h| h.iter().any(is_ours))))
}

fn relay_command(dir: &Path) -> String {
    format!("sh '{}'", dir.join(RELAY_SCRIPT).display())
}

fn has_relay(settings: &Value, dir: &Path) -> bool {
    settings["statusLine"]["command"].as_str().is_some_and(|c| c == relay_command(dir))
}

/// Quotes a text for a POSIX shell.
fn shell_quote(text: &str) -> String {
    format!("'{}'", text.replace('\'', r"'\''"))
}

/// The relay posts the status line input to the companion in the background, then runs the status
/// line that was there before (or prints nothing).
fn relay_script(token: &str, previous: Option<&str>) -> String {
    let mut script = format!(
        "#!/bin/sh\n\
         # Desk Monitor: sends the status line input (plan limits) to the companion, then runs the\n\
         # previous status line. Written by the Desk Monitor app; remove it from the app.\n\
         input=$(cat)\n\
         printf '%s' \"$input\" | curl -s -m 1 -H 'Content-Type: application/json' -H {} --data-binary @- {}statusline >/dev/null 2>&1 &\n",
        shell_quote(&format!("X-DM-Token: {token}")),
        base_url()
    );
    if let Some(command) = previous {
        script += &format!("printf '%s' \"$input\" | sh -c {}\n", shell_quote(command));
    }
    script
}

/// Current state of the installation.
pub fn state() -> InstallState {
    let result = settings_path().and_then(|path| Ok((read_settings(&path)?, path)));
    match result {
        Ok((settings, path)) => InstallState {
            hooks: has_hooks(&settings),
            statusline: path.parent().is_some_and(|dir| has_relay(&settings, dir)),
            error: String::new(),
        },
        Err(error) => InstallState { error, ..Default::default() },
    }
}

/// Installs or removes the hooks.
pub fn set_hooks(enabled: bool, token: &str) -> Result<(), String> {
    let path = settings_path()?;
    let mut settings = read_settings(&path)?;
    if enabled {
        add_hooks(&mut settings, token);
    } else {
        remove_hooks(&mut settings);
    }
    write_settings(&path, &settings)
}

/// Installs or removes the status line relay.
pub fn set_statusline(enabled: bool, token: &str) -> Result<(), String> {
    if cfg!(windows) {
        return Err("Le relais de la status line n'existe que sur macOS et Linux".into());
    }
    let path = settings_path()?;
    let dir = path.parent().ok_or("Dossier introuvable")?.to_path_buf();
    let mut settings = read_settings(&path)?;
    let installed = has_relay(&settings, &dir);
    let backup = dir.join(RELAY_BACKUP);
    if enabled {
        // Keep the user's status line (once: installing again keeps the first backup)
        let previous = if installed {
            std::fs::read_to_string(&backup).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()).unwrap_or(Value::Null)
        } else {
            settings.get("statusLine").cloned().unwrap_or(Value::Null)
        };
        std::fs::write(&backup, previous.to_string()).map_err(|e| e.to_string())?;
        std::fs::write(dir.join(RELAY_SCRIPT), relay_script(token, previous["command"].as_str())).map_err(|e| e.to_string())?;
        let mut line = if previous.is_object() { previous.clone() } else { json!({ "type": "command" }) };
        line["command"] = json!(relay_command(&dir));
        settings["statusLine"] = line;
    } else {
        if !installed {
            return Ok(());
        }
        let previous = std::fs::read_to_string(&backup).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()).unwrap_or(Value::Null);
        match previous {
            Value::Object(_) => settings["statusLine"] = previous,
            _ => {
                settings.as_object_mut().unwrap().remove("statusLine");
            }
        }
        let _ = std::fs::remove_file(dir.join(RELAY_SCRIPT));
        let _ = std::fs::remove_file(&backup);
    }
    write_settings(&path, &settings)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn installs_again_and_removes_only_ours() {
        let mut settings = json!({
            "model": "opus",
            "hooks": { "Stop": [{ "hooks": [{ "type": "command", "command": "say done" }] }] }
        });
        add_hooks(&mut settings, "t1");
        add_hooks(&mut settings, "t2");
        assert!(has_hooks(&settings));
        let stop = settings["hooks"]["Stop"].as_array().unwrap();
        assert_eq!(stop.len(), 2);
        assert_eq!(stop[1]["hooks"][0]["headers"]["X-DM-Token"], "t2");
        assert_eq!(settings["hooks"]["PermissionRequest"][0]["hooks"][0]["timeout"], 110);

        remove_hooks(&mut settings);
        assert!(!has_hooks(&settings));
        assert_eq!(settings["hooks"], json!({ "Stop": [{ "hooks": [{ "type": "command", "command": "say done" }] }] }));
        assert_eq!(settings["model"], "opus");

        let mut empty = json!({});
        add_hooks(&mut empty, "t");
        remove_hooks(&mut empty);
        assert_eq!(empty, json!({}));
    }

    #[test]
    fn keeps_the_key_order() {
        let mut settings: Value = serde_json::from_str(r#"{"zeta": 1, "alpha": 2}"#).unwrap();
        add_hooks(&mut settings, "t");
        let keys: Vec<&String> = settings.as_object().unwrap().keys().collect();
        assert_eq!(keys, ["zeta", "alpha", "hooks"]);
    }

    #[test]
    fn relay_runs_the_previous_status_line() {
        let script = relay_script("abc", Some("echo 'hi'"));
        assert!(script.contains("-H 'X-DM-Token: abc'"));
        assert!(script.ends_with("printf '%s' \"$input\" | sh -c 'echo '\\''hi'\\'''\n"));
        assert!(!relay_script("abc", None).contains("sh -c"));
    }
}
