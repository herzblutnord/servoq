# Servo 0.4 integration audit

This document records how ServoQ consumes the Servo 0.4.0 release described in
the [June 2026 Servo update](https://servo.org/blog/2026/07/31/june-in-servo/)
and the [Servo 0.4 API documentation](https://doc.servo.org/servo/). It separates
engine behavior inherited from the crate from work that a desktop embedder must
perform itself.

## Dependency and API migration

- `servo` is pinned to the compatible `0.4.0` release and Cargo.lock contains
  the corresponding Servo 0.4 dependency family.
- `servo/webgpu` and `servo/media-gstreamer` are compiled into the real-engine
  build. WebGPU is still controlled by ServoQ's experimental-features setting.
- The locally patched `servo-media-gstreamer` was re-vendored from 0.4.0 and the
  ServoQ crash/audio-output hardening in DEVIATIONS.md §0k/§0l was reapplied.
- Servo 0.4's own lockfile pins `primeorder` to `0.14.0-rc.14`, matching its
  `p256`/`p384`/`p521` release-candidate stack; ServoQ mirrors that constraint.
- The new `PermissionFeature::Gamepad` and typed
  `ScreenWakeLock(WakeLockType)` variants are handled by the permission store.
- The new `ConsoleLogLevel::Dir` reaches both stderr diagnostics and the
  `servoq://debug` console.
- `ViewportDetails::device_size` is populated by Servo from ServoQ's existing
  real `screen_geometry` delegate. This backs the new device-size media queries.
- `WebView::rendering_context()` is available to ServoQ through the upgraded
  public API. ServoQ does not use the removed `WebView::send_error()` API.

## Web-platform changes inherited from Servo 0.4

The following are engine features and require no parallel Qt implementation.
They are available to every ServoQ webview after the dependency update:

- CSS `image(<color>)`, closest/farthest-corner sizing, deferred math
  expressions, `font-feature-settings` in `@font-face`, and the new height,
  aspect-ratio, orientation, pointer, hover, and device-size media queries.
- Shared workers and the scoped custom-element registry APIs.
- `console.dir()`; `Request`, `Response`, and `Blob` text streams; pointer
  capture; Element `ontouch*` handlers; KT128/KT256 digest; and ML-KEM/ML-DSA
  public-key retrieval.
- Variable-font/layout compatibility work, web-platform conformance fixes,
  the SpiderMonkey 140.12.0 update, constant-time SubtleCrypto comparisons,
  and the directory-listing injection fix.
- DevTools protocol response-body and multi-inspector fixes. ServoQ exposes the
  loopback-only server through Settings → Advanced, applies the toggle on the
  next launch, and asks before accepting an inspector connection.

The release's opt-in web features are connected to Settings → Advanced →
Enable experimental web platform features:

- upgraded CSS `attr()` (`layout_css_attr_enabled`);
- WebGPU (`dom_webgpu_enabled`, with the Cargo feature compiled);
- Web Animations (`dom_web_animations_enabled`);
- File `webkitRelativePath` / entries API (`dom_entries_api_enabled`);
- legacy touch handler exposure on desktop (`dom_touch_events_legacy_apis_enabled`).

SharedWorker is enabled by Servo 0.4 by default and therefore does not need a
ServoQ preference override.

## Desktop embedder work

ServoQ implements the release's applicable servoshell-facing behavior rather
than relying on the reference shell:

- Qt finger and tablet/pen events are forwarded as Servo 0.4 `TouchEvent`s with
  the correct `TouchPointerType`, including cancellation and multitouch IDs.
- `<select multiple>` uses a native checklist dialog with initial selection,
  disabled entries, optgroup headings, cancellation, and a valid empty result.
  Single selects retain their anchored native menu.
- Local files dropped on the browser open in the current tab; additional files
  open in background tabs.
- Existing behavior already covers the other desktop-shell release items:
  horizontal tab overflow scroll buttons, fullscreen on the current Qt screen,
  and localhost/IP address inference. Host names and localhost addresses with an
  explicit port now follow servoshell and infer `http://` instead of being
  mistaken for a URL scheme or search query.

The wider August 2026 ServoShell and Ladybird comparison, including the Qt UI
fixes ported after the release audit, is recorded in
[`UPSTREAM_REFERENCE_AUDIT_2026_08.md`](UPSTREAM_REFERENCE_AUDIT_2026_08.md).

The Android 13 minimum-version change is not applicable to this Linux Qt desktop
target.

## Upstream work in progress

The blog also reports projects that were not completed Servo 0.4 features. They
must not be represented as production-ready ServoQ capabilities:

- Servo accessibility mapping is still evolving, and ServoQ does not yet graft
  Servo's AccessKit subtree into Qt's accessibility tree.
- Normal-page text selection was only started upstream. ServoQ already forwards
  the required mouse input; DEVIATIONS.md documents the remaining engine limit.
- Servo's new C embedding API was only started and is unrelated to ServoQ's Rust
  embedding path.

These exclusions do not disable a shipped Servo 0.4 feature; they record the
same upstream WIP boundary stated by the release post.
