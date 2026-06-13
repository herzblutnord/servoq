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

    // Result of the HTTP authentication dialog: accepted=false means Cancel
    // (the request proceeds without credentials).
    pub struct AuthDialogResult {
        pub accepted: bool,
        pub username: String,
        pub password: String,
    }

    // Screen and window-frame geometry for window.screen.* APIs, all in
    // device pixels. valid=false when the view has no screen yet.
    pub struct ScreenGeometryResult {
        pub valid: bool,
        pub screen_width: i32,
        pub screen_height: i32,
        pub available_width: i32,
        pub available_height: i32,
        pub window_x: i32,
        pub window_y: i32,
        pub window_width: i32,
        pub window_height: i32,
    }

    unsafe extern "C++" {
        include!("servoq/cpp/qt_app.h");
        fn run_qt_application() -> i32;
        fn servo_profile_data_dir() -> String;
        // Best installed CJK-capable font family for a generic family
        // ("serif"/"sans-serif"/"monospace"), locale-ordered, or "" if none.
        // Used to give Servo's generic font preferences CJK coverage so
        // characters Servo's built-in fallback list misses (CJK punctuation)
        // don't render as tofu. See servo_preferences().
        fn system_cjk_font_family(generic: &str) -> String;

        // Frame and delegate notifications: Rust -> C++ (safe per CXX's unsafe extern "C++" contract)
        include!("servoq/cpp/servo_callbacks.h");
        fn deliver_frame(tab_id: i32, bytes: &[u8], width: i32, height: i32);
        fn notify_url_changed(tab_id: i32, url: &str);
        fn notify_title_changed(tab_id: i32, title: &str);
        fn notify_load_started(tab_id: i32, url: &str);
        fn notify_load_finished(tab_id: i32);
        fn notify_status_changed(tab_id: i32, text: &str);
        fn notify_pdf_navigation_requested(tab_id: i32, url: &str);
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
        // System clipboard bridge for Servo's ClipboardDelegate (QClipboard).
        // Main-thread only: the delegate fires inside spin_event_loop.
        fn clipboard_clear();
        fn clipboard_get_text() -> String;
        fn clipboard_set_text(text: &str);
        // HTTP auth (401/407): modal username/password dialog, like the other
        // *_sync embedder dialogs. for_proxy selects the proxy wording.
        fn show_authentication_dialog_sync(
            tab_id: i32,
            url: &str,
            for_proxy: bool,
        ) -> AuthDialogResult;
        // Permission prompt (Permissions API / notifications / geolocation…).
        // Consults the per-origin PermissionStore first; prompts only when no
        // decision is stored. Returns the allow/deny answer.
        fn request_permission_sync(tab_id: i32, origin: &str, feature: &str) -> bool;
        // window.screen.* backing data (device pixels).
        fn get_screen_geometry(tab_id: i32) -> ScreenGeometryResult;
        // window.moveTo / window.resizeTo: coordinates/size in device pixels;
        // C++ applies the single-tab popup policy and defers the change.
        fn request_window_move_to(tab_id: i32, x: i32, y: i32);
        fn request_window_resize_to(tab_id: i32, width: i32, height: i32);
        // Screenshot result: RGBA8 pixels (width*height*4) or empty = failed.
        fn notify_screenshot_taken(tab_id: i32, data: &[u8], width: i32, height: i32);
        // JS evaluation result: success carries JSON text, failure the error.
        fn notify_javascript_result(tab_id: i32, request_id: u64, success: bool, result: &str);
        // Media Session API event (W3C mediasession) for MPRIS integration.
        // kind: 0 = SetMetadata, 1 = PlaybackStateChange, 2 = SetPositionState.
        // playback_state: 1 = none, 2 = playing, 3 = paused (only for kind 1).
        // Metadata fields populated only for kind 0; position fields for kind 2.
        fn notify_media_session_event(
            tab_id: i32,
            kind: i32,
            playback_state: i32,
            title: &str,
            artist: &str,
            album: &str,
            duration: f64,
            position: f64,
            playback_rate: f64,
        );
        // Page console message (console.log/warn/error…) for the servoq://debug
        // console panel. Only forwarded while console capture is enabled
        // (set_console_capture_enabled) so loop-heavy pages don't flood the FFI.
        fn notify_console_message(tab_id: i32, level: i32, message: &str);
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

        // Asynchronous viewport screenshot; result arrives via
        // notify_screenshot_taken once the page is render-stable.
        fn take_screenshot(id: i32);

        // Debug tooling: evaluate JavaScript in the page; the JSON-serialized
        // result (or error text) arrives via notify_javascript_result with
        // the same request_id.
        fn evaluate_javascript(id: i32, request_id: u64, script: &str);

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
        // Called by unmap_shared_servo_subsurface so navigation paint holding
        // knows the shared surface no longer shows any tab's pixels.
        fn notify_wayland_subsurface_unmapped();

        // Input forwarding: called by WebContentView event handlers
        fn forward_mouse_move(id: i32, x: f32, y: f32);
        fn forward_mouse_button(id: i32, action: i32, button: i32, x: f32, y: f32, mods: u32);
        fn forward_wheel(id: i32, dx: f64, dy: f64, x: f32, y: f32);
        // Touchpad pinch (Qt::ZoomNativeGesture): scale is the per-step
        // multiplicative factor (1.0 + Qt value), x/y the gesture center in
        // device pixels.
        fn forward_pinch_zoom(id: i32, scale: f32, x: f32, y: f32);
        fn forward_key(id: i32, down: bool, key_char: u32, qt_key: i32, mods: u32);
        fn forward_focus(id: i32, focused: bool);
        fn forward_resize(id: i32, w: i32, h: i32, scale: f32);
        fn forward_theme_change(id: i32, dark: bool);
        fn set_webview_active(id: i32, active: bool);

        fn reload_blocklists() -> bool;
        fn user_blocklist_path() -> String;

        // Site data / cookies management (M4.5). list_site_data returns
        // newline-separated "site\tstorage_type_bits" rows (eTLD+1 sites);
        // clear_site_data_for takes newline-separated site names. clear_all_cookies
        // wipes the whole jar; clear_http_cache empties the network cache (M4.6).
        fn list_site_data() -> String;
        fn clear_site_data_for(sites: &str);
        fn clear_all_cookies();
        fn clear_http_cache();

        // Media Session (M5.6): forward an MPRIS control action to the page's
        // media session. action: 0 Play, 1 Pause, 2 SeekBackward, 3 SeekForward,
        // 4 PreviousTrack, 5 NextTrack, 6 SkipAd, 7 Stop, 8 SeekTo.
        fn media_session_action(tab_id: i32, action: i32);

        // Toggles console-message forwarding (notify_console_message). Off by
        // default; the servoq://debug page turns it on while open.
        fn set_console_capture_enabled(enabled: bool);

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
    forward_mouse_button, forward_mouse_move, forward_pinch_zoom, forward_resize,
    forward_theme_change, forward_wheel, init_servo, notify_wayland_subsurface_unmapped,
    present_wayland_webview, set_webview_active, shutdown_servo, tick_servo, tick_webview,
};

// Page zoom — always present; no-ops when servo-engine feature is off
pub use crate::servo_engine::{page_zoom, set_page_zoom};

// Screenshot and JS evaluation — always present; report failure when
// servo-engine is off
pub use crate::servo_engine::{evaluate_javascript, take_screenshot};

pub use crate::blocklist::{reload_blocklists, user_blocklist_path};
pub use crate::servo_engine::{experimental_features_enabled, set_experimental_features_enabled};

// Site data / cookies / cache management and media session control — always
// present; no-ops (or empty results) when the servo-engine feature is off.
pub use crate::servo_engine::{
    clear_all_cookies, clear_http_cache, clear_site_data_for, list_site_data,
    media_session_action, set_console_capture_enabled,
};
