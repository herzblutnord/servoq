# ServoQ Feature Milestones

Re-sorting of typical browser features by implementation effort,
grouped by related work. Status was determined by auditing the codebase
(June 2026). Legend:

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
| Vertical / horizontal tabs | ✅ | incl. collapse + hover-expand modes |
| Tab close / middle-click close / reorder | ✅ | hardened against the mid-interaction-close bug |
| Browser-chrome keyboard shortcuts | ✅ | Ctrl+L/T/W/Shift+T, Ctrl+Tab/PgUp/PgDn, Ctrl+1-9, zoom, find |
| Browser-chrome context menus | ✅ | tab bar, bookmarks; web-content menu via Servo |
| Web-content context menu | ✅ | `show_context_menu_sync` (modal QMenu pattern) |
| Window.open / popup tabs | ✅ | `request_create_new` → `request_open_tab_for_id` |
| Focus handling | ✅ | |
| Mouse/keyboard input + scroll forwarding | ✅ | incl. Ctrl+C/X/V → EditingAction, Ctrl+A fix |
| Cursor changes | ✅ | |
| Page zoom | ✅ | Ctrl+± / Ctrl+0, reset chip in location bar |
| Fullscreen | ✅ | |
| Theme propagation to pages + chrome theme | ✅ | palette-driven chrome, `notify_theme_change` |
| Navigation policy + resource interception | ✅ | content blocking + blocklists + per-site allowlist |
| Crash UI | ✅ | inline crash page in the view |
| Animation/event-loop driving | ✅ | wake-event architecture; see DEVIATIONS.md |
| Window management UI (single window) | ✅ | geometry/maximized persistence (multi-window is L, see M5) |
| Session restore | ✅ | opt-in URL-level open tabs + active tab restore |

## Partially implemented

| Feature | Status | Gap | Picked up in |
|---|---|---|---|
| Clipboard support | ◐ | Ctrl+C/X/V forwarded as EditingActions; no `ClipboardDelegate` so JS/async clipboard and Servo-initiated copy don't reach the system clipboard | M3 |
| Notifications | ◐ | tray popup shown unconditionally; no permission prompt / per-origin persistence | M3 |
| Console messages | ◐ | stderr behind `SERVOQ_DEBUG`; no debug-page panel | M4 |
| Settings page | ◐ | menu-based settings only; no dedicated page | M4 |
| History page/search | ◐ | menu + autocomplete only; no full searchable view | M4 |
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
| 1.2 | `<select>` dropdown | ✅ | Ladybird `WebContentView.cpp` select dropdown; Servo `SelectElement` |
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

## Milestone 3 — Engine bridges (small Servo-API features, S–M each)

| # | Feature | Status | Notes |
|---|---|---|---|
| 3.1 | Clipboard delegate | ◐ | bridge Servo `ClipboardDelegate` ↔ QClipboard |
| 3.2 | HTTP auth prompt | ✗ | `request_authentication`; servoshell dialog.rs reference |
| 3.3 | Permission prompts + per-origin persistence | ✗ | `request_permission`; gates Notifications properly |
| 3.4 | Pinch zoom | ✗ | `adjust_pinch_zoom` from touchpad gesture events |
| 3.5 | Window move/resize requests + screen geometry | ✗ | `request_move_to`/`request_resize_to`/`screen_geometry` |
| 3.6 | Screenshots | ✗ | `take_screenshot`; menu action + Ctrl+Shift+S |
| 3.7 | JS evaluation (debug tooling) | ✗ | `evaluate_javascript`; feeds servoq://debug later |

## Milestone 4 — Browsing-data UIs & internal pages (M each)

| # | Feature | Status | Notes |
|---|---|---|---|
| 4.1 | History page with search | ◐ | native Qt view, deletion, search |
| 4.2 | Bookmark manager + import/export | ✗ | add/edit/remove/search + Netscape-HTML import/export |
| 4.3 | Settings page | ◐ | consolidate menu settings into a page |
| 4.4 | Internal pages scheme (`servoq://`) + debug page | ✗ | shell-state debug page; console-message panel |
| 4.5 | Site data / cookies clearing | ✗ | `SiteDataManager` UI |
| 4.6 | HTTP cache clearing | ✗ | `NetworkManager::clear_cache` |
| 4.7 | External PDF handoff | ✗ | MIME/extension detect via `load_web_resource`, open externally |
| 4.8 | Qt chrome accessibility pass | ✗ | accessible names, tab order, focus rings |

## Milestone 5 — Large / architectural (L, needs design first)

| # | Feature | Status | Notes |
|---|---|---|---|
| 5.1 | Download panel + downloads | ✗ | Servo 0.2 has no download API; needs `load_web_resource`-based design |
| 5.2 | User scripts/styles management | ✗ | `UserContentManager` is already wired; needs UI + persistence |
| 5.3 | Profiles | ✗ | separate config/cache dirs; Servo storage paths |
| 5.4 | Private windows | ✗ | blocked on Servo-side storage isolation |
| 5.5 | Multi-window | ✗ | conflicts with single shared Wayland surface (DEVIATIONS.md); needs per-window surface rework |
| 5.6 | Media session integration | ✗ | MPRIS + `notify_media_session_event` |
| 5.7 | Accessibility tree bridge (AccessKit) | ✗ | hardest item in the table |
| 5.8 | DevTools connection prompt | ✗ | with devtools server toggle |
| 5.9 | Local sync architecture | ✗ | out of scope until storage stabilizes |
