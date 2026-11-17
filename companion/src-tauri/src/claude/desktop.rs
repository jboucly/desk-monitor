//! The Claude desktop app: whether it runs, and the plan limits it records in
//! `plan-usage-history.json` (percent used of the 5-hour and 7-day windows).

use super::limits::Sample;
use serde_json::{json, Value};
use std::path::PathBuf;
use std::time::SystemTime;
use sysinfo::{ProcessRefreshKind, ProcessesToUpdate, System};

/// Folder of the desktop app's data.
fn app_dir() -> Option<PathBuf> {
    #[cfg(target_os = "macos")]
    return std::env::var_os("HOME").map(|home| PathBuf::from(home).join("Library/Application Support/Claude"));
    // Not tested: the Windows app keeps its data in %APPDATA%\Claude
    #[cfg(windows)]
    return std::env::var_os("APPDATA").map(|dir| PathBuf::from(dir).join("Claude"));
    #[cfg(not(any(target_os = "macos", windows)))]
    return std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config/Claude"));
}

#[derive(Default)]
pub struct Desktop {
    modified: Option<SystemTime>,
    /// Last limits read: `{five_hour: {pct}, week: {pct}, at, source}`
    pub limits: Option<Value>,
    /// Every sample of the file, oldest first, for the limits view
    pub history: Vec<Sample>,
    pub running: bool,
}

impl Desktop {
    /// Reads the limits again when the file changed, and checks the app's process.
    pub fn refresh(&mut self, system: &mut System) {
        system.refresh_processes_specifics(ProcessesToUpdate::All, true, ProcessRefreshKind::nothing());
        self.running = system.processes().values().any(|p| {
            let name = p.name().to_string_lossy();
            name == "Claude" || name == "Claude.exe"
        });

        let Some(path) = app_dir().map(|dir| dir.join("plan-usage-history.json")) else { return };
        let Ok(modified) = std::fs::metadata(&path).and_then(|m| m.modified()) else { return };
        if self.modified == Some(modified) {
            return;
        }
        self.modified = Some(modified);
        let text = std::fs::read_to_string(&path).unwrap_or_default();
        self.limits = parse_history(&text);
        self.history = samples(&text);
    }
}

/// Latest sample of the history: `{"samples": [{"t": ms, "u": {"fh": %, "sd": %}}]}`.
fn parse_history(text: &str) -> Option<Value> {
    let history: Value = serde_json::from_str(text).ok()?;
    let latest = history["samples"].as_array()?.iter().filter(|s| s["t"].is_number()).max_by_key(|s| s["t"].as_i64())?;
    let used = &latest["u"];
    Some(json!({
        "five_hour": { "pct": used["fh"].as_f64()? },
        "week": { "pct": used["sd"].as_f64()? },
        "at": latest["t"].as_i64()? / 1000,
        "source": "desktop",
    }))
}

/// All the samples of the history, oldest first.
fn samples(text: &str) -> Vec<Sample> {
    let Ok(history) = serde_json::from_str::<Value>(text) else { return Vec::new() };
    let mut samples: Vec<Sample> = history["samples"]
        .as_array()
        .map(|list| {
            list.iter()
                .filter_map(|s| Some(Sample { t: s["t"].as_i64()? / 1000, five_hour: s["u"]["fh"].as_f64()?, week: s["u"]["sd"].as_f64()? }))
                .collect()
        })
        .unwrap_or_default();
    samples.sort_by_key(|s| s.t);
    samples
}

#[cfg(test)]
mod tests {
    use super::{parse_history, samples};

    #[test]
    fn reads_the_latest_sample() {
        let text = r#"{"version":2,"samples":[{"t":2000000,"u":{"fh":4,"sd":6}},{"t":1000000,"u":{"fh":1,"sd":2}}]}"#;
        let limits = parse_history(text).unwrap();
        assert_eq!(limits["five_hour"]["pct"], 4.0);
        assert_eq!(limits["week"]["pct"], 6.0);
        assert_eq!(limits["at"], 2000);
        assert!(parse_history("{}").is_none());
        let all = samples(text);
        assert_eq!((all.len(), all[0].t, all[1].week), (2, 1000, 6.0));
    }
}
