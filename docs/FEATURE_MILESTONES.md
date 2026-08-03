# ServoQ Feature Milestones

Re-sorting of typical browser features by implementation effort,
grouped by related work. Status was determined by auditing the codebase
(August 2026). The Servo 0.4 release-specific audit is in
[`SERVO_0_4_INTEGRATION.md`](SERVO_0_4_INTEGRATION.md). Legend:

- ✅ **done** — implemented, no work planned
- ◐ **partial** — exists but with noted gaps
- ✗ **missing** — not implemented
- Effort: **S** (hours), **M** (a day-ish), **L** (multiple days / needs design)

## Already implemented (no milestone)

| Feature (from table) | Status | Notes |
|---|---|---|
| One webview per tab | ✅ | `create_webview*` / registry in `servo_engine.rs` |
| Tab rendering / repaint | ✅ | Wayland subsurface + software fallback; see DEVIATIONS.md §0 |
| Tab title / URL / loading / status updates | ✅ | full delegate wiring |
| Favicon updates | ✅ | Ladybird-style largest-icon selection (`cpp/Favicon.cpp`) |
| Back/forward + tab history mirror | ✅ | incl. long-press history menus on the buttons |
| Reload, load URL from address bar | ✅ | |
| Address bar grammar + search fallback | ✅ | `WebViewURL::sanitize_url` (localhost/IP/scheme/search) |
| Search engine settings | ✅ | menu-based, custom engines supported |
| Configurable homepage / new-tab URL | ✅ | settings menu, Home toolbar button, configurable blank/home/custom new tabs |
| Bookmark storage + bookmarks bar | ✅ | JSON store (Chromium-style), folders, bar with menus; icons from favicons.db |
| Persistent history database | ✅ | SQLite urls/visits + favicons.db (see docs/STORAGE.md); URL-bar autocomplete + History menu |
| Persistent cookies / website preferences | ✅ | Servo profile storage is wired to `AppDataLocation/servo-profile`; Servo owns cookie semantics and persists its public cookie jar, HSTS, auth cache, and web storage |
| Vertical / horizontal tabs | ✅ | incl. collapse + hover-expand modes |
| Tab close / middle-click close / reorder | ✅ | hardened against the mid-interaction-close bug |
| Browser-chrome keyboard shortcuts | ✅ | Ctrl+L/T/W/Shift+T, Ctrl+Tab/PgUp/PgDn, Ctrl+1-9, zoom, find |
| Browser-chrome context menus | ✅ | tab bar, bookmarks; web-content menu via Servo |
| Web-content context menu | ✅ | `show_context_menu_sync` (modal QMenu pattern) |
| Window.open / popup tabs | ✅ | `request_create_new` → `request_open_tab_for_id` |
| Focus handling | ✅ | |
| Mouse/keyboard/touch/pen input + scroll forwarding | ✅ | incl. typed Servo 0.4 touch events, Ctrl+C/X/V → EditingAction, Ctrl+A fix |
| Desktop file drag-and-drop | ✅ | dropped local files open in the current tab; additional files open in background tabs |
| Cursor changes | ✅ | |
| Page zoom | ✅ | Ctrl+± / Ctrl+0, reset chip in location bar |
| Fullscreen | ✅ | |
| Theme propagation to pages + chrome theme | ✅ | palette-driven chrome, `notify_theme_change` |
| Navigation policy + resource interception | ✅ | content blocking + blocklists + per-site allowlist |
| Crash UI | ✅ | inline crash page in the view |
| Animation/event-loop driving | ✅ | wake-event architecture; see DEVIATIONS.md |
| Window management UI (single window) | ✅ | geometry/maximized persistence (multi-window is L, see M5) |
| Session restore | ✅ | opt-in URL-level open tabs + active tab restore |
| Audio / video playback (`<audio>`/`<video>`) | ✅ | `servo/media-gstreamer` backend (system GStreamer), vendored + hardened: panic-proofed audio paths, native PipeWire output with pulse/alsa fallback, startup codec-capability warning (DEVIATIONS.md §0l). H.264/AAC + VP9/Opus verified. Software frame-upload path; GL zero-copy video is gated by init order (see DEVIATIONS.md §0j). True crash isolation (out-of-process media) is blocked by Servo's in-process model — see §0l |

## Partially implemented

| Feature | Status | Gap | Picked up in |
|---|---|---|---|
| Clipboard support | ✅ | resolved by M3.1: QClipboard-backed `ClipboardDelegate`; JS/async clipboard and Servo-initiated copy reach the system clipboard | — |
| Notifications | ✅ | resolved by M3.3: gated by the permission prompt + per-origin persistence | — |
| Console messages | ✅ | resolved by M4.4: servoq://debug console panel (capture-gated) + stderr behind `SERVOQ_DEBUG` | — |
| Settings page | ✅ | resolved by M4.3: servoq://settings consolidates the menu settings | — |
| History page/search | ✅ | resolved by M4.1: servoq://history searchable view with deletion | — |
| Private-window shell behavior | ◐→L | nothing yet; true private storage needs Servo-side isolation | M5 |

## Milestone 1 — Content dialogs & form controls  ✅ **implemented (June 2026)**

Web pages were broken without these: `<select>` did nothing visible,
`alert()`/`confirm()`/`prompt()` were silently auto-answered, file/color inputs
dead. All four ride the existing `show_embedder_control` → synchronous C++ FFI
pattern proven by the context menu, with Ladybird's Qt dialogs as the direct
reference (`UI/Qt/Tab.cpp`, `UI/Qt/WebContentView.cpp`) and servoshell's
`desktop/dialog.rs` as the Servo-API reference. Implemented in
`cpp/WebDialogs.cpp` + `servo_engine.rs` `show_embedder_control`.

| # | Feature | Status | Reference |
|---|---|---|---|
| 1.1 | Simple JS dialogs (`alert`/`confirm`/`prompt`) | ✅ | Ladybird `Tab.cpp` on_request_alert/confirm/prompt; Servo `SimpleDialog` |
| 1.2 | `<select>` dropdown | ✅ | single-select menu plus checkbox dialog for `<select multiple>`, optgroups, disabled options, and an empty selection |
| 1.3 | Color picker (`<input type=color>`) | ✅ | Ladybird `Tab.cpp` on_request_color_picker; Servo `ColorPicker` |
| 1.4 | File picker (`<input type=file>`) | ✅ | Ladybird `Tab.cpp` on_request_file_picker; Servo `FilePicker` |
| 1.5 | `window.close()` from content | ✅ | Servo `notify_closed` → deferred tab close |

## Milestone 2 — Tabs & session continuity  ✅ **implemented (June 2026)**

| # | Feature | Status | Notes |
|---|---|---|---|
| 2.1 | Session restore (open tabs + active tab on start) | ✅ | URL-level restore (session.json, see docs/STORAGE.md); opt-in setting like Chrome's "continue where you left off"; restored tabs show cached favicons |
| 2.2 | Recently-closed-tabs menu | ✅ | History-menu submenu with favicons, individual restore, Reopen All, Clear, Ctrl+Shift+T; persists across restarts (capped 25) |
| 2.3 | Configurable homepage / new-tab URL | ✅ | setting + home behavior |
| 2.4 | Pinned tabs | ✅ | pin/unpin via tab context menu; pinned group kept first, compact favicon-only rendering (horizontal), pin indicator (vertical expanded), no close button, middle-click protected, drag stays within group, bulk closes skip pinned, persists with session |
| 2.5 | Tab search popup | ✅ | Ctrl+Shift+A / View → Search Tabs…; Chrome-style filterable popup with per-row close |

## Milestone 3 — Engine bridges (small Servo-API features, S–M each)  ✅ **implemented (June 2026)**

| # | Feature | Status | Notes |
|---|---|---|---|
| 3.1 | Clipboard delegate | ✅ | `QtClipboardDelegate` in `servo_engine.rs` bridges Servo `ClipboardDelegate` ↔ QClipboard (one clipboard connection per process; replaces the arboard default) |
| 3.2 | HTTP auth prompt | ✅ | `request_authentication` → modal Qt sign-in dialog (`WebDialogs.cpp`); proxy wording for 407, plain-HTTP warning, Cancel continues without credentials |
| 3.3 | Permission prompts + per-origin persistence | ✅ | `request_permission` → Allow/Block/Not-Now prompt (`WebDialogs.cpp`); explicit answers persist per origin in `permissions.json` (`PermissionStore`), dismiss denies once (Chrome semantics); Settings → Clear Site Permissions. Enabled the permission-gated prefs `dom_geolocation_enabled`, `dom_wakelock_enabled`, `dom_credential_management_enabled` (WebRTC/media capture still need more than prompts) |
| 3.4 | Pinch zoom | ✅ | `Qt::ZoomNativeGesture` (both the embedded Wayland QWindow and the software-path widget) → `forward_pinch_zoom` → `adjust_pinch_zoom`; per-step factor `1.0 + value`, Servo clamps to [1, 10] |
| 3.5 | Window move/resize requests + screen geometry | ✅ | `screen_geometry` backs `window.screen.*` with real QScreen + frame geometry (device px); `request_move_to`/`request_resize_to` honored only for single-tab windows (Firefox/Chrome popup policy), deferred via QTimer, outer→client size conversion, clamped to the available screen |
| 3.6 | Screenshots | ✅ | File → Take Screenshot… (Ctrl+Shift+S) → async `take_screenshot` (waits for render-stable page) → save-as-PNG dialog defaulting to `~/Pictures/Screenshot <timestamp>.png` |
| 3.7 | JS evaluation (debug tooling) | ✅ | `evaluate_javascript` bridge with request-id routing + JSON serialization of `JSValue`; manual test surface: View → Evaluate JavaScript… (Ctrl+Shift+J, visible only with `SERVOQ_DEBUG`); feeds servoq://debug later (M4.4) |

## Milestone 4 — Browsing-data UIs & internal pages (M each)

| # | Feature | Status | Notes |
|---|---|---|---|
| 4.1 | History page with search | ✅ | `servoq://history`: native Qt list, date sections, favicons, search filter, per-row delete + clear-all (`InternalPageView`) |
| 4.2 | Bookmark manager + import/export | ✗ | add/edit/remove/search + Netscape-HTML import/export |
| 4.3 | Settings page | ✅ | `servoq://settings`: consolidates home/new-tab, appearance, search, privacy settings into a page; applied live via `BrowserWindow::onSettingsChangedFromPage` |
| 4.4 | Internal pages scheme (`servoq://`) + debug page | ✅ | `InternalPageView` hosted per-tab in a content `QStackedWidget`; `servoq://settings/history/downloads/debug`; debug page shows shell state + a console-message panel (capture-gated). Surface handling: see DEVIATIONS.md §0h |
| 4.5 | Site data / cookies clearing UI | ✅ | Settings → Privacy: "Manage site data" lists eTLD+1 sites from `SiteDataManager::site_data` with per-site/all removal; "Clear browsing data" clears history/cookies/cache |
| 4.6 | HTTP cache clearing | ✅ | wired via `NetworkManager::clear_cache` from the Clear browsing data dialog (servoq::clear_http_cache) |
| 4.7 | Inline PDF viewer | ✅ | `servoq://pdf` native Qt PDF view (`QPdfDocument`/`QPdfView`); local files plus obvious `.pdf` main-frame URLs from typed URLs, Open File, and clicked links; Ctrl+wheel zoom, Save As…, password prompt |
| 4.8 | Qt chrome accessibility pass | ✗ | accessible names, tab order, focus rings |

## Milestone 5 — Large / architectural (L, needs design first)

| # | Feature | Status | Notes |
|---|---|---|---|
| 5.1 | Download panel + downloads | ✗ | Servo 0.4 has no download API; needs `load_web_resource`-based design |
| 5.2 | User scripts/styles management | ◐ | user *scripts* load from `<AppData>/userscripts/*.js` in filename order (servoshell `--userscripts` equivalent, Servo 0.4.0), gated by Settings → Advanced → "Enable user scripts" (default off; applies to pages loaded afterwards); missing: in-browser script editor/manager and user *stylesheets* |
| 5.3 | Profiles | ✗ | separate config/cache dirs; Servo storage paths |
| 5.4 | Private windows | ✗ | blocked on Servo-side storage isolation |
| 5.5 | Multi-window | ✗ | conflicts with single shared Wayland surface (DEVIATIONS.md); needs per-window surface rework |
| 5.6 | Media session integration | ✅ | MPRIS via QtDBus (`MprisManager`): `notify_media_session_event` → `org.mpris.MediaPlayer2.servoq` metadata/playback state; desktop controls drive `media_session_action` → `WebView::notify_media_session_action_event` |
| 5.7 | Accessibility tree bridge (AccessKit) | ✗ | Servo's June work remains upstream WIP; a Qt accessibility-tree graft and action bridge are still required |
| 5.8 | DevTools connection prompt | ✅ | Settings → Advanced opt-in; loopback-only `127.0.0.1:7000`, restart required; every tokenless inspector connection prompts Allow/Deny |
| 5.9 | Local sync architecture | ✗ | out of scope until storage stabilizes |
