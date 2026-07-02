// Per-second SERVOQ_PERF counters for the Qt-side render/wake paths, shared by
// WebContentView.cpp (present scheduling) and servo_callbacks.cpp (wake events).

#pragma once

#include <chrono>
#include <cstdint>

namespace ServoQ {

struct QtPerfStats {
    std::chrono::steady_clock::time_point window_start { std::chrono::steady_clock::now() };
    uint64_t wake_events { 0 };
    uint64_t wake_events_coalesced { 0 };
    uint64_t qwindow_update_requests { 0 };
    uint64_t qwindow_exposes { 0 };
    uint64_t qwindow_presents { 0 };
    uint64_t qwindow_present_requests { 0 };
    uint64_t qwindow_present_requests_coalesced { 0 };
    uint64_t software_frames { 0 };
    uint64_t software_paints { 0 };
    uint64_t draw_image_calls { 0 };
    // Present-request sources (counted at requestWaylandRepaint entry).
    uint64_t present_req_frame_ready { 0 };
    uint64_t present_req_expose { 0 };
    uint64_t present_req_resize { 0 };
    uint64_t present_req_activation { 0 };
    uint64_t present_req_retry { 0 };
    // Why requests did not start a present.
    uint64_t present_skipped_in_progress { 0 };
    uint64_t present_skipped_pending { 0 };
    uint64_t present_skipped_rate_capped { 0 };
    // Main-thread time spent inside present (make_current+paint+swap) and how
    // many presents exceeded one refresh interval — the freeze signature.
    uint64_t present_busy_ms { 0 };
    uint64_t slow_presents { 0 };
};

QtPerfStats& qt_perf_stats();

// Logs and resets the counters, at most once per second; no-op unless SERVOQ_PERF.
void maybe_log_qt_perf();

} // namespace ServoQ
