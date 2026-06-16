// servoq:: free functions Rust calls (via CXX) to push frames and notifications
// into the Qt layer. Needs rust/cxx.h in the include path.

#pragma once

#include "rust/cxx.h"

#include <cstdint>
#include <type_traits>

#ifndef CXX_DEFAULT_VALUE
#define CXX_DEFAULT_VALUE(value) = value
#endif

namespace servoq {

// Mirror of the CXX shared struct from src/bridge.rs, defined under CXX's own
// include guard so whichever copy is parsed first wins.
#ifndef CXXBRIDGE1_STRUCT_servoq$PromptDialogResult
#define CXXBRIDGE1_STRUCT_servoq$PromptDialogResult
struct PromptDialogResult final {
  bool accepted CXX_DEFAULT_VALUE(false);
  ::rust::String value;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_servoq$PromptDialogResult

#ifndef CXXBRIDGE1_STRUCT_servoq$AuthDialogResult
#define CXXBRIDGE1_STRUCT_servoq$AuthDialogResult
struct AuthDialogResult final {
  bool accepted CXX_DEFAULT_VALUE(false);
  ::rust::String username;
  ::rust::String password;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_servoq$AuthDialogResult

#ifndef CXXBRIDGE1_STRUCT_servoq$ScreenGeometryResult
#define CXXBRIDGE1_STRUCT_servoq$ScreenGeometryResult
struct ScreenGeometryResult final {
  bool valid CXX_DEFAULT_VALUE(false);
  ::std::int32_t screen_width CXX_DEFAULT_VALUE(0);
  ::std::int32_t screen_height CXX_DEFAULT_VALUE(0);
  ::std::int32_t available_width CXX_DEFAULT_VALUE(0);
  ::std::int32_t available_height CXX_DEFAULT_VALUE(0);
  ::std::int32_t window_x CXX_DEFAULT_VALUE(0);
  ::std::int32_t window_y CXX_DEFAULT_VALUE(0);
  ::std::int32_t window_width CXX_DEFAULT_VALUE(0);
  ::std::int32_t window_height CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_servoq$ScreenGeometryResult

// Push a complete RGBA frame (device pixels) to the WebContentView identified by tab_id.
// Called from notify_new_frame_ready after webview.paint() + read_to_image().
void deliver_frame(::std::int32_t tab_id,
                   ::rust::Slice<const ::std::uint8_t> bytes,
                   ::std::int32_t width,
                   ::std::int32_t height);

// Delegate notification callbacks — update Tab chrome.
void notify_url_changed(::std::int32_t tab_id, ::rust::Str url);
void notify_title_changed(::std::int32_t tab_id, ::rust::Str title);
void notify_load_started(::std::int32_t tab_id, ::rust::Str url);
void notify_load_finished(::std::int32_t tab_id);
void notify_status_changed(::std::int32_t tab_id, ::rust::Str text);
void notify_pdf_navigation_requested(::std::int32_t tab_id, ::rust::Str url);

// Called when Servo panics / crashes during event loop spin.
void notify_webview_crashed(::std::int32_t tab_id, ::rust::Str reason);
void notify_request_blocked(::std::int32_t tab_id, ::rust::Str url);
bool content_blocking_enabled();
bool content_blocking_host_allowlisted(::rust::Str host);
bool webcontent_frame_pending(::std::int32_t tab_id);
void request_wayland_window_repaint(::std::int32_t tab_id);

// Favicon: RGBA8 pixel data, width × height pixels. Empty data = clear to default globe.
void notify_favicon_changed(::std::int32_t tab_id,
                            ::rust::Slice<const ::std::uint8_t> data,
                            ::std::int32_t width,
                            ::std::int32_t height);

// Cursor: ServoQ cursor code; WebContentView.cpp maps it explicitly to Qt.
void notify_cursor_changed(::std::int32_t tab_id, ::std::int32_t cursor_shape);

// Fullscreen state change.
void notify_fullscreen_changed(::std::int32_t tab_id, bool fullscreen);

// History list: newline-separated URLs and current index.
void notify_history_changed(::std::int32_t tab_id, ::rust::Str urls, ::std::int32_t current);

// window.open(): open a Tab widget for a WebView already registered in the Rust engine.
void request_open_tab_for_id(::std::int32_t tab_id);

// Context menu (synchronous): newline-delimited "action_id\tlabel\tenabled" or "sep".
// link_url is non-empty when the right-click target was a hyperlink.
// Returns selected action_id (>=0) or -1 for dismissed / handled internally.
::std::int32_t show_context_menu_sync(::std::int32_t tab_id, ::rust::Str items_str, ::rust::Str link_url);

// Web Notification API: show a desktop notification via QSystemTrayIcon.
void show_notification(::std::int32_t tab_id, ::rust::Str title, ::rust::Str body);

// Embedder controls (form controls & script dialogs), in WebDialogs.cpp: each
// opens a modal Qt dialog on the main thread and returns the user's choice.
void show_alert_dialog_sync(::std::int32_t tab_id, ::rust::Str message);
bool show_confirm_dialog_sync(::std::int32_t tab_id, ::rust::Str message);
PromptDialogResult show_prompt_dialog_sync(::std::int32_t tab_id, ::rust::Str message, ::rust::Str default_value);
// items: lines "opt\t<id>\t<label>\t<disabled 0/1>\t<selected 0/1>\t<in_group 0/1>"
// or "group\t<label>"; x/y = element bottom-left, width = element width (device px).
// Returns the chosen option id (>=0) or -1 for dismissed.
::std::int32_t show_select_dropdown_sync(::std::int32_t tab_id, ::rust::Str items, ::std::int32_t x, ::std::int32_t y, ::std::int32_t width);
// Returns packed 0xRRGGBB (>=0) or -1 for cancelled.
::std::int32_t show_color_picker_sync(::std::int32_t tab_id, ::std::uint8_t red, ::std::uint8_t green, ::std::uint8_t blue);
// filters: newline-separated extensions without dot. Returns newline-separated
// selected paths; empty = cancelled.
::rust::String show_file_picker_sync(::std::int32_t tab_id, ::rust::Str filters, bool allow_multiple);
// window.close() from web content: close the owning tab (deferred via QTimer).
void notify_webview_close_requested(::std::int32_t tab_id);

// System clipboard bridge for Servo's ClipboardDelegate (QClipboard-backed).
// Called on the main thread only (Servo delegate callbacks fire inside
// spin_event_loop); implemented in WebContentView.cpp.
void clipboard_clear();
::rust::String clipboard_get_text();
void clipboard_set_text(::rust::Str text);

// HTTP authentication (401/407): modal username/password dialog
// (WebDialogs.cpp). accepted=false means Cancel — the request proceeds
// without credentials and the server's 401 page is shown.
AuthDialogResult show_authentication_dialog_sync(::std::int32_t tab_id, ::rust::Str url, bool for_proxy);

// Permission prompt (WebDialogs.cpp). Checks PermissionStore for a stored
// per-origin decision first; otherwise shows a modal Allow/Block prompt.
// Allow/Block persist; dismissing denies once without persisting.
bool request_permission_sync(::std::int32_t tab_id, ::rust::Str origin, ::rust::Str feature);

// window.screen.* backing data in device pixels (WebContentView.cpp).
ScreenGeometryResult get_screen_geometry(::std::int32_t tab_id);

// window.moveTo / window.resizeTo from page content (device pixels).
// Honored only for single-tab windows (the Firefox/Chrome popup policy) and
// applied deferred via QTimer so the delegate callback unwinds first.
void request_window_move_to(::std::int32_t tab_id, ::std::int32_t x, ::std::int32_t y);
void request_window_resize_to(::std::int32_t tab_id, ::std::int32_t width, ::std::int32_t height);

// Screenshot result for servoq::take_screenshot (WebContentView.cpp): RGBA8
// pixels, empty data = failure. Prompts for a save location and writes PNG.
void notify_screenshot_taken(::std::int32_t tab_id,
                             ::rust::Slice<const ::std::uint8_t> data,
                             ::std::int32_t width,
                             ::std::int32_t height);

// JS evaluation result for servoq::evaluate_javascript (WebContentView.cpp):
// JSON text on success, error description on failure. Routed to the callback
// registered under request_id by ServoQ::evaluate_javascript_in_tab.
void notify_javascript_result(::std::int32_t tab_id, ::std::uint64_t request_id, bool success, ::rust::Str result);

// Media Session API event for MPRIS integration (M5.6). kind: 0 SetMetadata,
// 1 PlaybackStateChange, 2 SetPositionState. playback_state: 1 none, 2 playing,
// 3 paused (kind 1 only). Metadata fields valid for kind 0; position fields for
// kind 2. Routed to MprisManager.
void notify_media_session_event(::std::int32_t tab_id, ::std::int32_t kind,
                                ::std::int32_t playback_state, ::rust::Str title,
                                ::rust::Str artist, ::rust::Str album, double duration,
                                double position, double playback_rate);

// Page console message for the servoq://debug console panel (M4.4). level:
// 0 Log, 1 Debug, 2 Info, 3 Warn, 4 Error, 5 Trace. Only delivered while console
// capture is enabled (servoq::set_console_capture_enabled).
void notify_console_message(::std::int32_t tab_id, ::std::int32_t level, ::rust::Str message);

// Posts QEvent(User+1) to qApp to wake the Qt event loop from any thread.
// Called by QtEventLoopWaker::wake() from Servo's background threads.
void servoq_wake_event_loop();
void mark_servo_wake_event_consumed();
void begin_servo_shutdown();
bool servo_shutdown_started();

} // namespace servoq
