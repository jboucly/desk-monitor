//! Window that shows the web editor served by the screen, so that the user does not need a browser.
//!
//! The page comes from the screen: it gets no access to the Tauri commands (the capabilities only
//! grant the "main" window, and remote URLs get nothing unless declared).

use tauri::{AppHandle, Manager, Url, WebviewUrl, WebviewWindowBuilder};

pub const LABEL: &str = "editor";

/// Opens the editor window on `url`, or brings it to the front (reloaded there when the screen's
/// address changed).
pub fn open(app: &AppHandle, url: &str) -> Result<(), String> {
    let url: Url = url.parse().map_err(|e| format!("adresse de l'éditeur invalide ({e})"))?;
    set_regular(app, true);
    if let Some(window) = app.get_webview_window(LABEL) {
        if window.url().map(|current| current.origin() != url.origin()).unwrap_or(true) {
            window.navigate(url).map_err(|e| e.to_string())?;
        }
        let _ = window.unminimize();
        let _ = window.show();
        let _ = window.set_focus();
        return Ok(());
    }
    WebviewWindowBuilder::new(app, LABEL, WebviewUrl::External(url))
        .title("Desk Monitor — Éditeur")
        .inner_size(1280.0, 840.0)
        .min_inner_size(900.0, 600.0)
        .center()
        .focused(true)
        .build()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

/// The editor window was closed.
pub fn closed(app: &AppHandle) {
    set_regular(app, false);
}

/// A menu bar app has no Dock icon and is not in Cmd+Tab: fine for the overlay, not for an
/// editor window that the user switches to and from. Regular while the editor is open (macOS).
fn set_regular(app: &AppHandle, regular: bool) {
    #[cfg(target_os = "macos")]
    {
        let policy = if regular { tauri::ActivationPolicy::Regular } else { tauri::ActivationPolicy::Accessory };
        let _ = app.set_activation_policy(policy);
    }
    #[cfg(not(target_os = "macos"))]
    let _ = (app, regular);
}
