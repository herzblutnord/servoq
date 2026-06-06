---
name: servo-engine-m1
description: Phase 2 Milestone 1 implementation: servo-engine Cargo feature, WebContentView widget replacing WebContentPlaceholder, CXX bridge extensions, SoftwareRenderingContext readback pipeline.
metadata:
  type: project
---

Completed Phase 2 Milestone 1. Key decisions:

**Feature gate:** `servo-engine` (default OFF). servo = "0.2.0" optional. Also needs dpi/"0.1", euclid/"0.22", url/"2", keyboard-types/"0.7" as optional deps.

**Architecture:** servo_engine.rs inner `engine` module gated by #[cfg(feature)]. Public API functions always present (no-ops without feature). thread_local! RefCell<Option<EngineState>> holds Servo + tab HashMap.

**Critical borrow pattern:** clone_servo()/clone_webview() drop the RefCell borrow BEFORE calling spin_event_loop()/webview methods, so delegate callbacks can borrow ENGINE freely.

**paint() contract:** paint() called inside notify_new_frame_ready ONLY. present() NOT called before read_to_image (preserves back buffer).

**Widget:** WebContentView replaces WebContentPlaceholder in Tab. Shows placeholder until first frame. setTab()/setTabId() wire the global registry. showEvent defers create_webview until real dimensions available.

**Bridge direction Rust→C++:** deliver_frame, notify_url_changed, notify_title_changed, notify_load_started, notify_load_finished, notify_status_changed declared in servo_callbacks.h (includes rust/cxx.h), implemented in WebContentView.cpp using g_view_registry().

**Servo API deviation:** task spec said ServoBuilder::new(rendering_context) but 0.2.0 API has ServoBuilder::default().build(); rendering_context goes to WebViewBuilder::new(&servo, rc_ctx).

**Why:** Keeps default build fast; servo feature pulls SpiderMonkey and takes very long.

**How to apply:** Never modify vendor/. Always check doc.servo.org before using servo API. The 6 dead_code warnings in bridge.rs (for notify_* functions) are expected without the feature.
