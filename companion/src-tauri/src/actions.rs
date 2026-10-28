//! Shortcut actions sent by the screen: key chords, application launch, URL (see protocol/README.md).

use enigo::{Direction, Enigo, Key, Keyboard, Settings};
use serde_json::Value;

/// Runs an action that does not need the main thread (launch, url).
/// Key chords go through `press_keys`, which the caller runs on the main thread.
pub fn run(action: &Value) -> Result<(), String> {
    match action.get("kind").and_then(Value::as_str).unwrap_or("") {
        "launch" => launch(str_field(action, "target")?),
        "url" => open::that_detached(str_field(action, "url")?).map_err(|e| format!("URL impossible à ouvrir : {e}")),
        "keys" => Err("keys must be run with press_keys".into()),
        other => Err(format!("Action « {other} » inconnue de l'app")),
    }
}

/// True for actions that synthesize key presses (main thread, and Accessibility on macOS).
pub fn is_keys(action: &Value) -> bool {
    action.get("kind").and_then(Value::as_str) == Some("keys")
}

fn str_field<'a>(action: &'a Value, key: &str) -> Result<&'a str, String> {
    action.get(key).and_then(Value::as_str).filter(|s| !s.trim().is_empty()).ok_or_else(|| format!("champ « {key} » manquant"))
}

/// Starts an application by name ("Spotify") or path.
fn launch(target: &str) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    let result = {
        // A path or a bundle opens directly; a bare name is looked up by Launch Services
        let mut command = std::process::Command::new("/usr/bin/open");
        if target.contains('/') {
            command.arg(target);
        } else {
            command.args(["-a", target]);
        }
        command.status()
    };
    #[cfg(windows)]
    let result = std::process::Command::new("cmd").args(["/C", "start", "", target]).status();
    #[cfg(not(any(target_os = "macos", windows)))]
    let result = std::process::Command::new(target).spawn().map(|_| std::process::ExitStatus::default());

    match result {
        Ok(status) if status.success() => Ok(()),
        Ok(_) => Err(format!("« {target} » introuvable")),
        Err(e) => Err(format!("« {target} » ne démarre pas : {e}")),
    }
}

/// Presses chords like "cmd+shift+4" or "ctrl+c ctrl+v" (chords separated by spaces).
pub fn press_keys(spec: &str) -> Result<(), String> {
    let chords: Vec<(Vec<Key>, Key)> = spec.split_whitespace().map(parse_chord).collect::<Result<_, _>>()?;
    if chords.is_empty() {
        return Err("raccourci vide".into());
    }
    let mut enigo = Enigo::new(&Settings::default()).map_err(|e| format!("clavier indisponible : {e}"))?;
    for (modifiers, key) in chords {
        for m in &modifiers {
            enigo.key(*m, Direction::Press).map_err(key_error)?;
        }
        let pressed = enigo.key(key, Direction::Click).map_err(key_error);
        // Modifiers are always released, even when the key failed
        for m in modifiers.iter().rev() {
            let _ = enigo.key(*m, Direction::Release);
        }
        pressed?;
    }
    Ok(())
}

fn key_error(e: enigo::InputError) -> String {
    format!("touche refusée : {e}")
}

fn parse_chord(chord: &str) -> Result<(Vec<Key>, Key), String> {
    let parts: Vec<String> = chord.split('+').map(|p| p.trim().to_lowercase()).filter(|p| !p.is_empty()).collect();
    let (last, mods) = parts.split_last().ok_or_else(|| format!("raccourci « {chord} » invalide"))?;
    let modifiers = mods
        .iter()
        .map(|m| match m.as_str() {
            "cmd" | "command" | "meta" | "win" | "super" => Ok(Key::Meta),
            "ctrl" | "control" => Ok(Key::Control),
            "alt" | "option" | "opt" => Ok(Key::Alt),
            "shift" => Ok(Key::Shift),
            other => Err(format!("modificateur « {other} » inconnu")),
        })
        .collect::<Result<Vec<_>, _>>()?;
    Ok((modifiers, parse_key(last)?))
}

fn parse_key(name: &str) -> Result<Key, String> {
    let key = match name {
        "enter" | "return" => Key::Return,
        "tab" => Key::Tab,
        "space" => Key::Space,
        "esc" | "escape" => Key::Escape,
        "backspace" => Key::Backspace,
        "delete" | "del" => Key::Delete,
        "up" => Key::UpArrow,
        "down" => Key::DownArrow,
        "left" => Key::LeftArrow,
        "right" => Key::RightArrow,
        "home" => Key::Home,
        "end" => Key::End,
        "pageup" => Key::PageUp,
        "pagedown" => Key::PageDown,
        "volumeup" => Key::VolumeUp,
        "volumedown" => Key::VolumeDown,
        "mute" => Key::VolumeMute,
        "playpause" | "play" => Key::MediaPlayPause,
        "next" => Key::MediaNextTrack,
        "prev" | "previous" => Key::MediaPrevTrack,
        _ => {
            if let Some(n) = name.strip_prefix('f').and_then(|n| n.parse::<u8>().ok()) {
                return function_key(n);
            }
            let mut chars = name.chars();
            match (chars.next(), chars.next()) {
                (Some(c), None) if c.is_ascii_digit() => digit_key(c),
                (Some(c), None) => Key::Unicode(c),
                _ => return Err(format!("touche « {name} » inconnue")),
            }
        }
    };
    Ok(key)
}

fn function_key(n: u8) -> Result<Key, String> {
    Ok(match n {
        1 => Key::F1,
        2 => Key::F2,
        3 => Key::F3,
        4 => Key::F4,
        5 => Key::F5,
        6 => Key::F6,
        7 => Key::F7,
        8 => Key::F8,
        9 => Key::F9,
        10 => Key::F10,
        11 => Key::F11,
        12 => Key::F12,
        _ => return Err(format!("touche F{n} non prise en charge")),
    })
}

/// Digits use the physical key of the number row, like system shortcuts do: on an AZERTY keyboard
/// "4" is typed with Shift, but cmd+shift+4 still means the "4" key.
fn digit_key(c: char) -> Key {
    #[cfg(target_os = "macos")]
    {
        // kVK_ANSI_0 … kVK_ANSI_9
        const CODES: [u32; 10] = [0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19];
        Key::Other(CODES[c.to_digit(10).unwrap_or(0) as usize])
    }
    #[cfg(windows)]
    {
        // VK_0 … VK_9
        Key::Other(c as u32)
    }
    #[cfg(not(any(target_os = "macos", windows)))]
    {
        Key::Unicode(c)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_chords() {
        let (mods, key) = parse_chord("cmd+shift+4").unwrap();
        assert_eq!(mods, vec![Key::Meta, Key::Shift]);
        assert!(matches!(key, Key::Other(_)));
        assert_eq!(parse_chord("ctrl+c").unwrap().1, Key::Unicode('c'));
        assert_eq!(parse_chord("F5").unwrap().1, Key::F5);
        assert!(parse_chord("hyper+x").is_err());
    }
}
