//! History of the plan limits, for the limits view: the desktop app's samples
//! (`plan-usage-history.json`) and the status line's readings, which the companion keeps itself in
//! `claude-limits.json` (its config folder). From them come the 7-day curve, the pace (use compared
//! with the share of the window already gone) and, when no source gives it, an estimate of the
//! weekly reset (the last big drop of the weekly use).

use serde_json::{json, Value};
use std::path::PathBuf;

const FIVE_HOURS: i64 = 5 * 3600;
const WEEK: i64 = 7 * 86400;
/// Readings kept, and shown on the curve
const KEEP: i64 = 8 * 86400;
const SHOWN: i64 = 7 * 86400;
/// A status line reading is kept at most this often, unless the use moved
const RECORD_EVERY: i64 = 5 * 60;

/// One reading: Unix seconds, percent used of the 5-hour and of the weekly window.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Sample {
    pub t: i64,
    pub five_hour: f64,
    pub week: f64,
}

/// Status line readings, saved in the companion's folder.
#[derive(Default)]
pub struct History {
    samples: Vec<Sample>,
    path: Option<PathBuf>,
}

impl History {
    /// Loads the readings saved in `path` (none when `path` is None, as in the tests).
    pub fn load(path: Option<PathBuf>) -> History {
        let samples = path
            .as_ref()
            .and_then(|p| std::fs::read_to_string(p).ok())
            .and_then(|text| serde_json::from_str::<Vec<(i64, f64, f64)>>(&text).ok())
            .unwrap_or_default()
            .into_iter()
            .map(|(t, five_hour, week)| Sample { t, five_hour, week })
            .collect();
        History { samples, path }
    }

    /// Keeps a status line reading (`{five_hour: {pct}, week: {pct}, at}`).
    pub fn record(&mut self, limits: &Value) {
        let (Some(five_hour), Some(week), Some(t)) =
            (limits["five_hour"]["pct"].as_f64(), limits["week"]["pct"].as_f64(), limits["at"].as_i64())
        else {
            return;
        };
        if let Some(last) = self.samples.last() {
            let moved = (last.five_hour - five_hour).abs() >= 1.0 || (last.week - week).abs() >= 1.0;
            if t - last.t < RECORD_EVERY && !moved {
                return;
            }
        }
        self.samples.push(Sample { t, five_hour, week });
        self.samples.retain(|s| t - s.t < KEEP);
        if let Some(path) = &self.path {
            let rows: Vec<(i64, f64, f64)> = self.samples.iter().map(|s| (s.t, s.five_hour, s.week)).collect();
            if let Err(e) = std::fs::write(path, serde_json::to_string(&rows).unwrap_or_default()) {
                log::warn!("cannot save the Claude limits history: {e}");
            }
        }
    }

    pub fn samples(&self) -> &[Sample] {
        &self.samples
    }
}

/// The limits view: the current limits (as in the snapshot) with their pace, an estimated weekly
/// reset when it is unknown, and the readings of the last 7 days (one per hour at most).
pub fn detail(current: Option<&Value>, statusline: &[Sample], desktop: &[Sample], now: i64) -> Value {
    let mut samples: Vec<Sample> = statusline.iter().chain(desktop).filter(|s| now - s.t < KEEP).copied().collect();
    samples.sort_by_key(|s| s.t);

    let mut view = json!({ "view": "limits" });
    if let Some(current) = current {
        for key in ["at", "source"] {
            view[key] = current[key].clone();
        }
        view["five_hour"] = with_pace(&current["five_hour"], FIVE_HOURS, None, now);
        view["week"] = with_pace(&current["week"], WEEK, last_reset(&samples), now);
    }

    // The curve: the last reading of each hour
    let mut history: Vec<Value> = Vec::new();
    let mut last_hour = i64::MIN;
    for sample in samples.iter().filter(|s| now - s.t < SHOWN) {
        let hour = sample.t / 3600;
        let point = json!([sample.t, sample.five_hour, sample.week]);
        if hour == last_hour {
            *history.last_mut().unwrap() = point;
        } else {
            history.push(point);
            last_hour = hour;
        }
    }
    view["history"] = json!(history);
    view
}

/// One window with its pace: percent used minus the percent of the window already gone. The
/// window's start comes from its reset, else from `reset_seen` (the time of its last reset), and
/// then gives an estimated reset.
fn with_pace(limit: &Value, length: i64, reset_seen: Option<i64>, now: i64) -> Value {
    let mut out = limit.clone();
    let Some(pct) = limit["pct"].as_f64() else { return out };
    let reset = match limit["resets_at"].as_i64() {
        Some(reset) => Some(reset),
        None => {
            // A window starts at its reset: the next one comes a window later
            let estimate = reset_seen.map(|seen| seen + length).filter(|reset| *reset > now);
            if let Some(reset) = estimate {
                out["resets_at"] = json!(reset);
                out["estimated"] = json!(true);
            }
            estimate
        }
    };
    if let Some(reset) = reset {
        let gone = (now - (reset - length)).clamp(0, length) as f64 / length as f64 * 100.0;
        out["pace"] = json!(((pct - gone) * 10.0).round() / 10.0);
    }
    out
}

/// Time of the last reset of the weekly window: the middle of the last big drop of its use.
fn last_reset(samples: &[Sample]) -> Option<i64> {
    samples.windows(2).rev().find(|pair| pair[0].week - pair[1].week >= 10.0).map(|pair| (pair[0].t + pair[1].t) / 2)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(t: i64, five_hour: f64, week: f64) -> Sample {
        Sample { t, five_hour, week }
    }

    #[test]
    fn records_at_most_every_five_minutes_unless_it_moved() {
        let mut history = History::load(None);
        let reading = |t: i64, pct: f64| json!({ "five_hour": { "pct": pct }, "week": { "pct": 10 }, "at": t });
        history.record(&reading(1000, 5.0));
        history.record(&reading(1100, 5.4));
        history.record(&reading(1200, 7.0));
        history.record(&reading(1300 + RECORD_EVERY, 7.0));
        let times: Vec<i64> = history.samples().iter().map(|s| s.t).collect();
        assert_eq!(times, [1000, 1200, 1300 + RECORD_EVERY]);
    }

    #[test]
    fn computes_the_pace_from_the_reset() {
        let now = 1_000_000;
        // Half of the 5 hours gone, 70 % used: 20 points above the pace
        let current = json!({ "five_hour": { "pct": 70.0, "resets_at": now + FIVE_HOURS / 2 }, "week": { "pct": 30.0 }, "at": now, "source": "statusline" });
        let view = detail(Some(&current), &[], &[], now);
        assert_eq!(view["five_hour"]["pace"], 20.0);
        assert!(view["week"].get("pace").is_none());
        assert_eq!(view["source"], "statusline");
    }

    #[test]
    fn estimates_the_weekly_reset_from_a_drop() {
        let now = 2_000_000;
        // The weekly use fell from 90 to 2 two days ago: the window ends in 5 days
        let desktop = [sample(now - 2 * 86400 - 600, 10.0, 90.0), sample(now - 2 * 86400 + 600, 1.0, 2.0), sample(now - 60, 20.0, 30.0)];
        let current = json!({ "five_hour": { "pct": 20.0 }, "week": { "pct": 30.0 }, "at": now - 60, "source": "desktop" });
        let view = detail(Some(&current), &[], &desktop, now);
        assert_eq!(view["week"]["resets_at"], now - 2 * 86400 + WEEK);
        assert_eq!(view["week"]["estimated"], true);
        // 2 of 7 days gone (28.6 %), 30 % used
        assert_eq!(view["week"]["pace"], 1.4);
        // The two readings around the drop share an hour: the curve keeps the later one
        assert_eq!(view["history"].as_array().unwrap().len(), 2);
    }

    #[test]
    fn keeps_one_reading_per_hour() {
        let now = 3_600 * 1000 + 1800;
        let readings = [sample(now - 3000, 1.0, 1.0), sample(now - 2000, 2.0, 2.0), sample(now - 10, 3.0, 3.0)];
        let view = detail(None, &readings, &[], now);
        let history = view["history"].as_array().unwrap();
        assert_eq!(history.len(), 2);
        assert_eq!(history[0], json!([now - 2000, 2.0, 2.0]));
    }
}
