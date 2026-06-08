// Copyright (c) 2024-2025, Valentin Gusel
// SPDX-License-Identifier: BSD-2-Clause
mod blocklist;
mod bridge;
mod servo_controller;
mod servo_engine;

fn main() {
    let code = bridge::ffi::run_qt_application();
    bridge::shutdown_servo();
    std::process::exit(code);
}
