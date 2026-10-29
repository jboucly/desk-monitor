//! Desk Monitor PC companion: a menu bar / tray app that streams metrics to the desk screen
//! and runs the shortcuts tapped on it.

mod actions;
mod config;
mod link;
mod media;
mod metrics;
mod overlay;

use link::{Command, Link, LinkState};
use tauri::menu::{Menu, MenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Manager, State, WebviewWindow, WindowEvent};

/// True when the app may synthesize key presses (macOS Accessibility). Always true elsewhere.
pub fn accessibility_granted(prompt: bool) -> bool {
    #[cfg(target_os = "macos")]
    {
        if prompt {
            macos_accessibility_client::accessibility::application_is_trusted_with_prompt()
        } else {
            macos_accessibility_client::accessibility::application_is_trusted()
        }
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = prompt;
        true
    }
}

#[tauri::command]
fn link_state(link: State<Link>) -> LinkState {
    link.state.lock().unwrap().clone()
}

#[tauri::command]
fn start_pairing(link: State<Link>) {
    let _ = link.commands.send(Command::StartPairing);
}

#[tauri::command]
fn submit_code(link: State<Link>, code: String) {
    let _ = link.commands.send(Command::SubmitCode(code));
}

#[tauri::command]
fn set_host(link: State<Link>, host: String) {
    let _ = link.commands.send(Command::SetHost(host.trim().to_string()));
}

#[tauri::command]
fn forget(link: State<Link>) {
    let _ = link.commands.send(Command::Forget);
}

/// Whether key shortcuts can work; `prompt` opens the macOS permission dialog.
#[tauri::command]
fn accessibility(prompt: bool) -> bool {
    accessibility_granted(prompt)
}

/// Escape key in the overlay.
#[tauri::command]
fn hide_overlay(window: WebviewWindow) {
    overlay::hide(&window);
}

/// Folder of the bundled MediaRemote adapter (macOS): in the app's resources, or in the source
/// tree when running `tauri dev`.
fn media_adapter_dir(app: &AppHandle) -> Option<std::path::PathBuf> {
    let bundled = app.path().resource_dir().ok().map(|dir| dir.join("mediaremote"));
    let source = Some(std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("resources/mediaremote"));
    [bundled, source].into_iter().flatten().find(|dir| dir.join("mediaremote-adapter.pl").exists())
}

pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _, _| overlay::show(app)))
        .plugin(tauri_plugin_autostart::init(tauri_plugin_autostart::MacosLauncher::LaunchAgent, None))
        .invoke_handler(tauri::generate_handler![link_state, start_pairing, submit_code, set_host, forget, accessibility, hide_overlay])
        .setup(|app| {
            // Menu bar app: no Dock icon on macOS
            #[cfg(target_os = "macos")]
            app.set_activation_policy(tauri::ActivationPolicy::Accessory);

            let store = config::ConfigStore::new(app.path().app_config_dir()?);
            let media = media::start(media_adapter_dir(app.handle()));
            let link = link::start(app.handle().clone(), store, media);
            app.manage(link);
            app.manage(overlay::Overlay::default());
            if let Some(window) = app.get_webview_window("main") {
                overlay::setup(&window);
            }

            let open = MenuItem::with_id(app, "open", "Ouvrir Desk Monitor", true, None::<&str>)?;
            let quit = MenuItem::with_id(app, "quit", "Quitter", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&open, &quit])?;
            let mut tray = TrayIconBuilder::with_id("main")
                .tooltip("Desk Monitor")
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "open" => overlay::show(app),
                    "quit" => app.exit(0),
                    _ => {}
                })
                // Left click toggles the overlay; right click keeps the menu (open, quit)
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, rect, .. } = event {
                        overlay::toggle(tray.app_handle(), rect);
                    }
                });
            if let Some(icon) = app.default_window_icon() {
                tray = tray.icon(icon.clone());
            }
            tray.build(app)?;

            // First launch: open the window so that the user can pair
            if !app.state::<Link>().state.lock().unwrap().paired {
                overlay::show(app.handle());
            }
            Ok(())
        })
        .on_window_event(|window, event| match event {
            // Closing the window keeps the app running in the menu bar
            WindowEvent::CloseRequested { api, .. } => {
                api.prevent_close();
                let _ = window.hide();
            }
            // Like a menu bar popover: a click elsewhere closes the overlay
            WindowEvent::Focused(false) => {
                if let Some(window) = window.app_handle().get_webview_window(window.label()) {
                    overlay::hide(&window);
                }
            }
            _ => {}
        })
        .run(tauri::generate_context!())
        .expect("error while running the Desk Monitor companion");
}
