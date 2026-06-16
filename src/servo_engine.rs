// Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
// Copyright (c) 2021, the SerenityOS developers.
// Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
// Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
// Copyright (c) 2023, Srikavin Ramkumar <me@srikavin.me>
// Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
// Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
// SPDX-License-Identifier: BSD-2-Clause
//
// Derived from Ladybird:
//   UI/Qt/WebContentView.cpp
//   Libraries/LibWeb/HTML/HTMLLinkElement.cpp
//   Libraries/LibWeb/Loader/ContentBlocker.cpp
// servo_engine.rs
//
// Real Servo embedder compiled only under the `servo-engine` Cargo feature.
// Without the feature every public function is a no-op, preserving the default
// (placeholder) build exactly as before.

// ============================================================
// Real engine — compiled only when the feature is active
// ============================================================
#[cfg(feature = "servo-engine")]
mod engine {
    use std::cell::{Cell, RefCell};
    use std::ffi::{c_char, c_void, CStr};
    use std::ptr::NonNull;
    use std::collections::HashMap;
    use std::path::{Path, PathBuf};
    use std::rc::Rc;
    use std::sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Once,
    };
    use std::time::{Duration, Instant};

    use dpi::PhysicalSize;
    use euclid::{Box2D, Point2D, Scale};
    use glow::HasContext;
    use servo::{Code, EventLoopWaker, Key, KeyState, Location, Modifiers, NamedKey};
    use servo::protocol_handler::ProtocolRegistry;
    use servo::{
        DeviceIndependentPixel, DevicePixel, EditingActionEvent, InputEvent,
        KeyboardEvent as ServoKeyboardEvent,
        LoadStatus, MouseButton, MouseButtonAction, MouseButtonEvent, MouseMoveEvent,
        NavigationRequest, Opts, PrefValue, Preferences, RenderingContext, Servo, ServoBuilder,
        SoftwareRenderingContext, Theme, UrlRequest, WebResourceLoad, WebResourceResponse, WebView,
        WebRenderDebugOption, WebViewBuilder, WebViewDelegate, WebViewPoint, WheelDelta, WheelEvent,
        WindowRenderingContext,
    };
    use servo::{AuthenticationRequest, PermissionFeature, PermissionRequest};
    use servo::{ClipboardDelegate, StringRequest};
    use servo::{DeviceIntPoint, DeviceIntRect, DeviceIntSize, ScreenGeometry};
    use servo::{ConsoleLogLevel, ContextMenuAction, ContextMenuItem, Cursor, PixelFormat};
    use servo::{CreateNewWebViewRequest, EmbedderControl};
    use servo::{RgbColor, SelectElementOptionOrOptgroup, SimpleDialog};
    use servo::UserContentManager;
    use servo::{
        MediaSessionActionType, MediaSessionEvent, MediaSessionPlaybackState, StorageType,
    };
    use servo::CookieSource;
    use raw_window_handle::{
        DisplayHandle, RawDisplayHandle, RawWindowHandle, WaylandDisplayHandle,
        WaylandWindowHandle, WindowHandle,
    };
    use url::Url;

    // ---- per-tab state stored in our engine registry ---------

    // These gate env vars are read ONCE and cached. std::env::var_os takes a
    // process-global lock (shared with every Servo worker thread) and scans the
    // environment, so calling it per-event on hot paths (e.g. forward_mouse_move
    // at ~1 kHz) caused lock contention that stalled both the UI and Servo's
    // compositing. Cached behind a OnceLock it is a single atomic load thereafter.
    fn debug_enabled() -> bool {
        static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        *V.get_or_init(|| std::env::var_os("SERVOQ_DEBUG").is_some())
    }

    fn perf_enabled() -> bool {
        static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        *V.get_or_init(|| std::env::var_os("SERVOQ_PERF").is_some())
    }

    // TEMPORARY DIAGNOSTICS (SERVOQ_DIAG) — opt-in, low-noise tracing for the
    // text-input and second-tab-crash investigation. Remove once root-caused.
    fn diag_enabled() -> bool {
        static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        *V.get_or_init(|| std::env::var_os("SERVOQ_DIAG").is_some())
    }

    fn diag(msg: impl std::fmt::Display) {
        if diag_enabled() {
            eprintln!("SERVOQ_DIAG(rust) {msg}");
        }
    }

    static SHUTTING_DOWN: AtomicBool = AtomicBool::new(false);
    static GL_INFO_LOGGED: AtomicBool = AtomicBool::new(false);
    // Console-message capture for servoq://debug. Off by default: pages can
    // console.log in rAF/scroll loops, so per-message FFI traffic only happens
    // while a debug page has explicitly turned capture on.
    static CONSOLE_CAPTURE: AtomicBool = AtomicBool::new(false);

    type EGLDisplay = *mut c_void;

    #[link(name = "EGL")]
    extern "C" {
        fn eglGetCurrentDisplay() -> EGLDisplay;
        fn eglQueryString(display: EGLDisplay, name: i32) -> *const c_char;
        // EGLBoolean eglSwapInterval(EGLDisplay, EGLint)
        fn eglSwapInterval(display: EGLDisplay, interval: i32) -> u32;
    }

    // Disable vsync-blocking on the buffer swap for the currently-current EGL
    // context. surfman's present_bound_surface() calls eglSwapBuffers(), which
    // with the default swap interval (1) BLOCKS the Qt main thread until the
    // Wayland compositor returns a wl_surface.frame callback. Right after the
    // shared subsurface is re-attached on a tab switch that callback is withheld
    // for seconds, freezing the entire UI (measured up to ~13 s/swap via
    // SERVOQ_PERF wl_swap_ms). Servo's RefreshDriver and our own present
    // coalescing already pace frames, so interval=0 (swap returns immediately) is
    // both safe and what we want. Must be called while the context is current
    // (i.e. after RenderingContext::make_current()).
    fn disable_swap_vsync_for_current_context() {
        unsafe {
            let display = eglGetCurrentDisplay();
            if !display.is_null() {
                eglSwapInterval(display, 0);
            }
        }
    }

    const EGL_VENDOR: i32 = 0x3053;
    const EGL_VERSION: i32 = 0x3054;
    const EGL_CLIENT_APIS: i32 = 0x308D;

    fn debug_log(event: &str, id: i32) {
        if debug_enabled() {
            eprintln!("SERVOQ_DEBUG {event} tab_id={id}");
        }
    }

    fn debug_log_detail(event: &str, id: i32, detail: impl std::fmt::Display) {
        if debug_enabled() {
            eprintln!("SERVOQ_DEBUG {event} tab_id={id} {detail}");
        }
    }

    fn tab_exists(id: i32) -> bool {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .is_some_and(|engine| engine.tabs.contains_key(&id))
        })
    }

    fn tab_is_active(id: i32) -> bool {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|engine| engine.tabs.get(&id))
                .is_some_and(|tab| tab.active)
        })
    }

    fn log_ignored_closed_callback(event: &str, id: i32) {
        debug_log_detail("ignored_callback_closed_webview", id, event);
    }

    #[derive(Default)]
    struct PerfStats {
        window_start: Option<Instant>,
        ticks: u64,
        tick_time: Duration,
        frames: u64,
        skipped_pending_frames: u64,
        frame_time: Duration,
        frame_bytes: u64,
        wayland_frame_ready: u64,
        wayland_presents: u64,
        wayland_present_time: Duration,
        wayland_make_current_time: Duration,
        wayland_paint_time: Duration,
        wayland_swap_time: Duration,
        skipped_reentrant_ticks: u64,
    }

    thread_local! {
        static PERF_STATS: RefCell<PerfStats> = RefCell::new(PerfStats::default());
    }

    fn record_tick_time(elapsed: Duration) {
        if !perf_enabled() {
            return;
        }
        PERF_STATS.with(|stats| {
            let mut stats = stats.borrow_mut();
            if stats.window_start.is_none() {
                stats.window_start = Some(Instant::now());
            }
            stats.ticks += 1;
            stats.tick_time += elapsed;
            maybe_log_perf(&mut stats);
        });
    }

    fn record_frame_delivered(elapsed: Duration, bytes: u64) {
        if !perf_enabled() {
            return;
        }
        PERF_STATS.with(|stats| {
            let mut stats = stats.borrow_mut();
            if stats.window_start.is_none() {
                stats.window_start = Some(Instant::now());
            }
            stats.frames += 1;
            stats.frame_time += elapsed;
            stats.frame_bytes += bytes;
            maybe_log_perf(&mut stats);
        });
    }

    fn record_frame_skipped_pending() {
        if !perf_enabled() {
            return;
        }
        PERF_STATS.with(|stats| {
            let mut stats = stats.borrow_mut();
            if stats.window_start.is_none() {
                stats.window_start = Some(Instant::now());
            }
            stats.skipped_pending_frames += 1;
            maybe_log_perf(&mut stats);
        });
    }

    fn record_wayland_frame_ready() {
        if !perf_enabled() {
            return;
        }
        PERF_STATS.with(|stats| {
            let mut stats = stats.borrow_mut();
            if stats.window_start.is_none() {
                stats.window_start = Some(Instant::now());
            }
            stats.wayland_frame_ready += 1;
            maybe_log_perf(&mut stats);
        });
    }

    fn record_wayland_present(
        total: Duration,
        make_current: Duration,
        paint: Duration,
        swap: Duration,
        size: PhysicalSize<u32>,
        url: &str,
    ) {
        if !perf_enabled() {
            return;
        }
        if paint > Duration::from_millis(50) {
            eprintln!(
                "SERVOQ_PERF slow_wayland_present url={url:?} size={}x{} total_ms={:.2} make_current_ms={:.2} paint_ms={:.2} swap_ms={:.2}",
                size.width,
                size.height,
                total.as_secs_f64() * 1000.0,
                make_current.as_secs_f64() * 1000.0,
                paint.as_secs_f64() * 1000.0,
                swap.as_secs_f64() * 1000.0
            );
        }
        PERF_STATS.with(|stats| {
            let mut stats = stats.borrow_mut();
            if stats.window_start.is_none() {
                stats.window_start = Some(Instant::now());
            }
            stats.wayland_presents += 1;
            stats.wayland_present_time += total;
            stats.wayland_make_current_time += make_current;
            stats.wayland_paint_time += paint;
            stats.wayland_swap_time += swap;
            maybe_log_perf(&mut stats);
        });
    }

    fn record_skipped_reentrant_tick() {
        if !perf_enabled() {
            return;
        }
        PERF_STATS.with(|stats| {
            let mut stats = stats.borrow_mut();
            if stats.window_start.is_none() {
                stats.window_start = Some(Instant::now());
            }
            stats.skipped_reentrant_ticks += 1;
            maybe_log_perf(&mut stats);
        });
    }

    fn maybe_log_perf(stats: &mut PerfStats) {
        let Some(window_start) = stats.window_start else {
            return;
        };
        if window_start.elapsed() < Duration::from_secs(1) {
            return;
        }
        eprintln!(
            "SERVOQ_PERF rust ticks={} tick_ms={:.2} skipped_reentrant={} sw_frames={} sw_skipped_pending={} sw_frame_ms={:.2} sw_MiB={:.1} wl_frame_ready={} wl_presents={} wl_present_ms={:.2} wl_make_current_ms={:.2} wl_paint_ms={:.2} wl_swap_ms={:.2}",
            stats.ticks,
            stats.tick_time.as_secs_f64() * 1000.0,
            stats.skipped_reentrant_ticks,
            stats.frames,
            stats.skipped_pending_frames,
            stats.frame_time.as_secs_f64() * 1000.0,
            stats.frame_bytes as f64 / (1024.0 * 1024.0),
            stats.wayland_frame_ready,
            stats.wayland_presents,
            stats.wayland_present_time.as_secs_f64() * 1000.0,
            stats.wayland_make_current_time.as_secs_f64() * 1000.0,
            stats.wayland_paint_time.as_secs_f64() * 1000.0,
            stats.wayland_swap_time.as_secs_f64() * 1000.0,
        );
        *stats = PerfStats::default();
    }

    fn egl_string(name: i32) -> String {
        unsafe {
            let display = eglGetCurrentDisplay();
            if display.is_null() {
                return "<no-current-egl-display>".to_string();
            }
            let value = eglQueryString(display, name);
            if value.is_null() {
                return "<unavailable>".to_string();
            }
            CStr::from_ptr(value).to_string_lossy().into_owned()
        }
    }

    fn is_software_gl_renderer(renderer: &str) -> bool {
        let renderer = renderer.to_ascii_lowercase();
        renderer.contains("llvmpipe")
            || renderer.contains("softpipe")
            || renderer.contains("software")
            || renderer.contains("swrast")
    }

    // Queries GL/EGL identity from the current Wayland context.
    // Logs the SERVOQ_GL line once (first call only); always returns (is_software_gl, gl_renderer).
    // Returns None if make_current() fails.
    fn detect_and_log_wayland_gl(
        context: &WindowRenderingContext,
        size: PhysicalSize<u32>,
        scale: f32,
    ) -> Option<(bool, String)> {
        if let Err(error) = context.make_current() {
            eprintln!("SERVOQ_GL error=make_current_failed details={error:?}");
            return None;
        }

        let gl = context.glow_gl_api();
        let gl_vendor = unsafe { gl.get_parameter_string(glow::VENDOR) };
        let gl_renderer = unsafe { gl.get_parameter_string(glow::RENDERER) };
        let gl_version = unsafe { gl.get_parameter_string(glow::VERSION) };
        let glsl_version = unsafe { gl.get_parameter_string(glow::SHADING_LANGUAGE_VERSION) };
        let egl_vendor = egl_string(EGL_VENDOR);
        let egl_version = egl_string(EGL_VERSION);
        let egl_client_apis = egl_string(EGL_CLIENT_APIS);
        let software_gl = is_software_gl_renderer(&gl_renderer);
        let context_type = if gl_version.to_ascii_lowercase().contains("opengl es") {
            "gles"
        } else {
            "desktop-gl"
        };

        if !GL_INFO_LOGGED.swap(true, Ordering::AcqRel) {
            eprintln!(
                "SERVOQ_GL wayland_backend=true context_type={context_type} surface={}x{} dpr={scale} egl_vendor={egl_vendor:?} egl_version={egl_version:?} egl_client_apis={egl_client_apis:?} gl_vendor={gl_vendor:?} gl_renderer={gl_renderer:?} gl_version={gl_version:?} glsl_version={glsl_version:?} software_gl={software_gl}",
                size.width, size.height
            );
        }

        Some((software_gl, gl_renderer))
    }

    fn log_embedder_setup_once() {
        static LOG_ONCE: Once = Once::new();
        if !debug_enabled() {
            return;
        }
        LOG_ONCE.call_once(|| {
            eprintln!("SERVOQ_DEBUG servo_builder preferences=viewport_meta_enabled+dom_indexeddb_enabled rendering_context=SoftwareRenderingContext event_loop_waker=QtEventLoopWaker");
            match std::env::current_exe() {
                Ok(path) => eprintln!("SERVOQ_DEBUG current_exe={}", path.display()),
                Err(error) => eprintln!("SERVOQ_DEBUG current_exe_error={error}"),
            }
            for key in [
                "FONTCONFIG_PATH",
                "XDG_DATA_DIRS",
                "XDG_CONFIG_HOME",
                "HOME",
                "RUST_LOG",
            ] {
                let value = std::env::var(key).unwrap_or_else(|_| "<unset>".to_string());
                eprintln!("SERVOQ_DEBUG env {key}={value}");
            }
        });
    }

    // Qt-compatible EventLoopWaker: posts a custom QEvent to the Qt main thread.
    // QCoreApplication::postEvent() is thread-safe and wakes the Qt event loop
    // from Servo's background threads (paint, layout, font loading, etc.).
    // BrowserWindow::eventFilter() intercepts the event and calls tick_servo().
    struct QtEventLoopWaker;

    impl EventLoopWaker for QtEventLoopWaker {
        fn wake(&self) {
            crate::bridge::ffi::servoq_wake_event_loop();
        }

        fn clone_box(&self) -> Box<dyn EventLoopWaker> {
            Box::new(QtEventLoopWaker)
        }
    }

    // SAFETY: QCoreApplication::postEvent() is documented as thread-safe;
    // QtEventLoopWaker has no fields so there is nothing to race on.
    unsafe impl Send for QtEventLoopWaker {}
    unsafe impl Sync for QtEventLoopWaker {}

    // Mirrors servoshell EXPERIMENTAL_PREFS — all enabled by default.
    // The user can disable them via Settings → Experimental Web Platform Features.
    // ServoQ additions beyond the servoshell list:
    // - dom_cookiestore_enabled — the async CookieStore API is a thin layer
    //   over the same engine cookie jar that ServoQ now persists
    //   (docs/STORAGE.md), and its core surface (get/getAll/set/delete +
    //   change events) is implemented in Servo 0.2.
    // - dom_geolocation_enabled, dom_wakelock_enabled,
    //   dom_credential_management_enabled — permission-gated features that
    //   were kept off until the M3.3 permission prompts landed; requests now
    //   go through request_permission + the per-origin PermissionStore.
    // The remaining default-off prefs stay off: WebRTC and Media Capture need
    // more than prompts (no capture backend), and ServiceWorker/SharedWorker/
    // Web Animations/AdoptedStyleSheets are partial enough that
    // feature-detecting sites break.
    pub(super) const EXPERIMENTAL_PREFS: &[&str] = &[
        "dom_async_clipboard_enabled",
        "dom_cookiestore_enabled",
        "dom_credential_management_enabled",
        "dom_exec_command_enabled",
        "dom_fontface_enabled",
        "dom_geolocation_enabled",
        "dom_indexeddb_enabled",
        "dom_intersection_observer_enabled",
        "dom_navigator_protocol_handlers_enabled",
        "dom_notification_enabled",
        "dom_offscreen_canvas_enabled",
        "dom_permissions_enabled",
        "dom_sanitizer_enabled",
        "dom_storage_manager_api_enabled",
        "dom_wakelock_enabled",
        "dom_webgl2_enabled",
        "dom_webgpu_enabled",
        "layout_columns_enabled",
        "layout_container_queries_enabled",
        "layout_grid_enabled",
        "layout_variable_fonts_enabled",
    ];

    fn servo_preferences() -> Preferences {
        let mut preferences = Preferences::default();
        preferences.viewport_meta_enabled = true;
        // Experimental features are enabled at startup via set_experimental_features_enabled
        // (called from applySettings after init_servo). Listed here for clarity only.

        // media_glvideo_enabled (GL-accelerated <video>) stays OFF. It would gate
        // servo-media's GL player thread + WebRender external-image handler, but that
        // only helps if Servo::initialize_gl_accelerated_media has already shared a
        // GL/EGL context — and that call must run *before* Servo::new. ServoQ builds
        // Servo eagerly at startup (init_servo, a font-cache-crash safeguard) with a
        // SoftwareRenderingContext, before any Wayland EGL surface exists, so there is
        // no GL context to share at the required time. Video therefore decodes through
        // servo-media's software upload path (GStreamer → image frames → WebRender),
        // which is verified working for H.264/AAC and VP9/Opus. Audio is independent
        // of this pref. Wiring true zero-copy GL video would require building Servo
        // lazily against the Wayland context, which conflicts with the early-init
        // design — a deliberate, isolated follow-up, not a one-liner.

        // CJK coverage for the generic font families. Servo's built-in fallback
        // list (servo-fonts) misses the CJK Symbols & Punctuation block (「」『』
        // 、。 etc.) on common Linux setups — e.g. the family "Droid Sans Fallback"
        // it relies on resolves via fontconfig to a non-CJK font — so those
        // characters render as tofu. Point the generic families at an installed,
        // locale-appropriate CJK font (Latin glyphs are unchanged: Noto Sans/Serif
        // CJK carry the same Latin design as Noto Sans/Serif). Empty when no CJK
        // font is installed, leaving Servo's platform default in place.
        let sans = crate::bridge::ffi::system_cjk_font_family("sans-serif");
        if !sans.is_empty() {
            preferences.fonts_sans_serif = sans.clone();
            preferences.fonts_default = sans;
        }
        let serif = crate::bridge::ffi::system_cjk_font_family("serif");
        if !serif.is_empty() {
            preferences.fonts_serif = serif;
        }
        preferences
    }

    fn servo_profile_dir() -> Option<PathBuf> {
        let app_data_dir = crate::bridge::ffi::servo_profile_data_dir();
        if app_data_dir.is_empty() {
            eprintln!(
                "ServoQ storage: Qt AppDataLocation is empty; Servo cookies will not persist"
            );
            return None;
        }

        let app_data_path = PathBuf::from(app_data_dir);
        let mut path = app_data_path.clone();
        path.push("servo-profile");
        if let Err(error) = std::fs::create_dir_all(&path) {
            eprintln!(
                "ServoQ storage: cannot create Servo profile directory {}: {error}; Servo cookies will not persist",
                path.display()
            );
            return None;
        }
        restrict_profile_dir_permissions(&app_data_path);
        restrict_profile_dir_permissions(&path);
        Some(path)
    }

    // Browser profiles are private per OS user (Firefox creates profile
    // directories 0700); Qt's mkpath and create_dir_all honor the umask and
    // typically leave 0755, which would let other local users read
    // cookie_jar.json, history.db, and the rest of the profile.
    fn restrict_profile_dir_permissions(path: &Path) {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            if let Err(error) =
                std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700))
            {
                eprintln!(
                    "ServoQ storage: cannot restrict permissions on {}: {error}",
                    path.display()
                );
            }
        }
        #[cfg(not(unix))]
        let _ = path;
    }

    // Servo's resource thread persists auth_cache.json (plaintext HTTP
    // Basic/Digest usernames and passwords) next to the cookie jar whenever a
    // config_dir is set. Chrome and Firefox keep HTTP auth session-only, so
    // remove the file before Servo can load it and again after the shutdown
    // write — credentials must not outlive the session on disk.
    fn remove_persisted_http_auth_cache(profile_dir: &Path) {
        let path = profile_dir.join("auth_cache.json");
        if let Err(error) = std::fs::remove_file(&path) {
            if error.kind() != std::io::ErrorKind::NotFound {
                eprintln!(
                    "ServoQ storage: cannot remove persisted HTTP auth cache {}: {error}",
                    path.display()
                );
            }
        }
    }

    fn servo_opts() -> Opts {
        let mut opts = Opts::default();
        let profile_dir = servo_profile_dir();
        if let Some(dir) = &profile_dir {
            remove_persisted_http_auth_cache(dir);
        }
        opts.config_dir = profile_dir;
        opts.temporary_storage = false;
        opts
    }

    fn build_servo() -> Servo {
        log_embedder_setup_once();
        let opts = servo_opts();
        let servo = ServoBuilder::default()
            .opts(opts)
            .preferences(servo_preferences())
            .protocol_registry(ProtocolRegistry::default())
            .event_loop_waker(Box::new(QtEventLoopWaker))
            .build();
        servo.setup_logging();
        // Servo 0.2 persists its network cookie jar when config_dir is set.
        // Clean session-only cookies at startup as well so a previous crash or
        // forced kill cannot turn them into restart-persistent cookies.
        servo.site_data_manager().clear_session_cookies();
        servo
    }

    fn create_engine_state(rendering_context: EngineRenderingContext) -> EngineState {
        let servo = build_servo();
        let user_content_manager = Rc::new(UserContentManager::new(&servo));
        EngineState {
            servo,
            user_content_manager,
            clipboard_delegate: Rc::new(QtClipboardDelegate),
            tabs: HashMap::new(),
            rendering_context,
        }
    }

    fn configure_webview_diagnostics(webview: &WebView) {
        if std::env::var_os("SERVOQ_WR_DEBUG").is_some() {
            webview.toggle_webrender_debugging(WebRenderDebugOption::Profiler);
            webview.toggle_sampling_profiler(Duration::from_millis(10), Duration::from_secs(30));
        }
    }

    fn cursor_to_servoq_code(cursor: &Cursor) -> i32 {
        match cursor {
            Cursor::None => 0,
            Cursor::Default => 1,
            Cursor::Pointer => 2,
            Cursor::ContextMenu => 3,
            Cursor::Help => 4,
            Cursor::Progress => 5,
            Cursor::Wait => 6,
            Cursor::Cell => 7,
            Cursor::Crosshair => 8,
            Cursor::Text => 9,
            Cursor::VerticalText => 10,
            Cursor::Alias => 11,
            Cursor::Copy => 12,
            Cursor::Move => 13,
            Cursor::NoDrop => 14,
            Cursor::NotAllowed => 15,
            Cursor::Grab => 16,
            Cursor::Grabbing => 17,
            Cursor::EResize => 18,
            Cursor::NResize => 19,
            Cursor::NeResize => 20,
            Cursor::NwResize => 21,
            Cursor::SResize => 22,
            Cursor::SeResize => 23,
            Cursor::SwResize => 24,
            Cursor::WResize => 25,
            Cursor::EwResize => 26,
            Cursor::NsResize => 27,
            Cursor::NeswResize => 28,
            Cursor::NwseResize => 29,
            Cursor::ColResize => 30,
            Cursor::RowResize => 31,
            Cursor::AllScroll => 32,
            Cursor::ZoomIn => 33,
            Cursor::ZoomOut => 34,
        }
    }

    // Stable per-feature keys for the permission prompt and PermissionStore;
    // Permissions-API descriptor names where they exist.
    fn permission_feature_name(feature: PermissionFeature) -> &'static str {
        match feature {
            PermissionFeature::Geolocation => "geolocation",
            PermissionFeature::Notifications => "notifications",
            PermissionFeature::Push => "push",
            PermissionFeature::Midi => "midi",
            PermissionFeature::Camera => "camera",
            PermissionFeature::Microphone => "microphone",
            PermissionFeature::Speaker => "speaker-selection",
            PermissionFeature::DeviceInfo => "device-info",
            PermissionFeature::BackgroundSync => "background-sync",
            PermissionFeature::Bluetooth => "bluetooth",
            PermissionFeature::PersistentStorage => "persistent-storage",
            PermissionFeature::ScreenWakeLock => "screen-wake-lock",
        }
    }

    fn context_menu_action_id(action: ContextMenuAction) -> i32 {
        match action {
            ContextMenuAction::GoBack => 0,
            ContextMenuAction::GoForward => 1,
            ContextMenuAction::Reload => 2,
            ContextMenuAction::CopyLink => 3,
            ContextMenuAction::OpenLinkInNewWebView => 4,
            ContextMenuAction::CopyImageLink => 5,
            ContextMenuAction::OpenImageInNewView => 6,
            ContextMenuAction::Cut => 7,
            ContextMenuAction::Copy => 8,
            ContextMenuAction::Paste => 9,
            ContextMenuAction::SelectAll => 10,
        }
    }

    fn context_menu_action_from_id(id: i32) -> Option<ContextMenuAction> {
        match id {
            0 => Some(ContextMenuAction::GoBack),
            1 => Some(ContextMenuAction::GoForward),
            2 => Some(ContextMenuAction::Reload),
            3 => Some(ContextMenuAction::CopyLink),
            4 => Some(ContextMenuAction::OpenLinkInNewWebView),
            5 => Some(ContextMenuAction::CopyImageLink),
            6 => Some(ContextMenuAction::OpenImageInNewView),
            7 => Some(ContextMenuAction::Cut),
            8 => Some(ContextMenuAction::Copy),
            9 => Some(ContextMenuAction::Paste),
            10 => Some(ContextMenuAction::SelectAll),
            _ => None,
        }
    }

    fn resource_type_for_destination(destination: &str, is_for_main_frame: bool) -> &'static str {
        if is_for_main_frame {
            return "document";
        }
        match destination {
            "Audio" | "Track" | "Video" => "media",
            "Document" => "document",
            "Embed" | "Object" => "object",
            "Font" => "font",
            "Frame" | "IFrame" => "subdocument",
            "Image" => "image",
            "AudioWorklet" | "PaintWorklet" | "Script" | "ServiceWorker" | "SharedWorker" | "Worker" => "script",
            "Style" => "stylesheet",
            _ => "other",
        }
    }

    fn is_probably_pdf_navigation_url(url: &Url) -> bool {
        matches!(url.scheme(), "file" | "http" | "https")
            && url.path().to_ascii_lowercase().ends_with(".pdf")
    }

    fn content_blocking_allowed_for_url(url: &Url) -> bool {
        if !content_blocking_enabled() {
            return false;
        }
        if let Some(host) = url.host_str() {
            if crate::bridge::ffi::content_blocking_host_allowlisted(host) {
                return false;
            }
        }
        true
    }

    fn should_block_request(url: &Url, source_url: &Url, request_type: &str) -> bool {
        crate::blocklist::should_block(url, source_url, request_type)
    }

    fn content_blocking_enabled() -> bool {
        crate::bridge::ffi::content_blocking_enabled()
    }

    fn notify_request_blocked(tab_id: i32, url: &Url) {
        crate::bridge::ffi::notify_request_blocked(tab_id, url.as_str());
    }

    struct TabEntry {
        webview: WebView,
        // Cached values updated by delegate callbacks
        current_url: String,
        title: String,
        loading: bool,
        status_text: String,
        active: bool,
        qt_modifiers: u32,
        paint_hold: NavPaintHold,
        physical_size: PhysicalSize<u32>,
        hidpi_scale_factor: Scale<f32, DeviceIndependentPixel, DevicePixel>,
    }

    // Navigation paint holding (the Chrome behavior: keep showing the old page
    // until the new one can paint content). At navigation commit Servo swaps
    // the frame tree to the new pipeline before that pipeline has any display
    // list, so webrender paints pure `shell_background_color_rgba` (white)
    // until the new document's first styled layout — a full-white flash
    // between clicking a link and the page rendering. The embedder gets no
    // first-contentful-paint signal, so detect content from the pixels: while
    // a hold is active, every painted frame is read back and a uniformly
    // white frame is not presented — visually identical to not presenting at
    // all, so nothing is lost — while the first frame with any non-white
    // pixel is presented immediately and ends the hold. The hold is armed at
    // `LoadStatus::Started` (emitted from the NEW document's
    // set_ready_state(Loading), i.e. at commit, exactly when blank frames
    // begin) and at webview creation (a fresh webview never fires Started for
    // its initial load), and released by content, `LoadStatus::Complete`
    // (a genuinely blank page must still appear), a crash, or the timeout.
    #[derive(Clone, Copy, Debug, PartialEq)]
    enum NavPaintHold {
        /// No top-level navigation in flight; frames present normally.
        Idle,
        /// Navigation in flight: present nothing until a frame has content.
        Held { deadline: Instant },
    }

    const NAVIGATION_PAINT_HOLD_TIMEOUT_MS: u64 = 10_000;

    fn navigation_paint_hold() -> NavPaintHold {
        NavPaintHold::Held {
            deadline: Instant::now() + Duration::from_millis(NAVIGATION_PAINT_HOLD_TIMEOUT_MS),
        }
    }

    fn set_paint_hold(id: i32, hold: NavPaintHold) {
        ENGINE.with(|s| {
            if let Some(tab) = s.borrow_mut().as_mut().and_then(|e| e.tabs.get_mut(&id)) {
                tab.paint_hold = hold;
            }
        });
    }

    /// Whether the tab is inside a navigation paint hold. Clears an expired
    /// hold so the timeout falls back to presenting whatever Servo paints.
    fn paint_hold_active(id: i32) -> bool {
        ENGINE.with(|s| {
            let mut state = s.borrow_mut();
            let Some(tab) = state.as_mut().and_then(|e| e.tabs.get_mut(&id)) else {
                return false;
            };
            match tab.paint_hold {
                NavPaintHold::Idle => false,
                NavPaintHold::Held { deadline } => {
                    if Instant::now() < deadline {
                        true
                    } else {
                        tab.paint_hold = NavPaintHold::Idle;
                        false
                    }
                }
            }
        })
    }

    /// True when every pixel equals Servo's navigation clear color (opaque
    /// white) — i.e. the frame carries no content worth presenting. Early
    /// exit on the first differing byte keeps contentful frames cheap.
    fn frame_is_blank_white(rgba_bytes: &[u8]) -> bool {
        rgba_bytes.iter().all(|&byte| byte == 0xFF)
    }

    #[derive(Clone)]
    enum EngineRenderingContext {
        Software(Rc<SoftwareRenderingContext>),
        WaylandWindow(Rc<WindowRenderingContext>),
    }

    impl EngineRenderingContext {
        fn as_rendering_context(&self) -> Rc<dyn RenderingContext> {
            match self {
                EngineRenderingContext::Software(context) => context.clone(),
                EngineRenderingContext::WaylandWindow(context) => context.clone(),
            }
        }

        fn software(&self) -> Option<Rc<SoftwareRenderingContext>> {
            match self {
                EngineRenderingContext::Software(context) => Some(context.clone()),
                EngineRenderingContext::WaylandWindow(_) => None,
            }
        }

        fn wayland_window(&self) -> Option<Rc<WindowRenderingContext>> {
            match self {
                EngineRenderingContext::Software(_) => None,
                EngineRenderingContext::WaylandWindow(context) => Some(context.clone()),
            }
        }
    }

    struct EngineState {
        // Servo must be dropped before WebViews and rendering contexts. Keep fields
        // that own WebViews/RenderingContexts after this one so Rust drops Servo first.
        // See https://github.com/servo/servo/issues/36711.
        servo: Servo,
        user_content_manager: Rc<UserContentManager>,
        clipboard_delegate: Rc<QtClipboardDelegate>,
        tabs: HashMap<i32, TabEntry>,
        rendering_context: EngineRenderingContext,
    }

    // System clipboard backed by QClipboard instead of Servo's default arboard
    // delegate. Servo routes every engine-side clipboard operation here: the
    // async clipboard API (navigator.clipboard), execCommand copy/cut/paste,
    // and the EditingAction events ServoQ dispatches for Ctrl+C/X/V. Going
    // through Qt keeps one clipboard connection per process (arboard would
    // open a second Wayland data-control connection) and makes Servo-initiated
    // copies visible to the rest of the Qt chrome immediately. All delegate
    // methods are invoked on the main thread from spin_event_loop, which is
    // the thread QClipboard requires.
    struct QtClipboardDelegate;

    impl ClipboardDelegate for QtClipboardDelegate {
        fn clear(&self, _webview: WebView) {
            crate::bridge::ffi::clipboard_clear();
        }

        fn get_text(&self, _webview: WebView, request: StringRequest) {
            request.success(crate::bridge::ffi::clipboard_get_text());
        }

        fn set_text(&self, _webview: WebView, new_contents: String) {
            crate::bridge::ffi::clipboard_set_text(&new_contents);
        }
    }

    // ---- delegate ------------------------------------------

    struct ServoDelegate {
        tab_id: i32,
        rendering_context: EngineRenderingContext,
        animating: Cell<bool>,
        // G1: resize() before the first spin_event_loop() is silently discarded
        // because the paint thread hasn't started processing messages yet. On the
        // first notify_new_frame_ready (compositor is live), re-send the stored
        // physical size so the layout thread relays with the correct viewport.
        initial_resize_done: Cell<bool>,
    }

    impl ServoDelegate {
        fn paint_and_deliver(&self, webview: &WebView) {
            if !tab_exists(self.tab_id) {
                debug_log("ignored_frame_closed_webview", self.tab_id);
                return;
            }
            if !tab_is_active(self.tab_id) {
                webview.set_throttled(true);
                webview.hide();
                return;
            }
            let Some(rendering_context) = self.rendering_context.software() else {
                record_wayland_frame_ready();
                crate::bridge::ffi::request_wayland_window_repaint(self.tab_id);
                return;
            };

            if crate::bridge::ffi::webcontent_frame_pending(self.tab_id) {
                debug_log("skipped_frame_pending_qt_paint", self.tab_id);
                record_frame_skipped_pending();
                return;
            }
            let started = Instant::now();
            webview.paint();
            let size = rendering_context.size();
            let w = size.width;
            let h = size.height;
            if w == 0 || h == 0 {
                return;
            }
            let rect: Box2D<i32, DevicePixel> =
                Box2D::new(Point2D::origin(), Point2D::new(w as i32, h as i32));
            if let Some(image) = rendering_context.read_to_image(rect) {
                if paint_hold_active(self.tab_id) {
                    if frame_is_blank_white(image.as_raw()) {
                        // Navigation paint hold: keep the previous m_frame (or
                        // the new-tab placeholder) instead of a blank flash.
                        debug_log("held_blank_navigation_frame", self.tab_id);
                        return;
                    }
                    set_paint_hold(self.tab_id, NavPaintHold::Idle);
                }
                if debug_enabled() {
                    debug_log_detail("deliver_frame", self.tab_id, format!("{w}x{h}"));
                }
                let bytes = (w as u64) * (h as u64) * 4;
                crate::bridge::ffi::deliver_frame(
                    self.tab_id,
                    image.as_raw().as_slice(),
                    w as i32,
                    h as i32,
                );
                record_frame_delivered(started.elapsed(), bytes);
            }
        }
    }

    impl WebViewDelegate for ServoDelegate {
        // notify_new_frame_ready: paint() into context, read pixels, push to C++.
        // CRITICAL: paint() is called here (not before); present() is NOT called
        // before read_to_image so the back buffer is preserved.
        fn notify_new_frame_ready(&self, webview: WebView) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }

            // Do this before logging. Hidden Wayland tabs can keep receiving compositor
            // frame callbacks for a while after hide()/set_throttled(true); logging each
            // one can itself flood the Qt event loop and make tab activation fragile.
            if !tab_exists(self.tab_id) {
                debug_log("ignored_frame_closed_webview", self.tab_id);
                return;
            }
            if !tab_is_active(self.tab_id) {
                webview.set_throttled(true);
                webview.hide();
                return;
            }

            debug_log("notify_new_frame_ready", self.tab_id);

            // G1: first callback proves the compositor is alive; re-send stored size
            // so any resize() calls issued before the first spin are honoured.
            if !self.initial_resize_done.get() {
                self.initial_resize_done.set(true);
                let (size, scale) = ENGINE.with(|s| {
                    s.borrow()
                        .as_ref()
                        .and_then(|e| e.tabs.get(&self.tab_id))
                        .map(|t| (t.physical_size, t.hidpi_scale_factor))
                        .unwrap_or((PhysicalSize::new(1, 1), Scale::new(1.0)))
                });
                webview.set_hidpi_scale_factor(scale);
                webview.resize(size);
                debug_log_detail(
                    "initial_resize",
                    self.tab_id,
                    format!("{}x{} scale={}", size.width, size.height, scale.0),
                );
            }

            self.paint_and_deliver(&webview);
        }

        fn notify_url_changed(&self, _webview: WebView, url: Url) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }
            if !tab_exists(self.tab_id) {
                log_ignored_closed_callback("notify_url_changed", self.tab_id);
                return;
            }
            debug_log_detail("notify_url_changed", self.tab_id, &url);
            ENGINE.with(|s| {
                let mut s = s.borrow_mut();
                if let Some(e) = s.as_mut() {
                    if let Some(t) = e.tabs.get_mut(&self.tab_id) {
                        t.current_url = url.to_string();
                    }
                }
            });
            // notify_url_changed fires ONLY for top-level document URL changes
            // (subframe navigations don't trigger it), so this is the one place
            // we can route a PDF into the inline viewer without hijacking
            // iframe/embedded PDFs. The webview has committed the PDF as a real
            // history entry; the viewer's Back does a real go_back. We suppress
            // the normal URL change so chrome doesn't briefly show the PDF as a
            // failed web page before the viewer takes over.
            if is_probably_pdf_navigation_url(&url) {
                crate::bridge::ffi::notify_pdf_navigation_requested(self.tab_id, url.as_str());
                return;
            }
            crate::bridge::ffi::notify_url_changed(self.tab_id, url.as_str());
        }

        fn notify_page_title_changed(&self, _webview: WebView, title: Option<String>) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }
            if !tab_exists(self.tab_id) {
                log_ignored_closed_callback("notify_page_title_changed", self.tab_id);
                return;
            }
            let title_str = title.as_deref().unwrap_or("New Tab");
            debug_log_detail("notify_title_changed", self.tab_id, title_str);
            ENGINE.with(|s| {
                let mut s = s.borrow_mut();
                if let Some(e) = s.as_mut() {
                    if let Some(t) = e.tabs.get_mut(&self.tab_id) {
                        t.title = title_str.to_string();
                    }
                }
            });
            crate::bridge::ffi::notify_title_changed(self.tab_id, title_str);
        }

        fn notify_status_text_changed(&self, _webview: WebView, status: Option<String>) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }
            if !tab_exists(self.tab_id) {
                log_ignored_closed_callback("notify_status_text_changed", self.tab_id);
                return;
            }
            let text = status.as_deref().unwrap_or("");
            debug_log_detail("notify_status_changed", self.tab_id, text);
            ENGINE.with(|s| {
                let mut s = s.borrow_mut();
                if let Some(e) = s.as_mut() {
                    if let Some(t) = e.tabs.get_mut(&self.tab_id) {
                        t.status_text = text.to_string();
                    }
                }
            });
            crate::bridge::ffi::notify_status_changed(self.tab_id, text);
        }

        fn notify_load_status_changed(&self, webview: WebView, status: LoadStatus) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }
            if !tab_exists(self.tab_id) {
                log_ignored_closed_callback("notify_load_status_changed", self.tab_id);
                return;
            }
            if debug_enabled() {
                debug_log_detail("load_status", self.tab_id, format!("{status:?}"));
            }
            let is_loading = !matches!(status, LoadStatus::Complete);
            let url_for_start = if matches!(status, LoadStatus::Started) {
                Some(ENGINE.with(|s| {
                    s.borrow()
                        .as_ref()
                        .and_then(|e| e.tabs.get(&self.tab_id))
                        .map(|t| t.current_url.clone())
                        .unwrap_or_default()
                }))
            } else {
                None
            };
            ENGINE.with(|s| {
                let mut s = s.borrow_mut();
                if let Some(e) = s.as_mut() {
                    if let Some(t) = e.tabs.get_mut(&self.tab_id) {
                        t.loading = is_loading;
                    }
                }
            });
            match status {
                LoadStatus::Started => {
                    set_paint_hold(self.tab_id, navigation_paint_hold());
                    let url = url_for_start.unwrap_or_default();
                    crate::bridge::ffi::notify_load_started(self.tab_id, &url);
                }
                LoadStatus::Complete => {
                    set_paint_hold(self.tab_id, NavPaintHold::Idle);
                    crate::bridge::ffi::notify_load_finished(self.tab_id);
                    // Servo may not issue another frame notification after load completion.
                    // Force one software paint/read so Qt receives the final post-load pixels.
                    self.paint_and_deliver(&webview);
                }
                _ => {}
            }
        }

        fn notify_animating_changed(&self, _webview: WebView, animating: bool) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }
            if !tab_exists(self.tab_id) {
                log_ignored_closed_callback("notify_animating_changed", self.tab_id);
                return;
            }
            debug_log_detail("animating", self.tab_id, animating);
            self.animating.set(animating);
        }

        // show an error page. Matches the reference notify_crashed() contract.
        fn notify_crashed(&self, _webview: WebView, reason: String, _backtrace: Option<String>) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }
            if !tab_exists(self.tab_id) {
                return;
            }
            eprintln!("Servo WebView crashed: {reason}");
            set_paint_hold(self.tab_id, NavPaintHold::Idle);
            crate::bridge::ffi::notify_webview_crashed(self.tab_id, &reason);
        }

        // window.close() from script: Servo asks the embedder to close this webview.
        fn notify_closed(&self, _webview: WebView) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }
            if !tab_exists(self.tab_id) {
                log_ignored_closed_callback("notify_closed", self.tab_id);
                return;
            }
            debug_log_detail("notify_closed", self.tab_id, "window.close()");
            crate::bridge::ffi::notify_webview_close_requested(self.tab_id);
        }

        fn request_navigation(&self, _webview: WebView, navigation_request: NavigationRequest) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                navigation_request.deny();
                return;
            }
            // NOTE: PDF navigations are NOT intercepted here. request_navigation
            // fires for subframe navigations too and exposes no top-level signal
            // (is_for_main_frame just means destination==Document, true for
            // iframes), so intercepting here hijacks iframe/embedded PDFs. PDFs
            // are routed from notify_url_changed instead (top-level only).
            if content_blocking_allowed_for_url(&navigation_request.url)
                && should_block_request(&navigation_request.url, &navigation_request.url, "document")
            {
                notify_request_blocked(self.tab_id, &navigation_request.url);
                navigation_request.deny();
                return;
            }
            navigation_request.allow();
        }

        fn load_web_resource(&self, _webview: WebView, load: WebResourceLoad) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }
            let request = load.request();
            let url = request.url.clone();
            let source_url = request.referrer_url.as_ref().unwrap_or(&url);
            let destination = format!("{:?}", request.destination);
            let request_type = resource_type_for_destination(&destination, request.is_for_main_frame);
            // PDFs are not intercepted here either (this hook can't distinguish
            // a top-level document load from a subframe one). See notify_url_changed.
            if content_blocking_allowed_for_url(source_url)
                && should_block_request(&url, source_url, request_type)
            {
                notify_request_blocked(self.tab_id, &url);
                let intercepted = load.intercept(WebResourceResponse::new(url));
                intercepted.finish();
            }
        }

        fn notify_favicon_changed(&self, webview: WebView) {
            if SHUTTING_DOWN.load(Ordering::Acquire) { return; }
            if !tab_exists(self.tab_id) { return; }
            if let Some(favicon) = webview.favicon() {
                let w = favicon.width as i32;
                let h = favicon.height as i32;
                let raw = favicon.data();
                if debug_enabled() {
                    eprintln!("[servoq favicon] tab_id={} format={:?} size={}x{} bytes={}", self.tab_id, favicon.format, w, h, raw.len());
                }
                let rgba8: Vec<u8> = match favicon.format {
                    PixelFormat::RGBA8 => raw.to_vec(),
                    PixelFormat::BGRA8 => {
                        let mut out = raw.to_vec();
                        for chunk in out.chunks_mut(4) {
                            chunk.swap(0, 2);
                        }
                        out
                    }
                    PixelFormat::RGB8 => {
                        raw.chunks(3).flat_map(|c| [c[0], c[1], c[2], 255u8]).collect()
                    }
                    PixelFormat::K8 => {
                        raw.iter().flat_map(|&k| [k, k, k, 255u8]).collect()
                    }
                    PixelFormat::KA8 => {
                        raw.chunks(2).flat_map(|c| [c[0], c[0], c[0], c[1]]).collect()
                    }
                };
                crate::bridge::ffi::notify_favicon_changed(self.tab_id, &rgba8, w, h);
            } else {
                crate::bridge::ffi::notify_favicon_changed(self.tab_id, &[], 0, 0);
            }
        }

        fn notify_cursor_changed(&self, _webview: WebView, cursor: Cursor) {
            if SHUTTING_DOWN.load(Ordering::Acquire) { return; }
            if !tab_exists(self.tab_id) { return; }
            // Cursor changes fire frequently while moving over a page; keep the
            // Debug-format of the cursor off the hot path unless logging is on.
            if debug_enabled() {
                debug_log_detail("notify_cursor_changed", self.tab_id, format!("{cursor:?}"));
            }
            crate::bridge::ffi::notify_cursor_changed(self.tab_id, cursor_to_servoq_code(&cursor));
        }

        fn notify_history_changed(&self, _webview: WebView, entries: Vec<url::Url>, current: usize) {
            if SHUTTING_DOWN.load(Ordering::Acquire) { return; }
            if !tab_exists(self.tab_id) { return; }
            let urls: Vec<&str> = entries.iter().map(|u| u.as_str()).collect();
            let serialized = urls.join("\n");
            crate::bridge::ffi::notify_history_changed(self.tab_id, &serialized, current as i32);
        }

        fn notify_fullscreen_state_changed(&self, _webview: WebView, is_fullscreen: bool) {
            if SHUTTING_DOWN.load(Ordering::Acquire) { return; }
            if !tab_exists(self.tab_id) { return; }
            crate::bridge::ffi::notify_fullscreen_changed(self.tab_id, is_fullscreen);
        }

        fn show_console_message(&self, _webview: WebView, level: ConsoleLogLevel, message: String) {
            // Page console output is opt-in: pages can console.log in rAF/scroll
            // loops, and an unconditional synchronous stderr write (or FFI hop)
            // per message stalls the main thread on such sites. Only do work when
            // SERVOQ_DEBUG is on (stderr) or the servoq://debug console panel has
            // turned capture on (FFI to the C++ ring buffer).
            let capture = CONSOLE_CAPTURE.load(Ordering::Relaxed);
            if !debug_enabled() && !capture {
                return;
            }
            let level_code = match level {
                ConsoleLogLevel::Log => 0,
                ConsoleLogLevel::Debug => 1,
                ConsoleLogLevel::Info => 2,
                ConsoleLogLevel::Warn => 3,
                ConsoleLogLevel::Error => 4,
                ConsoleLogLevel::Trace => 5,
            };
            if debug_enabled() {
                const NAMES: [&str; 6] = ["LOG", "DEBUG", "INFO", "WARN", "ERROR", "TRACE"];
                eprintln!("[servoq console][tab={}][{}] {}", self.tab_id, NAMES[level_code as usize], message);
            }
            if capture && tab_exists(self.tab_id) {
                crate::bridge::ffi::notify_console_message(self.tab_id, level_code, &message);
            }
        }

        fn notify_media_session_event(&self, _webview: WebView, event: MediaSessionEvent) {
            if SHUTTING_DOWN.load(Ordering::Acquire) { return; }
            if !tab_exists(self.tab_id) { return; }
            match event {
                MediaSessionEvent::SetMetadata(m) => {
                    crate::bridge::ffi::notify_media_session_event(
                        self.tab_id, 0, 0, &m.title, &m.artist, &m.album, 0.0, 0.0, 0.0,
                    );
                }
                MediaSessionEvent::PlaybackStateChange(state) => {
                    let code = match state {
                        MediaSessionPlaybackState::None_ => 1,
                        MediaSessionPlaybackState::Playing => 2,
                        MediaSessionPlaybackState::Paused => 3,
                    };
                    crate::bridge::ffi::notify_media_session_event(
                        self.tab_id, 1, code, "", "", "", 0.0, 0.0, 0.0,
                    );
                }
                MediaSessionEvent::SetPositionState(p) => {
                    crate::bridge::ffi::notify_media_session_event(
                        self.tab_id, 2, 0, "", "", "", p.duration, p.position, p.playback_rate,
                    );
                }
            }
        }

        fn request_create_new(&self, _parent_webview: WebView, request: CreateNewWebViewRequest) {
            if SHUTTING_DOWN.load(Ordering::Acquire) { return; }
            let new_id = crate::servo_controller::create_tab();
            let (rc, clipboard, size, scale) = ENGINE.with(|s| {
                let s = s.borrow();
                let e = match s.as_ref() {
                    Some(e) => e,
                    None => return (None, None, PhysicalSize::new(800, 600), Scale::new(1.0f32)),
                };
                let (size, scale) = e.tabs.get(&self.tab_id)
                    .map(|t| (t.physical_size, t.hidpi_scale_factor))
                    .unwrap_or((PhysicalSize::new(800, 600), Scale::new(1.0)));
                (
                    Some(e.rendering_context.as_rendering_context()),
                    Some(e.clipboard_delegate.clone()),
                    size,
                    scale,
                )
            });
            let (Some(rc), Some(clipboard)) = (rc, clipboard) else { return; };
            let delegate: Rc<dyn WebViewDelegate> = Rc::new(ServoDelegate {
                tab_id: new_id,
                rendering_context: ENGINE.with(|s| s.borrow().as_ref().map(|e| e.rendering_context.clone()).unwrap()),
                animating: Cell::new(false),
                initial_resize_done: Cell::new(false),
            });
            let webview = request.builder(rc)
                .hidpi_scale_factor(scale)
                .clipboard_delegate(clipboard)
                .delegate(delegate)
                .build();
            configure_webview_diagnostics(&webview);
            ENGINE.with(|s| {
                if let Some(e) = s.borrow_mut().as_mut() {
                    e.tabs.insert(new_id, TabEntry {
                        webview,
                        current_url: String::new(),
                        title: "New Tab".to_string(),
                        loading: false,
                        status_text: String::new(),
                        active: true,
                        qt_modifiers: 0,
                        paint_hold: navigation_paint_hold(),
                        physical_size: size,
                        hidpi_scale_factor: scale,
                    });
                }
            });
            debug_log_detail("popup_new_webview", new_id, "url=<pending>");
            crate::bridge::ffi::request_open_tab_for_id(new_id);
        }

        fn show_embedder_control(&self, _webview: WebView, embedder_control: EmbedderControl) {
            if SHUTTING_DOWN.load(Ordering::Acquire) { return; }
            if !tab_exists(self.tab_id) { return; }
            match embedder_control {
                EmbedderControl::ContextMenu(menu) => {
                    let mut items_str = String::new();
                    for item in menu.items() {
                        match item {
                            ContextMenuItem::Item { label, action, enabled } => {
                                items_str.push_str(&format!("{}\t{}\t{}\n",
                                    context_menu_action_id(*action), label, enabled));
                            }
                            ContextMenuItem::Separator => {
                                items_str.push_str("sep\n");
                            }
                        }
                    }
                    let link_url = menu.element_info().link_url
                        .as_ref()
                        .map(|u| u.to_string())
                        .unwrap_or_default();
                    let selected = crate::bridge::ffi::show_context_menu_sync(
                        self.tab_id, &items_str, &link_url);
                    if let Some(action) = context_menu_action_from_id(selected) {
                        menu.select(action);
                    } else {
                        menu.dismiss();
                    }
                }
                // Simple dialogs are synchronous in spec terms (script is blocked),
                // so a modal Qt dialog matches; the SPINNING guard makes the nested
                // Qt event loop safe, as with the context menu above.
                EmbedderControl::SimpleDialog(dialog) => match dialog {
                    SimpleDialog::Alert(alert) => {
                        crate::bridge::ffi::show_alert_dialog_sync(self.tab_id, alert.message());
                        alert.confirm();
                    }
                    SimpleDialog::Confirm(confirm) => {
                        if crate::bridge::ffi::show_confirm_dialog_sync(self.tab_id, confirm.message()) {
                            confirm.confirm();
                        } else {
                            confirm.dismiss();
                        }
                    }
                    SimpleDialog::Prompt(mut prompt) => {
                        let result = crate::bridge::ffi::show_prompt_dialog_sync(
                            self.tab_id, prompt.message(), prompt.current_value());
                        if result.accepted {
                            prompt.set_current_value(&result.value);
                            prompt.confirm();
                        } else {
                            prompt.dismiss();
                        }
                    }
                },
                EmbedderControl::SelectElement(mut select) => {
                    // Labels are user content; tabs/newlines would break the
                    // line protocol, so flatten them to spaces.
                    let escape = |s: &str| s.replace(['\t', '\n'], " ");
                    let selected = select.selected_options();
                    let mut items = String::new();
                    let push_option =
                        |items: &mut String, option: &servo::SelectElementOption, in_group: bool| {
                            items.push_str(&format!(
                                "opt\t{}\t{}\t{}\t{}\t{}\n",
                                option.id,
                                escape(&option.label),
                                option.is_disabled as u8,
                                selected.contains(&option.id) as u8,
                                in_group as u8
                            ));
                        };
                    for entry in select.options() {
                        match entry {
                            SelectElementOptionOrOptgroup::Option(option) => {
                                push_option(&mut items, option, false);
                            }
                            SelectElementOptionOrOptgroup::Optgroup { label, options } => {
                                items.push_str(&format!("group\t{}\n", escape(label)));
                                for option in options {
                                    push_option(&mut items, option, true);
                                }
                            }
                        }
                    }
                    let position = select.position();
                    let chosen = crate::bridge::ffi::show_select_dropdown_sync(
                        self.tab_id,
                        &items,
                        position.min.x,
                        position.max.y,
                        position.width(),
                    );
                    if chosen >= 0 {
                        // selected_options carries option ids, mirroring
                        // servoshell desktop/dialog.rs.
                        select.select(vec![chosen as usize]);
                        select.submit();
                    }
                    // Dismissed: dropping resubmits the unchanged selection.
                }
                EmbedderControl::ColorPicker(mut picker) => {
                    let current = picker.current_color().unwrap_or(RgbColor {
                        red: 0,
                        green: 0,
                        blue: 0,
                    });
                    let result = crate::bridge::ffi::show_color_picker_sync(
                        self.tab_id, current.red, current.green, current.blue);
                    if result >= 0 {
                        picker.select(Some(RgbColor {
                            red: ((result >> 16) & 0xff) as u8,
                            green: ((result >> 8) & 0xff) as u8,
                            blue: (result & 0xff) as u8,
                        }));
                        picker.submit();
                    }
                    // Cancelled: dropping resubmits the unchanged color.
                }
                EmbedderControl::FilePicker(mut picker) => {
                    let filters: String = picker
                        .filter_patterns()
                        .iter()
                        .map(|pattern| pattern.0.as_str())
                        .collect::<Vec<_>>()
                        .join("\n");
                    let result = crate::bridge::ffi::show_file_picker_sync(
                        self.tab_id, &filters, picker.allow_select_multiple());
                    if result.is_empty() {
                        picker.dismiss();
                    } else {
                        let paths: Vec<PathBuf> = result
                            .split('\n')
                            .filter(|s| !s.is_empty())
                            .map(PathBuf::from)
                            .collect();
                        picker.select(&paths);
                        picker.submit();
                    }
                }
                // IME integration is out of scope for now (see WebContentView.h).
                EmbedderControl::InputMethod(_) => {}
            }
        }

        // Backs window.screen.* / window.screenX/outerWidth/…: real screen and
        // window-frame geometry from Qt, in device pixels. Returning None
        // would make pages see a screen the size of the webview.
        fn screen_geometry(&self, _webview: WebView) -> Option<ScreenGeometry> {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return None;
            }
            let g = crate::bridge::ffi::get_screen_geometry(self.tab_id);
            if !g.valid {
                return None;
            }
            Some(ScreenGeometry {
                size: DeviceIntSize::new(g.screen_width, g.screen_height),
                available_size: DeviceIntSize::new(g.available_width, g.available_height),
                window_rect: DeviceIntRect::from_origin_and_size(
                    DeviceIntPoint::new(g.window_x, g.window_y),
                    DeviceIntSize::new(g.window_width, g.window_height),
                ),
            })
        }

        // window.moveTo / window.resizeTo from page content. C++ applies the
        // popup-window policy (only honored for a single-tab window, like
        // Firefox/Chrome) and defers the actual move/resize off the delegate
        // callback.
        fn request_move_to(&self, _webview: WebView, point: DeviceIntPoint) {
            if SHUTTING_DOWN.load(Ordering::Acquire) || !tab_exists(self.tab_id) {
                return;
            }
            crate::bridge::ffi::request_window_move_to(self.tab_id, point.x, point.y);
        }

        fn request_resize_to(&self, _webview: WebView, size: DeviceIntSize) {
            if SHUTTING_DOWN.load(Ordering::Acquire) || !tab_exists(self.tab_id) {
                return;
            }
            crate::bridge::ffi::request_window_resize_to(self.tab_id, size.width, size.height);
        }

        // Permissions API / permission-gated features (notifications,
        // geolocation, wake lock, …). The script thread blocks on its own
        // channel awaiting the answer, so the synchronous modal-dialog FFI
        // pattern applies. C++ owns the per-origin persistence
        // (PermissionStore): a stored Allow/Block answers without UI, an
        // explicit Allow/Block in the prompt is persisted, and dismissing
        // denies once without persisting — Chrome's semantics.
        fn request_permission(&self, _webview: WebView, request: PermissionRequest) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                request.deny();
                return;
            }
            if !tab_exists(self.tab_id) {
                log_ignored_closed_callback("request_permission", self.tab_id);
                request.deny();
                return;
            }
            let origin = ENGINE.with(|s| {
                s.borrow()
                    .as_ref()
                    .and_then(|e| e.tabs.get(&self.tab_id))
                    .and_then(|t| Url::parse(&t.current_url).ok())
                    .map(|u| u.origin().ascii_serialization())
                    .unwrap_or_default()
            });
            let feature = permission_feature_name(request.feature());
            debug_log_detail(
                "request_permission",
                self.tab_id,
                format!("origin={origin} feature={feature}"),
            );
            if crate::bridge::ffi::request_permission_sync(self.tab_id, &origin, feature) {
                request.allow();
            } else {
                request.deny();
            }
        }

        // HTTP Basic/Digest authentication (401/407). Synchronous modal dialog
        // like the other embedder controls: the fetch blocks on its own
        // (network) thread awaiting the oneshot response, and the SPINNING
        // guard makes the nested Qt event loop safe. Dropping the request
        // without responding continues the load without credentials, which
        // surfaces the server's 401 page — the same behavior as Cancel in
        // Chrome and Firefox. Credentials are forwarded to Servo only; ServoQ
        // never logs or stores them (see remove_persisted_http_auth_cache).
        fn request_authentication(
            &self,
            _webview: WebView,
            authentication_request: AuthenticationRequest,
        ) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }
            if !tab_exists(self.tab_id) {
                log_ignored_closed_callback("request_authentication", self.tab_id);
                return;
            }
            let result = crate::bridge::ffi::show_authentication_dialog_sync(
                self.tab_id,
                authentication_request.url().as_str(),
                authentication_request.for_proxy(),
            );
            if result.accepted {
                authentication_request.authenticate(result.username, result.password);
            }
        }

        fn show_notification(&self, _webview: WebView, notification: servo::Notification) {
            crate::bridge::ffi::show_notification(
                self.tab_id,
                notification.title.as_str(),
                notification.body.as_str(),
            );
        }
    }

    // ---- thread-local engine state -------------------------

    thread_local! {
        static ENGINE: RefCell<Option<EngineState>> = const { RefCell::new(None) };
        // Re-entrancy guard: prevents spin_event_loop() from being called while
        // it is already on the call stack. In Qt's single-threaded event model
        // this cannot happen via concurrent threads, but it CAN happen if a
        // delegate callback triggers further Qt event processing (e.g. a modal
        // dialog or explicit processEvents()). The guard is cheap and defensive.
        static SPINNING: Cell<bool> = const { Cell::new(false) };
        // Which tab's pixels the shared Wayland subsurface currently shows:
        // Some(id) after a successful swap, None after C++ unmaps the
        // subsurface (empty-tab activation). Navigation paint holding uses it
        // to decide whether skipping a blank frame would leave another tab's
        // stale content visible.
        static WAYLAND_SURFACE_CONTENT_TAB: Cell<Option<i32>> = const { Cell::new(None) };
    }

    struct SpinGuard;

    impl SpinGuard {
        fn try_acquire() -> Option<SpinGuard> {
            SPINNING.with(|s| {
                if s.get() {
                    None
                } else {
                    s.set(true);
                    Some(SpinGuard)
                }
            })
        }
    }

    impl Drop for SpinGuard {
        fn drop(&mut self) {
            SPINNING.with(|s| s.set(false));
        }
    }

    fn resize_webview_to_entry(entry: &TabEntry) {
        entry.webview.set_hidpi_scale_factor(entry.hidpi_scale_factor);
        entry.webview.resize(entry.physical_size);
    }

    // Clone the Servo handle out of the RefCell so the borrow is dropped before
    // spin_event_loop() runs (which fires delegate callbacks that borrow ENGINE).
    fn clone_servo() -> Option<Servo> {
        if SHUTTING_DOWN.load(Ordering::Acquire) {
            return None;
        }
        ENGINE.with(|s| s.borrow().as_ref().map(|e| e.servo.clone()))
    }

    fn clone_webview(id: i32) -> Option<WebView> {
        if SHUTTING_DOWN.load(Ordering::Acquire) {
            return None;
        }
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()?
                .tabs
                .get(&id)
                .map(|t| t.webview.clone())
        })
    }

    // ---- public functions -----------------------------------

    // Initialize the Servo engine at application startup, before the main window
    // is shown and before any Qt show/resize/paint events can reach WebContentView.
    //
    // Hypothesis A (lazy-init timing): Servo is currently initialized lazily in
    // create_webview(), which is called from WebContentView::showEvent(). At that
    // point Qt is already delivering show/resize/paint events concurrently with
    // Servo's internal thread-pool and font-system startup. If layout requests a
    // fallback font while the font cache is still being populated, the fallback
    // cache can return a stale or uninitialized FontRef whose high bits contain
    // codepoint data — exactly the pattern seen in the crash dumps.
    //
    // Calling init_servo() from run_qt_application() before window.show() gives
    // Servo's internal subsystems (constellation, script, layout, font cache) time
    // to fully initialize before any web content or font shaping is requested.
    pub fn init_servo() {
        SHUTTING_DOWN.store(false, Ordering::Release);
        ENGINE.with(|state| {
            let mut state = state.borrow_mut();
            if state.is_some() {
                return;
            }
            eprintln!("[servoq] init_servo: initializing Servo at startup");
            *state = Some(create_engine_state(EngineRenderingContext::Software(Rc::new(
                // Placeholder size -- resized to the actual widget dimensions in
                // create_webview() before any WebView is built.
                    SoftwareRenderingContext::new(PhysicalSize::new(800, 600))
                        .expect("SoftwareRenderingContext::new failed"),
            ))));
            eprintln!("[servoq] init_servo: done");
        });
    }

    pub fn create_webview(id: i32, url_str: &str, w: i32, h: i32, scale: f32) {
        if SHUTTING_DOWN.load(Ordering::Acquire) {
            return;
        }
        let w = (w.max(1)) as u32;
        let h = (h.max(1)) as u32;
        let url = Url::parse(url_str).unwrap_or_else(|_| Url::parse("about:blank").unwrap());
        debug_log_detail(
            "create_webview",
            id,
            format!("raw_url={url_str} final_url_to_servo={url} size={w}x{h} scale={scale}"),
        );

        ENGINE.with(|state| {
            let mut state = state.borrow_mut();
            let engine = state.get_or_insert_with(|| {
                create_engine_state(EngineRenderingContext::Software(Rc::new(
                    SoftwareRenderingContext::new(PhysicalSize::new(w, h))
                        .expect("SoftwareRenderingContext::new failed"),
                )))
            });

            let size = PhysicalSize::new(w, h);
            let scale_factor = Scale::<f32, DeviceIndependentPixel, DevicePixel>::new(scale);

            if let Some(tab) = engine.tabs.get_mut(&id) {
                debug_log("create_webview_existing", id);
                tab.physical_size = size;
                tab.hidpi_scale_factor = scale_factor;
                if tab.active {
                    resize_webview_to_entry(tab);
                }
                return;
            }

            if let Some(context) = engine.rendering_context.software() {
                context.resize(size);
            }

            let delegate: Rc<dyn WebViewDelegate> = Rc::new(ServoDelegate {
                tab_id: id,
                rendering_context: engine.rendering_context.clone(),
                animating: Cell::new(false),
                initial_resize_done: Cell::new(false),
            });

            let rc_ctx: Rc<dyn RenderingContext> = engine.rendering_context.as_rendering_context();
            let webview = WebViewBuilder::new(&engine.servo, rc_ctx)
                .url(url.clone())
                .hidpi_scale_factor(scale_factor)
                .user_content_manager(engine.user_content_manager.clone())
                .clipboard_delegate(engine.clipboard_delegate.clone())
                .delegate(delegate)
                .build();
            configure_webview_diagnostics(&webview);

            engine.tabs.insert(
                id,
                TabEntry {
                    webview,
                    current_url: url.to_string(),
                    title: "New Tab".to_string(),
                    loading: false,
                    status_text: String::new(),
                    active: true,
                    qt_modifiers: 0,
                    paint_hold: navigation_paint_hold(),
                    physical_size: size,
                    hidpi_scale_factor: scale_factor,
                },
            );
        });
    }

    fn create_wayland_rendering_context(
        wl_display: u64,
        wl_surface: u64,
        size: PhysicalSize<u32>,
    ) -> Result<Rc<WindowRenderingContext>, String> {
        eprintln!(
            "SERVOQ_WAYLAND display=0x{wl_display:x} surface=0x{wl_surface:x} size={}x{}",
            size.width, size.height
        );

        // Root-cause fix: Surfman's Adapter::Software::set_environment_variables(), called
        // during SoftwareRenderingContext creation in init_servo(), sets LIBGL_ALWAYS_SOFTWARE=1
        // in the process environment. Mesa reads this env var inside eglInitialize() — not at
        // eglCreateContext time — to decide which GL driver to load. If it is set at that point,
        // Mesa loads LLVMpipe instead of the hardware driver (AMD/radeonsi).
        //
        // Surfman's HardwarePrime adapter clears LIBGL_ALWAYS_SOFTWARE later, in
        // create_context_descriptor(), but that runs after eglInitialize() — too late.
        //
        // Clearing it here, before Connection::from_display_handle() triggers eglInitialize(),
        // ensures Mesa selects the hardware driver for this Wayland window EGL display.
        // The SoftwareRenderingContext's own EGL display was already initialised before the
        // env var was set, so clearing it here does not affect software-mode operation.
        if std::env::var_os("LIBGL_ALWAYS_SOFTWARE").is_some() {
            std::env::remove_var("LIBGL_ALWAYS_SOFTWARE");
            eprintln!(
                "[servoq] cleared LIBGL_ALWAYS_SOFTWARE before Wayland EGL display init \
                 (set by software rendering context; would cause LLVMpipe selection)"
            );
        }

        let display_ptr = NonNull::new(wl_display as *mut c_void)
            .ok_or_else(|| "wl_display pointer is null".to_string())?;
        let surface_ptr = NonNull::new(wl_surface as *mut c_void)
            .ok_or_else(|| "wl_surface pointer is null".to_string())?;

        let display = RawDisplayHandle::Wayland(WaylandDisplayHandle::new(display_ptr));
        let window = RawWindowHandle::Wayland(WaylandWindowHandle::new(surface_ptr));
        // SAFETY: Qt owns the Wayland display/surface for at least as long as
        // the embedded QWindow. If Qt destroys the QWindow, WebContentView closes
        // the Servo WebView before the Rust rendering context is dropped.
        let display_handle = unsafe { DisplayHandle::borrow_raw(display) };
        let window_handle = unsafe { WindowHandle::borrow_raw(window) };

        WindowRenderingContext::new(display_handle, window_handle, size)
            .map(Rc::new)
            .map_err(|error| format!("WindowRenderingContext::new failed: {error:?}"))
    }

    pub fn create_webview_wayland_window(
        id: i32,
        url_str: &str,
        w: i32,
        h: i32,
        scale: f32,
        wl_display: u64,
        wl_surface: u64,
        allow_software_gl: bool,
    ) -> bool {
        if SHUTTING_DOWN.load(Ordering::Acquire) {
            return false;
        }
        if diag_enabled() {
            diag(format!(
                "create_webview_wayland_window id={id} {w}x{h} scale={scale} wl_surface=0x{wl_surface:x} active={}",
                tab_is_active(id)
            ));
        }
        let w = (w.max(1)) as u32;
        let h = (h.max(1)) as u32;
        let size = PhysicalSize::new(w, h);
        let scale_factor = Scale::<f32, DeviceIndependentPixel, DevicePixel>::new(scale);
        let url = Url::parse(url_str).unwrap_or_else(|_| Url::parse("about:blank").unwrap());

        let reused_existing_wayland_context = ENGINE.with(|state| {
            let mut state = state.borrow_mut();
            let Some(engine) = state.as_mut() else {
                return false;
            };
            if !matches!(engine.rendering_context, EngineRenderingContext::WaylandWindow(_)) {
                return false;
            }

            if let Some(tab) = engine.tabs.get_mut(&id) {
                tab.physical_size = size;
                tab.hidpi_scale_factor = scale_factor;
                if tab.active {
                    resize_webview_to_entry(tab);
                }
                if perf_enabled() {
                    eprintln!(
                        "SERVOQ_PERF renderer=wayland-window tab_id={id} webview_id={id} active_tab_id={id} wayland_surface_count=1 window_rendering_context_instances=1 new-tab-path=reuse-existing-webview"
                    );
                }
                return true;
            }

            let delegate: Rc<dyn WebViewDelegate> = Rc::new(ServoDelegate {
                tab_id: id,
                rendering_context: engine.rendering_context.clone(),
                animating: Cell::new(false),
                initial_resize_done: Cell::new(false),
            });

            let webview = WebViewBuilder::new(
                &engine.servo,
                engine.rendering_context.as_rendering_context(),
            )
            .url(url.clone())
            .hidpi_scale_factor(scale_factor)
            .user_content_manager(engine.user_content_manager.clone())
            .clipboard_delegate(engine.clipboard_delegate.clone())
            .delegate(delegate)
            .build();
            configure_webview_diagnostics(&webview);

            engine.tabs.insert(
                id,
                TabEntry {
                    webview,
                    current_url: url.to_string(),
                    title: "New Tab".to_string(),
                    loading: false,
                    status_text: String::new(),
                    active: true,
                    qt_modifiers: 0,
                    paint_hold: navigation_paint_hold(),
                    physical_size: size,
                    hidpi_scale_factor: scale_factor,
                },
            );
            if perf_enabled() {
                eprintln!(
                    "SERVOQ_PERF renderer=wayland-window tab_id={id} webview_id={id} active_tab_id={id} wayland_surface_count=1 window_rendering_context_instances=1 new-tab-path=reuse-existing-window-context"
                );
            }
            true
        });
        if reused_existing_wayland_context {
            return true;
        }

        let software_webviews_already_exist = ENGINE.with(|state| {
            state.borrow().as_ref().is_some_and(|engine| {
                matches!(engine.rendering_context, EngineRenderingContext::Software(_))
                    && !engine.tabs.is_empty()
            })
        });
        if software_webviews_already_exist {
            eprintln!("[servoq] Wayland window renderer unavailable: software WebViews already exist; falling back to software");
            return false;
        }

        let wayland_context = match create_wayland_rendering_context(wl_display, wl_surface, size) {
            Ok(context) => context,
            Err(reason) => {
                eprintln!(
                    "[servoq] Wayland window renderer unavailable: {reason}; falling back to software"
                );
                return false;
            }
        };

        let (software_gl, gl_renderer) =
            detect_and_log_wayland_gl(&wayland_context, size, scale).unwrap_or((false, String::new()));

        if software_gl {
            if !allow_software_gl {
                eprintln!(
                    "[servoq] auto renderer: Wayland renderer selected software GL ({gl_renderer}); falling back to software"
                );
                return false;
            }
            eprintln!("[servoq] WARNING: Wayland renderer is using software GL: {gl_renderer}");
        }

        ENGINE.with(|state| {
            let mut state = state.borrow_mut();
            let engine = state.get_or_insert_with(|| {
                create_engine_state(EngineRenderingContext::WaylandWindow(wayland_context.clone()))
            });

            if !engine.tabs.is_empty() && !matches!(engine.rendering_context, EngineRenderingContext::WaylandWindow(_)) {
                eprintln!("[servoq] Wayland window renderer unavailable: software WebViews already exist; falling back to software");
                return false;
            }
            if engine.tabs.is_empty() {
                engine.rendering_context = EngineRenderingContext::WaylandWindow(wayland_context.clone());
                if perf_enabled() {
                    eprintln!(
                        "SERVOQ_PERF renderer=wayland-window tab_id={id} webview_id={id} active_tab_id={id} wayland_surface_count=1 window_rendering_context_instances=1 new-tab-path=create-window-context software_context=false window_context=true size={}x{} scale={scale}",
                        size.width, size.height
                    );
                }
            }

            if let Some(tab) = engine.tabs.get_mut(&id) {
                tab.physical_size = size;
                tab.hidpi_scale_factor = scale_factor;
                if tab.active {
                    resize_webview_to_entry(tab);
                }
                return true;
            }

            let delegate: Rc<dyn WebViewDelegate> = Rc::new(ServoDelegate {
                tab_id: id,
                rendering_context: engine.rendering_context.clone(),
                animating: Cell::new(false),
                initial_resize_done: Cell::new(false),
            });

            let webview = WebViewBuilder::new(
                &engine.servo,
                engine.rendering_context.as_rendering_context(),
            )
            .url(url.clone())
            .hidpi_scale_factor(scale_factor)
            .user_content_manager(engine.user_content_manager.clone())
            .clipboard_delegate(engine.clipboard_delegate.clone())
            .delegate(delegate)
            .build();
            configure_webview_diagnostics(&webview);

            engine.tabs.insert(
                id,
                TabEntry {
                    webview,
                    current_url: url.to_string(),
                    title: "New Tab".to_string(),
                    loading: false,
                    status_text: String::new(),
                    active: true,
                    qt_modifiers: 0,
                    paint_hold: navigation_paint_hold(),
                    physical_size: size,
                    hidpi_scale_factor: scale_factor,
                },
            );
            true
        })
    }

    pub fn close_webview(id: i32) {
        if SHUTTING_DOWN.load(Ordering::Acquire) {
            return;
        }
        ENGINE.with(|s| {
            let mut s = s.borrow_mut();
            if let Some(e) = s.as_mut() {
                // Drop the TabEntry; dropping the WebView handle deregisters it from Servo.
                if e.tabs.remove(&id).is_some() {
                    debug_log("close_webview", id);
                } else {
                    debug_log("close_webview_missing", id);
                }
            }
        });
    }

    pub fn shutdown_servo() {
        if SHUTTING_DOWN.swap(true, Ordering::AcqRel) {
            return;
        }
        debug_log("shutdown_servo", 0);

        // Freeze-on-close guard. Dropping the engine runs Servo's
        // `Drop for ServoInner`, which blocks this (Qt UI) thread in
        // `while spin_event_loop() { sleep(500us) }` until the constellation
        // reports `FinishedShuttingDown`. If any component blocks its own
        // teardown — most likely a GStreamer/PipeWire media-pipeline thread now
        // that media is enabled — that state is never reached and the UI thread
        // spins forever (the window "freezes instead of closing"). Servo 0.2
        // exposes no timeout on that drop and we do not fork Servo, so we bound
        // it from the outside: a detached watchdog force-exits the process if
        // graceful teardown overruns the deadline. The fast path still saves
        // cookies/profile (written during the drop); only a genuinely stuck
        // teardown falls back to a hard exit. Deadline: SERVOQ_SHUTDOWN_TIMEOUT_MS
        // (default 4000; 0 disables the watchdog).
        let timeout_ms = std::env::var("SERVOQ_SHUTDOWN_TIMEOUT_MS")
            .ok()
            .and_then(|v| v.parse::<u64>().ok())
            .unwrap_or(4000);
        let shutdown_done = Arc::new(AtomicBool::new(false));
        if timeout_ms > 0 {
            let shutdown_done = shutdown_done.clone();
            let _ = std::thread::Builder::new()
                .name("servoq-shutdown-watchdog".to_owned())
                .spawn(move || {
                    let deadline = Instant::now() + Duration::from_millis(timeout_ms);
                    while Instant::now() < deadline {
                        if shutdown_done.load(Ordering::Acquire) {
                            return;
                        }
                        std::thread::sleep(Duration::from_millis(25));
                    }
                    if !shutdown_done.load(Ordering::Acquire) {
                        eprintln!(
                            "[servoq] graceful shutdown exceeded {timeout_ms}ms (blocked engine \
                             or media-pipeline teardown); forcing process exit"
                        );
                        // _exit(0): immediate termination that bypasses atexit /
                        // global destructors, which could themselves deadlock on a
                        // lock held by the stuck thread. The OS reclaims the rest.
                        // Exit 0 because the user asked to close — a stuck teardown
                        // is not a user-facing failure.
                        unsafe { libc::_exit(0) };
                    }
                });
        }

        ENGINE.with(|s| {
            if let Some(engine) = s.borrow().as_ref() {
                engine.servo.site_data_manager().clear_session_cookies();
            }
            // Drop WebViews, Servo, and WindowRenderingContext deterministically.
            // In Wayland mode this must happen while Qt still owns a valid QWindow
            // and wl_surface; otherwise Surfman/EGL can destroy a surface whose
            // Wayland proxy has already been torn down by Qt.
            *s.borrow_mut() = None;
        });
        // Servo's resource thread has now written its profile files (the drop
        // above completes the engine shutdown); delete the plaintext HTTP auth
        // cache it persisted alongside the cookie jar.
        if let Some(profile_dir) = servo_profile_dir() {
            remove_persisted_http_auth_cache(&profile_dir);
        }
        SPINNING.with(|s| s.set(false));
        // Graceful teardown finished within the deadline — disarm the watchdog.
        shutdown_done.store(true, Ordering::Release);
    }

    // Global tick: called by the Qt EventLoopWaker event handler (BrowserWindow::eventFilter).
    // No tab ID — panics are logged to stderr; per-tab crash notification is handled
    // by tick_webview (timer path) which retains the tab ID for crash reporting.
    pub fn tick_servo_global() {
        if SHUTTING_DOWN.load(Ordering::Acquire) {
            return;
        }
        let _guard = match SpinGuard::try_acquire() {
            Some(g) => g,
            None => {
                record_skipped_reentrant_tick();
                return;
            } // Already spinning — skip re-entrant call
        };
        if let Some(servo) = clone_servo() {
            let started = Instant::now();
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                servo.spin_event_loop();
            }));
            record_tick_time(started.elapsed());
            if let Err(e) = result {
                let reason = if let Some(s) = e.downcast_ref::<String>() {
                    s.clone()
                } else if let Some(s) = e.downcast_ref::<&str>() {
                    (*s).to_string()
                } else {
                    "unknown panic in servo event loop (waker path)".to_string()
                };
                eprintln!("Servo panic (waker): {reason}");
            }
        }
    }

    // spin_event_loop() is called after dropping the ENGINE borrow (clone_servo()).
    // This allows delegate callbacks fired inside spin_event_loop to borrow ENGINE.
    // catch_unwind guards against Servo panicking during font loading or similar
    // background operations — a Rust panic must NOT cross the FFI boundary into C++.
    pub fn tick_webview(id: i32) {
        if SHUTTING_DOWN.load(Ordering::Acquire) {
            return;
        }
        let _guard = match SpinGuard::try_acquire() {
            Some(g) => g,
            None => {
                record_skipped_reentrant_tick();
                return;
            } // Already spinning — skip re-entrant call
        };
        if let Some(servo) = clone_servo() {
            let started = Instant::now();
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                servo.spin_event_loop();
            }));
            record_tick_time(started.elapsed());
            if let Err(e) = result {
                let reason = if let Some(s) = e.downcast_ref::<String>() {
                    s.clone()
                } else if let Some(s) = e.downcast_ref::<&str>() {
                    (*s).to_string()
                } else {
                    "unknown panic in servo event loop".to_string()
                };
                eprintln!("Servo panic caught: {reason}");
                crate::bridge::ffi::notify_webview_crashed(id, &reason);
            }
        }
    }

    pub fn notify_wayland_subsurface_unmapped() {
        WAYLAND_SURFACE_CONTENT_TAB.with(|t| t.set(None));
    }

    pub fn present_wayland_webview(id: i32) {
        if SHUTTING_DOWN.load(Ordering::Acquire) {
            return;
        }
        let Some((webview, context, size, url, active)) = ENGINE.with(|s| {
            let s = s.borrow();
            let engine = s.as_ref()?;
            let tab = engine.tabs.get(&id)?;
            let context = engine.rendering_context.wayland_window()?;
            Some((
                tab.webview.clone(),
                context,
                tab.physical_size,
                tab.current_url.clone(),
                tab.active,
            ))
        }) else {
            return;
        };
        if !active {
            debug_log_detail("wayland_present_skipped_inactive_tab", id, &url);
            if diag_enabled() {
                diag(format!("present_wayland_webview SKIPPED inactive tab id={id} url={url}"));
            }
            return;
        }
        if diag_enabled() {
            diag(format!("present_wayland_webview id={id} active={active}"));
        }

        let started = Instant::now();
        debug_log_detail("wayland_present_enter", id, &url);
        let make_current_started = Instant::now();
        if let Err(error) = context.make_current() {
            eprintln!("[servoq] Wayland window renderer present failed: make_current: {error:?}");
            return;
        }
        // The shared surface can be recreated on tab re-attach (set_window), which
        // resets the swap interval, so re-assert interval=0 each present while the
        // context is current. Cheap EGL call; prevents eglSwapBuffers from blocking
        // the main thread on a withheld frame callback.
        disable_swap_vsync_for_current_context();
        let make_current_time = make_current_started.elapsed();
        let paint_started = Instant::now();
        webview.paint();
        let paint_time = paint_started.elapsed();
        // Navigation paint hold: while the new document has nothing to show,
        // don't swap — the shared subsurface keeps showing this tab's previous
        // page (or stays unmapped behind the new-tab placeholder). The swap is
        // still required when the surface currently shows ANOTHER tab's pixels
        // (tab switch onto a mid-load tab): blank is correct there, stale
        // foreign content is not. Read failures fail open and present.
        if paint_hold_active(id) {
            let surface_size = context.size2d().to_i32();
            let rect: Box2D<i32, DevicePixel> =
                Box2D::new(Point2D::origin(), Point2D::new(surface_size.width, surface_size.height));
            match context.read_to_image(rect) {
                Some(image) if frame_is_blank_white(image.as_raw()) => {
                    let surface_shows_other_tab = WAYLAND_SURFACE_CONTENT_TAB
                        .with(|t| t.get())
                        .is_some_and(|content_tab| content_tab != id);
                    if !surface_shows_other_tab {
                        debug_log("held_blank_navigation_frame", id);
                        return;
                    }
                }
                _ => set_paint_hold(id, NavPaintHold::Idle),
            }
        }
        let swap_started = Instant::now();
        context.present();
        WAYLAND_SURFACE_CONTENT_TAB.with(|t| t.set(Some(id)));
        let swap_time = swap_started.elapsed();
        record_wayland_present(
            started.elapsed(),
            make_current_time,
            paint_time,
            swap_time,
            size,
            &url,
        );
        if debug_enabled() {
            debug_log_detail("wayland_present_leave", id, format!("url={url} swap_ms={:.2}", swap_time.as_secs_f64() * 1000.0));
        }
    }

    pub fn load_url(id: i32, url_str: &str) {
        let url = Url::parse(url_str).unwrap_or_else(|_| Url::parse("about:blank").unwrap());
        debug_log_detail(
            "load_url",
            id,
            format!("raw_url={url_str} final_url_to_servo={url}"),
        );
        if let Some(wv) = clone_webview(id) {
            wv.load_request(UrlRequest::new(url));
        } else {
            crate::servo_controller::load_url(id, url_str);
        }
    }

    pub fn go_back(id: i32) {
        debug_log("go_back", id);
        if let Some(wv) = clone_webview(id) {
            wv.go_back(1);
        } else {
            crate::servo_controller::go_back(id);
        }
    }

    pub fn go_forward(id: i32) {
        debug_log("go_forward", id);
        if let Some(wv) = clone_webview(id) {
            wv.go_forward(1);
        } else {
            crate::servo_controller::go_forward(id);
        }
    }

    pub fn reload(id: i32) {
        debug_log("reload", id);
        if let Some(wv) = clone_webview(id) {
            wv.reload();
        } else {
            crate::servo_controller::reload(id);
        }
    }

    pub fn close_tab(id: i32) {
        debug_log("close_tab", id);
        close_webview(id);
        crate::servo_controller::close_tab(id);
    }

    pub fn can_go_back(id: i32) -> bool {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.webview.can_go_back())
                .unwrap_or_else(|| crate::servo_controller::can_go_back(id))
        })
    }

    pub fn can_go_forward(id: i32) -> bool {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.webview.can_go_forward())
                .unwrap_or_else(|| crate::servo_controller::can_go_forward(id))
        })
    }

    pub fn current_url(id: i32) -> String {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.current_url.clone())
                .unwrap_or_else(|| crate::servo_controller::current_url(id))
        })
    }

    pub fn title(id: i32) -> String {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.title.clone())
                .unwrap_or_else(|| crate::servo_controller::title(id))
        })
    }

    pub fn loading(id: i32) -> bool {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.loading)
                .unwrap_or_else(|| crate::servo_controller::loading(id))
        })
    }

    pub fn status_text(id: i32) -> String {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.status_text.clone())
                .unwrap_or_else(|| crate::servo_controller::status_text(id))
        })
    }

    pub fn forward_mouse_move(id: i32, x: f32, y: f32) {
        let point = WebViewPoint::Device(Point2D::new(x, y));
        let event = InputEvent::MouseMove(MouseMoveEvent::new(point));
        if let Some(wv) = clone_webview(id) {
            if diag_enabled() {
                diag(format!("mouse_move id={id} device=({x:.1},{y:.1})"));
            }
            wv.notify_input_event(event);
        }
    }

    pub fn forward_mouse_button(id: i32, action: i32, button: i32, x: f32, y: f32, mods: u32) {
        let action = match action {
            0 => MouseButtonAction::Down,
            _ => MouseButtonAction::Up,
        };
        let button = match button {
            0 => MouseButton::Left,
            1 => MouseButton::Middle,
            2 => MouseButton::Right,
            _ => return,
        };
        let point = WebViewPoint::Device(Point2D::new(x, y));
        let event = InputEvent::MouseButton(MouseButtonEvent::new(action, button, point));
        let force_modifier_sync = matches!(action, MouseButtonAction::Down);
        if let Some(wv) = sync_qt_modifiers_before_mouse(id, mods, force_modifier_sync) {
            if diag_enabled() {
                diag(format!(
                    "mouse_button id={id} action={action:?} button={button:?} device=({x:.1},{y:.1}) mods={mods}"
                ));
            }
            wv.notify_input_event(event);
        }
    }

    pub fn forward_wheel(id: i32, dx: f64, dy: f64, x: f32, y: f32) {
        use servo::input_events::WheelMode;
        let delta = WheelDelta {
            x: -dx,
            y: -dy,
            z: 0.0,
            mode: WheelMode::DeltaPixel,
        };
        let point = WebViewPoint::Device(Point2D::new(x, y));
        let event = InputEvent::Wheel(WheelEvent::new(delta, point));
        if let Some(wv) = clone_webview(id) {
            if diag_enabled() {
                diag(format!("wheel id={id} delta=({dx:.1},{dy:.1}) device=({x:.1},{y:.1})"));
            }
            wv.notify_input_event(event);
        }
    }

    // Touchpad pinch gesture (Qt::ZoomNativeGesture). `scale` is the final
    // multiplicative factor for this gesture step (1.0 + Qt's incremental
    // value, matching servoshell's `delta + 1.0` for winit's PinchGesture);
    // Servo multiplies the current pinch zoom by it and clamps to [1, 10].
    // Pinch zoom magnifies the viewport without relayout — page zoom
    // (Ctrl+wheel / Ctrl+±) stays a separate mechanism.
    pub fn forward_pinch_zoom(id: i32, scale: f32, x: f32, y: f32) {
        if let Some(wv) = clone_webview(id) {
            if diag_enabled() {
                diag(format!("pinch_zoom id={id} scale={scale:.3} device=({x:.1},{y:.1})"));
            }
            wv.adjust_pinch_zoom(scale, Point2D::new(x, y));
        }
    }

    // key_char: Unicode code point from Qt event.text()[0], 0 for non-printable.
    // qt_key:   Qt::Key enum value.
    // mods:     Qt::KeyboardModifiers flags.
    // Ctrl chord -> Servo editing action. Servo does NOT perform copy/cut/paste
    // from a raw Ctrl+<key> keyboard event: servoshell intercepts these chords and
    // dispatches an InputEvent::EditingAction instead (see
    // vendor/reference-servo/ports/servoshell/desktop/headed_window.rs ~360-372).
    // We do the same. Routed here because forward_key is only ever called from
    // focused web content — the URL bar, find box and other Qt chrome widgets keep
    // their native Qt clipboard handling and never reach this path. Ctrl+A is NOT
    // turned into an editing action (Servo has no SelectAll action) — it stays a
    // keyboard event, but with a corrected logical key (see forward_key below).
    // True for a Control chord that is not AltGr. On Linux AltGr reports as
    // Ctrl+Alt and is used to type characters on international layouts, so it must
    // be excluded — a genuine Ctrl shortcut never holds Alt.
    const QT_SHIFT_MODIFIER: u32 = 0x0200_0000;
    const QT_CONTROL_MODIFIER: u32 = 0x0400_0000;
    const QT_ALT_MODIFIER: u32 = 0x0800_0000;
    const QT_META_MODIFIER: u32 = 0x1000_0000;
    const QT_TRACKED_MODIFIERS: u32 =
        QT_SHIFT_MODIFIER | QT_CONTROL_MODIFIER | QT_ALT_MODIFIER | QT_META_MODIFIER;

    fn normalize_qt_modifiers(mods: u32) -> u32 {
        mods & QT_TRACKED_MODIFIERS
    }

    fn remember_qt_modifiers(id: i32, mods: u32) {
        let normalized = normalize_qt_modifiers(mods);
        ENGINE.with(|s| {
            if let Some(tab) = s.borrow_mut().as_mut().and_then(|e| e.tabs.get_mut(&id)) {
                tab.qt_modifiers = normalized;
            }
        });
    }

    fn sync_qt_modifiers_before_mouse(id: i32, mods: u32, force: bool) -> Option<WebView> {
        let normalized = normalize_qt_modifiers(mods);
        let (webview, previous) = ENGINE.with(|s| {
            let mut state = s.borrow_mut();
            let tab = state.as_mut()?.tabs.get_mut(&id)?;
            let previous = tab.qt_modifiers;
            tab.qt_modifiers = normalized;
            Some((tab.webview.clone(), previous))
        })?;

        if force || previous != normalized {
            // Servo's constellation applies its last keyboard modifier state to
            // subsequent mouse events. Browser-level shortcuts can move focus
            // into Qt chrome after Servo saw Ctrl/Shift/Alt/Meta go down, so the
            // matching key-up never reaches Servo. Sync the stored modifier mask
            // before a mouse click so ordinary links do not become Ctrl-clicks.
            let kb_event = ServoKeyboardEvent::new_without_event(
                KeyState::Up,
                Key::Named(NamedKey::Unidentified),
                Code::Unidentified,
                Location::Standard,
                qt_mods_to_modifiers(normalized),
                false,
                false,
            );
            if diag_enabled() {
                diag(format!(
                    "sync_modifiers_before_mouse id={id} previous={previous} current={normalized}"
                ));
            }
            webview.notify_input_event(InputEvent::Keyboard(kb_event));
        }

        Some(webview)
    }

    fn ctrl_chord_active(mods: u32) -> bool {
        mods & QT_CONTROL_MODIFIER != 0 && mods & QT_ALT_MODIFIER == 0
    }

    fn ctrl_editing_action(qt_key: i32, mods: u32) -> Option<EditingActionEvent> {
        const QT_KEY_C: i32 = 0x43;
        const QT_KEY_X: i32 = 0x58;
        const QT_KEY_V: i32 = 0x56;
        if !ctrl_chord_active(mods) {
            return None;
        }
        match qt_key {
            QT_KEY_C => Some(EditingActionEvent::Copy),
            QT_KEY_X => Some(EditingActionEvent::Cut),
            QT_KEY_V => Some(EditingActionEvent::Paste),
            _ => None,
        }
    }

    pub fn forward_key(id: i32, down: bool, key_char: u32, qt_key: i32, mods: u32) {
        remember_qt_modifiers(id, mods);
        if let Some(action) = ctrl_editing_action(qt_key, mods) {
            // Swallow both press and release of the clipboard chord so the raw
            // 'c'/'x'/'v' key never additionally reaches Servo as text input.
            if down {
                // Log the action type only — never the clipboard contents.
                let action_name = match action {
                    EditingActionEvent::Copy => "copy",
                    EditingActionEvent::Cut => "cut",
                    EditingActionEvent::Paste => "paste",
                };
                diag(format!("editing_action id={id} action={action_name}"));
                if let Some(wv) = clone_webview(id) {
                    wv.notify_input_event(InputEvent::EditingAction(action));
                }
            }
            return;
        }
        let state = if down { KeyState::Down } else { KeyState::Up };
        const QT_KEY_A: i32 = 0x41;
        let key = if ctrl_chord_active(mods) && qt_key == QT_KEY_A {
            // Ctrl+A select-all. Servo exposes no EditingActionEvent::SelectAll
            // (the enum is only Copy/Cut/Paste), so — like servoshell — we forward
            // a keyboard event. But under Ctrl, Qt's event.text() is the SOH
            // control char, so qt_key_to_key() yields Key::Named(Unidentified) and
            // Servo's TextInput select-all shortcut (keyboard-types ShortcutMatcher,
            // which matches the *logical* key 'a' case-insensitively) never fires.
            // Forward the corrected logical key so Servo can match it. Only the
            // keydown triggers select_all in Servo; the keyup is harmless.
            diag(format!("select_all id={id} down={down} via=keyboard Key::Character(\"a\")"));
            Key::Character("a".to_string().into())
        } else {
            qt_key_to_key(key_char, qt_key)
        };
        let code = qt_key_to_code(qt_key);
        let modifiers = qt_mods_to_modifiers(mods);
        let kb_event = ServoKeyboardEvent::new_without_event(
            state,
            key,
            code,
            Location::Standard,
            modifiers,
            false,
            false,
        );
        let event = InputEvent::Keyboard(kb_event);
        let wv = clone_webview(id);
        // Runs per keypress in web content: do not build the payload (or probe
        // tab_is_active) unless diagnostics are on.
        if diag_enabled() {
            diag(format!(
                "forward_key id={id} down={down} key_char={key_char} qt_key={qt_key} mods={mods} webview_found={} active={}",
                wv.is_some(),
                tab_is_active(id)
            ));
        }
        if let Some(wv) = wv {
            wv.notify_input_event(event);
        }
    }

    pub fn forward_focus(id: i32, focused: bool) {
        let wv = clone_webview(id);
        if diag_enabled() {
            diag(format!(
                "forward_focus id={id} focused={focused} webview_found={} active={}",
                wv.is_some(),
                tab_is_active(id)
            ));
        }
        if let Some(wv) = wv {
            if focused {
                wv.focus();
            } else {
                wv.blur();
            }
        }
    }

    pub fn forward_theme_change(id: i32, dark: bool) {
        if let Some(wv) = clone_webview(id) {
            wv.notify_theme_change(if dark { Theme::Dark } else { Theme::Light });
        }
    }

    pub fn set_webview_active(id: i32, active: bool) {
        if diag_enabled() {
            diag(format!("set_webview_active id={id} active={active}"));
        }
        ENGINE.with(|s| {
            if let Some(entry) = s.borrow_mut().as_mut().and_then(|e| e.tabs.get_mut(&id)) {
                entry.active = active;
            }
        });
        if active {
            ENGINE.with(|s| {
                let mut s = s.borrow_mut();
                if let Some(e) = s.as_mut() {
                    if let Some(entry) = e.tabs.get(&id) {
                        resize_webview_to_entry(entry);
                    }
                }
            });
        }
        if let Some(wv) = clone_webview(id) {
            if active {
                wv.show();
                wv.set_throttled(false);
                wv.focus();
            } else {
                wv.blur();
                wv.set_throttled(true);
                wv.hide();
            }
        }
    }

    // Servo 0.2.0: WebView::set_page_zoom(f32) — clamped to [0.1, 10.0] internally
    pub fn set_page_zoom(id: i32, zoom: f32) {
        ENGINE.with(|s| {
            let s = s.borrow();
            let Some(entry) = s.as_ref().and_then(|e| e.tabs.get(&id)) else {
                return;
            };
            entry.webview.set_page_zoom(zoom);
            // Servo 0.2.0 keeps WebView::set_animating() crate-private. A same-size
            // resize uses public API and marks the painter for repaint after zoom.
            entry.webview.resize(entry.physical_size);
        });
    }

    pub fn page_zoom(id: i32) -> f32 {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.webview.page_zoom())
                .unwrap_or(1.0)
        })
    }

    // Asynchronous full-viewport screenshot. Servo waits for the page to be
    // ready (load events fired, render-blocking resources done, fonts/images
    // loaded, no pending frames) and then calls back with the pixels; the
    // result is pushed to C++ as RGBA8 via notify_screenshot_taken, with an
    // empty slice signalling failure. The callback fires inside a later
    // spin_event_loop on the main thread.
    pub fn take_screenshot(id: i32) {
        let Some(wv) = clone_webview(id) else {
            crate::bridge::ffi::notify_screenshot_taken(id, &[], 0, 0);
            return;
        };
        debug_log("take_screenshot", id);
        wv.take_screenshot(None, move |result| match result {
            Ok(image) => {
                let w = image.width() as i32;
                let h = image.height() as i32;
                crate::bridge::ffi::notify_screenshot_taken(id, image.as_raw(), w, h);
            }
            Err(error) => {
                eprintln!("[servoq] take_screenshot failed for tab {id}: {error:?}");
                crate::bridge::ffi::notify_screenshot_taken(id, &[], 0, 0);
            }
        });
    }

    // Debug tooling (M3.7): evaluate JavaScript in the tab's page context.
    // Results are serialized to JSON-ish text and pushed back through
    // notify_javascript_result with the caller-chosen request_id; this is the
    // engine half of the future servoq://debug page (M4.4). The callback
    // fires inside a later spin_event_loop on the main thread.
    pub fn evaluate_javascript(id: i32, request_id: u64, script: &str) {
        let Some(wv) = clone_webview(id) else {
            crate::bridge::ffi::notify_javascript_result(
                id,
                request_id,
                false,
                "webview does not exist",
            );
            return;
        };
        debug_log_detail("evaluate_javascript", id, format!("request_id={request_id}"));
        wv.evaluate_javascript(script, move |result| match result {
            Ok(value) => crate::bridge::ffi::notify_javascript_result(
                id,
                request_id,
                true,
                &js_value_to_json(&value),
            ),
            Err(error) => crate::bridge::ffi::notify_javascript_result(
                id,
                request_id,
                false,
                &format!("{error:?}"),
            ),
        });
    }

    fn json_escape(s: &str) -> String {
        let mut out = String::with_capacity(s.len());
        for c in s.chars() {
            match c {
                '"' => out.push_str("\\\""),
                '\\' => out.push_str("\\\\"),
                '\n' => out.push_str("\\n"),
                '\r' => out.push_str("\\r"),
                '\t' => out.push_str("\\t"),
                c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
                c => out.push(c),
            }
        }
        out
    }

    // JSON text for a JSValue. DOM references (Element/Window/…) cannot be
    // serialized as data, so they become descriptive strings, like the
    // "[object …]" placeholders a devtools console prints.
    fn js_value_to_json(value: &servo::JSValue) -> String {
        use servo::JSValue;
        match value {
            JSValue::Undefined => "undefined".to_string(),
            JSValue::Null => "null".to_string(),
            JSValue::Boolean(b) => b.to_string(),
            JSValue::Number(n) => {
                if n.fract() == 0.0 && n.is_finite() && n.abs() < 1e15 {
                    format!("{}", *n as i64)
                } else {
                    n.to_string()
                }
            }
            JSValue::String(s) => format!("\"{}\"", json_escape(s)),
            JSValue::Element(s) => format!("\"[object Element {}]\"", json_escape(s)),
            JSValue::ShadowRoot(s) => format!("\"[object ShadowRoot {}]\"", json_escape(s)),
            JSValue::Frame(s) => format!("\"[object Frame {}]\"", json_escape(s)),
            JSValue::Window(s) => format!("\"[object Window {}]\"", json_escape(s)),
            JSValue::Array(values) => {
                let items: Vec<String> = values.iter().map(js_value_to_json).collect();
                format!("[{}]", items.join(","))
            }
            JSValue::Object(map) => {
                let mut entries: Vec<(&String, &JSValue)> = map.iter().collect();
                entries.sort_by_key(|(key, _)| key.as_str());
                let items: Vec<String> = entries
                    .iter()
                    .map(|(key, value)| {
                        format!("\"{}\":{}", json_escape(key), js_value_to_json(value))
                    })
                    .collect();
                format!("{{{}}}", items.join(","))
            }
        }
    }

    pub fn set_experimental_features_enabled(enabled: bool) {
        if let Some(servo) = clone_servo() {
            for pref in EXPERIMENTAL_PREFS {
                // Servo::set_preference -> Preferences::set_value PANICS on an
                // unknown preference name. Guard every name with the generated
                // Preferences::exists() so a pref Servo has renamed/removed in a
                // future version is skipped with a log line instead of aborting.
                if Preferences::exists(pref) {
                    servo.set_preference(pref, PrefValue::Bool(enabled));
                    diag(format!("experimental_pref applied name={pref} value={enabled}"));
                } else {
                    eprintln!(
                        "SERVOQ: skipping unknown experimental preference '{pref}' \
                         (not present in this Servo build)"
                    );
                }
            }
        }
    }

    // Matches Ladybird WebContentView::update_viewport_size() (vendor line 760-766):
    // physical pixel dimensions are passed pre-scaled from WebContentView::resizeEvent.
    pub fn forward_resize(id: i32, w: i32, h: i32, scale: f32) {
        let size = PhysicalSize::new(w.max(1) as u32, h.max(1) as u32);
        let scale_factor = Scale::<f32, DeviceIndependentPixel, DevicePixel>::new(scale);
        debug_log_detail(
            "resize",
            id,
            format!("{}x{} scale={scale}", size.width, size.height),
        );
        ENGINE.with(|s| {
            let mut s = s.borrow_mut();
            if let Some(e) = s.as_mut() {
                if let Some(t) = e.tabs.get_mut(&id) {
                    t.physical_size = size;
                    t.hidpi_scale_factor = scale_factor;
                    if t.active {
                        resize_webview_to_entry(t);
                    }
                }
            }
        });
    }

    // ---- key mapping helpers --------------------------------

    fn qt_key_to_key(key_char: u32, qt_key: i32) -> Key {
        // Named keys by Qt::Key hex values
        match qt_key {
            0x01000000 => Key::Named(NamedKey::Escape),
            0x01000001 => Key::Named(NamedKey::Tab),
            0x01000002 => Key::Named(NamedKey::Tab), // Qt::Key_Backtab, Shift is carried in modifiers
            0x01000003 => Key::Named(NamedKey::Backspace),
            0x01000004 | 0x01000005 => Key::Named(NamedKey::Enter),
            0x01000006 => Key::Named(NamedKey::Insert),
            0x01000007 => Key::Named(NamedKey::Delete),

            0x01000010 => Key::Named(NamedKey::Home),
            0x01000011 => Key::Named(NamedKey::End),
            0x01000012 => Key::Named(NamedKey::ArrowLeft),
            0x01000013 => Key::Named(NamedKey::ArrowUp),
            0x01000014 => Key::Named(NamedKey::ArrowRight),
            0x01000015 => Key::Named(NamedKey::ArrowDown),
            0x01000016 => Key::Named(NamedKey::PageUp),
            0x01000017 => Key::Named(NamedKey::PageDown),

            0x01000020 => Key::Named(NamedKey::Shift),
            0x01000021 => Key::Named(NamedKey::Control),
            0x01000022 => Key::Named(NamedKey::Meta),
            0x01000023 => Key::Named(NamedKey::Alt),
            0x01000025 => Key::Named(NamedKey::CapsLock),

            0x01000030 => Key::Named(NamedKey::F1),
            0x01000031 => Key::Named(NamedKey::F2),
            0x01000032 => Key::Named(NamedKey::F3),
            0x01000033 => Key::Named(NamedKey::F4),
            0x01000034 => Key::Named(NamedKey::F5),
            0x01000035 => Key::Named(NamedKey::F6),
            0x01000036 => Key::Named(NamedKey::F7),
            0x01000037 => Key::Named(NamedKey::F8),
            0x01000038 => Key::Named(NamedKey::F9),
            0x01000039 => Key::Named(NamedKey::F10),
            0x0100003a => Key::Named(NamedKey::F11),
            0x0100003b => Key::Named(NamedKey::F12),
            _ => {
                if let Some(c) = char::from_u32(key_char) {
                    if !c.is_control() {
                        return Key::Character(c.to_string().into());
                    }
                }
                Key::Named(NamedKey::Unidentified)
            }
        }
    }

    fn qt_key_to_code(qt_key: i32) -> Code {
        match qt_key {
            0x20 => Code::Space,
            0x27 => Code::Quote,
            0x2c => Code::Comma,
            0x2d => Code::Minus,
            0x2e => Code::Period,
            0x2f => Code::Slash,
            0x30 => Code::Digit0,
            0x31 => Code::Digit1,
            0x32 => Code::Digit2,
            0x33 => Code::Digit3,
            0x34 => Code::Digit4,
            0x35 => Code::Digit5,
            0x36 => Code::Digit6,
            0x37 => Code::Digit7,
            0x38 => Code::Digit8,
            0x39 => Code::Digit9,
            0x3b => Code::Semicolon,
            0x3d => Code::Equal,
            0x41 => Code::KeyA,
            0x42 => Code::KeyB,
            0x43 => Code::KeyC,
            0x44 => Code::KeyD,
            0x45 => Code::KeyE,
            0x46 => Code::KeyF,
            0x47 => Code::KeyG,
            0x48 => Code::KeyH,
            0x49 => Code::KeyI,
            0x4a => Code::KeyJ,
            0x4b => Code::KeyK,
            0x4c => Code::KeyL,
            0x4d => Code::KeyM,
            0x4e => Code::KeyN,
            0x4f => Code::KeyO,
            0x50 => Code::KeyP,
            0x51 => Code::KeyQ,
            0x52 => Code::KeyR,
            0x53 => Code::KeyS,
            0x54 => Code::KeyT,
            0x55 => Code::KeyU,
            0x56 => Code::KeyV,
            0x57 => Code::KeyW,
            0x58 => Code::KeyX,
            0x59 => Code::KeyY,
            0x5a => Code::KeyZ,
            0x5b => Code::BracketLeft,
            0x5c => Code::Backslash,
            0x5d => Code::BracketRight,
            0x60 => Code::Backquote,
            0x01000000 => Code::Escape,
            0x01000001 => Code::Tab,
            0x01000003 => Code::Backspace,
            0x01000004 | 0x01000005 => Code::Enter,
            0x01000010 => Code::Home,
            0x01000011 => Code::End,
            0x01000012 => Code::ArrowLeft,
            0x01000013 => Code::ArrowUp,
            0x01000014 => Code::ArrowRight,
            0x01000015 => Code::ArrowDown,
            0x01000016 => Code::PageUp,
            0x01000017 => Code::PageDown,
            0x01000020 => Code::ShiftLeft,
            0x01000021 => Code::ControlLeft,
            0x01000022 => Code::MetaLeft,
            0x01000023 => Code::AltLeft,
            0x01000025 => Code::CapsLock,
            0x01000030 => Code::F1,
            0x01000031 => Code::F2,
            0x01000032 => Code::F3,
            0x01000033 => Code::F4,
            0x01000034 => Code::F5,
            0x01000035 => Code::F6,
            0x01000036 => Code::F7,
            0x01000037 => Code::F8,
            0x01000038 => Code::F9,
            0x01000039 => Code::F10,
            0x0100003a => Code::F11,
            0x0100003b => Code::F12,
            0x01000060 => Code::Delete,
            0x01000061 => Code::Insert,
            _ => Code::Unidentified,
        }
    }

    fn qt_mods_to_modifiers(mods: u32) -> Modifiers {
        let mut result = Modifiers::empty();
        if mods & 0x0200_0000 != 0 {
            result |= Modifiers::SHIFT;
        }
        if mods & 0x0400_0000 != 0 {
            result |= Modifiers::CONTROL;
        }
        if mods & 0x0800_0000 != 0 {
            result |= Modifiers::ALT;
        }
        if mods & 0x1000_0000 != 0 {
            result |= Modifiers::META;
        }
        result
    }

    // ---- site data / cookies / cache (M4.5, M4.6) ----------------------

    // Newline-separated "site\tstorage_bits" rows, one per eTLD+1 site that has
    // cookies, localStorage, or sessionStorage. storage_bits is the StorageType
    // bitflags value (1=Cookies, 2=Local, 4=Session).
    pub fn list_site_data() -> String {
        ENGINE.with(|s| {
            let s = s.borrow();
            let Some(e) = s.as_ref() else { return String::new() };
            let sites = e.servo.site_data_manager().site_data(StorageType::all());
            sites
                .iter()
                .map(|sd| format!("{}\t{}", sd.name(), sd.storage_types().bits()))
                .collect::<Vec<_>>()
                .join("\n")
        })
    }

    pub fn clear_site_data_for(sites_joined: &str) {
        let sites: Vec<&str> = sites_joined
            .split('\n')
            .map(|s| s.trim())
            .filter(|s| !s.is_empty())
            .collect();
        if sites.is_empty() {
            return;
        }
        ENGINE.with(|s| {
            if let Some(e) = s.borrow().as_ref() {
                e.servo
                    .site_data_manager()
                    .clear_site_data(&sites, StorageType::all());
            }
        });
    }

    pub fn clear_all_cookies() {
        ENGINE.with(|s| {
            if let Some(e) = s.borrow().as_ref() {
                e.servo.site_data_manager().clear_cookies();
            }
        });
    }

    pub fn clear_http_cache() {
        ENGINE.with(|s| {
            if let Some(e) = s.borrow().as_ref() {
                e.servo.network_manager().clear_cache();
            }
        });
    }

    // Live cookies Servo would attach to an HTTP request for `url`, so the
    // native PDF viewer's out-of-engine fetch carries the same session as the
    // page (session/HttpOnly cookies included). Servo owns the matching; we
    // only serialize. One row per cookie: "name\tvalue\tdomain\tpath\tsecure",
    // newline-separated. Empty when the engine is down or there are none.
    pub fn cookies_for_url(url: &str) -> String {
        let Ok(parsed) = Url::parse(url) else {
            return String::new();
        };
        ENGINE.with(|s| {
            let s = s.borrow();
            let Some(e) = s.as_ref() else {
                return String::new();
            };
            // CookieSource::HTTP mirrors a real network fetch: it includes
            // HttpOnly cookies (which auth sessions usually are).
            let cookies = e
                .servo
                .site_data_manager()
                .cookies_for_url(parsed, CookieSource::HTTP);
            cookies
                .iter()
                .map(|c| {
                    format!(
                        "{}\t{}\t{}\t{}\t{}",
                        c.name(),
                        c.value(),
                        c.domain().unwrap_or(""),
                        c.path().unwrap_or(""),
                        if c.secure().unwrap_or(false) { "1" } else { "0" },
                    )
                })
                .collect::<Vec<_>>()
                .join("\n")
        })
    }

    // ---- media session (M5.6) ------------------------------------------

    pub fn media_session_action(id: i32, action: i32) {
        let action_type = match action {
            0 => MediaSessionActionType::Play,
            1 => MediaSessionActionType::Pause,
            2 => MediaSessionActionType::SeekBackward,
            3 => MediaSessionActionType::SeekForward,
            4 => MediaSessionActionType::PreviousTrack,
            5 => MediaSessionActionType::NextTrack,
            6 => MediaSessionActionType::SkipAd,
            7 => MediaSessionActionType::Stop,
            8 => MediaSessionActionType::SeekTo,
            _ => return,
        };
        if let Some(wv) = clone_webview(id) {
            wv.notify_media_session_action_event(action_type);
        }
    }

    pub fn set_console_capture_enabled(enabled: bool) {
        CONSOLE_CAPTURE.store(enabled, Ordering::Relaxed);
    }
} // mod engine

// ============================================================
// Public API — always present, no-ops when feature is off
// ============================================================

// When servo-engine is on, these forward into the engine module above.
// When off they are stubs so the bridge compiles and the default build is
// unchanged.

pub fn init_servo() {
    #[cfg(feature = "servo-engine")]
    engine::init_servo();
}

pub fn create_webview(_id: i32, _url: &str, _w: i32, _h: i32, _scale: f32) {
    #[cfg(feature = "servo-engine")]
    engine::create_webview(_id, _url, _w, _h, _scale);
}

pub fn create_webview_wayland_window(
    _id: i32,
    _url: &str,
    _w: i32,
    _h: i32,
    _scale: f32,
    _wl_display: u64,
    _wl_surface: u64,
    _allow_software_gl: bool,
) -> bool {
    #[cfg(feature = "servo-engine")]
    return engine::create_webview_wayland_window(
        _id,
        _url,
        _w,
        _h,
        _scale,
        _wl_display,
        _wl_surface,
        _allow_software_gl,
    );
    #[allow(unreachable_code)]
    false
}

pub fn close_webview(_id: i32) {
    #[cfg(feature = "servo-engine")]
    engine::close_webview(_id);
}

pub fn shutdown_servo() {
    #[cfg(feature = "servo-engine")]
    engine::shutdown_servo();
}

pub fn tick_webview(_id: i32) {
    #[cfg(feature = "servo-engine")]
    engine::tick_webview(_id);
}

pub fn tick_servo() {
    #[cfg(feature = "servo-engine")]
    engine::tick_servo_global();
}

pub fn present_wayland_webview(_id: i32) {
    #[cfg(feature = "servo-engine")]
    engine::present_wayland_webview(_id);
}

pub fn notify_wayland_subsurface_unmapped() {
    #[cfg(feature = "servo-engine")]
    engine::notify_wayland_subsurface_unmapped();
}

pub fn forward_mouse_move(_id: i32, _x: f32, _y: f32) {
    #[cfg(feature = "servo-engine")]
    engine::forward_mouse_move(_id, _x, _y);
}

pub fn forward_mouse_button(_id: i32, _action: i32, _button: i32, _x: f32, _y: f32, _mods: u32) {
    #[cfg(feature = "servo-engine")]
    engine::forward_mouse_button(_id, _action, _button, _x, _y, _mods);
}

pub fn forward_wheel(_id: i32, _dx: f64, _dy: f64, _x: f32, _y: f32) {
    #[cfg(feature = "servo-engine")]
    engine::forward_wheel(_id, _dx, _dy, _x, _y);
}

pub fn forward_pinch_zoom(_id: i32, _scale: f32, _x: f32, _y: f32) {
    #[cfg(feature = "servo-engine")]
    engine::forward_pinch_zoom(_id, _scale, _x, _y);
}

pub fn forward_key(_id: i32, _down: bool, _key_char: u32, _qt_key: i32, _mods: u32) {
    #[cfg(feature = "servo-engine")]
    engine::forward_key(_id, _down, _key_char, _qt_key, _mods);
}

pub fn forward_focus(_id: i32, _focused: bool) {
    #[cfg(feature = "servo-engine")]
    engine::forward_focus(_id, _focused);
}

pub fn forward_resize(_id: i32, _w: i32, _h: i32, _scale: f32) {
    #[cfg(feature = "servo-engine")]
    engine::forward_resize(_id, _w, _h, _scale);
}

pub fn forward_theme_change(_id: i32, _dark: bool) {
    #[cfg(feature = "servo-engine")]
    engine::forward_theme_change(_id, _dark);
}

pub fn set_webview_active(_id: i32, _active: bool) {
    #[cfg(feature = "servo-engine")]
    engine::set_webview_active(_id, _active);
}

pub fn set_page_zoom(_id: i32, _zoom: f32) {
    #[cfg(feature = "servo-engine")]
    engine::set_page_zoom(_id, _zoom);
}

pub fn take_screenshot(_id: i32) {
    #[cfg(feature = "servo-engine")]
    engine::take_screenshot(_id);
    // Without the engine there are no pixels; report failure so the UI
    // doesn't wait forever.
    #[cfg(not(feature = "servo-engine"))]
    crate::bridge::ffi::notify_screenshot_taken(_id, &[], 0, 0);
}

pub fn evaluate_javascript(_id: i32, _request_id: u64, _script: &str) {
    #[cfg(feature = "servo-engine")]
    engine::evaluate_javascript(_id, _request_id, _script);
    #[cfg(not(feature = "servo-engine"))]
    crate::bridge::ffi::notify_javascript_result(_id, _request_id, false, "engine not available");
}

pub fn page_zoom(_id: i32) -> f32 {
    #[cfg(feature = "servo-engine")]
    return engine::page_zoom(_id);
    #[allow(unreachable_code)]
    1.0
}

// These are used only when servo-engine is on (bridge.rs conditionally re-exports them).
// They must still compile without the feature; the engine module is absent so we
// delegate to servo_controller for the no-op path.

#[cfg(feature = "servo-engine")]
pub use engine::{
    can_go_back, can_go_forward, close_tab, current_url, go_back, go_forward, load_url, loading,
    reload, status_text, title,
};

pub fn set_experimental_features_enabled(_enabled: bool) {
    #[cfg(feature = "servo-engine")]
    engine::set_experimental_features_enabled(_enabled);
}

pub fn experimental_features_enabled() -> bool {
    // The authoritative persisted value lives in Qt Settings on the C++ side.
    // This stub satisfies the CXX bridge; the real default is set in applySettings().
    true
}

pub fn list_site_data() -> String {
    #[cfg(feature = "servo-engine")]
    return engine::list_site_data();
    #[allow(unreachable_code)]
    String::new()
}

pub fn clear_site_data_for(_sites: &str) {
    #[cfg(feature = "servo-engine")]
    engine::clear_site_data_for(_sites);
}

pub fn clear_all_cookies() {
    #[cfg(feature = "servo-engine")]
    engine::clear_all_cookies();
}

pub fn clear_http_cache() {
    #[cfg(feature = "servo-engine")]
    engine::clear_http_cache();
}

pub fn cookies_for_url(_url: &str) -> String {
    #[cfg(feature = "servo-engine")]
    return engine::cookies_for_url(_url);
    #[allow(unreachable_code)]
    String::new()
}

pub fn media_session_action(_id: i32, _action: i32) {
    #[cfg(feature = "servo-engine")]
    engine::media_session_action(_id, _action);
}

pub fn set_console_capture_enabled(_enabled: bool) {
    #[cfg(feature = "servo-engine")]
    engine::set_console_capture_enabled(_enabled);
}
