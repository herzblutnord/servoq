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

// Posts QEvent(User+1) to qApp to wake the Qt event loop from any thread.
// Called by QtEventLoopWaker::wake() from Servo's background threads.
void servoq_wake_event_loop();

} // namespace servoq
