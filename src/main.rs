mod bridge;
mod servo_controller;
mod servo_engine;

fn main() {
    let code = bridge::ffi::run_qt_application();
    bridge::shutdown_servo();
    std::process::exit(code);
}
