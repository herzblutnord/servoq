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

    struct TabEntry {
        webview: WebView,
        rendering_context: Rc<SoftwareRenderingContext>,
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
                crate::bridge::ffi::deliver_frame(
                    self.tab_id,
                    image.as_raw().as_slice(),
                    w as i32,
                    h as i32,
                );
            }
        }

        fn notify_url_changed(&self, _webview: WebView, url: Url) {
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
            let title_str = title.as_deref().unwrap_or("New Tab");
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
            let text = status.as_deref().unwrap_or("");
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

        let rendering_context = Rc::new(
            SoftwareRenderingContext::new(PhysicalSize::new(w, h))
                .expect("SoftwareRenderingContext::new failed"),
        );

        let delegate: Rc<dyn WebViewDelegate> = Rc::new(ServoDelegate {
            tab_id: id,
            rendering_context: rendering_context.clone(),
            animating: Cell::new(false),
        });

        let url = Url::parse(url_str).unwrap_or_else(|_| Url::parse("about:blank").unwrap());

        ENGINE.with(|state| {
            let mut state = state.borrow_mut();
            let engine = state.get_or_insert_with(|| EngineState {
                servo: ServoBuilder::default().build(),
                tabs: HashMap::new(),
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
                    rendering_context,
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
                e.tabs.remove(&id);
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
        if let Some(wv) = clone_webview(id) {
            wv.load(url);
        }
    }

    pub fn go_back(id: i32) {
        if let Some(wv) = clone_webview(id) {
            wv.go_back(1);
        }
    }

    pub fn go_forward(id: i32) {
        if let Some(wv) = clone_webview(id) {
            wv.go_forward(1);
        }
    }

    pub fn reload(id: i32) {
        if let Some(wv) = clone_webview(id) {
            wv.reload();
        }
    }

    pub fn close_tab(id: i32) {
        close_webview(id);
        crate::servo_controller::close_tab(id);
    }

    pub fn can_go_back(id: i32) -> bool {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.webview.can_go_back())
                .unwrap_or(false)
        })
    }

    pub fn can_go_forward(id: i32) -> bool {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.webview.can_go_forward())
                .unwrap_or(false)
        })
    }

    pub fn current_url(id: i32) -> String {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.current_url.clone())
                .unwrap_or_else(|| "about:blank".to_string())
        })
    }

    pub fn title(id: i32) -> String {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.title.clone())
                .unwrap_or_else(|| "New Tab".to_string())
        })
    }

    pub fn loading(id: i32) -> bool {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.loading)
                .unwrap_or(false)
        })
    }

    pub fn status_text(id: i32) -> String {
        ENGINE.with(|s| {
            s.borrow()
                .as_ref()
                .and_then(|e| e.tabs.get(&id))
                .map(|t| t.status_text.clone())
                .unwrap_or_default()
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

    // Matches Ladybird WebContentView::update_viewport_size() (vendor line 760-766):
    // physical pixel dimensions are passed pre-scaled from WebContentView::resizeEvent.
    pub fn forward_resize(id: i32, w: i32, h: i32, _scale: f32) {
        let size = PhysicalSize::new(w.max(1) as u32, h.max(1) as u32);
        ENGINE.with(|s| {
            let mut s = s.borrow_mut();
            if let Some(e) = s.as_mut() {
                if let Some(t) = e.tabs.get_mut(&id) {
                    t.webview.resize(size);
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

// These are used only when servo-engine is on (bridge.rs conditionally re-exports them).
// They must still compile without the feature; the engine module is absent so we
// delegate to servo_controller for the no-op path.

#[cfg(feature = "servo-engine")]
pub use engine::{
    can_go_back, can_go_forward, close_tab, current_url, go_back, go_forward, load_url, loading,
    reload, status_text, title,
};
