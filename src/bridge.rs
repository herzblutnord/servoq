// Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
// Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
// Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
// Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
// Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
// Copyright (c) 2025, Simon Farre <simon.farre.cx@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause
//
// Derived from Ladybird:
//   UI/Qt/BrowserWindow.cpp
#[cxx::bridge(namespace = "servoq")]
pub mod ffi {
    // Result of a JS prompt() dialog: accepted=false means Cancel (JS gets null).
    pub struct PromptDialogResult {
        pub accepted: bool,
        pub value: String,
    }

    unsafe extern "C++" {
        include!("servoq/cpp/qt_app.h");
        fn run_qt_application() -> i32;
        fn servo_profile_data_dir() -> String;

        // Frame and delegate notifications: Rust -> C++ (safe per CXX's unsafe extern "C++" contract)
        include!("servoq/cpp/servo_callbacks.h");
        fn deliver_frame(tab_id: i32, bytes: &[u8], width: i32, height: i32);
        fn notify_url_changed(tab_id: i32, url: &str);
        fn notify_title_changed(tab_id: i32, title: &str);
        fn notify_load_started(tab_id: i32, url: &str);
        fn notify_load_finished(tab_id: i32);
        fn notify_status_changed(tab_id: i32, text: &str);
        fn notify_webview_crashed(tab_id: i32, reason: &str);
        fn notify_request_blocked(tab_id: i32, url: &str);
        fn content_blocking_enabled() -> bool;
        fn content_blocking_host_allowlisted(host: &str) -> bool;
        fn webcontent_frame_pending(tab_id: i32) -> bool;
        fn request_wayland_window_repaint(tab_id: i32);
        // Favicon: RGBA8 bytes (width*height*4), 0 size = clear to default icon.
        fn notify_favicon_changed(tab_id: i32, data: &[u8], width: i32, height: i32);
        // Cursor shape: ServoQ cursor code; C++ maps it explicitly to Qt.
        fn notify_cursor_changed(tab_id: i32, cursor_shape: i32);
        // Fullscreen toggle.
        fn notify_fullscreen_changed(tab_id: i32, fullscreen: bool);
        // History list: newline-separated URLs, current index.
        fn notify_history_changed(tab_id: i32, urls: &str, current: i32);
        // window.open() / target=_blank: create a new tab for an already-built WebView.
        fn request_open_tab_for_id(tab_id: i32);
        // Context menu: newline-separated "action_id\tlabel\tenabled" or "sep".
        // link_url is the href of the right-clicked link, or empty string if none.
        // Returns selected action_id (>=0) or -1 for dismissed / handled internally.
        fn show_context_menu_sync(tab_id: i32, items: &str, link_url: &str) -> i32;
        // Web Notification API desktop notification.
        fn show_notification(tab_id: i32, title: &str, body: &str);
        // Simple JS dialogs (synchronous + modal, like show_context_menu_sync):
        // alert() / confirm() / prompt().
        fn show_alert_dialog_sync(tab_id: i32, message: &str);
        fn show_confirm_dialog_sync(tab_id: i32, message: &str) -> bool;
        fn show_prompt_dialog_sync(
            tab_id: i32,
            message: &str,
            default_value: &str,
        ) -> PromptDialogResult;
        // <select> dropdown: newline-separated lines, either
        // "opt\t<id>\t<label>\t<disabled 0/1>\t<selected 0/1>\t<in_group 0/1>"
        // or "group\t<label>". x/y anchor the menu at the element's bottom-left
        // corner and width is the element width, all in device pixels.
        // Returns the chosen option id (>=0) or -1 for dismissed.
        fn show_select_dropdown_sync(tab_id: i32, items: &str, x: i32, y: i32, width: i32) -> i32;
        // <input type=color>: returns packed 0xRRGGBB (>=0) or -1 for cancelled.
        fn show_color_picker_sync(tab_id: i32, red: u8, green: u8, blue: u8) -> i32;
        // <input type=file>: filters = newline-separated extensions without dot
        // (Servo FilterPattern). Returns newline-separated selected paths;
        // empty string = cancelled.
        fn show_file_picker_sync(tab_id: i32, filters: &str, allow_multiple: bool) -> String;
        // window.close() from web content: close the tab owning this webview.
        fn notify_webview_close_requested(tab_id: i32);
        // Posts a custom QEvent to the Qt main thread to wake the event loop.
        // Called from Servo's background threads (paint, layout, font loading).
        // QCoreApplication::postEvent() is thread-safe.
        fn servoq_wake_event_loop();
    }

    extern "Rust" {
        // Called from run_qt_application() before window.show() to initialize
        // Servo at startup, before Qt show/resize/paint events begin flowing.
        fn init_servo();

        // Existing tab-lifecycle bridge (used by BrowserWindow / Tab chrome)
        fn create_tab() -> i32;
        fn close_tab(id: i32);
        fn load_url(id: i32, url: &str);
        fn go_back(id: i32);
        fn go_forward(id: i32);
        fn reload(id: i32);
        fn toggle_bookmark(id: i32);
        fn show_find_in_page(id: i32);
        fn hide_find_in_page(id: i32);

        fn current_url(id: i32) -> String;
        fn title(id: i32) -> String;
        fn loading(id: i32) -> bool;
        fn can_go_back(id: i32) -> bool;
        fn can_go_forward(id: i32) -> bool;
        fn status_text(id: i32) -> String;
        fn find_bar_visible(id: i32) -> bool;

        // Page zoom
        fn set_page_zoom(id: i32, zoom: f32);
        fn page_zoom(id: i32) -> f32;

        // Engine lifecycle: called by WebContentView when widget is shown/hidden/destroyed
        fn create_webview(id: i32, url: &str, w: i32, h: i32, scale: f32);
        fn create_webview_wayland_window(
            id: i32,
            url: &str,
            w: i32,
            h: i32,
            scale: f32,
            wl_display: u64,
            wl_surface: u64,
            allow_software_gl: bool,
        ) -> bool;
        fn close_webview(id: i32);
        fn shutdown_servo();
        fn tick_webview(id: i32);
        // Called by BrowserWindow::eventFilter on the Qt EventLoopWaker wake event.
        fn tick_servo();
        fn present_wayland_webview(id: i32);

        // Input forwarding: called by WebContentView event handlers
        fn forward_mouse_move(id: i32, x: f32, y: f32);
        fn forward_mouse_button(id: i32, action: i32, button: i32, x: f32, y: f32, mods: u32);
        fn forward_wheel(id: i32, dx: f64, dy: f64, x: f32, y: f32);
        fn forward_key(id: i32, down: bool, key_char: u32, qt_key: i32, mods: u32);
        fn forward_focus(id: i32, focused: bool);
        fn forward_resize(id: i32, w: i32, h: i32, scale: f32);
        fn forward_theme_change(id: i32, dark: bool);
        fn set_webview_active(id: i32, active: bool);

        fn reload_blocklists() -> bool;
        fn user_blocklist_path() -> String;

        // Experimental web platform features toggle (mirrors servoshell EXPERIMENTAL_PREFS).
        fn set_experimental_features_enabled(enabled: bool);
        fn experimental_features_enabled() -> bool;
    }
}

// ------------------------------------------------------------------
// Always-present: controller functions not affected by the engine
// ------------------------------------------------------------------
pub use crate::servo_controller::{
    create_tab, find_bar_visible, hide_find_in_page, show_find_in_page, toggle_bookmark,
};

// Navigation / state functions: placeholder when engine is off, real WebView when on
#[cfg(not(feature = "servo-engine"))]
pub use crate::servo_controller::{
    can_go_back, can_go_forward, close_tab, current_url, go_back, go_forward, load_url, loading,
    reload, status_text, title,
};

#[cfg(feature = "servo-engine")]
pub use crate::servo_engine::{
    can_go_back, can_go_forward, close_tab, current_url, go_back, go_forward, load_url, loading,
    reload, status_text, title,
};

// Engine-lifecycle and input functions (always present; no-ops when feature is off)
pub use crate::servo_engine::{
    close_webview, create_webview, create_webview_wayland_window, forward_focus, forward_key,
    forward_mouse_button, forward_mouse_move, forward_resize, forward_theme_change, forward_wheel,
    init_servo, present_wayland_webview, set_webview_active, shutdown_servo, tick_servo,
    tick_webview,
};

// Page zoom — always present; no-ops when servo-engine feature is off
pub use crate::servo_engine::{page_zoom, set_page_zoom};

pub use crate::blocklist::{reload_blocklists, user_blocklist_path};
pub use crate::servo_engine::{experimental_features_enabled, set_experimental_features_enabled};
