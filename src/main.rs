mod bridge;
mod servo_controller;
mod servo_engine;

fn main() {
    std::process::exit(bridge::ffi::run_qt_application());
}
