# Ladybird Qt UI Sync

## Upstream Reference

`vendor/reference-ladybird/UI/Qt` is the browser chrome source of truth. Do not edit `vendor/`.

ServoQ replaces Ladybird's LibWeb/WebView backend with Servo. Chrome files should otherwise track Ladybird Qt UI structure, constants, painting, object names, and behavior as directly as the current dependency set permits.

## File Mapping

| Ladybird file | Local file | Status | Reason for adaptation |
| --- | --- | --- | --- |
| `Application.cpp` / `Application.h` | none; Rust `main` plus `cpp/main.cpp` | temporary | ServoQ does not yet have Ladybird `Application`, settings, history store, process supervision, or app-global WebView services. |
| `BrowserWindow.cpp` / `BrowserWindow.h` | `cpp/BrowserWindow.cpp` / `cpp/BrowserWindow.h` | adapted | Uses `servoq::create_tab`, `servoq::load_url` controller IDs, and a reduced menu/action set. Ladybird tab creation and close flow inspected at `BrowserWindow.cpp:535-539` and `BrowserWindow.cpp:715-724`. |
| `Tab.cpp` / `Tab.h` | `cpp/Tab.cpp` / `cpp/Tab.h` | adapted | Toolbar structure follows Ladybird object names, toolbar height, location editor placement, loading timer, and URL callbacks, but engine calls go through Servo bridge. References: `Tab.cpp:121-154`, `Tab.cpp:225-235`, `Tab.cpp:253-287`, `Tab.cpp:649-667`. |
| `TabBar.cpp` / `TabBar.h` | `cpp/TabBar.cpp` / `cpp/TabBar.h` | near copy | Constants, custom painting, hover animation, tab sizing, close buttons, vertical tabs, and layout are ported with Qt-only replacements for AK helpers and without Ladybird menu/window-control classes. Active tab painting now follows `TabBar.cpp:467-479`; separators follow `TabBar.cpp:486-494`; close button class follows `TabBar.cpp:2015-2034`. |
| `LocationEdit.cpp` / `LocationEdit.h` | `cpp/LocationEdit.cpp` / `cpp/LocationEdit.h` | adapted | Object names, minimum height, display/full URL focus behavior, and trailing geometry follow Ladybird. Autocomplete, `SettingsObserver`, LibURL ownership, focus glow, public suffix highlighting, zoom pill, and search settings are not linked. References: `LocationEdit.cpp:190-197`, `LocationEdit.cpp:286-300`, `LocationEdit.cpp:391-438`, `LocationEdit.cpp:520-552`, `LocationEdit.cpp:584-591`. |
| `BookmarksBar.cpp` / `BookmarksBar.h` | `cpp/BookmarksBar.cpp` / `cpp/BookmarksBar.h` | adapted | Object name, toolbar role, button icon size, and max width follow Ladybird constants at `BookmarksBar.cpp:29-35`; backing store is local `Settings` instead of `LibWebView::BookmarkStore`. |
| `FindInPageWidget.cpp` / `FindInPageWidget.h` | `cpp/FindInPageWidget.cpp` / `cpp/FindInPageWidget.h` | adapted | Layout and controls follow Ladybird setup at `FindInPageWidget.cpp:18-80`; real Servo find-in-page is out of scope, so callbacks only toggle UI/bridge state. |
| `WebContentView.cpp` / `WebContentView.h` | `cpp/WebContentView.cpp` / `cpp/WebContentView.h` | Servo-specific | Public/event shape follows `WebContentView.h:47-78`; implementation forwards input/resizes to Servo and blits frames. LibWeb `ViewImplementation`, drag/drop, IME, GL/Metal rendering, select dropdown, and node picker are not ported. |
| `ChromeStyle.cpp` / `ChromeStyle.h` | `cpp/ChromeStyle.cpp` / `cpp/ChromeStyle.h` | adapted | Color functions and object-name selectors are ported from `ChromeStyle.cpp:18-248`, toolbar selectors from `ChromeStyle.cpp:314-413`, location selectors from `ChromeStyle.cpp:501-557`, tab selectors from `ChromeStyle.cpp:742-885`. Servo placeholder styling is local. |
| `ChromeLayout.h` | `cpp/ChromeLayout.h` | exact copy | Constants and layout policy match `ChromeLayout.h:13-52`, with namespace changed to `ServoQ` and `AK/Platform.h` removed for the local build. |
| `Icon.cpp` / `Icon.h` | `cpp/Icon.cpp` / `cpp/Icon.h` | near copy | Chrome icon drawing is ported; local namespace and resource paths differ. |
| `Settings.cpp` / `Settings.h` | `cpp/Settings.cpp` / `cpp/Settings.h` | project-specific | Local Qt settings cover window size, tab layout, and simple bookmarks. Ladybird settings/search provider/history APIs are not present. |
| `Menu.cpp` / `Menu.h` | none | temporary | Ladybird menu helpers depend on app/global action infrastructure not present in ServoQ. |
| `Autocomplete.cpp` / `Autocomplete.h` | none | temporary | Depends on `LibWebView::Autocomplete`, history, and search settings. |
| `StringUtils.cpp` / `StringUtils.h` | none | missing-dependency | Converts AK strings and uses Ladybird formatting helpers. ServoQ currently uses Qt strings directly. |
| `WindowControlButton.cpp` / `WindowControlButton.h` | custom code in `cpp/TabBar.cpp` | adapted | Local uses plain `QToolButton`; Ladybird uses a separate class and pressed-outside behavior. |
| `ladybird.qrc` | `cpp/resources.qrc` | adapted | Local resource file embeds copied `ladybird.png` under `/Icons`. Reference lines: `ladybird.qrc:1-5`. |
| `main.cpp` | `cpp/main.cpp` | adapted | Local entrypoint is built through Cargo/CXX and sets up ServoQ bridge. Ladybird app initialization is not available. |
| none | `cpp/WebViewURL.cpp` / `cpp/WebViewURL.h` | Servo-specific | Qt/Servo adapter for Ladybird URL/search behavior where `LibWebView/URL.h`, `LibURL`, search settings, and public suffix data are unavailable. |
| none | `cpp/WebContentPlaceholder.cpp` / `cpp/WebContentPlaceholder.h` | temporary | Default no-feature content surface. This is intentionally not in Ladybird chrome. |
| none | `cpp/servo_callbacks.h` / `src/servo_engine.rs` | Servo-specific | Servo engine backend and CXX callback boundary. |

## Deviations

| Local file/function | Difference from Ladybird | Reason class |
| --- | --- | --- |
| `cpp/BrowserWindow.cpp::createMenus` | Reduced File/Edit/View/Help menus; no Ladybird history, downloads, settings, devtools, profile, or recently closed actions. | missing-dependency-required |
| `cpp/BrowserWindow.cpp::createNewTab` | Uses `servoq::create_tab()` and integer controller IDs instead of `WebContentClient`, `URL::URL`, and `Application`. | Servo-required |
| `cpp/BrowserWindow.cpp::closeTab` | Deletes the tab locally and calls Servo close path; does not record Ladybird history store entries inspected at `BrowserWindow.cpp:715-724`. | missing-dependency-required |
| `cpp/Tab.cpp::Tab` | Uses `QLabel` hover label instead of Ladybird `HyperlinkLabel`; no tab context menu, clone tab, print, screenshot, devtools, or window-control toolbar support. | missing-dependency-required |
| `cpp/Tab.cpp::navigate` | Calls Servo bridge and queues initial URL in `WebContentView`; Ladybird calls `view().load(url)` at `Tab.cpp:649-651`. | Servo-required |
| `cpp/Tab.cpp::location_edit_return_pressed` | Sanitization happens through `WebViewURL` at submit time because local `LocationEdit` cannot own `URL::URL` or `LibWebView::sanitize_url`. Ladybird uses `LocationEdit::url()` after `LocationEdit` sanitizes on return at `LocationEdit.cpp:286-300` and `Tab.cpp:659-667`. | missing-dependency-required |
| `cpp/LocationEdit.cpp` | No autocomplete popup, inline completion, public suffix highlighting, focus glow, `SettingsObserver`, search-provider prompt, not-secure pill, or zoom pill. | missing-dependency-required |
| `cpp/LocationEdit.cpp::updateButtonPositions` | Button edge constants match Ladybird `LocationEdit.cpp:190-197` and `520-552`; leading text margin is simplified to the icon width because local code has no hidden URL/not-secure state machine. | missing-dependency-required |
| `cpp/WebViewURL.cpp::sanitize_url` | Uses Qt `QUrl` and a small local suffix allowlist instead of `LibWebView::sanitize_url`, `LibURL`, configured search provider, and full public suffix data. | missing-dependency-required |
| `cpp/TabBar.cpp` | Uses local `TabWidget` callbacks and a ServoQ MIME type; omits Ladybird global drag source pointers, tab context menu execution, and `Menu` integration. | build-system-required |
| `cpp/TabBar.cpp::paintEvent` | Active tab gradient/shadow, text alpha, and separators now match the inspected Ladybird painting blocks; drop indicator remains simpler than `TabBar.cpp:548-568`. | temporary |
| `cpp/TabBar.cpp::createWindowButton` | Plain `QToolButton` replaces Ladybird `WindowControlButton`; pressed-outside style property is not implemented. | missing-dependency-required |
| `cpp/WebContentView.cpp` | Engine work uses Servo create/load/tick/input/resize APIs; no LibWeb `ViewImplementation`, `WindowRenderingContext`, compositor shared buffer, select popup, drag/drop, IME, or node picker. | Servo-required |
| `cpp/FindInPageWidget.cpp` | UI controls are present, but match count and previous/next do not query real Servo content. | temporary |
| `cpp/BookmarksBar.cpp` | Uses tab-separated local settings entries; no Ladybird bookmark folders, context menus, drag/drop, or store observer. | missing-dependency-required |
| `cpp/ChromeStyle.cpp::web_placeholder_style_sheet` | Local default-mode content placeholder styling; no Ladybird equivalent. | temporary |
| `build.rs` | Cargo/CXX compiles Qt sources and generated qrc output instead of Ladybird CMake. | build-system-required |

## Rebase Notes

Files that should be easiest to replace or line-merge with future Ladybird versions:

- `cpp/ChromeLayout.h`: only namespace/build include differs.
- `cpp/Icon.cpp` and `cpp/Icon.h`: drawing code is isolated from Servo.
- `cpp/ChromeStyle.cpp` and `cpp/ChromeStyle.h`: keep adding upstream style functions/selectors directly; keep Servo-only content styling at the bottom.
- `cpp/TabBar.cpp` and `cpp/TabBar.h`: keep local Qt helper names mapped to upstream blocks and avoid adding Servo logic here.

Files that need manual merge because they contain Servo seams:

- `cpp/BrowserWindow.cpp` and `cpp/BrowserWindow.h`: tab lifecycle and menus meet Servo controller IDs.
- `cpp/Tab.cpp` and `cpp/Tab.h`: toolbar is Ladybird chrome, navigation and state are Servo bridge calls.
- `cpp/LocationEdit.cpp` and `cpp/LocationEdit.h`: should eventually move to a `URL::URL`-owning port when LibURL or an exact equivalent is available.
- `cpp/WebContentView.cpp` and `cpp/WebContentView.h`: public/event surface should track Ladybird; implementation is Servo backend.

Adapter files that normally should not conflict with Ladybird updates:

- `cpp/WebViewURL.cpp` and `cpp/WebViewURL.h`
- `cpp/WebContentPlaceholder.cpp` and `cpp/WebContentPlaceholder.h`
- `cpp/servo_callbacks.h`
- `src/servo_engine.rs`
- Rust bridge/controller modules under `src/`

## Remaining Fidelity Gaps

- LibURL and full public suffix data are absent. `WebViewURL` documents this and uses a small local suffix allowlist for current navigation needs.
- Search provider settings are absent. Text that is not a navigable URL currently fails navigation instead of becoming a configured search URL.
- `LocationEdit` lacks Ladybird autocomplete, inline completion, domain highlighting, focus glow, zoom pill, and not-secure pill.
- Browser menu/action coverage is reduced; Ladybird `Menu.*` and app-global action infrastructure are not present.
- Bookmarks/history are local stubs, not Ladybird stores.
- `FindInPageWidget` is UI-only over real Servo content.
- `WebContentView` has Servo-only rendering and event forwarding. GL / `WindowRenderingContext`, IME, drag/drop, select popup, and node picker are not implemented.
- Font/HarfBuzz crash diagnostics remain investigation-only. This document does not claim that crash is fixed.
- Default no-feature mode uses `WebContentPlaceholder`, which has no Ladybird equivalent and should stay isolated.

## Inspected Reference Lines

- `vendor/reference-ladybird/UI/Qt/ChromeLayout.h:13-52`
- `vendor/reference-ladybird/UI/Qt/ChromeStyle.cpp:18-248`
- `vendor/reference-ladybird/UI/Qt/ChromeStyle.cpp:314-413`
- `vendor/reference-ladybird/UI/Qt/ChromeStyle.cpp:501-557`
- `vendor/reference-ladybird/UI/Qt/ChromeStyle.cpp:742-885`
- `vendor/reference-ladybird/UI/Qt/TabBar.cpp:54-83`
- `vendor/reference-ladybird/UI/Qt/TabBar.cpp:177-203`
- `vendor/reference-ladybird/UI/Qt/TabBar.cpp:467-568`
- `vendor/reference-ladybird/UI/Qt/TabBar.cpp:1616-1856`
- `vendor/reference-ladybird/UI/Qt/TabBar.cpp:2015-2034`
- `vendor/reference-ladybird/UI/Qt/LocationEdit.cpp:190-197`
- `vendor/reference-ladybird/UI/Qt/LocationEdit.cpp:286-300`
- `vendor/reference-ladybird/UI/Qt/LocationEdit.cpp:391-438`
- `vendor/reference-ladybird/UI/Qt/LocationEdit.cpp:520-552`
- `vendor/reference-ladybird/UI/Qt/LocationEdit.cpp:584-591`
- `vendor/reference-ladybird/UI/Qt/Tab.cpp:121-154`
- `vendor/reference-ladybird/UI/Qt/Tab.cpp:225-235`
- `vendor/reference-ladybird/UI/Qt/Tab.cpp:253-287`
- `vendor/reference-ladybird/UI/Qt/Tab.cpp:649-667`
- `vendor/reference-ladybird/UI/Qt/BrowserWindow.cpp:535-539`
- `vendor/reference-ladybird/UI/Qt/BrowserWindow.cpp:715-724`
- `vendor/reference-ladybird/UI/Qt/WebContentView.h:47-78`
- `vendor/reference-ladybird/UI/Qt/WebContentView.cpp:458-477`
- `vendor/reference-ladybird/UI/Qt/WebContentView.cpp:499-588`
- `vendor/reference-ladybird/UI/Qt/WebContentView.cpp:646-652`
- `vendor/reference-ladybird/UI/Qt/WebContentView.cpp:676-714`
- `vendor/reference-ladybird/UI/Qt/WebContentView.cpp:774-783`
- `vendor/reference-ladybird/UI/Qt/WebContentView.cpp:964-1009`
- `vendor/reference-ladybird/UI/Qt/FindInPageWidget.cpp:18-80`
- `vendor/reference-ladybird/UI/Qt/BookmarksBar.cpp:29-35`
- `vendor/reference-ladybird/UI/Qt/ladybird.qrc:1-5`
