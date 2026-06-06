mod bridge;
mod servo_controller;

fn main() {
    std::process::exit(bridge::ffi::run_qt_application());
}
