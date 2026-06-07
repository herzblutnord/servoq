# ServoQ Chrome Deviations from Ladybird Reference

All fixes cite `// [ladybird: File:line]`. Reference path: `vendor/reference-ladybird/UI/Qt/`.

---

## FIXED

Changes applied during the Phase 1–4 audit that bring ServoQ's chrome into closer fidelity with the Ladybird reference.

### Phase 1 — Vertical Tabs Toggle Button

| Fix | Reference |
|-----|-----------|
| Toggle-sidebar `QAction` always added to toolbar in `Tab::buildToolbar()` (was absent unless vertical tabs already enabled in Settings) | Tab.cpp:176,213-215 |
| `m_sidebar_toggle_spacer` `QSpacerItem` added after the toggle button; resizes to `ToolbarSidebarToggleNavigationGap=8` when vertical tabs enabled | Tab.cpp:214, 581-587 |
| `Tab::updateToggleVerticalTabsIcon()` updates icon from `Settings::vertical_tabs_expanded()` | Tab.cpp:794-797 |
| `Tab::setVerticalTabsEnabled()` resizes spacer and updates icon | Tab.cpp:581-587 |
| `BrowserWindow::toggleVerticalTabsExpanded()` added; iterates all tabs to update icon after collapse/expand | Tab.cpp:176 |

### Phase 2 — TabBar

| Fix | Reference |
|-----|-----------|
| `rebuildLayoutForHorizontalTabs()`: `VerticalTabsButtonProperty` set to `true` (not `false`) — enables custom new-tab-button painting in horizontal mode | TabBar.cpp:1659 |
| Drop indicator: pen width 3, alpha 220, `Qt::RoundCap`, correct endpoint clamping | TabBar.cpp:548-568 |
| `deferUpdateVerticalTabsHoverExpanded()` uses `QTimer::singleShot(0,...)` instead of immediate call | TabBar.cpp:1944-1949 |
| `updateChromeStyle()` applies stylesheet to `m_tab_bar_row`, `m_vertical_tab_bar_column`, `m_vertical_tabs_separator`, `m_vertical_tabs_resize_handle` individually (not to `this`) | TabBar.cpp:1904-1916 |

### Phase 2 — BrowserWindow Keyboard Shortcuts

| Fix | Reference |
|-----|-----------|
| `QKeySequence::FindPrevious` and `FindNext` window-level shortcuts | BrowserWindow.cpp:311-323 |
| "Open Next Tab" action: `Ctrl+PageDown`, `Ctrl+Tab` | BrowserWindow.cpp:334-344 |
| "Open Previous Tab" action: `Ctrl+PageUp`, `Ctrl+Shift+Tab` | BrowserWindow.cpp:346-356 |
| `Ctrl+1–8` switch to tab by index; `Ctrl+9` switch to last tab | BrowserWindow.cpp:447-461 |
| `openNextTab()` / `openPreviousTab()` implementations | BrowserWindow.cpp:1065-1084 |

### Phase 2 — Tab Keyboard Shortcuts

| Fix | Reference |
|-----|-----------|
| `m_back_action`: `QKeySequence::Back` shortcut | Menu.cpp:172 |
| `m_forward_action`: `QKeySequence::Forward` shortcut | Menu.cpp:177 |
| `m_reload_action`: `Ctrl+R`, `F5` shortcuts | Menu.cpp:182 |

### Phase 2 — LocationEdit

| Fix | Reference |
|-----|-----------|
| `LocationActionButton` custom paint: rounded hover/pressed background (no Qt stylesheet dependency) | LocationEdit.cpp:42-77 |
| Focus glow animation via `QGraphicsDropShadowEffect` + `QVariantAnimation` | LocationEdit.cpp:206-217 |
| Leading icon size 18×18 (was 16×16) | LocationEdit.cpp:221 |
| `focusInEvent`: deferred full-URL show on first mouse click; animated focus glow | LocationEdit.cpp:389-411 |
| `focusOutEvent`: animated focus glow fade-out | LocationEdit.cpp:413-441 |
| `mouseReleaseEvent`: show full URL and select-all on deferred left-click | LocationEdit.cpp:501-507 |
| `keyPressEvent`: Escape restores URL text and clears focus | LocationEdit.cpp:461-499 |
| `changeEvent` override instead of `event()` for palette change; also catches `ApplicationPaletteChange` and `ThemeChange` | LocationEdit.cpp:381-387 |

### Phase 2 — FindInPageWidget

| Fix | Reference |
|-----|-----------|
| `m_result_label->setStyleSheet("font-weight: bold;")` set in constructor | FindInPageWidget.cpp:83 |
| `m_result_label->setVisible(false)` initial state | FindInPageWidget.cpp:82 |
| `m_find_text->setFocusPolicy(Qt::StrongFocus)` | FindInPageWidget.cpp:36 |
| `event()` updates button icons on `PaletteChange` | FindInPageWidget.cpp:100-102 |

### Phase 4 — ChromeStyle Stylesheet Constants

| Fix | Reference |
|-----|-----------|
| `tab_widget_style_sheet`: tab hover uses `control_hover` (= `chrome_control_surface_hover`, factor 0.82/0.62) not `chrome_surface_hover` (factor 0.34/0.52) | ChromeStyle.cpp:747 |
| `tab_widget_style_sheet`: destructive close button hover colors (`#C42B1C` / white) | ChromeStyle.cpp:235-243,868-876 |
| `tab_widget_style_sheet`: distinct sidebar separator = `mix(chrome_background, chrome_border, 0.44/0.58)` vs strip separator = `chrome_border` | ChromeStyle.cpp:753-755 |
| `tab_widget_style_sheet`: sidebar hover separator = `mix(chrome_background, chrome_border, 0.64/0.76)` | ChromeStyle.cpp:755 |
| `tab_widget_style_sheet`: text color uses `chrome_button_text` (not `chrome_text`) | ChromeStyle.cpp:750 |
| `tab_widget_style_sheet`: window control button styles added | ChromeStyle.cpp:849-882 |
| `bookmarks_bar_style_sheet`: uses `control_hover`/`control_pressed`/`chrome_control_border`/`chrome_button_text` | ChromeStyle.cpp:614-648 |
| `find_in_page_style_sheet`: richer selectors — QLineEdit, pressed state, QCheckBox/QLabel styling, accent color for focus | ChromeStyle.cpp:650-706 |

### Feature 1 — Page Zoom

| Fix | Reference |
|-----|-----------|
| `Tab::zoomIn()`, `Tab::zoomOut()`, `Tab::resetZoom()` using Servo `set_page_zoom(f32)` / `page_zoom() -> f32` API | BrowserWindow.cpp:1372-1374 |
| `ZoomStep=0.1`, `ZoomMin=0.1`, `ZoomMax=10.0`; `round_zoom()` helper | BrowserWindow.cpp:1370 |
| `m_reset_zoom_action` `QAction` shows "110%" text in LocationEdit when zoom ≠ 1.0 | Tab.cpp:233 |
| `LocationEdit::setZoomAction()` + zoom indicator pill (`m_zoom_indicator_button`) | LocationEdit.cpp:238-249,353-367 |
| `updateZoomIndicator()`: hidden at 1.0, shows rounded percent otherwise | LocationEdit.cpp:679-701 |
| `chrome_surface_recessed()` color helper for zoom pill background | ChromeStyle.cpp:138-145 |
| `location_edit_style_sheet()` zoom pill and not-secure pill selectors | ChromeStyle.cpp:501-612 |
| View menu: "Zoom In" `Ctrl++/=`, "Zoom Out" `Ctrl+-`, "Reset Zoom" `Ctrl+0` | BrowserWindow.cpp:358-360 |
| `BrowserWindow::wheelEvent()`: `Ctrl+scroll` calls `zoomIn()`/`zoomOut()` | BrowserWindow.cpp:1365-1376 |
| Rust bridge: `set_page_zoom(id, zoom)` / `page_zoom(id) -> f32` added to `bridge.rs` / `servo_engine.rs` | src/bridge.rs, src/servo_engine.rs |

### Feature 2 — "Not Secure" HTTP Indicator

| Fix | Reference |
|-----|-----------|
| `m_leading_icon_button` switches to "Not secure" text pill with `notSecure` property for `http:` URLs | LocationEdit.cpp:594-677 |
| Leading icon hidden for `https:` and other non-HTTP schemes | LocationEdit.cpp:594-677 |
| `updateLocationIcon()` called on `setUrl()`, `focusInEvent()`, `focusOutEvent()` | LocationEdit.cpp:225-226,353-367 |
| `QToolButton#LadybirdLocationIcon[notSecure="true"]` stylesheet with pill background, red text | ChromeStyle.cpp:548-570 |

### Feature 3 — Bookmark Folder Management

| Fix | Reference |
|-----|-----------|
| `BookmarkStore`: JSON-based persistent store (`QSaveFile` + `QStandardPaths::AppDataLocation`) replacing `Settings` bookmark list | BookmarkStore.h, BookmarkStore.cpp |
| `BookmarksBar::rebuild()` renders root bookmarks and folders; folders use `QToolButton::InstantPopup` + `QMenu` | BookmarksBar.cpp:205-273 |
| Bookmark button sizing: `BookmarkButtonMaxWidth=150`, `BookmarkButtonIconSize=16`, `BookmarkButtonMinHeight=24`, `BookmarkButtonVerticalPadding=8`, `BookmarkButtonHorizontalPadding=7`, `BookmarkButtonIconTextSpacing=6`, `BookmarkButtonTextElisionPadding=2` | BookmarksBar.cpp:29-35 |
| `paint_bookmark_button()`: custom paint suppresses Qt's icon/text rendering, draws elided text and icon manually | BookmarksBar.cpp:130-160 |
| Right-click context menus: edit/delete bookmark, rename/delete folder, new folder on empty bar | BookmarksBar.cpp:345-393 |
| Middle-click on bookmark/folder-child opens URL in new tab | BookmarksBar.cpp:323-343 |
| "Add Bookmark" dialog (Ctrl+D) pre-filled with page title/URL; folder selector | BrowserWindow.cpp:360 |
| `Tab::refreshBookmarkIcon()` reads from `BookmarkStore` (not `Settings`) | Tab.cpp |
| Legacy `Settings` bookmark list migrated to `BookmarkStore` on first launch | BookmarksBar.cpp constructor |
| Qt6 MOC runs on `BookmarkStore.h` and `BookmarksBar.h` in `build.rs` | build.rs |

### Bug 0 — CJK/HarfBuzz SIGSEGV and wrong rendering (root cause identified and fixed)

| Fix | Reference |
|-----|-----------|
| **Original misdiagnosis 1**: custom `install_servoq_fontconfig()` wrote `~/.cache/servoq/fonts.conf` and set `FONTCONFIG_FILE` to exclude Nerd Fonts and "Noto Sans Symbols". This was based on an incorrect read of the coredump. Removed entirely. | servo_engine.rs (removed) |
| **Original misdiagnosis 2**: the SIGSEGV was attributed to the missing `EventLoopWaker` (P6). The waker was added and is correct, but it did not stop the crash. | — |
| **Root cause (confirmed from coredump)**: `servo-fonts-0.2.0` enables `harfbuzz-sys` with `features = ["bundled"]`, statically linking HarfBuzz 8.4.0 into the `servoq` binary. On Linux, statically linked symbols are exported by default from ELF executables. The dynamic linker resolves `libfreetype.so.6`'s `hb_*` symbol references to `servoq`'s HarfBuzz 8.4.0, even though FreeType was compiled against system `libharfbuzz.so.0` (14.2.1 on Arch). The `hb_face_t` struct layout changed between 8.4.0 and 14.2.1: FreeType writes the `reference_table` callback at the 14.2.1 offset but 8.4.0's `hb_face_reference_table` reads it from a different offset, producing a garbage function pointer. Calling the garbage pointer → SIGSEGV. Confirmed by: (a) `nm -D servoq` shows `hb_font_create T` (exported), (b) coredump frame #10 shows `hb_font_create` at `servoq + 0xad64724` called from inside `libfreetype.so.6`'s `FT_Load_Glyph`, (c) crash address `0x00007f1a00040000` is the garbage function pointer being called by `hb_face_reference_table`. | build.rs, coredump |
| **Fixed**: added `-Wl,--exclude-libs,ALL` linker flag (Linux only) in `build.rs` via `cargo:rustc-link-arg`. This marks all static-archive symbols as hidden (`STB_LOCAL`), removing them from the `.dynsym` table. `libfreetype.so.6` now resolves `hb_*` to system `libharfbuzz.so.0` (14.2.1) — the same version it was compiled against. Servo's own shaping continues to use the bundled 8.4.0 (resolved at compile time; the two HarfBuzz environments never share objects). After the fix: `nm -D servoq | grep hb_` produces no output. | build.rs |
| **Also fixed (P6)**: `QtEventLoopWaker` implements `servo::EventLoopWaker`; its `wake()` calls `QCoreApplication::postEvent(qApp, …)` (thread-safe) from Servo's background threads. `BrowserWindow::eventFilter()` intercepts the wake event and calls `servoq::tick_servo()`. `ServoBuilder::event_loop_waker(Box::new(QtEventLoopWaker))` installs it at Servo creation. | servo_engine.rs, BrowserWindow.cpp |
| Timer interval reduced from 16ms (60 Hz polling) to 200ms (5 Hz safety fallback). Primary event-loop spinning is now waker-driven, matching servoshell's `ControlFlow::Wait` + waker model. | WebContentView.cpp |

### Bug 2+3 Follow-up — Status Bar Double Display

| Fix | Reference |
|-----|-----------|
| Removed `statusBar()->showMessage(servoq::status_text(...))` from `BrowserWindow::updateCurrentTabState()`. Reference BrowserWindow has no statusBar link-hover calls; status text goes only to the in-view `m_hover_label` | BrowserWindow.cpp (reference: no statusBar usage) |

### Bug C — Hidden Tab Still Delivering Frames

| Fix | Reference |
|-----|-----------|
| `deliver_frame()` now checks `view->isVisible()` before calling `receiveFrame()`. Servo shares one event loop across all tabs; `set_throttled(true)` is advisory and doesn't immediately suppress `notify_new_frame_ready`. Without the guard, hidden tabs still deliver frames, wasting CPU and racing with the visible tab's render state | WebContentView.cpp |

### Bug 1 — Close Button Wrong Position on First Render

| Fix | Reference |
|-----|-----------|
| `TabBar::tabLayoutChange()` override added; calls `setVerticalScrollOffset()` + `updateTabButtonGeometry()` so close button geometry is refreshed whenever Qt re-lays out the tab bar (initial show, tab insert/remove) | TabBar.cpp:427-432 |

### Bug 2 — Remove Placeholder / Idle Text

| Fix | Reference |
|-----|-----------|
| `WebContentPlaceholder` widget removed from `WebContentView`; widget shows blank background until first Servo frame | Tab.cpp:158 — `m_view` added directly to layout, no placeholder |
| Crash display replaced with inline `paintEvent` text (no QWidget child) — `m_crashed` + `m_crash_reason` fields track state | WebContentView.cpp |
| Stale "Servo renderer placeholder is idle" `statusBar()->showMessage()` removed from `BrowserWindow` constructor | BrowserWindow.cpp:84 |

### Bug 3 — Status Bar Text at Top-Left of WebView

| Fix | Reference |
|-----|-----------|
| `Tab::updateHoverLabel()` implemented: positions `m_hover_label` at bottom-left via `move(0, height() - label->height())`; shifts to right side when mouse hovers over label | Tab.cpp:748-763 |
| Called from `on_link_hover()` after `setText()` | Tab.cpp:253-260 |
| `m_hover_label->setAutoFillBackground(true)` added in constructor | Tab.cpp:142 |

### Bug 4 — Stylesheet Missing .arg() Arguments

| Fix | Reference |
|-----|-----------|
| Removed `QString()` phantom for unused `%7` from `tab_widget_style_sheet()` `.arg()` chain. Qt's variadic `arg()` replaces the LOWEST `%N` per argument; `%7` absent from stylesheet meant `QString()` consumed `%8` and shifted all subsequent colors one position, leaving `pressed` with no placeholder | ChromeStyle.cpp:393-404 |

### Bug 5 — QUrl::userInfo() FullyDecoded Warning

| Fix | Reference |
|-----|-----------|
| `url.userInfo(QUrl::FullyDecoded)` → `url.userInfo(QUrl::PrettyDecoded)` in `WebViewURL.cpp`; Qt docs prohibit `FullyDecoded` in `userInfo()` | WebViewURL.cpp:190 |

### Bug 6 — Initial Viewport Sizing

| Fix | Reference |
|-----|-----------|
| `ServoDelegate::initial_resize_done: Cell<bool>` added; on first `notify_new_frame_ready` (compositor proven alive), re-sends stored `physical_size` + `hidpi_scale_factor` to ensure pre-spin resize is honoured | servo_engine.rs |
| G2: `viewport_meta_enabled = true` already set in `servo_preferences()` — verified, no change needed | servo_engine.rs:87 |
| G3: Lambda capture `[this, window]` → `[window]` in Tab.cpp BookmarksBar callback; `this` was unused | Tab.cpp:87 |

### Servo Embedding — Context Lifecycle and Paint Contract

| Fix | Reference |
|-----|-----------|
| `EngineState` now explicitly owns one shared `SoftwareRenderingContext` for all tabs instead of creating one context per WebView delegate | servoshell `ServoShellWindow::create_toplevel_webview()` shares `platform_window.rendering_context()` |
| `EngineState` declares `servo` before WebViews/rendering context so Rust drops Servo before the WebViews/context owners; comment cites Servo issue #36711 | servoshell `RunningAppState` drop-order comment |
| Active-tab resize now updates DPR and calls `webview.resize(PhysicalSize)` only; inactive-tab resize only stores pending physical size/DPR so the shared software context cannot be clobbered by a hidden WebView | Servo 0.2.0 `WebView::resize()` resizes the shared `RenderingContext`, updates renderer rect, and sends `ChangeViewportDetails`; servoshell `WebViewCollection::activate_webview()` active-WebView model |
| Activating a WebView reapplies its cached physical size and DPR through `webview.resize()` before `show()` / `focus()` | servoshell `WebViewCollection::activate_webview()` show/hide model |
| Hidden tabs are marked inactive and their frame callbacks are ignored before painting into the shared context, avoiding hidden WebViews overwriting the visible tab's software buffer | servoshell `WebViewCollection::activate_webview()` show/hide model |
| Software paint path calls `webview.paint()`, then reads pixels with `RenderingContext::read_to_image()`; it deliberately does not call `present()` because software `present()` swaps with `PreserveBuffer::No` | Servo 0.2.0 `RenderingContext` contract |
| Software frame delivery is coalesced before readback: if Qt already has an unpainted delivered frame, `notify_new_frame_ready` skips `webview.paint()` and `read_to_image()` for that callback | servoshell `notify_new_frame_ready()` only marks repaint-needed; actual present happens later on redraw |
| Servo wake events posted into Qt are coalesced with an atomic pending flag so background Servo threads cannot flood the Qt event queue with redundant `tick_servo()` calls | servoshell uses a winit event-loop proxy and `ControlFlow::Wait` rather than polling |
| Optional `SERVOQ_PERF=1` instrumentation logs Rust tick time, software frame readback/delivery bytes, skipped readbacks, Wayland frame-ready count, Wayland present count, make-current/paint/present timing, Qt wake coalescing, Qt embedded-window update/expose counts, and accidental software paint/drawImage counts | ServoQ diagnostic-only instrumentation; disabled by default |
| `SERVOQ_PERF=1` Wayland mode logs GL/EGL identity once after the context is current: EGL vendor/version/client APIs, GL vendor/renderer/version/GLSL version, GLES-vs-desktop-GL, surface size, DPR, and `software_gl=true/false`; it warns for llvmpipe/softpipe/software renderers | Hardware-vs-software GL diagnosis |
| Wayland mode GL/EGL identity is logged unconditionally once the context is current, not only when `SERVOQ_PERF=1`, so future hardware-vs-software GL regressions are visible on normal `SERVOQ_RENDERER=wayland-window` runs | `log_wayland_gl_info_once()` |
| **Wayland LLVMpipe root cause (confirmed)**: `SoftwareRenderingContext::new()` was still called during `init_servo()` even when the active runtime renderer was `SERVOQ_RENDERER=wayland-window`. Surfman's software adapter (`Adapter::Software::set_environment_variables()`) sets `LIBGL_ALWAYS_SOFTWARE=1` as a process-wide environment side effect. Later, when the Wayland window path called `Connection::from_display_handle()` / `WindowRenderingContext::new()`, Mesa read `LIBGL_ALWAYS_SOFTWARE=1` during `eglInitialize()` and selected LLVMpipe instead of AMD/radeonsi. Surfman's hardware adapter clears the variable later, but that happens after EGL initialization, too late to affect driver selection | servo_engine.rs, Surfman/Mesa EGL behavior |
| **Wayland LLVMpipe fix**: `create_wayland_rendering_context()` clears `LIBGL_ALWAYS_SOFTWARE` before `WindowRenderingContext::new()` and before `Connection::from_display_handle()` can trigger `eglInitialize()`. It logs when the variable was present/cleared and logs the Wayland `wl_display*` / `wl_surface*` addresses for diagnosis. Expected success output is `SERVOQ_GL ... gl_renderer="AMD Radeon Graphics (... radeonsi ...)" software_gl=false` | servo_engine.rs |
| Servo creation now matches servoshell more closely: `ServoBuilder::opts(Opts::default())`, `.preferences(...)`, `.protocol_registry(ProtocolRegistry::default())`, `.event_loop_waker(...)`, `build()`, then `servo.setup_logging()` | servoshell `desktop/app.rs` ServoBuilder sequence |
| `EngineState` owns a shared `UserContentManager`, and each `WebViewBuilder` receives `.user_content_manager(...)` like servoshell | servoshell `ServoShellWindow::create_toplevel_webview()` |
| `SERVOQ_WR_DEBUG=1` toggles Servo WebRender profiler debug and sampling profiler for new WebViews, for diagnosing where slow `webview.paint()` time is spent | servoshell WebRender debug actions |
| `WebContentView::resizeEvent()` clears the stale Qt frame, forwards physical size + DPR to Servo, immediately spins Servo once, and requests a Qt repaint; duplicate same-size/DPR resize events are ignored | servoshell pumps Servo immediately after window events and requests redraw through `notify_new_frame_ready` |
| `LoadStatus::Complete` forces one extra software paint/read so Qt receives a final post-load frame even when Servo does not emit another frame notification | Servo 0.2.0 `WebViewDelegate::notify_load_status_changed` behavior |
| Software blit now reuses `WebContentView::m_frame` storage and copies Rust frame bytes directly into it, reallocating only on size/format change instead of constructing `QImage(...).copy()` every frame | Qt `QImage::bits()` software blit optimization |
| Runtime renderer selection added: default/`SERVOQ_RENDERER=software` uses the existing software path; `SERVOQ_RENDERER=wayland-window` attempts an experimental Wayland-only native-window path and falls back to software when unavailable | ServoQ runtime selection; servoshell headed renderer parity attempt |
| Wayland window renderer creates an embedded `QWindow` with `QWidget::createWindowContainer()`, obtains `wl_display*` from `QNativeInterface::QWaylandApplication::display()`, obtains `wl_surface*` from private `QNativeInterface::Private::QWaylandWindow::surface()`, and passes both pointers to Rust | Qt 6 Wayland native interface |
| Rust constructs raw-window-handle 0.6 Wayland display/window handles and attempts `WindowRenderingContext::new(display_handle, window_handle, physical_size)` | servoshell `WindowRenderingContext::new()` path |
| Wayland window backend bypasses the software readback path: `notify_new_frame_ready` requests an embedded-window update, and update/expose calls present through `webview.paint()` + `WindowRenderingContext::present()` without `read_to_image()` or C++ frame transfer | servoshell redraw/present model |
| Wayland present requests are coalesced on the Qt side with `m_wayland_present_pending`, `m_wayland_present_in_progress`, and `m_wayland_dirty_after_present`: multiple Servo frame-ready callbacks before or during one `QWindow::UpdateRequest` produce at most one immediate `webview.paint()` + `present()`, with one follow-up update only if content became dirty during present | servoshell `set_needs_repaint()` + `request_redraw()` model |
| The 200ms safety tick timer is not started for the active Wayland window backend; Servo is driven by the event-loop waker and explicit resize/load/input paths instead of periodic polling | servoshell `ControlFlow::Wait` + event-loop waker model |
| Explicit shutdown path added: `BrowserWindow::closeEvent`, `QCoreApplication::aboutToQuit`, and `main.cpp` call `begin_servo_shutdown()`, which gates late Qt callbacks and calls Rust `shutdown_servo()` to drop WebViews/Servo/`WindowRenderingContext` while Qt still owns the embedded `QWindow`/`wl_surface` | Wayland/EGL/Surfman teardown ordering requirement |
| Software renderer remains the fallback and keeps the fixed resize model: Qt logical size × DPR is sent as physical size, DPR is passed separately through `set_hidpi_scale_factor()`, and `webview.resize(PhysicalSize)` drives Servo viewport updates | Servo 0.2.0 `WebView::resize()` contract |
| Wayland window renderer is intentionally Wayland-only. No X11/XCB/Xlib/XWayland path is implemented. If Qt is not running on Wayland, if native handles are unavailable, or if Servo's `WindowRenderingContext` creation fails, ServoQ prints a warning and keeps the software renderer. | Project platform constraint |

Wayland renderer hardware-GL sanity check:

```bash
SERVOQ_RENDERER=wayland-window cargo run --release --features servo-engine
SERVOQ_PERF=1 SERVOQ_RENDERER=wayland-window cargo run --release --features servo-engine
```

Expected GL line:

```text
SERVOQ_GL ... gl_renderer="AMD Radeon Graphics (... radeonsi ...)" software_gl=false
```

If `gl_renderer` ever regresses to `llvmpipe` / `software_gl=true`, first check for `LIBGL_ALWAYS_SOFTWARE=1` being set before `WindowRenderingContext::new()` / `eglInitialize()`. Do not start by optimizing QImage/readback or Servo paint scheduling.

Release profiling commands for unresolved Wayland `webview.paint()` slowness after hardware GL is confirmed:

```bash
cargo build --release --features servo-engine
SERVOQ_RENDERER=wayland-window perf record --call-graph dwarf -- target/release/servoq
perf report
SERVOQ_RENDERER=wayland-window perf top --call-graph dwarf -- target/release/servoq
```

### Track A — HiDPI Rendering

| Fix | Reference |
|-----|-----------|
| `WebContentView::startEngineIfNeeded()` passes physical pixels (`width() * devicePixelRatioF()`, `height() * devicePixelRatioF()`) and Qt DPR to Servo creation | WebContentView.cpp:761-765 |
| `ServoBuilder` now enables `Preferences::viewport_meta_enabled` and `Preferences::dom_indexeddb_enabled` before `build()` | Servo 0.2.0 `Preferences` API |
| Existing-webview creation and resize now call `webview.set_hidpi_scale_factor(Scale::new(dpr))` before `webview.resize(PhysicalSize)` and cache physical size for later repaint requests | Servo 0.2.0 `WebView` API |
| `set_page_zoom(id, zoom)` now forces a public-API repaint path by issuing same-size `webview.resize(cached_physical_size)` after `webview.set_page_zoom(zoom)` | Servo 0.2.0 `WebView` API |
| `paintEvent`: sets `m_frame.setDevicePixelRatio(devicePixelRatioF())` then draws with `painter.drawImage(QPoint(0,0), m_frame)` — no manual `painter.scale()` | WebContentView.cpp:690,696 |
| `event()`: handles `QEvent::DevicePixelRatioChange` through the same `forwardResizeToEngine()` path, so moving the window to a different-DPI monitor invalidates stale pixels, updates Servo viewport details, spins Servo, and repaints | WebContentView.cpp:712-714 |

### Track A — WebView Crash Handler

| Fix | Reference |
|-----|-----------|
| `ServoDelegate::notify_crashed()` implemented: forwards reason string to C++ via `bridge::ffi::notify_webview_crashed(tab_id, &reason)` | servo_engine.rs |
| `tick_webview` wraps `servo.spin_event_loop()` in `std::panic::catch_unwind`; on panic, extracts message and calls `notify_webview_crashed` | servo_engine.rs |
| C++ `notify_webview_crashed(tab_id, reason)` callback calls `view->receiveWebViewCrash(text)` on the owning `WebContentView` | servo_callbacks.h |
| `WebContentView::receiveWebViewCrash()`: stops tick timer, clears frame, shows placeholder with "⚠ Web content crashed: …" message | WebContentView.cpp |

### Track B — Open File (Ctrl+O)

| Fix | Reference |
|-----|-----------|
| `Open File…` action with `QKeySequence::Open` shortcut; uses `QFileDialog::getOpenFileName` + `QUrl::fromLocalFile().toString()` to navigate current tab | BrowserWindow.cpp |

### Track B — Reopen Last Closed Tab (Ctrl+Shift+T)

| Fix | Reference |
|-----|-----------|
| `m_closed_tabs: QVector<QPair<QString,QString>>` (url, title) stack, capped at 10 entries | BrowserWindow.h |
| `closeTab()` pushes URL+title before removal; enables `m_reopen_tab_action` | BrowserWindow.cpp |
| `m_reopen_tab_action` (Ctrl+Shift+T) pops the stack and calls `createNewTab(url)` | BrowserWindow.cpp |

### Track B — Servo 0.2.0 API Wiring

| Fix | Reference |
|-----|-----------|
| Servo preferences now enable `dom_fontface_enabled` and `layout_variable_fonts_enabled`; local Servo 0.2.0 exposes no CJK-specific preference fields, so system font discovery remains Servo/fontconfig default | Servo 0.2.0 `Preferences` API |
| Theme changes forward from `WebContentView` to `webview.notify_theme_change(Theme::Dark/Light)` on `QStyleHints::colorSchemeChanged`, `PaletteChange`, `ApplicationPaletteChange`, and `ThemeChange`; initial theme is sent after WebView creation | WebContentView.cpp:100-105,990-994 |
| Active-tab changes call Servo `show()`, `set_throttled(false)`, and `focus()`; inactive tabs call `blur()`, `set_throttled(true)`, and `hide()` | BrowserWindow.cpp:696-702; WebContentView.cpp:774-783 |
| `WebContentView::focusInEvent` / `focusOutEvent` continue forwarding to Servo `focus()` / `blur()` | WebContentView.cpp:646-653 |

### Track C — Content Blocking

| Fix | Reference |
|-----|-----------|
| `load_url()` now uses Servo `WebView::load_request(UrlRequest::new(url))`, preserving the new request API path | Servo 0.2.0 `WebView::load_request` / `UrlRequest` API |
| `ServoDelegate::request_navigation()` denies blocked main-frame / iframe navigations; `ServoDelegate::load_web_resource()` intercepts blocked subresources with an empty response | Servo 0.2.0 `WebViewDelegate::request_navigation` / `load_web_resource` API |
| Static content-blocking list: `doubleclick.net`, `googlesyndication.com`, `ads.twitter.com`, `facebook.net/en_US/fbevents.js` | ContentBlocker.cpp:29-58,233-236 |
| `WebContentView` emits `request_blocked(QString const& url_string)` via Rust-to-C++ `notify_request_blocked(tab_id, url)` callback | ServoQ bridge |
| Settings menu contains `QCheckBox "Block trackers and ads"` and persists `content_blocking/enabled`, default on | Settings.cpp:17,27-34 |

### Feature 4 — Permanent Storage Audit

| Verified | Reference |
|----------|-----------|
| `QSettings::IniFormat, QSettings::UserScope` ✅ | Settings.cpp |
| Window position (`window/last_position`) persisted on close ✅ | Settings.cpp |
| Window size (`window/last_size`) persisted on close ✅ | Settings.cpp |
| Window maximized state (`window/is_maximized`) ✅ | Settings.cpp |
| Vertical tabs enabled/expanded/hover-expand persisted ✅ | Settings.cpp |
| Menu bar visibility persisted ✅ | Settings.cpp |
| Bookmarks bar visibility persisted ✅ | Settings.cpp |
| Zoom is **not** persisted per-URL — matches reference behavior ✅ | Tab.cpp (zoom is ephemeral per-tab) |

---

## KNOWN NECESSARY

Intentional deviations required by the Servo embedding architecture or platform differences.

| Deviation | Reason |
|-----------|--------|
| `ServoQ::` namespace throughout (not `Ladybird::`) | Different project |
| No `LibWebView::Application` singleton | ServoQ uses `servoq::` Rust bridge functions directly |
| Custom `WebContentView` wrapping Servo's software renderer | Servo doesn't implement Ladybird's `WebContentView` interface |
| `QTabBar`-based tabs with `QTabBar::tab` CSS (vs fully custom-drawn tab buttons) | Reference renders tabs entirely in `paintEvent`; ServoQ's TabBar still uses native QTabBar hit-testing and geometry |
| `Settings` backed by `QSettings` (not `LibWebView::Settings`) | No AK/LibWebView dependency |
| `QSettings` org name is `"ServoQ"` (not reverse-domain `"org.servoq"`) | Changing it would silently discard existing user settings; accepted as-is |
| Hamburger menu visible only when menu bar is hidden | Same pattern; menu bar availability per-platform matches reference |
| No `Autocomplete` dropdown in LocationEdit | Depends on `LibWebView::Autocomplete` search-engine integration |
| `TOOLBAR_SIDEBAR_TOGGLE_NAVIGATION_GAP` = 8 | Exact match with Tab.cpp:94 |
| `expanded_sidebar_width` = 232, `collapsed_sidebar_width` = 52, `toolbar_height` = 42 | Exact match with ChromeLayout.h |
| Bookmark drag-reorder not implemented | `LibWebView::BookmarkStore` reorder API not available; would require custom drag-drop in `BookmarksBar` |
| Bookmark store uses local JSON file (not `LibWebView::BookmarkStore`) | No LibWebView dependency; behavior-equivalent: `QSaveFile` atomic writes, `QStandardPaths::AppDataLocation` |
| "Not secure" indicator uses leading icon only; no cert chain dialog on click | Servo bridge does not expose TLS certificate details |
| `m_reset_zoom_action` text sourced from `Tab` (not `view().reset_zoom_action()`) | ServoQ has no `LibWebView` action infrastructure; behavior-equivalent |
| Servo 0.2.0 local crate has no public `WebViewBuilder::size(...)`; initial physical viewport size is supplied through `SoftwareRenderingContext::new(PhysicalSize::new(...))`, which `WebView::viewport_details()` reads at construction | Servo API mismatch between provided docs and local crate |
| Servo 0.2.0 local crate keeps `WebView::set_animating()` `pub(crate)`; ServoQ uses public same-size `webview.resize()` after page zoom to mark the renderer for repaint | Servo API visibility mismatch between provided docs and local crate |
| Servo 0.2.0 local crate keeps `WebView::set_focused()` `pub(crate)`; ServoQ uses public `webview.focus()` / `webview.blur()` for focus routing | Servo API visibility mismatch between provided docs and local crate |

---

## BLOCKED

Features that cannot be ported because the required infrastructure is missing.

| Feature | Blocker |
|---------|---------|
| Autocomplete with search engine integration | `LibWebView::Autocomplete`, `WebView::Application::settings().search_engine()` |
| `focusInEvent` pre-fills search query from selected text | servo 0.2.0 `WebView::selected_text()` does not exist; no selection-change callback in `WebViewDelegate` |
| Print (Ctrl+P) | Not implemented in Servo bridge |
| DevTools / Inspect panel | No DevTools integration |
| Dynamic filter list (uBlock-style EasyList parsing) | Would require a filter parser crate and rule-list update/storage plumbing |
| Color scheme / Contrast / Motion menus | `LibWebView::Application` accessibility menus |
| New window action | Multiple windows not yet supported in ServoQ |
| Tab audio indicator button | servo 0.2.0 has no general audio-playing signal; `WebViewDelegate::notify_media_session_event(MediaSessionEvent)` only fires for pages using the Media Session API — `PlaybackStateChange(MediaSessionPlaybackState)` variants are `Playing`/`Paused`/`None_`, not a reliable tab-audio indicator |
| `update_result_label` with real match counts | servo 0.2.0 `WebView` has no `find()`, `find_next()`, `find_previous()` methods; `WebViewDelegate` has no `notify_find_result` / `notify_find_match_count` callback |
| `focusInEvent` select-in-page text → find bar | servo 0.2.0 `WebView` has no `selected_text()` method; `WebViewDelegate` has no selection-change callback |
