#[cxx::bridge(namespace = "servoq")]
pub mod ffi {
    unsafe extern "C++" {
        include!("servoq/cpp/qt_app.h");

        fn run_qt_application() -> i32;
    }

    extern "Rust" {
        fn load_url(url: &str);
        fn go_back();
        fn go_forward();
        fn reload();
        fn new_tab();
        fn close_tab(index: i32);
        fn toggle_bookmark();
        fn show_find_in_page();
        fn hide_find_in_page();

        fn current_url() -> String;
        fn title() -> String;
        fn loading() -> bool;
        fn can_go_back() -> bool;
        fn can_go_forward() -> bool;
        fn status_text() -> String;
        fn bookmarks_bar_visible() -> bool;
        fn find_bar_visible() -> bool;
    }
}

pub use crate::servo_controller::{
    bookmarks_bar_visible, can_go_back, can_go_forward, close_tab, current_url, find_bar_visible,
    go_back, go_forward, hide_find_in_page, load_url, loading, new_tab, reload, show_find_in_page,
    status_text, title, toggle_bookmark,
};
