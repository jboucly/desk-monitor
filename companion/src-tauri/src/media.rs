//! System "Now Playing" (the media shown by the macOS Control Center, whatever the app: Music,
//! Spotify, Chrome, Safari…), sent to the screen as `media` and `media_art` messages.

use serde_json::{json, Map, Value};
use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::sync::Arc;
use tokio::sync::watch;

/// Cover size on the screen, in pixels (square).
pub const COVER_SIZE: u32 = 160;

/// A cover ready for the screen.
pub struct Art {
    pub id: String,
    /// `media_art` message (RGB565 pixels in base64)
    pub message: Value,
    /// Dominant color, "#RRGGBB"
    pub color: String,
}

/// Latest now playing state. `media` is the `media` message, `None` when nothing plays.
#[derive(Clone, Default)]
pub struct Snapshot {
    pub media: Option<Value>,
    pub art: Option<Arc<Art>>,
}

/// Handle to the platform watcher.
#[derive(Clone)]
pub struct Media {
    pub updates: watch::Receiver<Snapshot>,
    #[allow(dead_code)]
    resources: Option<std::path::PathBuf>,
}

impl Media {
    /// Runs a command of the screen ("toggle", "next", "prev") on the current player.
    pub fn command(&self, cmd: &str) {
        #[cfg(target_os = "macos")]
        if let Some(dir) = &self.resources {
            macos::send(dir, cmd);
        }
        #[cfg(not(target_os = "macos"))]
        let _ = cmd;
    }
}

/// Starts watching. `resources` is the folder holding the bundled `mediaremote/` adapter (macOS).
pub fn start(resources: Option<std::path::PathBuf>) -> Media {
    let (tx, rx) = watch::channel(Snapshot::default());
    #[cfg(target_os = "macos")]
    if let Some(dir) = resources.clone() {
        tauri::async_runtime::spawn(macos::watch(dir, tx));
    }
    #[cfg(not(target_os = "macos"))]
    {
        // Windows (GlobalSystemMediaTransportControls) is not implemented yet: nothing is reported
        drop(tx);
    }
    Media { updates: rx, resources }
}

/// Builds the `media` message from the adapter's now playing dictionary.
fn media_message(info: &Map<String, Value>, app: &str, color: Option<&str>, art_id: Option<&str>) -> Option<Value> {
    let title = info.get("title").and_then(Value::as_str).filter(|t| !t.trim().is_empty())?;
    let playing = info.get("playing").and_then(Value::as_bool).unwrap_or(false);
    let mut elapsed = info.get("elapsedTime").and_then(Value::as_f64);
    // elapsedTime is the position at `timestamp`: bring it to now while playing
    if let (true, Some(base), Some(stamp)) = (playing, elapsed, info.get("timestamp").and_then(Value::as_str)) {
        if let Some(age) = seconds_since(stamp) {
            let rate = info.get("playbackRate").and_then(Value::as_f64).unwrap_or(1.0);
            elapsed = Some(base + age.max(0.0) * rate);
        }
    }
    let mut m = json!({
        "type": "media",
        "title": clean(title),
        "artist": clean(info.get("artist").and_then(Value::as_str).unwrap_or("")),
        "album": clean(info.get("album").and_then(Value::as_str).unwrap_or("")),
        "app": app,
        "playing": playing,
    });
    if let Some(duration) = info.get("duration").and_then(Value::as_f64).filter(|d| *d > 0.0) {
        m["duration"] = json!((duration * 10.0).round() / 10.0);
    }
    if let Some(elapsed) = elapsed {
        m["elapsed"] = json!((elapsed * 10.0).round() / 10.0);
    }
    if let Some(color) = color {
        m["color"] = json!(color);
    }
    if let Some(id) = art_id {
        m["art"] = json!(id);
    }
    Some(m)
}

/// The screen's fonts are Latin-1: typographic punctuation becomes its ASCII form.
fn clean(text: &str) -> String {
    text.chars()
        .flat_map(|c| -> Vec<char> {
            match c {
                '\u{2018}' | '\u{2019}' | '\u{201B}' | '\u{2032}' => vec!['\''],
                '\u{201C}' | '\u{201D}' | '\u{201E}' => vec!['"'],
                '\u{2010}'..='\u{2015}' | '\u{2212}' => vec!['-'],
                '\u{2026}' => vec!['.', '.', '.'],
                '\u{00A0}' | '\u{2009}' | '\u{202F}' => vec![' '],
                c => vec![c],
            }
        })
        .collect()
}

/// Seconds elapsed since an ISO 8601 UTC time ("2026-10-28T16:05:40Z").
fn seconds_since(stamp: &str) -> Option<f64> {
    let (date, time) = stamp.trim_end_matches('Z').split_once('T')?;
    let mut d = date.split('-').map(|p| p.parse::<i64>().ok());
    let (y, mo, da) = (d.next()??, d.next()??, d.next()??);
    let mut t = time.split(':');
    let (h, mi) = (t.next()?.parse::<i64>().ok()?, t.next()?.parse::<i64>().ok()?);
    let s: f64 = t.next()?.parse().ok()?;
    // Days from the civil date (proleptic Gregorian calendar)
    let yy = if mo <= 2 { y - 1 } else { y };
    let era = if yy >= 0 { yy } else { yy - 399 } / 400;
    let yoe = yy - era * 400;
    let doy = (153 * (mo + if mo > 2 { -3 } else { 9 }) + 2) / 5 + da - 1;
    let days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    let then = days as f64 * 86400.0 + (h * 3600 + mi * 60) as f64 + s;
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).ok()?.as_secs_f64();
    Some(now - then)
}

/// Decodes an artwork, crops it to a square, scales it to the cover size and converts it to the
/// screen's pixel format (RGB565, little-endian). Also picks a dominant color for the controls.
fn prepare_art(bytes: &[u8]) -> Option<Art> {
    let image = image::load_from_memory(bytes).ok()?.to_rgb8();
    let side = image.width().min(image.height());
    let x = (image.width() - side) / 2;
    let y = (image.height() - side) / 2;
    let square = image::imageops::crop_imm(&image, x, y, side, side).to_image();
    let cover = image::imageops::resize(&square, COVER_SIZE, COVER_SIZE, image::imageops::FilterType::Triangle);

    let mut pixels = Vec::with_capacity((COVER_SIZE * COVER_SIZE * 2) as usize);
    for p in cover.pixels() {
        let [r, g, b] = p.0;
        let rgb565 = ((r as u16 >> 3) << 11) | ((g as u16 >> 2) << 5) | (b as u16 >> 3);
        pixels.extend_from_slice(&rgb565.to_le_bytes());
    }

    let mut hasher = DefaultHasher::new();
    bytes.hash(&mut hasher);
    let id = format!("{:016x}", hasher.finish());
    Some(Art {
        message: json!({
            "type": "media_art",
            "id": id,
            "w": COVER_SIZE,
            "h": COVER_SIZE,
            "data": base64_encode(&pixels),
        }),
        color: dominant_color(&cover),
        id,
    })
}

/// Average of the most colorful pixels, kept bright enough to be visible on the dark theme.
fn dominant_color(image: &image::RgbImage) -> String {
    let (mut r, mut g, mut b, mut n) = (0u64, 0u64, 0u64, 0u64);
    for p in image.pixels() {
        let [pr, pg, pb] = p.0;
        let max = pr.max(pg).max(pb) as i32;
        let min = pr.min(pg).min(pb) as i32;
        // Skip greys, near-blacks and near-whites
        if max - min < 40 || max < 60 || min > 220 {
            continue;
        }
        r += pr as u64;
        g += pg as u64;
        b += pb as u64;
        n += 1;
    }
    let (mut r, mut g, mut b) = if n > 0 { ((r / n) as f64, (g / n) as f64, (b / n) as f64) } else { (124.0, 156.0, 255.0) };
    let max = r.max(g).max(b);
    if max < 170.0 {
        let k = 170.0 / max.max(1.0);
        (r, g, b) = ((r * k).min(255.0), (g * k).min(255.0), (b * k).min(255.0));
    }
    format!("#{:02X}{:02X}{:02X}", r as u8, g as u8, b as u8)
}

fn base64_encode(data: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for chunk in data.chunks(3) {
        let n = (chunk[0] as u32) << 16 | (*chunk.get(1).unwrap_or(&0) as u32) << 8 | *chunk.get(2).unwrap_or(&0) as u32;
        out.push(TABLE[(n >> 18) as usize & 63] as char);
        out.push(TABLE[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 { TABLE[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if chunk.len() > 2 { TABLE[n as usize & 63] as char } else { '=' });
    }
    out
}

fn base64_decode(text: &str) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(text.len() * 3 / 4);
    let (mut buffer, mut bits) = (0u32, 0);
    for c in text.bytes() {
        let value = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            b'=' | b'\n' | b'\r' => continue,
            _ => return None,
        };
        buffer = (buffer << 6) | value as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((buffer >> bits) as u8);
        }
    }
    Some(out)
}

#[cfg(target_os = "macos")]
mod macos {
    use super::*;
    use std::collections::HashMap;
    use std::path::{Path, PathBuf};
    use std::process::Stdio;
    use std::time::Duration;
    use tokio::io::{AsyncBufReadExt, BufReader};

    fn script(dir: &Path) -> PathBuf {
        dir.join("mediaremote-adapter.pl")
    }

    fn framework(dir: &Path) -> PathBuf {
        dir.join("MediaRemoteAdapter.framework")
    }

    /// MediaRemote command ids of the adapter's `send` command.
    pub fn send(dir: &Path, cmd: &str) {
        let id = match cmd {
            "toggle" => "2",
            "next" => "4",
            "prev" => "5",
            _ => return,
        };
        let _ = std::process::Command::new("/usr/bin/perl")
            .arg(script(dir))
            .arg(framework(dir))
            .args(["send", id])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn();
    }

    /// Display name of an app from its bundle identifier ("tv.plex.labs.plexamp" → "Plexamp").
    fn app_name(bundle: &str, cache: &mut HashMap<String, String>) -> String {
        if let Some(name) = cache.get(bundle) {
            return name.clone();
        }
        let query = format!("kMDItemCFBundleIdentifier == '{}'", bundle.replace('\'', ""));
        let name = std::process::Command::new("/usr/bin/mdfind")
            .arg(query)
            .output()
            .ok()
            .and_then(|out| {
                String::from_utf8_lossy(&out.stdout)
                    .lines()
                    .next()
                    .and_then(|path| Path::new(path).file_stem().map(|s| s.to_string_lossy().into_owned()))
            })
            .unwrap_or_else(|| bundle.rsplit('.').next().unwrap_or(bundle).to_string());
        cache.insert(bundle.to_string(), name.clone());
        name
    }

    /// Runs the adapter's stream and publishes snapshots. Restarts it when it stops.
    pub async fn watch(dir: PathBuf, tx: watch::Sender<Snapshot>) {
        let mut names = HashMap::new();
        loop {
            let child = tokio::process::Command::new("/usr/bin/perl")
                .arg(script(&dir))
                .arg(framework(&dir))
                .args(["stream", "--debounce=150"])
                .stdout(Stdio::piped())
                .stderr(Stdio::null())
                .kill_on_drop(true)
                .spawn();
            let mut child = match child {
                Ok(child) => child,
                Err(e) => {
                    log::error!("media adapter does not start: {e}");
                    return;
                }
            };
            let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
            let mut info: Map<String, Value> = Map::new();
            let mut art: Option<Arc<Art>> = None;
            let mut art_source = String::new();

            while let Ok(Some(line)) = lines.next_line().await {
                let Ok(update) = serde_json::from_str::<Value>(&line) else { continue };
                let Some(payload) = update.get("payload").and_then(Value::as_object) else { continue };
                if update.get("diff").and_then(Value::as_bool) == Some(true) {
                    for (key, value) in payload {
                        if value.is_null() {
                            info.remove(key);
                        } else {
                            info.insert(key.clone(), value.clone());
                        }
                    }
                } else {
                    info = payload.clone();
                }

                // New artwork: decode and scale it off the async runtime
                let data = info.get("artworkData").and_then(Value::as_str).unwrap_or("").to_string();
                if data != art_source {
                    art_source = data.clone();
                    art = if data.is_empty() {
                        None
                    } else {
                        tokio::task::spawn_blocking(move || base64_decode(&data).and_then(|bytes| prepare_art(&bytes)))
                            .await
                            .ok()
                            .flatten()
                            .map(Arc::new)
                    };
                }

                let bundle = info.get("bundleIdentifier").and_then(Value::as_str).unwrap_or("").to_string();
                let app = if bundle.is_empty() { String::new() } else { app_name(&bundle, &mut names) };
                let media = media_message(&info, &app, art.as_ref().map(|a| a.color.as_str()), art.as_ref().map(|a| a.id.as_str()));
                let _ = tx.send(Snapshot { media, art: art.clone() });
            }
            let _ = child.kill().await;
            log::warn!("media adapter stopped, restarting");
            let _ = tx.send(Snapshot::default());
            tokio::time::sleep(Duration::from_secs(5)).await;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cleans_typography() {
        assert_eq!(clean("L’été – “live”…"), "L'été - \"live\"...");
    }

    #[test]
    fn base64_round_trip() {
        let data = b"desk monitor";
        assert_eq!(base64_decode(&base64_encode(data)).unwrap(), data);
    }

    #[test]
    fn builds_media_message() {
        let info = json!({ "title": "Song", "artist": "A", "playing": false, "duration": 200.0, "elapsedTime": 12.0 });
        let m = media_message(info.as_object().unwrap(), "Plexamp", Some("#112233"), None).unwrap();
        assert_eq!(m["title"], "Song");
        assert_eq!(m["elapsed"], 12.0);
        assert_eq!(m["color"], "#112233");
        assert!(media_message(json!({ "playing": true }).as_object().unwrap(), "", None, None).is_none());
    }
}
