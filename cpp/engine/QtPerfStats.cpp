#include "engine/QtPerfStats.h"
#include "DebugFlags.h"

#include <QDebug>

namespace ServoQ {

QtPerfStats& qt_perf_stats()
{
    static QtPerfStats stats;
    return stats;
}

void maybe_log_qt_perf()
{
    if (!perf_enabled())
        return;
    auto& stats = qt_perf_stats();
    auto now = std::chrono::steady_clock::now();
    if (now - stats.window_start < std::chrono::seconds(1))
        return;

    qInfo().nospace()
        << "SERVOQ_PERF qt wake_events=" << stats.wake_events
        << " wake_coalesced=" << stats.wake_events_coalesced
        << " qwindow_update_requests=" << stats.qwindow_update_requests
        << " qwindow_exposes=" << stats.qwindow_exposes
        << " qwindow_presents=" << stats.qwindow_presents
        << " qwindow_present_requests=" << stats.qwindow_present_requests
        << " qwindow_present_requests_coalesced=" << stats.qwindow_present_requests_coalesced
        << " present_req[frame_ready=" << stats.present_req_frame_ready
        << " expose=" << stats.present_req_expose
        << " resize=" << stats.present_req_resize
        << " activation=" << stats.present_req_activation
        << " retry=" << stats.present_req_retry << "]"
        << " present_skipped[in_progress=" << stats.present_skipped_in_progress
        << " pending=" << stats.present_skipped_pending
        << " rate_capped=" << stats.present_skipped_rate_capped << "]"
        << " present_busy_ms=" << stats.present_busy_ms
        << " slow_presents=" << stats.slow_presents
        << " software_frames=" << stats.software_frames
        << " software_paints=" << stats.software_paints
        << " draw_image_calls=" << stats.draw_image_calls;
    stats = {};
    stats.window_start = now;
}

} // namespace ServoQ
