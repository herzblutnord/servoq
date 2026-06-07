# ServoQ Crash Debugging

Use this when `servoq` crashes in the Servo embedder, especially crashes in the font stack such as `hb_face_reference_table`, `hb_font_set_variations`, `FT_Load_Glyph`, or `FontContext::create_font`.

## Run With Logs

Build the Servo-enabled binary first:

```sh
CARGO_TARGET_DIR=target-servo CC=clang CXX=clang++ cargo build --features servo-engine
```

Run with ServoQ diagnostics enabled and collect stdout/stderr:

```sh
SERVOQ_DEBUG=1 RUST_BACKTRACE=full \
  CARGO_TARGET_DIR=target-servo CC=clang CXX=clang++ \
  cargo run --features servo-engine 2>&1 | tee /tmp/servoq.log
```

If you already built the binary and want fewer Cargo lines in the log:

```sh
SERVOQ_DEBUG=1 RUST_BACKTRACE=full \
  ./target-servo/debug/servoq 2>&1 | tee /tmp/servoq.log
```

Useful optional Servo/Rust logging knobs:

```sh
SERVOQ_DEBUG=1 RUST_BACKTRACE=full RUST_LOG=servo=debug,fonts=debug \
  ./target-servo/debug/servoq 2>&1 | tee /tmp/servoq.log
```

`/tmp/servoq.log` should include tab ids, active-tab state, navigation URLs, load status, frame delivery sizes, resize sizes/DPR, and WebView creation/destruction.

## Collect The Log

After a crash, keep the log intact:

```sh
cp /tmp/servoq.log /tmp/servoq.$(date +%Y%m%d-%H%M%S).log
sed -n '1,240p' /tmp/servoq.log
tail -240 /tmp/servoq.log
```

Look for the last lines matching:

```sh
rg 'SERVOQ_DEBUG|load_status|create_webview|close_webview|deliver_frame|resize|ignored_' /tmp/servoq.log
```

## Inspect The Core Dump

List recent dumps:

```sh
coredumpctl list servoq
```

Open the latest `servoq` dump in GDB:

```sh
coredumpctl gdb servoq
```

Inside GDB, collect all thread stacks:

```gdb
set pagination off
thread apply all bt full
info sharedlibrary
quit
```

To save the GDB output:

```sh
coredumpctl gdb servoq -q -ex 'set pagination off' -ex 'thread apply all bt full' -ex 'info sharedlibrary' -ex quit > /tmp/servoq-gdb.txt
```

## Check Font Configuration

The observed crash stack points into HarfBuzz/FreeType and Servo's font creation path. Capture system font state before changing packages or font config:

```sh
fc-match
fc-match sans
fc-match serif
fc-match monospace
fc-match 'Arial'
fc-match 'Noto Sans'
```

List installed fonts and paths:

```sh
fc-list > /tmp/fc-list.txt
wc -l /tmp/fc-list.txt
rg -i 'variable|noto|emoji|color|ttc|otf|ttf' /tmp/fc-list.txt
```

Check for broken font files reported by fontconfig:

```sh
FC_DEBUG=4 fc-match sans 2>&1 | tee /tmp/fc-debug.log
rg -i 'error|fail|reject|broken|invalid' /tmp/fc-debug.log
```

If a crash consistently follows a specific page, record the URL and the last ServoQ log block for that tab id:

```sh
rg 'tab_id=<TAB_ID>|SERVOQ_DEBUG' /tmp/servoq.log | tail -120
```

## Compare Embedder Setup

ServoQ currently embeds Servo through `SoftwareRenderingContext` and a timer-driven Qt event pump. The Servo 0.2.0 `winit_minimal` example uses `WindowRenderingContext`, supplies an `EventLoopWaker`, and calls `servo.setup_logging()` after building `Servo`.

Known differences to keep in mind when comparing with standalone Servo/nightly:

- ServoQ does not use GL or `WindowRenderingContext`.
- ServoQ does not currently install a custom Servo `EventLoopWaker`; it ticks Servo from the Qt timer.
- ServoQ does not automatically call `servo.setup_logging()` because Servo 0.2.0's implementation calls `log::set_boxed_logger(...).expect(...)`, which can panic if another logger is already installed.
- ServoQ uses Servo crate default `Opts` and default `Preferences` through `ServoBuilder::default()`.
- ServoQ does not configure custom resource paths, font paths, protocol registries, or preferences.

A font-stack crash with no stale-tab or closed-WebView log immediately before it is more likely in Servo/fontconfig/HarfBuzz/FreeType or in an embedder initialization difference than in tab switching itself. Do not mark it fixed without reproducing the crash and then verifying the same URL no longer crashes.
