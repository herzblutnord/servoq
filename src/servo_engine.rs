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
    use std::collections::HashMap;
    use std::rc::Rc;
    use std::sync::Once;

    use dpi::PhysicalSize;
    use euclid::{Box2D, Point2D, Scale};
    use servo::{Code, Key, KeyState, Location, Modifiers, NamedKey};
    use servo::{
        DeviceIndependentPixel, DevicePixel, InputEvent, KeyboardEvent as ServoKeyboardEvent,
        LoadStatus, MouseButton, MouseButtonAction, MouseButtonEvent, MouseMoveEvent,
        RenderingContext, Servo, ServoBuilder, SoftwareRenderingContext, WebView, WebViewBuilder,
        WebViewDelegate, WebViewPoint, WheelDelta, WheelEvent,
    };
    use url::Url;

    // ---- per-tab state stored in our engine registry ---------

    fn debug_enabled() -> bool {
        std::env::var_os("SERVOQ_DEBUG").is_some()
    }

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

    fn log_ignored_closed_callback(event: &str, id: i32) {
        debug_log_detail("ignored_callback_closed_webview", id, event);
    }

    fn log_embedder_setup_once() {
        static LOG_ONCE: Once = Once::new();
        if !debug_enabled() {
            return;
        }
        LOG_ONCE.call_once(|| {
            eprintln!("SERVOQ_DEBUG servo_builder opts=default preferences=default protocol_registry=default rendering_context=SoftwareRenderingContext event_loop_waker=ServoDefaultEventLoopWaker");
            match std::env::current_exe() {
                Ok(path) => eprintln!("SERVOQ_DEBUG current_exe={}", path.display()),
                Err(error) => eprintln!("SERVOQ_DEBUG current_exe_error={error}"),
            }
            for key in [
                "FONTCONFIG_FILE",
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

    struct TabEntry {
        webview: WebView,
        // Cached values updated by delegate callbacks
        current_url: String,
        title: String,
        loading: bool,
        status_text: String,
    }

    struct EngineState {
        servo: Servo,
        tabs: HashMap<i32, TabEntry>,
    }

    // ---- delegate ------------------------------------------

    struct ServoDelegate {
        tab_id: i32,
        rendering_context: Rc<SoftwareRenderingContext>,
        animating: Cell<bool>,
    }

    impl WebViewDelegate for ServoDelegate {
        // notify_new_frame_ready: paint() into context, read pixels, push to C++.
        // CRITICAL: paint() is called here (not before); present() is NOT called
        // before read_to_image so the back buffer is preserved.
        fn notify_new_frame_ready(&self, webview: WebView) {
            debug_log("notify_new_frame_ready", self.tab_id);
            if !tab_exists(self.tab_id) {
                debug_log("ignored_frame_closed_webview", self.tab_id);
                return;
            }
            webview.paint();
            let size = self.rendering_context.size();
            let w = size.width;
            let h = size.height;
            if w == 0 || h == 0 {
                return;
            }
            let rect: Box2D<i32, DevicePixel> =
                Box2D::new(Point2D::origin(), Point2D::new(w as i32, h as i32));
            if let Some(image) = self.rendering_context.read_to_image(rect) {
                debug_log_detail("deliver_frame", self.tab_id, format!("{w}x{h}"));
                crate::bridge::ffi::deliver_frame(
                    self.tab_id,
                    image.as_raw().as_slice(),
                    w as i32,
                    h as i32,
                );
            }
        }

        fn notify_url_changed(&self, _webview: WebView, url: Url) {
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

        fn notify_load_status_changed(&self, _webview: WebView, status: LoadStatus) {
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
                }
                _ => {}
            }
        }

        fn notify_animating_changed(&self, _webview: WebView, animating: bool) {
            if !tab_exists(self.tab_id) {
                log_ignored_closed_callback("notify_animating_changed", self.tab_id);
                return;
            }
            debug_log_detail("animating", self.tab_id, animating);
            self.animating.set(animating);
        }
    }

    // ---- thread-local engine state -------------------------

    thread_local! {
        static ENGINE: RefCell<Option<EngineState>> = const { RefCell::new(None) };
    }

    // Clone the Servo handle out of the RefCell so the borrow is dropped before
    // spin_event_loop() runs (which fires delegate callbacks that borrow ENGINE).
    fn clone_servo() -> Option<Servo> {
        ENGINE.with(|s| s.borrow().as_ref().map(|e| e.servo.clone()))
    }

    fn clone_webview(id: i32) -> Option<WebView> {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()?
                .tabs
                .get(&id)
                .map(|t| t.webview.clone())
        })
    }

    // ---- public functions -----------------------------------

    pub fn create_webview(id: i32, url_str: &str, w: i32, h: i32, scale: f32) {
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
            let engine = state.get_or_insert_with(|| EngineState {
                servo: {
                    log_embedder_setup_once();
                    ServoBuilder::default().build()
                },
                tabs: HashMap::new(),
            });

            if let Some(tab) = engine.tabs.get_mut(&id) {
                debug_log("create_webview_existing", id);
                tab.webview.resize(PhysicalSize::new(w, h));
                tab.webview.set_hidpi_scale_factor(
                    Scale::<f32, DeviceIndependentPixel, DevicePixel>::new(scale),
                );
                return;
            }

            let rendering_context = Rc::new(
                SoftwareRenderingContext::new(PhysicalSize::new(w, h))
                    .expect("SoftwareRenderingContext::new failed"),
            );

            let delegate: Rc<dyn WebViewDelegate> = Rc::new(ServoDelegate {
                tab_id: id,
                rendering_context: rendering_context.clone(),
                animating: Cell::new(false),
            });

            let rc_ctx: Rc<dyn RenderingContext> = rendering_context.clone();
            let webview = WebViewBuilder::new(&engine.servo, rc_ctx)
                .url(url.clone())
                .hidpi_scale_factor(Scale::<f32, DeviceIndependentPixel, DevicePixel>::new(
                    scale,
                ))
                .delegate(delegate)
                .build();

            engine.tabs.insert(
                id,
                TabEntry {
                    webview,
                    current_url: url.to_string(),
                    title: "New Tab".to_string(),
                    loading: false,
                    status_text: String::new(),
                },
            );
        });
    }

    pub fn close_webview(id: i32) {
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

    // spin_event_loop() is called after dropping the ENGINE borrow (clone_servo()).
    // This allows delegate callbacks fired inside spin_event_loop to borrow ENGINE.
    pub fn tick_webview(_id: i32) {
        if let Some(servo) = clone_servo() {
            servo.spin_event_loop();
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
            wv.load(url);
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

    // [ladybird: BrowserWindow.cpp:1372-1374] mirrors zoom_in/zoom_out/reset_zoom on view()
    // Servo 0.2.0: WebView::set_page_zoom(f32) — clamped to [0.1, 10.0] internally
    pub fn set_page_zoom(id: i32, zoom: f32) {
        if let Some(wv) = clone_webview(id) {
            wv.set_page_zoom(zoom);
        }
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
                    t.webview.resize(size);
                    t.webview.set_hidpi_scale_factor(scale_factor);
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

pub fn create_webview(_id: i32, _url: &str, _w: i32, _h: i32, _scale: f32) {
    #[cfg(feature = "servo-engine")]
    engine::create_webview(_id, _url, _w, _h, _scale);
}

pub fn close_webview(_id: i32) {
    #[cfg(feature = "servo-engine")]
    engine::close_webview(_id);
}

pub fn tick_webview(_id: i32) {
    #[cfg(feature = "servo-engine")]
    engine::tick_webview(_id);
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
