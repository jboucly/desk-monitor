//! Menu bar overlay: the companion window opens under the tray icon, floats above every app
//! (full screen ones included, on macOS) and hides as soon as it loses the focus.
//!
//! On macOS the window becomes a non-activating NSPanel: a regular window must activate the app
//! to take the keyboard, and macOS then leaves the Space of a full screen app.

use std::sync::Mutex;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Manager, PhysicalPosition, Rect, WebviewWindow};

/// Space between the tray icon and the overlay, and between the overlay and the screen edges.
const GAP: f64 = 6.0;

/// A click on the tray icon first blurs (hides) the open overlay, then reports the click:
/// a click this soon after a hide must not reopen it.
const REOPEN_GUARD: Duration = Duration::from_millis(300);

/// When the overlay was last hidden.
#[derive(Default)]
pub struct Overlay {
    hidden_at: Mutex<Option<Instant>>,
}

/// Turns the window into a floating panel shown above full screen apps and on every Space (macOS).
/// Must run on the main thread.
pub fn setup(window: &WebviewWindow) {
    #[cfg(target_os = "macos")]
    macos::into_panel(window);
    #[cfg(not(target_os = "macos"))]
    let _ = window;
}

/// Left click on the tray icon: opens the overlay under the icon, or closes it.
pub fn toggle(app: &AppHandle, icon: Rect) {
    let Some(window) = app.get_webview_window("main") else { return };
    if window.is_visible().unwrap_or(false) {
        hide(&window);
        return;
    }
    let just_hidden = app.state::<Overlay>().hidden_at.lock().unwrap().is_some_and(|at| at.elapsed() < REOPEN_GUARD);
    if !just_hidden {
        show_at(&window, Some(icon));
    }
}

/// Opens the overlay under the tray icon (menu item, second launch, first launch).
pub fn show(app: &AppHandle) {
    // AppKit calls: the single instance callback does not always run on the main thread
    let handle = app.clone();
    let _ = app.run_on_main_thread(move || {
        let Some(window) = handle.get_webview_window("main") else { return };
        let icon = handle.tray_by_id("main").and_then(|tray| tray.rect().ok().flatten());
        show_at(&window, icon);
    });
}

pub fn hide(window: &WebviewWindow) {
    *window.state::<Overlay>().hidden_at.lock().unwrap() = Some(Instant::now());
    let _ = window.hide();
}

fn show_at(window: &WebviewWindow, icon: Option<Rect>) {
    match icon.and_then(|icon| position_under(window, icon)) {
        Some(position) => {
            let _ = window.set_position(position);
        }
        // The tray icon is not laid out yet (first launch)
        None => {
            let _ = window.center();
        }
    }
    #[cfg(target_os = "macos")]
    macos::show(window);
    #[cfg(not(target_os = "macos"))]
    {
        let _ = window.show();
        let _ = window.set_focus();
    }
}

/// Top-left corner that centers the overlay under the icon, kept inside the icon's screen.
/// Opens above the icon when it sits at the bottom of the screen (Windows taskbar).
fn position_under(window: &WebviewWindow, icon: Rect) -> Option<PhysicalPosition<i32>> {
    let scale = window.scale_factor().ok()?;
    let origin = icon.position.to_physical::<f64>(scale);
    let size = icon.size.to_physical::<f64>(scale);
    if size.width <= 0.0 || size.height <= 0.0 {
        return None;
    }
    let overlay = window.outer_size().ok()?;
    let (width, height) = (overlay.width as f64, overlay.height as f64);
    let gap = GAP * scale;

    let mut x = origin.x + size.width / 2.0 - width / 2.0;
    let mut y = origin.y + size.height + gap;
    if let Ok(Some(monitor)) = window.monitor_from_point(origin.x, origin.y) {
        let area = monitor.work_area();
        let (left, top) = (area.position.x as f64, area.position.y as f64);
        let (right, bottom) = (left + area.size.width as f64, top + area.size.height as f64);
        if y + height > bottom {
            y = origin.y - height - gap;
        }
        x = x.clamp(left + gap, (right - width - gap).max(left + gap));
        y = y.max(top);
    }
    Some(PhysicalPosition::new(x.round() as i32, y.round() as i32))
}

#[cfg(target_os = "macos")]
mod macos {
    use objc2::{define_class, ClassType, MainThreadOnly};
    use objc2_app_kit::{NSPanel, NSWindowCollectionBehavior, NSWindowStyleMask};
    use tauri::WebviewWindow;

    /// NSStatusWindowLevel: the level of the menu bar extras, above normal and floating windows.
    const STATUS_WINDOW_LEVEL: isize = 25;

    define_class!(
        /// NSPanel that takes the keyboard although it is borderless (pairing code, address).
        // SAFETY: NSPanel has no subclassing requirements, and this class adds no ivars
        #[unsafe(super(NSPanel))]
        #[thread_kind = MainThreadOnly]
        #[name = "DeskMonitorPanel"]
        struct Panel;

        impl Panel {
            #[unsafe(method(canBecomeKeyWindow))]
            fn can_become_key_window(&self) -> bool {
                true
            }

            #[unsafe(method(canBecomeMainWindow))]
            fn can_become_main_window(&self) -> bool {
                false
            }
        }
    );

    fn panel(window: &WebviewWindow) -> Option<&NSPanel> {
        let ns_window = window.ns_window().ok()?;
        // SAFETY: Tauri returns the window's live NSWindow, whose class into_panel() swapped
        Some(unsafe { &*ns_window.cast::<NSPanel>() })
    }

    pub fn into_panel(window: &WebviewWindow) {
        let Ok(ns_window) = window.ns_window() else { return };
        // Same technique as the tauri-nspanel plugin: swap the class of tao's window for an
        // NSPanel subclass. The subclass adds no ivars, so tao's own stay where tao reads them.
        // SAFETY: live NSWindow, main thread (setup)
        unsafe { objc2::ffi::object_setClass(ns_window.cast(), Panel::class()) };
        let Some(panel) = panel(window) else { return };
        panel.setStyleMask(panel.styleMask() | NSWindowStyleMask::NonactivatingPanel);
        panel.setFloatingPanel(true);
        panel.setHidesOnDeactivate(false);
        // SAFETY: tao owns the window and releases it itself; AppKit must not release it on close
        unsafe { panel.setReleasedWhenClosed(false) };
        // FullScreenAuxiliary lets the panel show in the Space of a full screen app
        panel.setCollectionBehavior(
            NSWindowCollectionBehavior::CanJoinAllSpaces
                | NSWindowCollectionBehavior::FullScreenAuxiliary
                | NSWindowCollectionBehavior::Stationary
                | NSWindowCollectionBehavior::IgnoresCycle,
        );
        panel.setLevel(STATUS_WINDOW_LEVEL);
    }

    /// Shows the panel and gives it the keyboard without activating the app: activating it
    /// (what `set_focus` does) would switch away from a full screen app.
    pub fn show(window: &WebviewWindow) {
        if let Some(panel) = panel(window) {
            panel.makeKeyAndOrderFront(None);
        }
    }
}
