#[cxx::bridge(namespace = "servoq")]
pub mod ffi {
    unsafe extern "C++" {
        include!("servoq/cpp/qt_app.h");
        fn run_qt_application() -> i32;

        // Frame and delegate notifications: Rust -> C++ (safe per CXX's unsafe extern "C++" contract)
        include!("servoq/cpp/servo_callbacks.h");
        fn deliver_frame(tab_id: i32, bytes: &[u8], width: i32, height: i32);
        fn notify_url_changed(tab_id: i32, url: &str);
        fn notify_title_changed(tab_id: i32, title: &str);
        fn notify_load_started(tab_id: i32, url: &str);
        fn notify_load_finished(tab_id: i32);
        fn notify_status_changed(tab_id: i32, text: &str);
    }

    extern "Rust" {
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

        // Engine lifecycle: called by WebContentView when widget is shown/hidden/destroyed
        fn create_webview(id: i32, url: &str, w: i32, h: i32, scale: f32);
        fn close_webview(id: i32);
        fn tick_webview(id: i32);

        // Input forwarding: called by WebContentView event handlers
        fn forward_mouse_move(id: i32, x: f32, y: f32);
        fn forward_mouse_button(id: i32, action: i32, button: i32, x: f32, y: f32);
        fn forward_wheel(id: i32, dx: f64, dy: f64, x: f32, y: f32);
        fn forward_key(id: i32, down: bool, key_char: u32, qt_key: i32, mods: u32);
        fn forward_focus(id: i32, focused: bool);
        fn forward_resize(id: i32, w: i32, h: i32, scale: f32);
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
    close_webview, create_webview, forward_focus, forward_key, forward_mouse_button,
    forward_mouse_move, forward_resize, forward_wheel, tick_webview,
};
