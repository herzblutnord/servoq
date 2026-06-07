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

### Track A — HiDPI Rendering

| Fix | Reference |
|-----|-----------|
| `paintEvent`: sets `m_frame.setDevicePixelRatio(devicePixelRatioF())` then draws with `painter.drawImage(QPoint(0,0), m_frame)` — no manual `painter.scale()` | WebContentView.cpp:690,696 |
| `event()`: handles `QEvent::DevicePixelRatioChange` by calling `forwardResizeToEngine()` + `update()` so moving window to a different-DPI monitor reflows the engine frame | WebContentView.cpp:712-714 |

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

---

## BLOCKED

Features that cannot be ported because the required infrastructure is missing.

| Feature | Blocker |
|---------|---------|
| Autocomplete with search engine integration | `LibWebView::Autocomplete`, `WebView::Application::settings().search_engine()` |
| `focusInEvent` pre-fills search query from selected text | servo 0.2.0 `WebView::selected_text()` does not exist; no selection-change callback in `WebViewDelegate` |
| Print (Ctrl+P) | Not implemented in Servo bridge |
| DevTools / Inspect panel | No DevTools integration |
| Color scheme / Contrast / Motion menus | `LibWebView::Application` accessibility menus |
| New window action | Multiple windows not yet supported in ServoQ |
| Tab audio indicator button | servo 0.2.0 has no general audio-playing signal; `WebViewDelegate::notify_media_session_event(MediaSessionEvent)` only fires for pages using the Media Session API — `PlaybackStateChange(MediaSessionPlaybackState)` variants are `Playing`/`Paused`/`None_`, not a reliable tab-audio indicator |
| `update_result_label` with real match counts | servo 0.2.0 `WebView` has no `find()`, `find_next()`, `find_previous()` methods; `WebViewDelegate` has no `notify_find_result` / `notify_find_match_count` callback |
| `focusInEvent` select-in-page text → find bar | servo 0.2.0 `WebView` has no `selected_text()` method; `WebViewDelegate` has no selection-change callback |
