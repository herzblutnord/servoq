# ServoQ Chrome Deviations from Ladybird Reference

ServoQ keeps Ladybird-style Qt chrome while using Servo as the web engine. Ladybird is used as the browser-shell behavior reference, but ServoQ does not link or run LibWeb/LibWebView.

## Current Final Status

| Area | Status | Main ServoQ Files | Ladybird Reference |
|------|--------|-------------------|--------------------|
| New tab / start page | Implemented. New tabs and startup use a chrome-side grey empty placeholder, keep `about:blank` internal, leave the location field empty/focused, and create no web navigation or history entry until the user navigates. | `cpp/BrowserWindow.cpp`, `cpp/Tab.cpp`, `cpp/WebContentView.cpp`, `cpp/WebViewURL.cpp` | `UI/Qt/BrowserWindow.cpp`, `UI/Qt/Application.cpp`, `Libraries/LibWebView/Settings.cpp` (`URL::about_newtab()`) |
| Search engines | Implemented with Ladybird's built-in catalog and `%s` query templates. Custom engines persist in settings and require a unique name plus `%s` template. DuckDuckGo remains default. | `cpp/Settings.*`, `cpp/BrowserWindow.cpp`, `cpp/WebViewURL.cpp` | `Libraries/LibWebView/SearchEngine.cpp`, `SearchEngine.h`, `Settings.cpp` |
| History/location autocomplete | Implemented as local-history autocomplete for URL, title, and host substring matches. Remote suggestions are intentionally not implemented. | `cpp/LocationEdit.*`, `cpp/HistoryStore.*` | `UI/Qt/LocationEdit.cpp`, `UI/Qt/Autocomplete.*`, `Libraries/LibWebView/HistoryStore.cpp` |
| Bookmarks and folders | Implemented. Root bookmarks and folders now share one mixed persisted root order, matching Ladybird's root item semantics. Folder drag does not open the menu unless the mouse is released without dragging. | `cpp/BookmarkStore.*`, `cpp/BookmarksBar.*` | `Libraries/LibWebView/BookmarkStore.*`, `UI/Qt/BookmarksBar.cpp` |
| Bookmark favicons | Implemented. Favicon base64 PNGs persist on bookmark entries and the bookmarks bar rebuilds on exact URL favicon updates. | `cpp/BookmarkStore.*`, `cpp/BookmarksBar.cpp`, `cpp/WebContentView.cpp` | `Libraries/LibWebView/BookmarkStore::update_favicon()` |
| Content blocking network path | Implemented with `adblock` crate. ServoQ maps Servo `WebResourceRequest.destination`, `referrer_url`, and main-frame status to adblock request types; reload and exact-host allowlist are implemented. | `src/blocklist.rs`, `src/servo_engine.rs`, `src/bridge.rs`, `cpp/Settings.*`, `cpp/BrowserWindow.cpp`, `cpp/WebContentView.cpp` | `Libraries/LibWeb/Loader/ContentBlocker.h`, `ContentBlocker.cpp` |
| Cosmetic content blocking | Not implemented. Servo exposes global user stylesheets, but not a practical per-page/per-host cosmetic stylesheet path equivalent to Ladybird's dynamic `ContentBlocker` APIs. | `src/servo_engine.rs`, `src/blocklist.rs` | `Libraries/LibWeb/Loader/ContentBlocker.cpp`, `servo-0.2.0/user_content_manager.rs`, `servo-embedder-traits-0.2.0/user_contents.rs` |
| Scriptlets | Not implemented. Servo exposes `UserScript`, but ServoQ does not have a safe uBlock-compatible scriptlet resource/execution model. | `src/servo_engine.rs` | Ladybird `LibWeb::ContentBlocker` Rust FFI and adblock resources |
| DevTools | Not implemented in this pass by request. | `src/servo_engine.rs`, `cpp/BrowserWindow.cpp` | `servo-0.2.0/servo_delegate.rs` |

## Fixed Historical Issues

| Issue | Current State |
|-------|---------------|
| CJK/HarfBuzz crash | Fixed by hiding bundled static HarfBuzz symbols with `-Wl,--exclude-libs,ALL`; system FreeType now resolves `hb_*` to system HarfBuzz instead of ServoQ's bundled HarfBuzz. |
| Resize correctness | Fixed. Qt logical size times DPR is sent as Servo physical size, DPR is tracked separately, and active/inactive tab resize handling avoids stale shared-context state. |
| Wayland hardware GL selection | Fixed. `LIBGL_ALWAYS_SOFTWARE` is cleared before Wayland EGL display initialization so Mesa selects the hardware driver when available. |
| Wayland second-tab freeze | Fixed before this pass. ServoQ uses one shared active embedded Wayland `QWindow`/container and reuses the existing `WindowRenderingContext` instead of creating one native Wayland renderer per tab. |
| Wayland tab-switch freeze | Fixed. Root cause: hiding the shared `createWindowContainer` on tab switch unmaps the embedded `wl_surface`. After remapping on the next tab show, the first `eglSwapBuffers()` call (from `present_wayland_webview`) blocks waiting for a Wayland compositor frame callback that may not arrive promptly for a freshly-remapped subsurface, freezing the Qt main thread indefinitely. Fix: (1) the container is created once with the `QStackedWidget` as its stable parent so `setParent()` is never called; (2) on Wayland-tab hide the container is moved off-screen (`move(-parentWidth, 0)`) rather than hidden, keeping the `wl_surface` mapped and EGL frame callbacks flowing; (3) the incoming tab's `showEvent` repositions it (Wayland tab) or truly hides it (non-Wayland/empty tab). |
| Bookmark favicon reset/globe regression | Fixed before this pass. Favicon callbacks are keyed by ServoQ tab ID and bookmark favicon updates target exact bookmarked URLs. |
| Bookmark DnD polish | Fixed. The bar shows a visible vertical insertion marker, folders can be dragged, and the root order now works across mixed bookmark/folder items. |
| Dynamic filter-list parser stale note | Removed. ServoQ uses the `adblock` crate; dynamic parsing is no longer blocked by lack of a parser crate. |
| Location bar left gap stale note | Removed. The toolbar/location layout has the Ladybird-style 32px side margins. |

## Current UI / Feature Parity

| Feature | ServoQ Current Implementation | Ladybird Implementation | Difference | Should Change? | Action |
|---------|-------------------------------|--------------------------|------------|----------------|--------|
| New tab/start page | Chrome-side grey empty placeholder; no web navigation, no data URL, no history entry, and the location field is empty/focused. | Settings default is `URL::about_newtab()`; new-tab action loads it, hides URL, focuses editor. | ServoQ intentionally does not load a Servo-rendered `about:newtab` page yet. | Later only if a real Servo custom protocol/internal page is worthwhile. | Simpler chrome-side placeholder avoids white flash and long data-URL window titles. |
| Search engines | Ladybird built-in catalog plus custom persisted templates. | `LibWebView::SearchEngine { name, query_url }`, `%s` replacement with percent-encoding. | Remote suggestions/settings web UI not ported. | No for this pass. | Implemented catalog/custom templates. |
| Favicons | Tab favicon and bookmark favicon persistence by exact URL. | Bookmark store stores base64 favicon on bookmark item. | Servo favicon source differs from LibWebView. | No. | Servo-adapted. |
| Tab context menu | Implemented in ServoQ custom tab bar. | Qt chrome menu actions. | Some action labels/availability may differ. | Later polish only. | Good enough. |
| Page/link/image/media context menus | Servo delegate context menu mapped to Qt menu actions. | LibWebView embedder controls/actions. | Media-specific coverage depends on Servo context data. | Later if Servo exposes richer data. | Servo-adapted. |
| Cursor changes | Servo cursor callbacks mapped to Qt cursors. | LibWebView cursor updates. | Engine-specific callback data. | No. | Good enough. |
| Back/forward history menus | Per-tab Servo history list shown on button context menus. | LibWebView navigation history model. | ServoQ uses URLs only. | Later title/favicon polish. | Good enough. |
| Global history menu | Persistent local JSON history, most recent first. | LibWebView history store with richer autocomplete ranking/storage. | ServoQ is simpler but persistent. | Partially improved. | Autocomplete added. |
| Bookmark drag/drop reorder | Mixed root order for bookmarks/folders, persisted JSON. | `BookmarkStore::root_items()` mixed vector plus `move_item()`. | ServoQ supports one folder level. | No for current UI. | Design-portable implementation mirrored. |
| Bookmark folder behavior | Root folders have menus and can be reordered among bookmarks. | Folder items in mixed tree. | ServoQ folder nesting remains one level. | Later only if nested folders are needed. | Good enough. |
| Fullscreen | Servo callback toggles BrowserWindow fullscreen. | LibWebView/Qt fullscreen handling. | Engine callback source differs. | No. | Servo-adapted. |
| `window.open()` / popup-to-new-tab | Servo new WebView request opens a tab for existing ID. | LibWebView creates new tab/view. | ServoQ must preserve Servo-created WebView IDs. | No. | Servo-adapted. |
| Console logging | Servo console callback logs tab ID and level. | LibWebView console message plumbing. | No console UI. | Later if desired. | Good enough. |
| Notifications | Servo notification callback shows desktop notification. | LibWebView/Application notification handling. | Minimal UI. | Later permission UI. | Good enough. |
| Network content blocking | `adblock` crate with bundled plus custom list, reload action, exact-host allowlist, Servo destination mapping. | `LibWeb::ContentBlocker::is_filtered(url, source_url, ResourceType)`. | ServoQ intercepts through Servo delegate, not LibWeb loader. | No. | Servo-adapted. |
| Cosmetic content blocking | Documented unsupported. | `cosmetic_style_sheet_for_url()`, `has_generic_cosmetic_selectors_for_url()`, `has_cosmetic_rules()`. | ServoQ lacks practical per-site dynamic CSS generation/injection parity. | Later if a scoped user-style API or safe reload model is added. | Not faked. |
| Filter-list management/update UI | Custom list path and reload action implemented; no network updater. | Ladybird settings configure list paths loaded by WebContent. | No downloader/updater. | Later. | Reload implemented. |
| Per-site content-blocking allowlist | Exact-host allowlist in `QSettings`. | LibWebView/settings-side site controls. | Exact host only; no subdomain policy yet. | Later if needed. | Implemented minimal sane behavior. |
| Settings UI organization | Menu-based controls for search, content blocking, custom lists, reload, per-site action. | Ladybird has settings web UI and menus. | ServoQ does not have a full settings page. | Later. | Kept menu-based and explicit. |
| `DEVIATIONS.md` accuracy | Current document is authoritative; historical notes are separated. | N/A | N/A | Maintain during future passes. | Cleaned. |

## Current Remaining UI / Feature Gaps

### Implementable Later

| Feature | Why Not Done Yet | Main Files | Ladybird Reference | Suggested Approach |
|---------|------------------|------------|--------------------|--------------------|
| DevTools / remote debugging UI | Explicitly excluded from this pass by request. Servo has delegate hooks, but ServoQ needs safe settings, token/port display, and connection policy. | `src/servo_engine.rs`, `cpp/BrowserWindow.cpp`, `cpp/Settings.*` | Servo `servo-0.2.0/servo_delegate.rs` | Add an explicit opt-in setting/env gate, surface port/token, and wire Servo delegate callbacks. |
| Filter-list network updater | Reload from disk is implemented; safe async network download/update UI is separate work. | `src/blocklist.rs`, `cpp/BrowserWindow.cpp` | `Libraries/LibWebView/Settings.cpp`, `Application.cpp` content blocker list paths | Store downloaded lists under `QStandardPaths::AppDataLocation`, update asynchronously, keep bundled fallback. |
| Content blocking subdomain policy | Exact-host allowlist is implemented and documented. | `cpp/Settings.*`, `src/servo_engine.rs` | `LibWeb::ContentBlocker::source_url_for_matching()` | Add explicit UI semantics for exact host vs domain/subdomain and migrate settings carefully. |
| Nested bookmark folders | Current UI/store supports root folders with bookmark children only. | `cpp/BookmarkStore.*`, `cpp/BookmarksBar.*` | `Libraries/LibWebView/BookmarkStore::BookmarkItem::Folder` recursively stores children | Refactor ServoQ folder children to `BookmarkRootEntry`-like recursive items if nested folder UI is needed. |
| Search suggestions | Local history autocomplete is implemented; remote suggestions are not. | `cpp/LocationEdit.*`, `cpp/HistoryStore.*` | `UI/Qt/Autocomplete.*`, `Libraries/LibWebView/Autocomplete.*` | Add optional provider behind settings; do not send keystrokes remotely by default. |
| Downloads UI | No confirmed Servo 0.2.0 download-start/progress embedder API found. | `src/servo_engine.rs`, `cpp/BrowserWindow.cpp` | Ladybird download UI and `Application::ask_user_for_download_path()` | Re-check newer Servo APIs; if available, add download model and UI. |
| Per-URL zoom persistence | Page zoom exists but is per-tab/session. | `cpp/Settings.*`, `cpp/Tab.cpp`, `src/servo_engine.rs` | `Libraries/LibWebView/Settings.cpp` zoom-per-host storage | Persist host zoom in settings and apply on navigation. |
| TLS/security info UI | ServoQ has the Not Secure indicator, but no certificate dialog. | `cpp/LocationEdit.*`, `src/servo_engine.rs` | Ladybird security/certificate UI | Implement only if Servo exposes per-navigation TLS certificate/security metadata. |

### Truly Blocked by Servo 0.2.0

| Feature | Missing Servo API | Evidence | Ladybird Reference |
|---------|-------------------|----------|--------------------|
| Find-in-page actual search | No public `WebView::find()`, `find_next()`, or `find_previous()` API found. | Local searches in `servo-0.2.0` found no WebView find methods or find-result delegate callbacks. | `UI/Qt/FindInPageWidget.*`, LibWebView find plumbing |
| Find match count | No match count callback on `WebViewDelegate`. | `servo-0.2.0/webview_delegate.rs` exposes load/status/history/context/favicon/etc., not find result counts. | Ladybird find result label updates |
| Selected text prefill/copy from page | No public `WebView::selected_text()` or selection-change callback. | Local Servo search found no WebView selected-text API. | `UI/Qt/LocationEdit.cpp`, Find prefill behavior |
| Stop loading | No public `WebView::stop_loading()` or navigation cancellation method for the current load. | Local Servo WebView API search did not expose stop-loading. | Ladybird reload/stop action behavior |
| General tab audio indicator | No general audio-playing state callback. | Servo exposes media session events, which are page API state and not reliable tab-audio detection. | Ladybird tab audio indicator |
| Print | No public print API on Servo `WebView`. | Local Servo WebView API search found no print method/delegate. | Ladybird print action |
| Cosmetic filtering parity | Servo `UserContentManager::add_stylesheet()` exists, but `UserStyleSheet` only has source URL metadata and updates take effect only after reload; no exposed per-page URL-scoped stylesheet API equivalent to Ladybird's dynamic cosmetic CSS path. | `servo-0.2.0/user_content_manager.rs`, `servo-embedder-traits-0.2.0/user_contents.rs` | `LibWeb::ContentBlocker::cosmetic_style_sheet_for_url()` |
| uBlock scriptlets | Servo `UserScript` exists, but ServoQ lacks a safe rule parser/resource executor and per-site scriptlet lifecycle; implementing fake scriptlets would be unsafe/misleading. | `servo-embedder-traits-0.2.0/user_contents.rs`, `adblock-0.12.5` resources/scriptlet comments | Ladybird/Rust FFI content blocker design |

### Ladybird Differences Kept Intentionally

| Difference | Reason | Ladybird Reference |
|------------|--------|--------------------|
| ServoQ uses Servo, not LibWeb/LibWebView runtime objects. | Project goal requires Servo as the web engine. | Entire `Libraries/LibWebView/` runtime design |
| New tabs use a chrome-side grey empty placeholder, not Ladybird's engine-rendered `about:newtab`. | Avoids a generated data URL, prevents full-page white flash/title pollution, and keeps the tab intentionally empty until the user navigates. | `UI/Qt/BrowserWindow.cpp`, `Libraries/LibWebView/Settings.cpp` |
| Settings are menu/QSettings based, not Ladybird's settings web UI. | Smaller ServoQ UI and no LibWebView settings dependency. | `Libraries/LibWebView/WebUI/SettingsUI.*` |
| Bookmark store is ServoQ JSON. | Mirrors Ladybird data semantics where needed, avoids AK/LibWebView dependency. | `Libraries/LibWebView/BookmarkStore.*` |
| Content blocking uses Servo delegate interception. | Ladybird's loader/fetch hooks are LibWeb internals unavailable in ServoQ. | `Libraries/LibWeb/Loader/ContentBlocker.*` |
| One active native Wayland render surface. | Current ServoQ Wayland lifecycle is designed around one shared active `WindowRenderingContext`; multiple simultaneous native surfaces are not proven safe. | Servoshell active WebView/rendering context model |
| Custom CSS cursor images are not supported. | Servo 0.2.0 exposes only keyword cursor variants through `WebViewDelegate::notify_cursor_changed`; image data and hotspot coordinates are not present in the embedder API path. | CSS cursor image handling in engine internals |
| KDE/Wayland app icon requires desktop identity installation. | Qt `setWindowIcon()` sets the in-process window icon, but KDE/Wayland/portals resolve the taskbar app icon from the desktop id. Use `scripts/install-dev-desktop-file.sh` to install `servoq.desktop` and `servoq.png` for development runs. | System-installed Ladybird desktop integration |

## Historical Debugging Notes

These notes are retained for context only; they are not current gaps.

| Historical Topic | Root Cause / Resolution |
|------------------|-------------------------|
| HarfBuzz/CJK crash | Bundled HarfBuzz symbols from Servo were exported by the ServoQ ELF binary and interposed system FreeType's HarfBuzz calls. Hiding static archive symbols fixed the ABI mismatch. |
| Event-loop wakeups | Servo background work requires a Qt event-loop waker. `QtEventLoopWaker` posts a Qt event, and `BrowserWindow::eventFilter()` calls `tick_servo()`. |
| Resize bugs | Initial and active-tab resizes must send physical size plus DPR to Servo; inactive tabs cache pending size until activation. |
| Wayland LLVMpipe selection | Creating a software Surfman context set `LIBGL_ALWAYS_SOFTWARE=1`; clearing it before Wayland EGL initialization restored hardware GL selection. |
| Second-tab Wayland freeze | Multiple tab-created Wayland surfaces/contexts were unsafe. ServoQ now shares one active embedded Wayland window/container and reuses the current `WindowRenderingContext`. |
| Tab-switch freeze (returning to a previously loaded tab) | `setParent()` on the shared `createWindowContainer` widget during every tab switch reconfigures the Wayland subsurface. On Mesa/EGL this can leave `eglSwapBuffers` blocking indefinitely. Fixed by giving the container a single stable parent (the `QStackedWidget`) at creation time and never reparenting it; tab switches only call `updateContainerGeometry()` + `raise()`. |
| Hidden tab frame delivery | Hidden tabs can still emit Servo frame callbacks. Qt-side frame delivery ignores invisible views so hidden tabs cannot overwrite visible software frames. |
