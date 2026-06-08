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
    use std::rc::Rc;
    use std::sync::{
        atomic::{AtomicBool, Ordering},
        Once,
    };
    use std::time::{Duration, Instant};

    use dpi::PhysicalSize;
    use euclid::{Box2D, Point2D, Scale};
    use glow::HasContext;
    use servo::{Code, EventLoopWaker, Key, KeyState, Location, Modifiers, NamedKey};
    use servo::protocol_handler::ProtocolRegistry;
    use servo::{
        DeviceIndependentPixel, DevicePixel, InputEvent, KeyboardEvent as ServoKeyboardEvent,
        LoadStatus, MouseButton, MouseButtonAction, MouseButtonEvent, MouseMoveEvent,
        NavigationRequest, Opts, Preferences, RenderingContext, Servo, ServoBuilder,
        SoftwareRenderingContext, Theme, UrlRequest, WebResourceLoad, WebResourceResponse, WebView,
        WebRenderDebugOption, WebViewBuilder, WebViewDelegate, WebViewPoint, WheelDelta, WheelEvent,
        WindowRenderingContext,
    };
    use servo::{ConsoleLogLevel, ContextMenuAction, ContextMenuItem, Cursor, PixelFormat};
    use servo::{CreateNewWebViewRequest, EmbedderControl};
    use servo::UserContentManager;
    use raw_window_handle::{
        DisplayHandle, RawDisplayHandle, RawWindowHandle, WaylandDisplayHandle,
        WaylandWindowHandle, WindowHandle,
    };
    use url::Url;

    // ---- per-tab state stored in our engine registry ---------

    fn debug_enabled() -> bool {
        std::env::var_os("SERVOQ_DEBUG").is_some()
    }

    fn perf_enabled() -> bool {
        std::env::var_os("SERVOQ_PERF").is_some()
    }

    static SHUTTING_DOWN: AtomicBool = AtomicBool::new(false);
    static GL_INFO_LOGGED: AtomicBool = AtomicBool::new(false);

    type EGLDisplay = *mut c_void;

    #[link(name = "EGL")]
    extern "C" {
        fn eglGetCurrentDisplay() -> EGLDisplay;
        fn eglQueryString(display: EGLDisplay, name: i32) -> *const c_char;
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

    fn servo_preferences() -> Preferences {
        let mut preferences = Preferences::default();
        preferences.viewport_meta_enabled = true;
        preferences.dom_indexeddb_enabled = true;
        preferences.dom_fontface_enabled = true;
        preferences.layout_variable_fonts_enabled = true;
        preferences
    }

    fn build_servo() -> Servo {
        log_embedder_setup_once();
        let servo = ServoBuilder::default()
            .opts(Opts::default())
            .preferences(servo_preferences())
            .protocol_registry(ProtocolRegistry::default())
            .event_loop_waker(Box::new(QtEventLoopWaker))
            .build();
        servo.setup_logging();
        servo
    }

    fn create_engine_state(rendering_context: EngineRenderingContext) -> EngineState {
        let servo = build_servo();
        let user_content_manager = Rc::new(UserContentManager::new(&servo));
        EngineState {
            servo,
            user_content_manager,
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
        physical_size: PhysicalSize<u32>,
        hidpi_scale_factor: Scale<f32, DeviceIndependentPixel, DevicePixel>,
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
        tabs: HashMap<i32, TabEntry>,
        rendering_context: EngineRenderingContext,
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
                debug_log("ignored_frame_hidden_webview", self.tab_id);
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
                debug_log_detail("deliver_frame", self.tab_id, format!("{w}x{h}"));
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
            debug_log_detail("load_status", self.tab_id, format!("{status:?}"));
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
                    let url = url_for_start.unwrap_or_default();
                    crate::bridge::ffi::notify_load_started(self.tab_id, &url);
                }
                LoadStatus::Complete => {
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

        // [ladybird: WebContentView crash signal] — Servo crashed; notify the Qt side so it can
        // show an error page. Matches the reference notify_crashed() contract.
        fn notify_crashed(&self, _webview: WebView, reason: String, _backtrace: Option<String>) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                return;
            }
            if !tab_exists(self.tab_id) {
                return;
            }
            eprintln!("Servo WebView crashed: {reason}");
            crate::bridge::ffi::notify_webview_crashed(self.tab_id, &reason);
        }

        fn request_navigation(&self, _webview: WebView, navigation_request: NavigationRequest) {
            if SHUTTING_DOWN.load(Ordering::Acquire) {
                navigation_request.deny();
                return;
            }
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
                if std::env::var_os("SERVOQ_DEBUG").is_some() {
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
            debug_log_detail("notify_cursor_changed", self.tab_id, format!("{cursor:?}"));
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
            let level_str = match level {
                ConsoleLogLevel::Log => "LOG",
                ConsoleLogLevel::Debug => "DEBUG",
                ConsoleLogLevel::Info => "INFO",
                ConsoleLogLevel::Warn => "WARN",
                ConsoleLogLevel::Error => "ERROR",
                ConsoleLogLevel::Trace => "TRACE",
            };
            eprintln!("[servoq console][tab={}][{}] {}", self.tab_id, level_str, message);
        }

        fn request_create_new(&self, _parent_webview: WebView, request: CreateNewWebViewRequest) {
            if SHUTTING_DOWN.load(Ordering::Acquire) { return; }
            let new_id = crate::servo_controller::create_tab();
            let (rc, size, scale) = ENGINE.with(|s| {
                let s = s.borrow();
                let e = match s.as_ref() {
                    Some(e) => e,
                    None => return (None, PhysicalSize::new(800, 600), Scale::new(1.0f32)),
                };
                let (size, scale) = e.tabs.get(&self.tab_id)
                    .map(|t| (t.physical_size, t.hidpi_scale_factor))
                    .unwrap_or((PhysicalSize::new(800, 600), Scale::new(1.0)));
                (Some(e.rendering_context.as_rendering_context()), size, scale)
            });
            let Some(rc) = rc else { return; };
            let delegate: Rc<dyn WebViewDelegate> = Rc::new(ServoDelegate {
                tab_id: new_id,
                rendering_context: ENGINE.with(|s| s.borrow().as_ref().map(|e| e.rendering_context.clone()).unwrap()),
                animating: Cell::new(false),
                initial_resize_done: Cell::new(false),
            });
            let webview = request.builder(rc)
                .hidpi_scale_factor(scale)
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
                        physical_size: size,
                        hidpi_scale_factor: scale,
                    });
                }
            });
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
                    let selected = crate::bridge::ffi::show_context_menu_sync(self.tab_id, &items_str);
                    if let Some(action) = context_menu_action_from_id(selected) {
                        menu.select(action);
                    } else {
                        menu.dismiss();
                    }
                }
                _ => {}
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
        ENGINE.with(|s| {
            // Drop WebViews, Servo, and WindowRenderingContext deterministically.
            // In Wayland mode this must happen while Qt still owns a valid QWindow
            // and wl_surface; otherwise Surfman/EGL can destroy a surface whose
            // Wayland proxy has already been torn down by Qt.
            *s.borrow_mut() = None;
        });
        SPINNING.with(|s| s.set(false));
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
    // [ladybird: WebContentView crash signal, B2 fix]
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
            return;
        }

        let started = Instant::now();
        debug_log_detail("wayland_present_enter", id, &url);
        let make_current_started = Instant::now();
        if let Err(error) = context.make_current() {
            eprintln!("[servoq] Wayland window renderer present failed: make_current: {error:?}");
            return;
        }
        let make_current_time = make_current_started.elapsed();
        let paint_started = Instant::now();
        webview.paint();
        let paint_time = paint_started.elapsed();
        let swap_started = Instant::now();
        context.present();
        let swap_time = swap_started.elapsed();
        record_wayland_present(
            started.elapsed(),
            make_current_time,
            paint_time,
            swap_time,
            size,
            &url,
        );
        debug_log_detail("wayland_present_leave", id, format!("url={url} swap_ms={:.2}", swap_time.as_secs_f64() * 1000.0));
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
            wv.notify_input_event(event);
        }
    }

    pub fn forward_mouse_button(id: i32, action: i32, button: i32, x: f32, y: f32) {
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
        if let Some(wv) = clone_webview(id) {
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
            wv.notify_input_event(event);
        }
    }

    // key_char: Unicode code point from Qt event.text()[0], 0 for non-printable.
    // qt_key:   Qt::Key enum value.
    // mods:     Qt::KeyboardModifiers flags.
    pub fn forward_key(id: i32, down: bool, key_char: u32, qt_key: i32, mods: u32) {
        let state = if down { KeyState::Down } else { KeyState::Up };
        let key = qt_key_to_key(key_char, qt_key);
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
        if let Some(wv) = clone_webview(id) {
            wv.notify_input_event(event);
        }
    }

    pub fn forward_focus(id: i32, focused: bool) {
        if let Some(wv) = clone_webview(id) {
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

    // [ladybird: BrowserWindow.cpp:1372-1374] mirrors zoom_in/zoom_out/reset_zoom on view()
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

pub fn forward_mouse_move(_id: i32, _x: f32, _y: f32) {
    #[cfg(feature = "servo-engine")]
    engine::forward_mouse_move(_id, _x, _y);
}

pub fn forward_mouse_button(_id: i32, _action: i32, _button: i32, _x: f32, _y: f32) {
    #[cfg(feature = "servo-engine")]
    engine::forward_mouse_button(_id, _action, _button, _x, _y);
}

pub fn forward_wheel(_id: i32, _dx: f64, _dy: f64, _x: f32, _y: f32) {
    #[cfg(feature = "servo-engine")]
    engine::forward_wheel(_id, _dx, _dy, _x, _y);
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
