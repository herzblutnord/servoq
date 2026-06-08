// servo_callbacks.h
//
// C++ free functions in namespace servoq that Rust calls (via CXX unsafe extern "C++")
// to push frame data and delegate notifications into the Qt widget layer.
// This header is included by the CXX bridge and by WebContentView.cpp; it must
// NOT be included from files that lack rust/cxx.h in their include path.

#pragma once

#include "rust/cxx.h"

#include <cstdint>

namespace servoq {

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

// Called when Servo panics / crashes during event loop spin. [ladybird: WebContentView crash signal]
void notify_webview_crashed(::std::int32_t tab_id, ::rust::Str reason);
void notify_request_blocked(::std::int32_t tab_id, ::rust::Str url);
bool content_blocking_enabled();
bool webcontent_frame_pending(::std::int32_t tab_id);
void request_wayland_window_repaint(::std::int32_t tab_id);

// Favicon: RGBA8 pixel data, width × height pixels. Empty data = clear to default globe.
void notify_favicon_changed(::std::int32_t tab_id,
                            ::rust::Slice<const ::std::uint8_t> data,
                            ::std::int32_t width,
                            ::std::int32_t height);

// Cursor: Qt::CursorShape integer value.
void notify_cursor_changed(::std::int32_t tab_id, ::std::int32_t cursor_shape);

// Fullscreen state change.
void notify_fullscreen_changed(::std::int32_t tab_id, bool fullscreen);

// History list: newline-separated URLs and current index.
void notify_history_changed(::std::int32_t tab_id, ::rust::Str urls, ::std::int32_t current);

// window.open(): open a Tab widget for a WebView already registered in the Rust engine.
void request_open_tab_for_id(::std::int32_t tab_id);

// Context menu (synchronous): newline-delimited "action_id\tlabel\tenabled" or "sep".
// Returns selected action_id (>=0) or -1 for dismissed.
::std::int32_t show_context_menu_sync(::std::int32_t tab_id, ::rust::Str items_str);

// Web Notification API: show a desktop notification via QSystemTrayIcon.
void show_notification(::std::int32_t tab_id, ::rust::Str title, ::rust::Str body);

// Posts QEvent(User+1) to qApp to wake the Qt event loop from any thread.
// Called by QtEventLoopWaker::wake() from Servo's background threads.
void servoq_wake_event_loop();
void mark_servo_wake_event_consumed();
void begin_servo_shutdown();
bool servo_shutdown_started();

} // namespace servoq
