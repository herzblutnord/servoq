#[cxx::bridge(namespace = "servoq")]
pub mod ffi {
    unsafe extern "C++" {
        include!("servoq/cpp/qt_app.h");

        fn run_qt_application() -> i32;
    }

    extern "Rust" {
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
    }
}

pub use crate::servo_controller::{
    can_go_back, can_go_forward, close_tab, create_tab, current_url, find_bar_visible, go_back,
    go_forward, hide_find_in_page, load_url, loading, reload, show_find_in_page, status_text,
    title, toggle_bookmark,
};
