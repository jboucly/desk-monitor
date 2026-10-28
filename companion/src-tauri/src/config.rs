//! Companion settings, stored as JSON in the app's config folder.

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

/// Address used when the user has not typed one: the screen announces itself with mDNS.
pub const DEFAULT_HOST: &str = "desk-monitor.local";

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Config {
    /// Screen address ("desk-monitor.local", "192.168.1.50", "localhost:8081" for the simulator).
    #[serde(default)]
    pub host: String,
    /// Token given by the screen at pairing time.
    #[serde(default)]
    pub token: String,
}

impl Config {
    /// Address to connect to, falling back to the mDNS name.
    pub fn host_or_default(&self) -> String {
        if self.host.trim().is_empty() {
            DEFAULT_HOST.to_string()
        } else {
            self.host.trim().to_string()
        }
    }
}

/// Reads and writes the config file.
#[derive(Clone)]
pub struct ConfigStore {
    path: PathBuf,
}

impl ConfigStore {
    pub fn new(dir: PathBuf) -> Self {
        Self { path: dir.join("config.json") }
    }

    pub fn load(&self) -> Config {
        std::fs::read_to_string(&self.path)
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default()
    }

    pub fn save(&self, config: &Config) {
        if let Some(dir) = self.path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        match serde_json::to_string_pretty(config) {
            Ok(text) => {
                if let Err(e) = std::fs::write(&self.path, text) {
                    log::error!("cannot save {}: {e}", self.path.display());
                }
            }
            Err(e) => log::error!("cannot serialize the config: {e}"),
        }
    }
}
