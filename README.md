# ServoQ

A Qt6-based browser UI built around the [Servo](https://servo.org/) browser engine.

## About

ServoQ is a port of the [Ladybird Browser](https://github.com/LadybirdBrowser/ladybird) Qt UI to the Servo browser engine. The UI code is derived as closely as possible from Ladybird's Qt frontend, adapted to drive Servo in place of Ladybird's own engine (LibWeb/LibJS).

The goal is a functional, native Qt6 browser shell that takes advantage of Servo's Rust-based, memory-safe rendering pipeline.

The active UI is Qt 6 Widgets/C++ under `cpp/`, with Rust/Servo integration under `src/`. The earlier QML/CXX-Qt prototype path has been removed; `vendor/reference-ladybird/` remains a vendored reference for comparison and audit.

## Attribution

**Ladybird Browser** — https://github.com/LadybirdBrowser/ladybird  
The Qt UI code in this repository is ported from Ladybird, developed by the Ladybird Browser Initiative and contributors. Ladybird is licensed under the BSD 2-Clause License. Original per-file copyright notices are preserved throughout the source tree.

**Servo** — https://github.com/servo/servo  
The browser engine powering ServoQ. Servo is an experimental browser engine written in Rust, originally created by Mozilla Research and now maintained under Linux Foundation Europe. Servo is licensed under the Mozilla Public License 2.0.

## License

ServoQ is licensed under the BSD 2-Clause License. See [LICENSE](LICENSE) for details.

Servo is used as a dependency and remains under its own MPL 2.0 license.
