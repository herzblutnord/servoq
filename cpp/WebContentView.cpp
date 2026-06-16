/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/WebContentView.cpp
 */
// Qt widget that hosts the Servo engine (or a placeholder before the first
// frame / when the engine is disabled).

#include "BrowserWindow.h"
#include "BookmarkStore.h"
#include "ChromeStyle.h"
#include "Favicon.h"
#include "InternalPageView.h"
#include "NewTabTrace.h"
#include "WebContentView.h"
#include "Settings.h"
#include "Tab.h"
#include "servo_callbacks.h"
#include "servoq/src/bridge.rs.h"

#include <QApplication>
#include <QCoreApplication>
#include <QSystemTrayIcon>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFocusEvent>
#include <QMessageBox>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QDebug>
#include <QResizeEvent>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <QStackedWidget>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <QtGui/qpa/qplatformwindow_p.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <QClipboard>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

// ── Global registry (file-local, accessible to both namespace blocks below) ──
// Maps tab_id -> WebContentView* so Rust-side callbacks can locate their widget.

static QMap<int, ServoQ::WebContentView*>& g_view_registry()
{
    static QMap<int, ServoQ::WebContentView*> s_registry;
    return s_registry;
}

static std::atomic_bool& g_servo_wake_pending()
{
    static std::atomic_bool s_pending { false };
    return s_pending;
}

static std::atomic_bool& g_servo_shutting_down()
{
    static std::atomic_bool s_shutting_down { false };
    return s_shutting_down;
}

// Cached once: qEnvironmentVariableIsSet takes a process-global lock contended on
// hot paths (docs/DEVIATIONS.md §0c).
static bool debug_enabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_DEBUG");
    return v;
}

static bool perf_enabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_PERF");
    return v;
}

// Opt-in low-noise tracing (SERVOQ_DIAG).
bool servoq_diag_enabled()
{
    static int cached = -1;
    if (cached < 0)
        cached = qEnvironmentVariableIsSet("SERVOQ_DIAG") ? 1 : 0;
    return cached == 1;
}

QString servoq_diag_describe(QObject const* o)
{
    if (!o)
        return QStringLiteral("<null>");
    auto name = o->objectName();
    return QStringLiteral("%1{%2}")
        .arg(QString::fromUtf8(o->metaObject()->className()))
        .arg(name.isEmpty() ? QStringLiteral("-") : name);
}

void servoq_diag_log(QString const& msg)
{
    if (servoq_diag_enabled())
        qInfo().noquote().nospace() << "SERVOQ_DIAG " << msg;
}

enum class RendererMode { Auto, Software, WaylandWindow };

static RendererMode parse_renderer_mode()
{
    auto val = qEnvironmentVariable("SERVOQ_RENDERER");
    if (val.isEmpty() || val == QStringLiteral("auto"))
        return RendererMode::Auto;
    if (val == QStringLiteral("software"))
        return RendererMode::Software;
    if (val == QStringLiteral("wayland-window"))
        return RendererMode::WaylandWindow;
    qWarning().nospace() << "[servoq] unknown SERVOQ_RENDERER='" << val << "'; using auto";
    return RendererMode::Auto;
}

static RendererMode g_renderer_mode()
{
    static RendererMode mode = parse_renderer_mode();
    return mode;
}

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

static QtPerfStats& qt_perf_stats()
{
    static QtPerfStats stats;
    return stats;
}

// Monotonic ms clock for the present duty-cycle cap (main-thread only).
static qint64 present_clock_ms()
{
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer.elapsed();
}

// Cap presents at the display refresh rate; faster is wasted main-thread work.
static qint64 min_present_interval_ms()
{
    qreal hz = 60.0;
    if (auto* screen = QGuiApplication::primaryScreen()) {
        if (screen->refreshRate() > 1.0)
            hz = screen->refreshRate();
    }
    return qMax<qint64>(1, static_cast<qint64>(1000.0 / hz));
}

static ServoQ::ServoWaylandContentWindow*& g_wayland_window()
{
    static ServoQ::ServoWaylandContentWindow* s_window { nullptr };
    return s_window;
}

static QWidget*& g_wayland_container()
{
    static QWidget* s_container { nullptr };
    return s_container;
}

static ServoQ::WebContentView*& g_wayland_owner()
{
    static ServoQ::WebContentView* s_owner { nullptr };
    return s_owner;
}

static int& g_wayland_owner_generation()
{
    static int s_generation { 0 };
    return s_generation;
}

// Commit the toplevel wl_surface directly so pending subsurface moves apply now
// and Qt's frame callback fires, un-stalling chrome repaints (docs/DEVIATIONS.md
// §0d). Safe from here: Qt stages/commits its own toplevel state on this thread.
// SERVOQ_NO_DIRECT_WL_COMMIT=1 disables for diagnosis.
static bool direct_wl_commit_disabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_NO_DIRECT_WL_COMMIT");
    return v;
}

static void commit_toplevel_wl_surface(QWidget* widget_in_window, char const* trace_marker)
{
    if (direct_wl_commit_disabled() || !widget_in_window)
        return;
    auto* toplevel = widget_in_window->window();
    if (!toplevel)
        return;
    auto* handle = toplevel->windowHandle();
    if (!handle)
        return;
    auto* wayland_window = handle->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!wayland_window)
        return;
    auto* surface = wayland_window->surface();
    if (!surface)
        return;
    wl_surface_commit(surface);
    if (auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>(); app && app->display())
        wl_display_flush(app->display());
    ServoQ::newtab_trace_point(trace_marker);
}

static void park_shared_wayland_container()
{
    auto* container = g_wayland_container();
    if (!container)
        return;
    // Move off-screen, don't hide: hiding unmaps the wl_surface and the next
    // eglSwapBuffers after remap can block forever (docs/DEVIATIONS.md §0d).
    // -16384 clears any real monitor (8K is 7680 px).
    container->move(-16384, 0);
    // move() is parent-surface state, applied only on the next toplevel commit;
    // queue a repaint so the park takes effect now instead of after some
    // incidental chrome repaint.
    if (auto* toplevel = container->window())
        toplevel->update();
    ServoQ::newtab_trace_point("park_shared_wayland_container_toplevel_update_queued");
    // The queued update above is NOT sufficient on its own: Qt's repaint can be
    // stalled on a starved frame callback (see commit_toplevel_wl_surface).
    commit_toplevel_wl_surface(container, "park_toplevel_wl_surface_committed");
}

static void maybe_log_qt_perf()
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

static void debug_log(char const* event, int tab_id)
{
    if (debug_enabled())
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " tab_id=" << tab_id;
}

static void debug_log(char const* event, int tab_id, QString const& detail)
{
    if (debug_enabled())
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " tab_id=" << tab_id << " " << detail;
}

static void debug_log_favicon(int tab_id, QString const& detail)
{
    debug_log("favicon", tab_id, detail);
}

static char const* cursor_code_name(int code)
{
    switch (code) {
    case 0: return "none";
    case 1: return "default";
    case 2: return "pointer";
    case 3: return "context-menu";
    case 4: return "help";
    case 5: return "progress";
    case 6: return "wait";
    case 7: return "cell";
    case 8: return "crosshair";
    case 9: return "text";
    case 10: return "vertical-text";
    case 11: return "alias";
    case 12: return "copy";
    case 13: return "move";
    case 14: return "no-drop";
    case 15: return "not-allowed";
    case 16: return "grab";
    case 17: return "grabbing";
    case 18: return "e-resize";
    case 19: return "n-resize";
    case 20: return "ne-resize";
    case 21: return "nw-resize";
    case 22: return "s-resize";
    case 23: return "se-resize";
    case 24: return "sw-resize";
    case 25: return "w-resize";
    case 26: return "ew-resize";
    case 27: return "ns-resize";
    case 28: return "nesw-resize";
    case 29: return "nwse-resize";
    case 30: return "col-resize";
    case 31: return "row-resize";
    case 32: return "all-scroll";
    case 33: return "zoom-in";
    case 34: return "zoom-out";
    default: return "unknown";
    }
}

static Qt::CursorShape cursor_shape_from_servoq_code(int code)
{
    switch (code) {
    case 2:
        return Qt::PointingHandCursor;
    case 9:
    case 10:
        return Qt::IBeamCursor;
    case 6:
        return Qt::WaitCursor;
    case 5:
        return Qt::BusyCursor;
    case 7:
    case 8:
        return Qt::CrossCursor;
    case 13:
    case 32:
        return Qt::SizeAllCursor;
    case 16:
        return Qt::OpenHandCursor;
    case 17:
        return Qt::ClosedHandCursor;
    case 14:
    case 15:
        return Qt::ForbiddenCursor;
    case 4:
        return Qt::WhatsThisCursor;
    case 18:
    case 25:
    case 26:
        return Qt::SizeHorCursor;
    case 19:
    case 22:
    case 27:
        return Qt::SizeVerCursor;
    case 20:
    case 24:
    case 28:
        return Qt::SizeBDiagCursor;
    case 21:
    case 23:
    case 29:
        return Qt::SizeFDiagCursor;
    case 30:
        return Qt::SplitHCursor;
    case 31:
        return Qt::SplitVCursor;
    case 11:
        return Qt::DragLinkCursor;
    case 12:
        return Qt::DragCopyCursor;
    case 0:
    case 1:
    case 3:
    case 33:
    case 34:
    default:
        return Qt::ArrowCursor;
    }
}

static char const* qt_cursor_name(Qt::CursorShape shape)
{
    switch (shape) {
    case Qt::ArrowCursor: return "ArrowCursor";
    case Qt::PointingHandCursor: return "PointingHandCursor";
    case Qt::IBeamCursor: return "IBeamCursor";
    case Qt::WaitCursor: return "WaitCursor";
    case Qt::BusyCursor: return "BusyCursor";
    case Qt::CrossCursor: return "CrossCursor";
    case Qt::SizeAllCursor: return "SizeAllCursor";
    case Qt::OpenHandCursor: return "OpenHandCursor";
    case Qt::ClosedHandCursor: return "ClosedHandCursor";
    case Qt::ForbiddenCursor: return "ForbiddenCursor";
    case Qt::WhatsThisCursor: return "WhatsThisCursor";
    case Qt::SizeHorCursor: return "SizeHorCursor";
    case Qt::SizeVerCursor: return "SizeVerCursor";
    case Qt::SizeBDiagCursor: return "SizeBDiagCursor";
    case Qt::SizeFDiagCursor: return "SizeFDiagCursor";
    case Qt::SplitHCursor: return "SplitHCursor";
    case Qt::SplitVCursor: return "SplitVCursor";
    case Qt::DragLinkCursor: return "DragLinkCursor";
    case Qt::DragCopyCursor: return "DragCopyCursor";
    default: return "Other";
    }
}

static void debug_log(char const* event, int tab_id, QSize const& size, qreal dpr)
{
    if (debug_enabled()) {
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " tab_id=" << tab_id
                          << " physical=" << size.width() << "x" << size.height()
                          << " dpr=" << dpr;
    }
}

static bool is_using_dark_system_theme(QWidget const& widget)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    auto color_scheme = QGuiApplication::styleHints()->colorScheme();
    if (color_scheme == Qt::ColorScheme::Dark)
        return true;
    if (color_scheme == Qt::ColorScheme::Light)
        return false;
#endif
    auto color = widget.palette().color(QPalette::Window);
    return color.lightnessF() < 0.5;
}

// ────────────────────────────────────────────────────────────────────────────
// ServoQ::WebContentView implementation
// ────────────────────────────────────────────────────────────────────────────

namespace ServoQ {

class ServoWaylandContentWindow final : public QWindow {
public:
    explicit ServoWaylandContentWindow(WebContentView* owner)
        : m_owner(owner)
    {
        setSurfaceType(QWindow::SurfaceType::OpenGLSurface);
    }

    void setOwner(WebContentView* owner)
    {
        m_owner = owner;
    }

    void requestServoPresent()
    {
        m_present_generation_requested = g_wayland_owner_generation();
        requestUpdate();
    }

protected:
    bool event(QEvent* event) override
    {
        // Touchpad pinch arrives on the embedded QWindow (it has pointer
        // focus while the cursor is over the page). Mirrors Ladybird's
        // ZoomNativeGesture handling; Servo applies the magnification.
        if (event->type() == QEvent::NativeGesture) {
            auto const& gesture = *static_cast<QNativeGestureEvent const*>(event);
            if (gesture.gestureType() == Qt::ZoomNativeGesture && m_owner
                && !g_servo_shutting_down().load(std::memory_order_acquire)) {
                qreal dpr = devicePixelRatio();
                auto pos = gesture.position();
                servoq::forward_pinch_zoom(m_owner->tabId(),
                    static_cast<float>(1.0 + gesture.value()),
                    static_cast<float>(pos.x() * dpr),
                    static_cast<float>(pos.y() * dpr));
                return true;
            }
        }
        if (event->type() == QEvent::UpdateRequest) {
            qt_perf_stats().qwindow_update_requests++;
            if (g_servo_shutting_down().load(std::memory_order_acquire))
                return true;
            if (!m_owner || g_wayland_owner() != m_owner || m_present_generation_requested != g_wayland_owner_generation()) {
                if (m_owner)
                    m_owner->m_wayland_present_pending = false;
                debug_log("wayland_present_skipped_stale_owner", m_owner ? m_owner->tabId() : 0);
                maybe_log_qt_perf();
                return true;
            }
            if (!m_owner->isVisible() || (g_wayland_container() && !g_wayland_container()->isVisible())) {
                m_owner->m_wayland_present_pending = false;
                auto* container = g_wayland_container();
                if (debug_enabled())
                    debug_log("wayland_present_skipped_hidden", m_owner->tabId(),
                        QStringLiteral("view_visible=%1 container_visible=%2 owner=%3 pending_cleared=1")
                            .arg(m_owner->isVisible() ? 1 : 0)
                            .arg(container && container->isVisible() ? 1 : 0)
                            .arg(g_wayland_owner() ? g_wayland_owner()->tabId() : 0));
                maybe_log_qt_perf();
                return true;
            }
            if (m_owner->waylandRendererActive() && m_owner->takeWaylandPresentPending()) {
                qt_perf_stats().qwindow_presents++;
                auto owner_generation = g_wayland_owner_generation();
                if (servoq_diag_enabled())
                    servoq_diag_log(QStringLiteral("present ENTER tab_id=%1 owner_tab=%2 gen=%3 (present is only reached for current owner)")
                        .arg(m_owner->tabId())
                        .arg(g_wayland_owner() ? g_wayland_owner()->tabId() : 0)
                        .arg(owner_generation));
                m_owner->m_wayland_present_in_progress = true;
                // Per-present path: don't build the detail QStrings unless on.
                if (debug_enabled())
                    debug_log("wayland_present_enter", m_owner->tabId(),
                        QStringLiteral("owner_generation=%1").arg(owner_generation));
                qint64 const present_started_ms = present_clock_ms();
                {
                    // Names the present in SERVOQ_JANK if a swap blocks >200ms —
                    // per-second PERF counters can't show a stall in progress.
                    NewTabTraceScope scope("wayland_present", m_owner->tabId());
                    servoq::present_wayland_webview(m_owner->tabId());
                }
                qint64 const present_finished_ms = present_clock_ms();
                m_owner->m_wayland_present_in_progress = false;
                // Commit the parent after each present so the compositor fires
                // Qt's frame callback and chrome repaints don't starve (§0d).
                commit_toplevel_wl_surface(g_wayland_container(), "present_toplevel_wl_surface_committed");
                // Feed the duty-cycle cap (admission is measured from completion).
                m_owner->m_last_present_duration_ms = present_finished_ms - present_started_ms;
                m_owner->m_last_present_request_ms = present_finished_ms;
                qt_perf_stats().present_busy_ms +=
                    static_cast<uint64_t>(present_finished_ms - present_started_ms);
                if (present_finished_ms - present_started_ms > min_present_interval_ms())
                    qt_perf_stats().slow_presents++;
                if (debug_enabled())
                    debug_log("wayland_present_leave", m_owner->tabId(),
                        QStringLiteral("owner_generation=%1 current_owner=%2 current_generation=%3")
                            .arg(owner_generation)
                            .arg(g_wayland_owner() ? g_wayland_owner()->tabId() : 0)
                            .arg(g_wayland_owner_generation()));
                if (g_wayland_owner() != m_owner || owner_generation != g_wayland_owner_generation()) {
                    m_owner->m_wayland_dirty_after_present = false;
                    maybe_log_qt_perf();
                    return true;
                }
                if (m_owner->m_wayland_dirty_after_present) {
                    m_owner->m_wayland_dirty_after_present = false;
                    m_owner->requestWaylandRepaint(WebContentView::PresentRequestReason::Retry);
                }
                maybe_log_qt_perf();
            }
            return true;
        }
        return QWindow::event(event);
    }

    void exposeEvent(QExposeEvent*) override
    {
        qt_perf_stats().qwindow_exposes++;
        if (isExposed() && m_owner && m_owner->waylandRendererActive() && !g_servo_shutting_down().load(std::memory_order_acquire))
            m_owner->requestWaylandRepaint(WebContentView::PresentRequestReason::Expose);
        maybe_log_qt_perf();
    }

    void resizeEvent(QResizeEvent*) override
    {
        if (m_owner && !g_servo_shutting_down().load(std::memory_order_acquire))
            m_owner->forwardResizeToEngine();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!m_owner || g_servo_shutting_down().load(std::memory_order_acquire))
            return;
        qreal dpr = devicePixelRatio();
        servoq::forward_mouse_move(m_owner->tabId(),
            static_cast<float>(event->position().x() * dpr),
            static_cast<float>(event->position().y() * dpr));
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("SWCW::mousePressEvent owner_tab=%1 focusWidget=%2 focusWindow=%3")
                .arg(m_owner ? m_owner->tabId() : 0)
                .arg(servoq_diag_describe(QApplication::focusWidget()))
                .arg(servoq_diag_describe(QGuiApplication::focusWindow())));
        if (m_owner && !g_servo_shutting_down().load(std::memory_order_acquire)) {
            m_owner->takeFocusFromContentClick();
            m_owner->forwardWindowMouseButton(0, qtMouseButtonToServo(event->button()), event);
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_owner && !g_servo_shutting_down().load(std::memory_order_acquire)) {
            m_owner->forwardWindowMouseButton(1, qtMouseButtonToServo(event->button()), event);
            if (m_owner->handleMiddleClickLinkFallback(event))
                event->accept();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        mousePressEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (!m_owner || g_servo_shutting_down().load(std::memory_order_acquire))
            return;
        if (m_owner->handleCtrlWheelZoom(event))
            return;
        double dx = 0.0;
        double dy = 0.0;
        if (auto pixel_delta = -event->pixelDelta(); !pixel_delta.isNull()) {
            dx = pixel_delta.x();
            dy = pixel_delta.y();
        } else {
            auto angle_delta = -event->angleDelta();
            double step_x = static_cast<double>(angle_delta.x()) / 120.0
                            * static_cast<double>(QApplication::wheelScrollLines());
            double step_y = static_cast<double>(angle_delta.y()) / 120.0
                            * static_cast<double>(QApplication::wheelScrollLines());
            static constexpr double scroll_step_size = 40.0;
            dx = step_x * scroll_step_size;
            dy = step_y * scroll_step_size;
        }

        qreal dpr = devicePixelRatio();
        servoq::forward_wheel(m_owner->tabId(), dx, dy,
            static_cast<float>(event->position().x() * dpr),
            static_cast<float>(event->position().y() * dpr));
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("SWCW::keyPressEvent owner_tab=%1 key=0x%2 text='%3'")
                .arg(m_owner ? m_owner->tabId() : 0)
                .arg(event->key(), 0, 16)
                .arg(event->text()));
        if (!m_owner || g_servo_shutting_down().load(std::memory_order_acquire))
            return;
        auto text = event->text();
        uint32_t key_char = text.isEmpty() ? 0u : static_cast<uint32_t>(text[0].unicode());
        servoq::forward_key(m_owner->tabId(), true, key_char,
            static_cast<int32_t>(event->key()),
            static_cast<uint32_t>(event->modifiers()));
    }

    void keyReleaseEvent(QKeyEvent* event) override
    {
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("SWCW::keyReleaseEvent owner_tab=%1 key=0x%2")
                .arg(m_owner ? m_owner->tabId() : 0)
                .arg(event->key(), 0, 16));
        if (!m_owner || g_servo_shutting_down().load(std::memory_order_acquire))
            return;
        auto text = event->text();
        uint32_t key_char = text.isEmpty() ? 0u : static_cast<uint32_t>(text[0].unicode());
        servoq::forward_key(m_owner->tabId(), false, key_char,
            static_cast<int32_t>(event->key()),
            static_cast<uint32_t>(event->modifiers()));
    }

    void focusInEvent(QFocusEvent*) override
    {
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("SWCW::focusInEvent owner_tab=%1").arg(m_owner ? m_owner->tabId() : 0));
        if (m_owner && !g_servo_shutting_down().load(std::memory_order_acquire))
            servoq::forward_focus(m_owner->tabId(), true);
    }

    void focusOutEvent(QFocusEvent*) override
    {
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("SWCW::focusOutEvent owner_tab=%1").arg(m_owner ? m_owner->tabId() : 0));
        if (m_owner && !g_servo_shutting_down().load(std::memory_order_acquire))
            servoq::forward_focus(m_owner->tabId(), false);
    }

private:
    static int qtMouseButtonToServo(Qt::MouseButton button)
    {
        if (button == Qt::LeftButton)
            return 0;
        if (button == Qt::MiddleButton)
            return 1;
        if (button == Qt::RightButton)
            return 2;
        return -1;
    }

    WebContentView* m_owner { nullptr };
    int m_present_generation_requested { 0 };
};

// Unmap the shared Servo subsurface for an empty tab via attach(NULL)+commit: a
// parked-but-mapped subsurface throttles the toplevel frame callback and freezes
// chrome animations (§0d). Keeps the wl_surface/Qt state intact (unlike hide()),
// so the next activation present remaps it.
static void unmap_shared_servo_subsurface(char const* trace_marker)
{
    if (direct_wl_commit_disabled())
        return;
    auto* window = g_wayland_window();
    if (!window)
        return;
    auto* wayland_window = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!wayland_window)
        return;
    auto* surface = wayland_window->surface();
    if (!surface)
        return;
    wl_surface_attach(surface, nullptr, 0, 0);
    wl_surface_commit(surface);
    if (auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>(); app && app->display())
        wl_display_flush(app->display());
    servoq::notify_wayland_subsurface_unmapped();
    newtab_trace_point(trace_marker);
}

WebContentView::WebContentView(QWidget* parent)
    : QWidget(parent)
    , m_engine_tick_timer(new QTimer(this))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    // Empty new tabs and crash states are painted directly by this widget; real
    // pages are handed to Servo on first navigation.

    // Fallback tick timer: a safety net in case a Servo wake event is dropped or
    // coalesced (QtEventLoopWaker normally wakes the loop immediately).
    m_engine_tick_timer->setInterval(200);
    m_engine_tick_timer->setSingleShot(false);
    connect(m_engine_tick_timer, &QTimer::timeout, this, [this] {
        if (g_servo_shutting_down().load(std::memory_order_acquire))
            return;
        servoq::tick_webview(m_tab_id);
    });

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        QTimer::singleShot(0, this, [this] {
            notifyThemeChange();
            update();
        });
    });
#endif
}

WebContentView::~WebContentView()
{
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("~WebContentView tab_id=%1 is_owner=%2 wayland_active=%3 mouse_buttons=%4 present_in_progress=%5")
            .arg(m_tab_id)
            .arg(g_wayland_owner() == this ? 1 : 0)
            .arg(m_wayland_renderer_active ? 1 : 0)
            .arg(static_cast<int>(QApplication::mouseButtons()))
            .arg(m_wayland_present_in_progress ? 1 : 0));
    m_engine_tick_timer->stop();
    if (g_wayland_owner() == this) {
        g_wayland_owner() = nullptr;
        ++g_wayland_owner_generation();
        if (g_wayland_window())
            g_wayland_window()->setOwner(nullptr);
        if (g_wayland_container())
            g_wayland_container()->hide();
    }
    if (m_tab_id != 0) {
        debug_log("close_webview", m_tab_id);
        favicon_tab_closed(m_tab_id);
        g_view_registry().remove(m_tab_id);
        if (!g_servo_shutting_down().load(std::memory_order_acquire))
            servoq::close_webview(m_tab_id);
    }
}

void WebContentView::setTab(Tab* tab)
{
    m_tab = tab;
}

WebContentView* WebContentView::findByTabId(int tab_id)
{
    return g_view_registry().value(tab_id, nullptr);
}

void WebContentView::setTabId(int tab_id)
{
    if (m_tab_id != 0)
        g_view_registry().remove(m_tab_id);
    m_tab_id = tab_id;
    if (m_tab_id != 0) {
        debug_log("register_view", m_tab_id);
        g_view_registry().insert(m_tab_id, this);
    }
}

void WebContentView::setUrl(QString const& /*url*/)
{
    // No-op: kept for Tab.cpp call-site compatibility; empty state is painted.
}

void WebContentView::setStatus(QString const& /*status*/)
{
    // No-op: kept for call-site compatibility; empty state is painted.
}

void WebContentView::setInitialUrl(QString const& url)
{
    m_initial_url = url.isEmpty() ? QStringLiteral("about:blank") : url;
}

void WebContentView::setEmptyNewTab(bool empty_new_tab)
{
    if (m_empty_new_tab == empty_new_tab)
        return;
    m_empty_new_tab = empty_new_tab;
    if (m_empty_new_tab) {
        m_frame = {};
        m_pending_frame_repaint = false;
        if (m_wayland_container && g_wayland_owner() == this) {
            g_wayland_owner() = nullptr;
            ++g_wayland_owner_generation();
            if (g_wayland_window())
                g_wayland_window()->setOwner(nullptr);
            park_shared_wayland_container();
        }
        m_engine_tick_timer->stop();
    }
    update();
}

void WebContentView::setInternalPageActive(bool active)
{
    m_internal_page_active = active;
    if (!active)
        return;
    // Release the shared surface, park the container, and unmap the subsurface
    // (same teardown as an empty tab) so the native internal page widget owns the
    // page-column area.
    m_engine_tick_timer->stop();
    if (m_wayland_container && g_wayland_owner() == this) {
        g_wayland_owner() = nullptr;
        ++g_wayland_owner_generation();
        if (g_wayland_window())
            g_wayland_window()->setOwner(nullptr);
        park_shared_wayland_container();
    }
    if (g_wayland_container())
        unmap_shared_servo_subsurface("internal_page_servo_subsurface_unmapped");
}

bool WebContentView::ensureEngineStarted()
{
    auto started = startEngineIfNeeded();
    debug_log("ensure_engine_started", m_tab_id,
        QStringLiteral("started=%1 empty=%2").arg(started ? 1 : 0).arg(m_empty_new_tab ? 1 : 0));
    return started;
}

bool WebContentView::waylandRendererRequested() const
{
    auto mode = g_renderer_mode();
    return mode == RendererMode::Auto || mode == RendererMode::WaylandWindow;
}

// True when this view's Tab is the stack's current widget; reliable during the
// show/hide transitions of a tab switch (unlike isVisible()).
bool WebContentView::isCurrentlyActiveTab() const
{
    if (!m_tab)
        return false;
    auto* stacked = qobject_cast<QStackedWidget*>(m_tab->parentWidget());
    return stacked && stacked->currentWidget() == m_tab;
}

bool WebContentView::isCurrentWaylandOwner() const
{
    return g_wayland_owner() == this;
}

// static
WebContentView* WebContentView::currentWaylandOwner()
{
    return g_wayland_owner();
}

// static
QWidget* WebContentView::sharedWaylandContainer()
{
    return g_wayland_container();
}

bool WebContentView::attachSharedWaylandWindow()
{
    if (!waylandRendererRequested())
        return false;
    if (!g_wayland_window()) {
        g_wayland_window() = new ServoWaylandContentWindow(this);
        // Parent the container to the stable page-column widget, not the
        // QStackedWidget: the stack's per-page hide/show would map/unmap the
        // wl_subsurface and block eglSwapBuffers on remap (§0d).
        QWidget* stack = (m_tab && m_tab->parentWidget()) ? m_tab->parentWidget() : nullptr;
        QWidget* stable_parent = (stack && stack->parentWidget()) ? stack->parentWidget() : (stack ? stack : this);
        debug_log("wayland_container_created", m_tab_id,
            QStringLiteral("stable_parent_class=%1").arg(stable_parent->metaObject()->className()));
        g_wayland_container() = QWidget::createWindowContainer(g_wayland_window(), stable_parent);
        g_wayland_container()->setFocusPolicy(Qt::StrongFocus);
        g_wayland_container()->setMouseTracking(true);
        g_wayland_container()->hide();
        if (perf_enabled()) {
            qInfo().nospace()
                << "SERVOQ_PERF wayland_surface_count=1 window_rendering_context_instances=0 "
                << "new-tab-path=shared-qt-surface-created tab_id=" << m_tab_id
                << " webview_id=" << m_tab_id;
        }
    }

    m_wayland_window = g_wayland_window();
    m_wayland_container = g_wayland_container();
    if (!m_wayland_window || !m_wayland_container)
        return false;

    if (g_wayland_owner() != this) {
        // Only the active tab may claim the shared container; reaching here from a
        // background tab is a bug, so fail loudly rather than corrupt the active tab.
        if (!isCurrentlyActiveTab()) {
            qWarning().nospace()
                << "SERVOQ_WARN attachSharedWaylandWindow from non-active tab " << m_tab_id
                << " current_owner=" << (g_wayland_owner() ? g_wayland_owner()->tabId() : 0)
                << " owner_generation=" << g_wayland_owner_generation();
            if (servoq_diag_enabled())
                servoq_diag_log(QStringLiteral("attachSharedWaylandWindow REJECTED non-active tab_id=%1 owner_tab=%2 gen=%3")
                    .arg(m_tab_id)
                    .arg(g_wayland_owner() ? g_wayland_owner()->tabId() : 0)
                    .arg(g_wayland_owner_generation()));
            return false;
        }
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("attachSharedWaylandWindow CLAIM tab_id=%1 prev_owner=%2 gen=%3")
                .arg(m_tab_id)
                .arg(g_wayland_owner() ? g_wayland_owner()->tabId() : 0)
                .arg(g_wayland_owner_generation()));

        auto* previous_owner = g_wayland_owner();
        if (previous_owner && previous_owner != this) {
            previous_owner->m_wayland_present_pending = false;
            previous_owner->m_wayland_dirty_after_present = false;
            previous_owner->m_wayland_present_in_progress = false;
            previous_owner->m_last_present_duration_ms = 0;
            previous_owner->m_last_present_request_ms = -1000;
        }
        // Clean slate for the claiming view: the activation present must not be
        // capped by congestion measured under the previous owner/attachment.
        m_last_present_duration_ms = 0;
        m_last_present_request_ms = -1000;
        g_wayland_owner() = this;
        ++g_wayland_owner_generation();
        m_wayland_window->setOwner(this);
        // Do NOT call setParent() — the container lives under the stable
        // page-column widget for the app lifetime. Just reposition it.
        updateContainerGeometry();
        // The move above is parent-surface state; force a toplevel commit or a
        // newly-navigated tab presents into a still-parked subsurface and the page
        // doesn't appear for seconds (§0d).
        if (m_wayland_container) {
            if (auto* toplevel = m_wayland_container->window())
                toplevel->update();
        }
        ServoQ::newtab_trace_point("attach_claim_toplevel_update_queued", m_tab_id);
        commit_toplevel_wl_surface(m_wayland_container, "attach_claim_toplevel_wl_surface_committed");
        if (perf_enabled()) {
            qInfo().nospace()
                << "SERVOQ_PERF wayland_surface_count=1 window_rendering_context_instances=1 "
                << "tab_switch_path=shared-qt-surface-attached previous_tab_id="
                << (previous_owner ? previous_owner->m_tab_id : 0)
                << " active_tab_id=" << m_tab_id
                << " webview_id=" << m_tab_id
                << " owner_generation=" << g_wayland_owner_generation();
        }
        debug_log("wayland_owner_changed", m_tab_id,
            QStringLiteral("previous=%1 owner_generation=%2 visible=1")
                .arg(previous_owner ? previous_owner->m_tab_id : 0)
                .arg(g_wayland_owner_generation()));
    }
    return true;
}

void WebContentView::updateContainerGeometry()
{
    if (!m_wayland_container)
        return;
    auto* parent = m_wayland_container->parentWidget();
    if (!parent) {
        m_wayland_container->setGeometry(rect());
        return;
    }
    QPoint origin = mapTo(parent, QPoint(0, 0));
    m_wayland_container->setGeometry(origin.x(), origin.y(), width(), height());

    // Verify container does not overlap the tab bar (which lives above m_page_column).
    // Overlap would allow the native subsurface to intercept tab-bar mouse events.
    if (qEnvironmentVariableIsSet("SERVOQ_DEBUG")) {
        auto container_global = m_wayland_container->mapToGlobal(QPoint(0, 0));
        QRect container_global_rect(container_global, m_wayland_container->size());
        // Walk up to find the top-level window and check geometry sanity.
        auto* top = window();
        if (top) {
            auto window_global = top->mapToGlobal(QPoint(0, 0));
            // Container must be fully within window x-bounds.
            if (container_global_rect.left() < window_global.x()
                || container_global_rect.right() > window_global.x() + top->width()) {
                qWarning().nospace()
                    << "SERVOQ_WARN container outside window x-bounds:"
                    << " container=" << container_global_rect
                    << " window=(" << window_global.x() << "," << window_global.y()
                    << " " << top->width() << "x" << top->height() << ")"
                    << " tab_id=" << m_tab_id;
            }
        }
    }
}

bool WebContentView::startWaylandRendererIfPossible(int physical_width, int physical_height, qreal dpr, bool allow_software_gl)
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
        qWarning().nospace() << "[servoq] Wayland window renderer unavailable: Qt platform is "
                             << QGuiApplication::platformName() << "; falling back to software";
        return false;
    }
    if (!attachSharedWaylandWindow())
        return false;

    // This function is only reached for the currently-active tab (background tabs
    // are deferred in startEngineIfNeeded). Container and window ops are always safe.
    debug_log("start_wayland_renderer", m_tab_id,
        QStringLiteral("physical=%1x%2")
            .arg(physical_width)
            .arg(physical_height));

    updateContainerGeometry();
    m_wayland_container->show();
    m_wayland_container->raise();
    m_wayland_window->show();
    m_wayland_window->create();

    auto* app_interface = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!app_interface || !app_interface->display()) {
        qWarning() << "[servoq] Wayland window renderer unavailable: wl_display unavailable; falling back to software";
        m_wayland_container->hide();
        m_wayland_window->hide();
        return false;
    }

    auto* window_interface = m_wayland_window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!window_interface || !window_interface->surface()) {
        qWarning() << "[servoq] Wayland window renderer unavailable: wl_surface unavailable; falling back to software";
        m_wayland_container->hide();
        m_wayland_window->hide();
        return false;
    }

    auto url_std = m_initial_url.toStdString();
    auto ok = servoq::create_webview_wayland_window(
        m_tab_id,
        url_std.c_str(),
        physical_width,
        physical_height,
        static_cast<float>(dpr),
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(app_interface->display())),
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window_interface->surface())),
        allow_software_gl);
    if (!ok) {
        m_wayland_container->hide();
        m_wayland_window->hide();
        return false;
    }

    m_wayland_renderer_active = true;
    m_wayland_present_pending = false;
    m_wayland_present_in_progress = false;
    m_wayland_dirty_after_present = false;
    m_engine_tick_timer->stop();
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    if (perf_enabled()) {
        qInfo().nospace()
            << "SERVOQ_PERF renderer=wayland-window active=true qt_container=true qimage_paint=false physical="
            << physical_width << "x" << physical_height
            << " widget_logical=" << width() << "x" << height()
            << " container_logical=" << m_wayland_container->width() << "x" << m_wayland_container->height()
            << " qwindow_logical=" << m_wayland_window->width() << "x" << m_wayland_window->height()
            << " browser_logical=" << (window() ? window()->width() : 0) << "x" << (window() ? window()->height() : 0)
            << " dpr=" << dpr;
    }
    return true;
}

void WebContentView::receiveFrame(QImage const& frame)
{
    if (m_wayland_renderer_active || g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    qt_perf_stats().software_frames++;
    debug_log("deliver_frame_target", m_tab_id, frame.size(), devicePixelRatioF());
    m_frame = frame;
    m_pending_frame_repaint = true;
    m_crashed = false;
    update();
}

void WebContentView::receiveFrameBytes(uint8_t const* bytes, int width, int height)
{
    if (m_wayland_renderer_active || g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    qt_perf_stats().software_frames++;
    debug_log("deliver_frame_target", m_tab_id, QSize(width, height), devicePixelRatioF());
    if (m_frame.size() != QSize(width, height) || m_frame.format() != QImage::Format_RGBA8888)
        m_frame = QImage(width, height, QImage::Format_RGBA8888);

    auto const byte_count = qsizetype(width) * qsizetype(height) * 4;
    std::memcpy(m_frame.bits(), bytes, static_cast<size_t>(byte_count));
    m_pending_frame_repaint = true;
    m_crashed = false;
    update();
}

// No placeholder widget: crash state is tracked via m_crashed + m_crash_reason and
// drawn directly in paintEvent so there is no QWidget child to manage.
void WebContentView::receiveWebViewCrash(QString const& reason)
{
    m_engine_tick_timer->stop();
    if (m_wayland_container)
        m_wayland_container->hide();
    m_wayland_renderer_active = false;
    m_frame = {};
    m_pending_frame_repaint = false;
    m_crashed = true;
    m_crash_reason = reason;
    update();
}

void WebContentView::receiveRequestBlocked(QString const& url)
{
    emit request_blocked(url);
}

void WebContentView::requestWaylandRepaint(PresentRequestReason reason)
{
    {
        auto& stats = qt_perf_stats();
        switch (reason) {
        case PresentRequestReason::FrameReady: stats.present_req_frame_ready++; break;
        case PresentRequestReason::Expose: stats.present_req_expose++; break;
        case PresentRequestReason::Resize: stats.present_req_resize++; break;
        case PresentRequestReason::Activation: stats.present_req_activation++; break;
        case PresentRequestReason::Retry: stats.present_req_retry++; break;
        case PresentRequestReason::Shutdown: break;
        }
    }
    if (m_internal_page_active) {
        debug_log("wayland_present_skipped_internal_page", m_tab_id);
        return;
    }
    if (!m_webview_created) {
        debug_log("wayland_present_skipped_request_no_webview", m_tab_id);
        return;
    }
    if (!m_wayland_renderer_active || !m_wayland_window || g_servo_shutting_down().load(std::memory_order_acquire)) {
        if (debug_enabled())
            debug_log("wayland_present_skipped_request_inactive_renderer", m_tab_id,
                QStringLiteral("wayland_active=%1 window=%2")
                    .arg(m_wayland_renderer_active ? 1 : 0)
                    .arg(m_wayland_window ? 1 : 0));
        return;
    }
    if (!isVisible()) {
        debug_log("wayland_present_skipped_request_hidden_view", m_tab_id);
        return;
    }
    if (g_wayland_owner() != this) {
        if (debug_enabled())
            debug_log("wayland_present_skipped_request_inactive_owner", m_tab_id,
                QStringLiteral("current_owner=%1").arg(g_wayland_owner() ? g_wayland_owner()->tabId() : 0));
        return;
    }
    if (m_wayland_container && (m_wayland_container->width() <= 1 || m_wayland_container->height() <= 1)) {
        if (debug_enabled())
            debug_log("wayland_present_skipped_request_zero_geometry", m_tab_id,
                QStringLiteral("geom=%1,%2 %3x%4 visible=%5")
                    .arg(m_wayland_container->x()).arg(m_wayland_container->y())
                    .arg(m_wayland_container->width()).arg(m_wayland_container->height())
                    .arg(m_wayland_container->isVisible() ? 1 : 0));
        // Do NOT clear m_wayland_present_pending: the container will get valid
        // geometry on the next layout pass and exposeEvent will retry.
        return;
    }
    qt_perf_stats().qwindow_present_requests++;
    if (m_wayland_present_in_progress) {
        m_wayland_dirty_after_present = true;
        debug_log("wayland_present_deferred_in_progress", m_tab_id);
        qt_perf_stats().qwindow_present_requests_coalesced++;
        qt_perf_stats().present_skipped_in_progress++;
        maybe_log_qt_perf();
        return;
    }
    if (m_wayland_present_pending) {
        debug_log("wayland_present_coalesced_pending", m_tab_id);
        qt_perf_stats().qwindow_present_requests_coalesced++;
        qt_perf_stats().present_skipped_pending++;
        maybe_log_qt_perf();
        return;
    }
    // Duty-cycle cap for repeated frame traffic only (a slow present stretches the
    // idle gap so blocking swaps can't saturate the main thread); deferred requests
    // retry, and Activation/Expose/Resize bypass the cap (docs/DEVIATIONS.md §0).
    if (reason == PresentRequestReason::FrameReady || reason == PresentRequestReason::Retry) {
        constexpr qint64 kMaxAdaptivePresentIntervalMs = 250;
        qint64 const interval = qMax(min_present_interval_ms(),
            qMin(m_last_present_duration_ms, kMaxAdaptivePresentIntervalMs));
        qint64 const now = present_clock_ms();
        qint64 const since = now - m_last_present_request_ms;
        if (since < interval) {
            if (!m_present_throttle_scheduled) {
                m_present_throttle_scheduled = true;
                int const scheduled_generation = g_wayland_owner_generation();
                QTimer::singleShot(static_cast<int>(interval - since), this, [this, scheduled_generation] {
                    m_present_throttle_scheduled = false;
                    // Stale retry from before a tab switch: drop it. The new
                    // owner's activation issues its own (uncapped) request.
                    if (scheduled_generation != g_wayland_owner_generation())
                        return;
                    requestWaylandRepaint(PresentRequestReason::Retry);
                });
            }
            qt_perf_stats().qwindow_present_requests_coalesced++;
            qt_perf_stats().present_skipped_rate_capped++;
            maybe_log_qt_perf();
            return;
        }
        m_last_present_request_ms = now;
    }
    m_wayland_present_pending = true;
    if (debug_enabled())
        debug_log("wayland_present_requested", m_tab_id,
            QStringLiteral("owner_generation=%1").arg(g_wayland_owner_generation()));
    m_wayland_window->requestServoPresent();
    maybe_log_qt_perf();
}

bool WebContentView::takeWaylandPresentPending()
{
    if (!m_wayland_present_pending)
        return false;
    m_wayland_present_pending = false;
    return true;
}

void WebContentView::notifyThemeChange()
{
    if (!m_webview_created || g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    servoq::forward_theme_change(m_tab_id, is_using_dark_system_theme(*this));
}

// Matches Ladybird WebContentView::set_zoom_level (WebContentView.h:81).
// TODO: forward to webview.set_page_zoom() once per-tab zoom is wired.
void WebContentView::set_zoom_level(double /*zoom_level*/) {}

// Engine creation is deferred until the tab is active so that width()/height()
// carry real layout-assigned values and background tabs never touch the shared
// Wayland wl_surface owned by the currently-visible tab.
bool WebContentView::startEngineIfNeeded()
{
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("startEngineIfNeeded tab_id=%1 webview_created=%2 empty=%3 isActiveTab=%4 owner_tab=%5")
            .arg(m_tab_id)
            .arg(m_webview_created ? 1 : 0)
            .arg(m_empty_new_tab ? 1 : 0)
            .arg(isCurrentlyActiveTab() ? 1 : 0)
            .arg(g_wayland_owner() ? g_wayland_owner()->tabId() : 0));
    if (m_tab_id == 0 || m_webview_created)
        return false;
    if (m_empty_new_tab)
        return false;

    // A background tab must not create a WebView on the shared wl_surface (it
    // corrupts the active tab). Return true so navigate() skips load_url; the URL
    // waits in m_initial_url until this tab activates.
    if (QGuiApplication::platformName() == QStringLiteral("wayland")
        && waylandRendererRequested()
        && !isCurrentlyActiveTab()) {
        debug_log("engine_creation_deferred_background_wayland", m_tab_id, QStringLiteral(""));
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("startEngineIfNeeded DEFER background wayland tab_id=%1").arg(m_tab_id));
        return true;
    }

    m_webview_created = true;

    // Physical pixel dimensions — matches Ladybird update_viewport_size() line 761:
    //   auto scaled_width = int(width() * m_device_pixel_ratio);
    qreal dpr = devicePixelRatioF();
    int pw = qMax(1, static_cast<int>(width() * dpr));
    int ph = qMax(1, static_cast<int>(height() * dpr));

    auto mode = g_renderer_mode();

    auto use_software = [&] {
        debug_log("create_webview", m_tab_id, QSize(pw, ph), dpr);
        auto url_std = m_initial_url.toStdString();
        servoq::create_webview(m_tab_id, url_std.c_str(), pw, ph, static_cast<float>(dpr));
        notifyThemeChange();
    };

    if (mode == RendererMode::WaylandWindow) {
        // Explicit mode: attempt Wayland, allow software GL (Rust will warn), fall back to
        // software only if the renderer cannot initialize at all.
        if (startWaylandRendererIfPossible(pw, ph, dpr, /*allow_software_gl=*/true)) {
            debug_log("create_wayland_webview", m_tab_id, QSize(pw, ph), dpr);
            notifyThemeChange();
            return true;
        }
        use_software();
        return true;
    }

    if (mode == RendererMode::Auto) {
        if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
            if (perf_enabled())
                qInfo() << "[servoq] auto renderer: Qt is not running on Wayland; using software renderer";
        } else if (startWaylandRendererIfPossible(pw, ph, dpr, /*allow_software_gl=*/false)) {
            // Hardware Wayland renderer selected.
            debug_log("create_wayland_webview", m_tab_id, QSize(pw, ph), dpr);
            notifyThemeChange();
            return true;
        }
        // Wayland unavailable or software GL detected — fall through to software.
    }

    // RendererMode::Software, or auto fallback.
    use_software();
    return true;
}

void WebContentView::forwardResizeToEngine()
{
    if (m_tab_id == 0 || !m_webview_created || g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    qreal dpr = devicePixelRatioF();
    int pw = qMax(1, static_cast<int>(width() * dpr));
    int ph = qMax(1, static_cast<int>(height() * dpr));
    QSize physical_size(pw, ph);
    if (m_last_forwarded_physical_size == physical_size && qFuzzyCompare(m_last_forwarded_dpr, dpr))
        return;
    m_last_forwarded_physical_size = physical_size;
    m_last_forwarded_dpr = dpr;

    debug_log("resize", m_tab_id, physical_size, dpr);
    m_frame = {};
    m_pending_frame_repaint = false;
    m_wayland_present_pending = false;
    m_wayland_dirty_after_present = false;
    servoq::forward_resize(m_tab_id, pw, ph, static_cast<float>(dpr));
    servoq::tick_servo();
    if (m_wayland_renderer_active)
        requestWaylandRepaint(PresentRequestReason::Resize);
    else
        update();
}

// Set devicePixelRatio on the image so Qt maps physical pixels to the widget's
// logical rect (avoids a painter.scale trick that breaks on DPR changes).
void WebContentView::paintEvent(QPaintEvent*)
{
    if (m_wayland_renderer_active)
        return;
    qt_perf_stats().software_paints++;
    if (m_empty_new_tab) {
        QPainter painter(this);
        painter.fillRect(rect(), ChromeStyle::chrome_background(palette()));
        painter.setPen(ChromeStyle::chrome_muted_text(palette()));
        auto font = painter.font();
        font.setPointSizeF(font.pointSizeF() + 2.0);
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("ServoQ\nSearch or enter address"));
        return;
    }
    if (!m_frame.isNull() && !m_crashed) {
        m_frame.setDevicePixelRatio(devicePixelRatioF());
        QPainter painter(this);
        painter.drawImage(QPoint(0, 0), m_frame);
        qt_perf_stats().draw_image_calls++;
        m_pending_frame_repaint = false;
        maybe_log_qt_perf();
        return;
    }
    if (m_crashed) {
        QPainter painter(this);
        painter.fillRect(rect(), palette().window());
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(rect(), Qt::AlignCenter,
            QStringLiteral("⚠ Web content crashed\n\n") + m_crash_reason);
    }
    // Before first frame: blank background (Qt default — no painting needed).
}

// resizeEvent — mirrors Ladybird WebContentView::resizeEvent (vendor line 710-714):
//   WebContentViewBase::resizeEvent(event); update_viewport_size();
void WebContentView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // Resync the container on window resize (owner-guarded). Not in
    // forwardResizeToEngine(): that also runs on park/unpark and would un-park.
    if (m_wayland_container && g_wayland_owner() == this)
        updateContainerGeometry();
    forwardResizeToEngine();
}

// leaveEvent — mirrors Ladybird WebContentView::leaveEvent (vendor line 499-510).
void WebContentView::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
}

// Mouse events — coordinate mapping matches Ladybird enqueue_native_event line 1014:
//   position = { event.position().x() * m_device_pixel_ratio, ... }

void WebContentView::mouseMoveEvent(QMouseEvent* event)
{
    if (g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    qreal dpr = devicePixelRatioF();
    float x = static_cast<float>(event->position().x() * dpr);
    float y = static_cast<float>(event->position().y() * dpr);
    servoq::forward_mouse_move(m_tab_id, x, y);
    QWidget::mouseMoveEvent(event);
}

void WebContentView::forwardMouseButton(int action, int button, QMouseEvent* ev)
{
    if (button < 0 || g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    qreal dpr = devicePixelRatioF();
    float x = static_cast<float>(ev->position().x() * dpr);
    float y = static_cast<float>(ev->position().y() * dpr);
    servoq::forward_mouse_button(m_tab_id, action, button, x, y,
        static_cast<uint32_t>(ev->modifiers()));
}

void WebContentView::forwardWindowMouseButton(int action, int button, QMouseEvent* ev)
{
    if (button < 0 || g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    qreal dpr = m_wayland_window ? m_wayland_window->devicePixelRatio() : devicePixelRatioF();
    float x = static_cast<float>(ev->position().x() * dpr);
    float y = static_cast<float>(ev->position().y() * dpr);
    servoq::forward_mouse_button(m_tab_id, action, button, x, y,
        static_cast<uint32_t>(ev->modifiers()));
}

void WebContentView::takeFocusFromContentClick()
{
    bool diag = servoq_diag_enabled();
    if (diag)
        servoq_diag_log(QStringLiteral("takeFocusFromContentClick ENTER tab_id=%1 is_owner=%2 focusBefore=%3")
            .arg(m_tab_id)
            .arg(g_wayland_owner() == this ? 1 : 0)
            .arg(servoq_diag_describe(QApplication::focusWidget())));
    if (auto* focus_widget = QApplication::focusWidget()) {
        if (focus_widget != this && focus_widget != m_wayland_container)
            focus_widget->clearFocus();
    }
    // Keep Qt keyboard focus on this widget, not m_wayland_container: that
    // container is a keyboard dead-end on Wayland and swallows page keystrokes
    // (docs/DEVIATIONS.md §1).
    setFocus(Qt::MouseFocusReason);
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::forward_focus(m_tab_id, true);
    if (diag)
        servoq_diag_log(QStringLiteral("takeFocusFromContentClick EXIT  tab_id=%1 focusAfter=%2 (container=%3) forward_focus(true) sent")
            .arg(m_tab_id)
            .arg(servoq_diag_describe(QApplication::focusWidget()))
            .arg(servoq_diag_describe(m_wayland_container)));
}

bool WebContentView::handleCtrlWheelZoom(QWheelEvent* event)
{
    if ((event->modifiers() & Qt::ControlModifier) == 0) {
        m_ctrl_wheel_zoom_remainder = 0.0;
        return false;
    }
    if (!m_tab)
        return false;

    double angle_delta = event->angleDelta().y();
    double pixel_delta = event->pixelDelta().y();
    double delta = angle_delta != 0.0 ? angle_delta : pixel_delta;
    double threshold = angle_delta != 0.0 ? 120.0 : 80.0;
    if (delta == 0.0) {
        event->accept();
        return true;
    }

    m_ctrl_wheel_zoom_remainder += delta;
    if (std::abs(m_ctrl_wheel_zoom_remainder) < threshold) {
        event->accept();
        return true;
    }

    auto direction = m_ctrl_wheel_zoom_remainder > 0.0 ? 1.0 : -1.0;
    m_ctrl_wheel_zoom_remainder -= direction * threshold;

    if (direction > 0.0)
        m_tab->zoomIn();
    else
        m_tab->zoomOut();

    if (debug_enabled()) {
        qInfo().nospace()
            << "SERVOQ_DEBUG ctrl_wheel_zoom tab_id=" << m_tab_id
            << " direction=" << (direction > 0.0 ? "in" : "out")
            << " delta=" << delta;
    }
    event->accept();
    return true;
}

bool WebContentView::handleMiddleClickLinkFallback(QMouseEvent* event)
{
    if (!m_tab || event->button() != Qt::MiddleButton)
        return false;
    return m_tab->openHoveredLinkInNewTab();
}

// action 0 = Down, 1 = Up  (maps to MouseButtonAction::Down/Up in servo_engine.rs)
// button 0 = Left, 1 = Middle, 2 = Right

void WebContentView::mousePressEvent(QMouseEvent* event)
{
    if (g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    takeFocusFromContentClick();
    int button = -1;
    if (event->button() == Qt::LeftButton)        button = 0;
    else if (event->button() == Qt::MiddleButton) button = 1;
    else if (event->button() == Qt::RightButton)  button = 2;
    if (button >= 0)
        forwardMouseButton(0, button, event);
}

void WebContentView::mouseReleaseEvent(QMouseEvent* event)
{
    if (g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    int button = -1;
    if (event->button() == Qt::LeftButton)        button = 0;
    else if (event->button() == Qt::MiddleButton) button = 1;
    else if (event->button() == Qt::RightButton)  button = 2;
    if (button >= 0)
        forwardMouseButton(1, button, event);
    if (handleMiddleClickLinkFallback(event))
        event->accept();
}

// mouseDoubleClickEvent — mirrors Ladybird (vendor line 591-595):
// Qt sends this instead of a second press → forward as press.
void WebContentView::mouseDoubleClickEvent(QMouseEvent* event)
{
    mousePressEvent(event);
}

// wheelEvent — pixel/angle conversion mirrors Ladybird (vendor lines 1031-1048).
void WebContentView::wheelEvent(QWheelEvent* event)
{
    if (g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    if (handleCtrlWheelZoom(event))
        return;
    double dx = 0.0;
    double dy = 0.0;

    if (auto pixel_delta = -event->pixelDelta(); !pixel_delta.isNull()) {
        dx = pixel_delta.x();
        dy = pixel_delta.y();
    } else {
        auto angle_delta = -event->angleDelta();
        double step_x = static_cast<double>(angle_delta.x()) / 120.0
                        * static_cast<double>(QApplication::wheelScrollLines());
        double step_y = static_cast<double>(angle_delta.y()) / 120.0
                        * static_cast<double>(QApplication::wheelScrollLines());
        static constexpr double scroll_step_size = 40.0;
        dx = step_x * scroll_step_size;
        dy = step_y * scroll_step_size;
    }

    qreal dpr = devicePixelRatioF();
    float x = static_cast<float>(event->position().x() * dpr);
    float y = static_cast<float>(event->position().y() * dpr);
    servoq::forward_wheel(m_tab_id, dx, dy, x, y);
    event->accept();
}

// Key events — mirrors Ladybird keyPressEvent / keyReleaseEvent (vendor 458-477).
void WebContentView::keyPressEvent(QKeyEvent* event)
{
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("WCV::keyPressEvent tab_id=%1 key=0x%2 text='%3' is_owner=%4 focusWidget=%5")
            .arg(m_tab_id)
            .arg(event->key(), 0, 16)
            .arg(event->text())
            .arg(g_wayland_owner() == this ? 1 : 0)
            .arg(servoq_diag_describe(QApplication::focusWidget())));
    if (g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    auto text = event->text();
    uint32_t key_char = text.isEmpty() ? 0u : static_cast<uint32_t>(text[0].unicode());
    servoq::forward_key(m_tab_id, true, key_char,
                        static_cast<int32_t>(event->key()),
                        static_cast<uint32_t>(event->modifiers()));
}

void WebContentView::keyReleaseEvent(QKeyEvent* event)
{
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("WCV::keyReleaseEvent tab_id=%1 key=0x%2")
            .arg(m_tab_id).arg(event->key(), 0, 16));
    if (g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    auto text = event->text();
    uint32_t key_char = text.isEmpty() ? 0u : static_cast<uint32_t>(text[0].unicode());
    servoq::forward_key(m_tab_id, false, key_char,
                        static_cast<int32_t>(event->key()),
                        static_cast<uint32_t>(event->modifiers()));
}

// showEvent/hideEvent must NOT touch the shared Wayland container or ownership —
// that's managed solely by the deferred onBecomeActiveTab/onBecomeInactiveTab.
void WebContentView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    debug_log("show", m_tab_id);
    startEngineIfNeeded();
    forwardResizeToEngine();
    if (m_webview_created && !m_wayland_renderer_active)
        m_engine_tick_timer->start();
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::set_webview_active(m_tab_id, true);
}

void WebContentView::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    debug_log("hide", m_tab_id);
    m_engine_tick_timer->stop();
    // Cancel in-flight presents. g_wayland_owner and container geometry/visibility
    // are updated by TabWidget::activateTab (deferred) — not here.
    if (m_wayland_renderer_active) {
        m_wayland_present_pending = false;
        m_wayland_dirty_after_present = false;
        m_wayland_present_in_progress = false;
    }
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::set_webview_active(m_tab_id, false);
}

// Activation transaction (deferred from TabWidget::activateTab): the only paths
// that change g_wayland_owner or move/show the shared container on a tab switch.
void WebContentView::onBecomeInactiveTab()
{
    NewTabTraceScope trace_scope("WCV_onBecomeInactiveTab", m_tab_id);
    bool was_owner = g_wayland_owner() == this;
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("onBecomeInactiveTab tab_id=%1 was_owner=%2 gen=%3 wayland_active=%4")
            .arg(m_tab_id).arg(was_owner ? 1 : 0).arg(g_wayland_owner_generation()).arg(m_wayland_renderer_active ? 1 : 0));
    debug_log("become_inactive_tab", m_tab_id,
        QStringLiteral("was_owner=%1 wayland_active=%2")
            .arg(was_owner ? 1 : 0)
            .arg(m_wayland_renderer_active ? 1 : 0));

    m_wayland_present_pending = false;
    m_wayland_dirty_after_present = false;
    m_wayland_present_in_progress = false;
    // Forget this stint's present congestion, or the duty-cycle cap delays the
    // tab's FIRST paint when it is re-activated.
    m_last_present_duration_ms = 0;
    m_last_present_request_ms = -1000;

    if (was_owner) {
        g_wayland_owner() = nullptr;
        ++g_wayland_owner_generation();
        if (m_wayland_window)
            m_wayland_window->setOwner(nullptr);
        // Park off-screen — NEVER hide. Hiding unmaps the wl_surface; the
        // first eglSwapBuffers after remapping blocks on a frame callback that
        // never arrives, freezing the Qt main thread. See park_shared_wayland_container.
        park_shared_wayland_container();
    }
    // Leave m_wayland_renderer_active set: only ownership transfers. Clearing it
    // would re-run renderer setup on every return-to-tab and re-trigger the
    // remap/eglSwapBuffers race (§0d).
}

void WebContentView::onBecomeActiveTab()
{
    // Guard against a stale deferred activation.
    if (!isCurrentlyActiveTab()) {
        debug_log("become_active_tab_stale_dropped", m_tab_id,
            QStringLiteral("stack_current=%1")
                .arg(m_tab && m_tab->parentWidget()
                    ? QString::number(
                        qobject_cast<QStackedWidget*>(m_tab->parentWidget())
                            ? qobject_cast<QStackedWidget*>(m_tab->parentWidget())->currentIndex()
                            : -1)
                    : QStringLiteral("no_parent")));
        return;
    }

    // Internal page (servoq://) showing: keep the Servo surface released/unmapped
    // so the native Qt page stays visible; don't attach or present.
    if (m_internal_page_active) {
        setInternalPageActive(true);
        debug_log("become_active_tab_internal_page", m_tab_id);
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
    debug_log("become_active_tab", m_tab_id,
        QStringLiteral("wayland_active=%1 webview_created=%2")
            .arg(m_wayland_renderer_active ? 1 : 0)
            .arg(m_webview_created ? 1 : 0));
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("onBecomeActiveTab tab_id=%1 wayland_active=%2 webview_created=%3 owner_tab=%4 gen=%5")
            .arg(m_tab_id).arg(m_wayland_renderer_active ? 1 : 0).arg(m_webview_created ? 1 : 0)
            .arg(g_wayland_owner() ? g_wayland_owner()->tabId() : 0).arg(g_wayland_owner_generation()));

    NewTabTraceScope trace_scope("WCV_onBecomeActiveTab", m_tab_id);

    // Start engine for background-deferred Wayland tabs that haven't been
    // created yet. After this call, m_webview_created and m_wayland_renderer_active
    // reflect the real state.
    {
        NewTabTraceScope scope("startEngineIfNeeded", m_tab_id);
        startEngineIfNeeded();
    }
    {
        NewTabTraceScope scope("forwardResizeToEngine", m_tab_id);
        forwardResizeToEngine();
    }

    if (m_wayland_renderer_active && m_wayland_container) {
        // Normal case: re-attach the parked container to this tab. The container
        // was moved off-screen by onBecomeInactiveTab but never unmapped, so
        // updateContainerGeometry + show() repositions it without any surface remap.
        {
            NewTabTraceScope scope("attachSharedWaylandWindow", m_tab_id);
            if (!attachSharedWaylandWindow()) {
                debug_log("become_active_tab_return_attach_failed", m_tab_id,
                    QStringLiteral("owner=%1").arg(g_wayland_owner() ? g_wayland_owner()->tabId() : 0));
                return;
            }
        }
        {
            NewTabTraceScope scope("container_show_raise", m_tab_id);
            updateContainerGeometry();
            m_wayland_container->show(); // no-op if already visible; surface stays mapped
            m_wayland_container->raise();
            if (m_wayland_window) {
                m_wayland_window->show();
                m_wayland_window->create();
            }
        }
        // Queue the toplevel repaint before the first present: subsurface state is
        // parent state, and a present racing ahead of the commit can block on a
        // buffer release only that commit unlocks (§0d).
        {
            NewTabTraceScope scope("toplevel_update", m_tab_id);
            if (auto* toplevel = m_wayland_container->window())
                toplevel->update();
        }
        {
            NewTabTraceScope scope("requestWaylandRepaint_activation", m_tab_id);
            requestWaylandRepaint(PresentRequestReason::Activation);
        }
        debug_log("become_active_tab_container_shown", m_tab_id,
            QStringLiteral("geom=%1,%2 %3x%4 gen=%5")
                .arg(m_wayland_container->x()).arg(m_wayland_container->y())
                .arg(m_wayland_container->width()).arg(m_wayland_container->height())
                .arg(g_wayland_owner_generation()));
    } else if (!m_webview_created && g_wayland_container()) {
        // Empty/not-yet-started tab. Park the container so no stale content
        // from the previous tab is visible. Park (not hide) to keep the surface
        // mapped — see park_shared_wayland_container.
        if (g_wayland_owner()) {
            g_wayland_owner()->m_wayland_present_pending = false;
            g_wayland_owner()->m_wayland_dirty_after_present = false;
            g_wayland_owner()->m_wayland_present_in_progress = false;
        }
        g_wayland_owner() = nullptr;
        ++g_wayland_owner_generation();
        if (g_wayland_window())
            g_wayland_window()->setOwner(nullptr);
        park_shared_wayland_container();
        // Empty tab presents nothing, so a parked-but-mapped subsurface freezes
        // chrome animations; unmap it (see helper, §0d).
        unmap_shared_servo_subsurface("empty_tab_servo_subsurface_unmapped");
        debug_log("become_active_tab_container_parked", m_tab_id, QStringLiteral(""));
    } else if (m_webview_created && !m_wayland_renderer_active) {
        debug_log("become_active_tab_loaded_non_wayland", m_tab_id, QStringLiteral(""));
        update();
    }

    if (m_webview_created && !m_wayland_renderer_active)
        m_engine_tick_timer->start();

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    debug_log("become_active_tab_done", m_tab_id,
        QStringLiteral("elapsed_us=%1").arg(elapsed_us));
    if (elapsed_us > 50000)
        qWarning().nospace()
            << "SERVOQ_WARN onBecomeActiveTab slow: tab_id=" << m_tab_id
            << " elapsed_us=" << elapsed_us;
}

// focusInEvent / focusOutEvent — mirrors Ladybird (vendor lines 646-652).
void WebContentView::focusInEvent(QFocusEvent* event)
{
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("WCV::focusInEvent tab_id=%1 reason=%2 -> forward_focus(true)")
            .arg(m_tab_id).arg(static_cast<int>(event->reason())));
    QWidget::focusInEvent(event);
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::forward_focus(m_tab_id, true);
}

void WebContentView::focusOutEvent(QFocusEvent* event)
{
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("WCV::focusOutEvent tab_id=%1 reason=%2 -> forward_focus(false)")
            .arg(m_tab_id).arg(static_cast<int>(event->reason())));
    QWidget::focusOutEvent(event);
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::forward_focus(m_tab_id, false);
}

// event() — ensures Tab key reaches keyPressEvent instead of Qt focus navigation;
// handles DevicePixelRatioChange for multi-monitor DPI transitions;
// mirrors Ladybird WebContentView::event() (vendor 964-1009).
bool WebContentView::event(QEvent* ev)
{
    if (servoq_diag_enabled() && (ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease))
        servoq_diag_log(QStringLiteral("WCV::event intercept type=%1 tab_id=%2")
            .arg(ev->type() == QEvent::KeyPress ? QStringLiteral("KeyPress") : QStringLiteral("KeyRelease"))
            .arg(m_tab_id));
    if (g_servo_shutting_down().load(std::memory_order_acquire))
        return QWidget::event(ev);
    if (ev->type() == QEvent::KeyPress) {
        keyPressEvent(static_cast<QKeyEvent*>(ev));
        return true;
    }
    if (ev->type() == QEvent::KeyRelease) {
        keyReleaseEvent(static_cast<QKeyEvent*>(ev));
        return true;
    }
    if (ev->type() == QEvent::DevicePixelRatioChange) {
        forwardResizeToEngine(); // sends new physical size + new hidpi scale factor
    }
    // Software-rendering path: pinch gestures land on the widget when no
    // embedded Wayland QWindow exists (the Wayland path handles them in
    // ServoWaylandContentWindow::event above).
    if (ev->type() == QEvent::NativeGesture) {
        auto const& gesture = *static_cast<QNativeGestureEvent const*>(ev);
        if (gesture.gestureType() == Qt::ZoomNativeGesture && m_webview_created) {
            qreal dpr = devicePixelRatioF();
            auto pos = gesture.position();
            servoq::forward_pinch_zoom(m_tab_id,
                static_cast<float>(1.0 + gesture.value()),
                static_cast<float>(pos.x() * dpr),
                static_cast<float>(pos.y() * dpr));
            return true;
        }
    }
    if (ev->type() == QEvent::PaletteChange || ev->type() == QEvent::ApplicationPaletteChange || ev->type() == QEvent::ThemeChange) {
        QTimer::singleShot(0, this, [this] {
            notifyThemeChange();
            update();
        });
    }
    return QWidget::event(ev);
}

} // namespace ServoQ

// servoq::* callback implementations (declared in servo_callbacks.h, called from
// Rust via the CXX bridge); they use g_view_registry() in this TU.

namespace servoq {

static ServoQ::WebContentView* find_view(::std::int32_t tab_id)
{
    return g_view_registry().value(static_cast<int>(tab_id), nullptr);
}

bool servo_shutdown_started()
{
    return g_servo_shutting_down().load(std::memory_order_acquire);
}

void begin_servo_shutdown()
{
    bool expected = false;
    if (!g_servo_shutting_down().compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    g_servo_wake_pending().store(false, std::memory_order_release);
    for (auto* view : g_view_registry()) {
        if (view)
            view->requestWaylandRepaint(ServoQ::WebContentView::PresentRequestReason::Shutdown);
    }
    servoq::shutdown_servo();
}

void deliver_frame(::std::int32_t tab_id,
                   ::rust::Slice<const ::std::uint8_t> bytes,
                   ::std::int32_t width,
                   ::std::int32_t height)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    if (!view) {
        debug_log("ignored_frame_missing_view", tab_id);
        return;
    }
    // Skip frames for hidden tabs: set_throttled is only advisory, so without this
    // a hidden tab still delivers frames and can race the visible tab's render.
    if (!view->isVisible()) {
        debug_log("skipped_frame_hidden_view", tab_id);
        return;
    }
    view->receiveFrameBytes(bytes.data(), width, height);
}

void notify_url_changed(::std::int32_t tab_id, ::rust::Str url)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    auto text = QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()));
    if (!view || !view->tab()) {
        debug_log("ignored_url_changed_missing_view", tab_id, text);
        return;
    }
    debug_log("notify_url_changed", tab_id, text);
    view->tab()->on_url_change(text);
}

void notify_title_changed(::std::int32_t tab_id, ::rust::Str title)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    auto text = QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size()));
    if (!view || !view->tab()) {
        debug_log("ignored_title_changed_missing_view", tab_id, text);
        return;
    }
    debug_log("notify_title_changed", tab_id, text);
    view->tab()->on_title_change(text);
}

void notify_load_started(::std::int32_t tab_id, ::rust::Str url)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    auto text = QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()));
    if (!view || !view->tab()) {
        debug_log("ignored_load_started_missing_view", tab_id, text);
        return;
    }
    debug_log("notify_load_started", tab_id, text);
    view->tab()->on_load_start(text);
}

void notify_load_finished(::std::int32_t tab_id)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    if (!view || !view->tab()) {
        debug_log("ignored_load_finished_missing_view", tab_id);
        return;
    }
    debug_log("notify_load_finished", tab_id);
    view->tab()->on_load_finish();
    ServoQ::start_favicon_probe(view);
}

void notify_pdf_navigation_requested(::std::int32_t tab_id, ::rust::Str url)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    auto text = QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()));
    if (!view || !view->tab()) {
        debug_log("ignored_pdf_navigation_missing_view", tab_id, text);
        return;
    }
    QPointer<ServoQ::Tab> tab = view->tab();
    QTimer::singleShot(0, view, [tab, text] {
        if (!tab)
            return;
        tab->navigate(ServoQ::InternalPageView::urlForPdfSource(text));
    });
}

void notify_status_changed(::std::int32_t tab_id, ::rust::Str text)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    auto status = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    if (!view || !view->tab()) {
        debug_log("ignored_status_changed_missing_view", tab_id, status);
        return;
    }
    debug_log("notify_status_changed", tab_id, status);
    view->tab()->on_link_hover(status);
}

void notify_webview_crashed(::std::int32_t tab_id, ::rust::Str reason)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    if (!view) {
        debug_log("ignored_crashed_missing_view", tab_id);
        return;
    }
    auto text = QString::fromUtf8(reason.data(), static_cast<qsizetype>(reason.size()));
    debug_log("notify_webview_crashed", tab_id, text);
    view->receiveWebViewCrash(text);
}

void notify_request_blocked(::std::int32_t tab_id, ::rust::Str url)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    auto text = QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()));
    if (!view) {
        debug_log("ignored_request_blocked_missing_view", tab_id, text);
        return;
    }
    debug_log("request_blocked", tab_id, text);
    view->receiveRequestBlocked(text);
}

bool content_blocking_enabled()
{
    return ServoQ::Settings::the()->content_blocking_enabled();
}

bool content_blocking_host_allowlisted(::rust::Str host)
{
    auto host_string = QString::fromUtf8(host.data(), static_cast<qsizetype>(host.size()));
    return ServoQ::Settings::the()->content_blocking_disabled_for_host(host_string);
}

bool webcontent_frame_pending(::std::int32_t tab_id)
{
    if (servo_shutdown_started())
        return true;
    auto* view = find_view(tab_id);
    return view && view->hasPendingFrameRepaint();
}

void request_wayland_window_repaint(::std::int32_t tab_id)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    if (view)
        view->requestWaylandRepaint(ServoQ::WebContentView::PresentRequestReason::FrameReady);
}

// Called from Servo background threads (QtEventLoopWaker::wake): posts a thread-safe
// event that BrowserWindow::eventFilter turns into a tick on the main thread.
void servoq_wake_event_loop()
{
    if (servo_shutdown_started())
        return;
    bool expected = false;
    qt_perf_stats().wake_events++;
    if (!g_servo_wake_pending().compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        qt_perf_stats().wake_events_coalesced++;
        maybe_log_qt_perf();
        return;
    }

    static constexpr QEvent::Type ServoWakeType = QEvent::Type(QEvent::User + 1);
    QCoreApplication::postEvent(qApp, new QEvent(ServoWakeType));
    maybe_log_qt_perf();
}

void mark_servo_wake_event_consumed()
{
    g_servo_wake_pending().store(false, std::memory_order_release);
}

static ServoQ::BrowserWindow* find_browser_window()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = dynamic_cast<ServoQ::BrowserWindow*>(widget))
            return window;
    }
    return nullptr;
}

void notify_favicon_changed(::std::int32_t tab_id,
                            ::rust::Slice<const ::std::uint8_t> data,
                            ::std::int32_t width,
                            ::std::int32_t height)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    if (!view || !view->tab())
        return;
    if (width <= 0 || height <= 0 || data.empty()) {
        debug_log_favicon(tab_id,
            QStringLiteral("page_url=%1 source=servo decoded_output=empty action=clear").arg(view->tab()->url()));
        view->tab()->on_favicon_change({});
        return;
    }
    debug_log_favicon(tab_id,
        QStringLiteral("page_url=%1 source=servo input_bytes=%2 detected_input_format=servo-decoded-rgba decoded_output=%3x%4")
            .arg(view->tab()->url())
            .arg(data.size())
            .arg(width)
            .arg(height));
    QImage img(data.data(), width, height, QImage::Format_RGBA8888);
    ServoQ::apply_favicon_bitmap(view, img.copy());
}

void notify_cursor_changed(::std::int32_t tab_id, ::std::int32_t cursor_shape)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    if (!view)
        return;
    auto qt_shape = cursor_shape_from_servoq_code(cursor_shape);
    auto qt_cursor = QCursor(qt_shape);
    view->setCursor(qt_cursor);
    bool applied_container = false;
    bool applied_window = false;
    if (auto* container = g_wayland_container()) {
        container->setCursor(qt_cursor);
        applied_container = true;
    }
    if (auto* window = g_wayland_window()) {
        window->setCursor(qt_cursor);
        applied_window = true;
    }
    if (debug_enabled()) {
        qInfo().nospace()
            << "SERVOQ_DEBUG notify_cursor_changed tab_id=" << tab_id
            << " raw_cursor_code=" << cursor_shape
            << " servo_cursor=" << cursor_code_name(cursor_shape)
            << " qt_cursor=" << qt_cursor_name(qt_shape)
            << " apply_webcontent=1"
            << " apply_wayland_container=" << (applied_container ? 1 : 0)
            << " apply_wayland_window=" << (applied_window ? 1 : 0);
    }
}

void notify_fullscreen_changed(::std::int32_t tab_id, bool fullscreen)
{
    if (servo_shutdown_started())
        return;
    if (auto* window = find_browser_window())
        window->setFullscreen(fullscreen);
    (void)tab_id;
}

void notify_history_changed(::std::int32_t tab_id, ::rust::Str urls, ::std::int32_t current)
{
    if (servo_shutdown_started())
        return;
    auto* view = find_view(tab_id);
    if (!view || !view->tab())
        return;
    auto url_str = QString::fromUtf8(urls.data(), static_cast<qsizetype>(urls.size()));
    auto url_list = url_str.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    view->tab()->on_history_changed(url_list, static_cast<int>(current));
}

void request_open_tab_for_id(::std::int32_t tab_id)
{
    if (servo_shutdown_started())
        return;
    debug_log("popup_new_webview", tab_id);
    if (auto* window = find_browser_window())
        window->openTabForExistingId(static_cast<int>(tab_id));
}

::std::int32_t show_context_menu_sync(::std::int32_t tab_id, ::rust::Str items_str, ::rust::Str link_url_raw)
{
    if (servo_shutdown_started())
        return -1;
    auto* view = find_view(tab_id);
    if (!view)
        return -1;

    auto link_url = QString::fromUtf8(link_url_raw.data(), static_cast<qsizetype>(link_url_raw.size()));
    auto items_text = QString::fromUtf8(items_str.data(), static_cast<qsizetype>(items_str.size()));
    auto lines = items_text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    // No parent: prevents double-free if view's Tab is deleted during menu.exec() nested event loop.
    QMenu menu;
    QMap<QAction*, int> action_map;
    QAction* copy_link_action = nullptr;

    for (auto const& line : lines) {
        if (line.trimmed() == QStringLiteral("sep")) {
            menu.addSeparator();
            continue;
        }
        auto parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 3)
            continue;
        bool ok = false;
        int action_id = parts[0].toInt(&ok);
        if (!ok)
            continue;
        // Skip Servo's native CopyLink (action_id 3) when the link URL is known;
        // the Qt-side "Copy link" action below handles the clipboard directly.
        if (!link_url.isEmpty() && action_id == 3)
            continue;
        auto label = parts[1];
        bool enabled = (parts[2].trimmed() != QStringLiteral("false"));
        auto* act = menu.addAction(label);
        act->setEnabled(enabled);
        action_map[act] = action_id;
        // Insert "Copy link" immediately after "Open link in new tab" (action_id 4).
        if (!link_url.isEmpty() && action_id == 4 && !copy_link_action)
            copy_link_action = menu.addAction(QObject::tr("Copy Link"));
    }

    if (!link_url.isEmpty() && !copy_link_action) {
        // "Open link in new tab" was not in the list; place "Copy link" first.
        copy_link_action = new QAction(QObject::tr("Copy Link"), &menu);
        auto existing = menu.actions();
        menu.insertAction(existing.isEmpty() ? nullptr : existing.first(), copy_link_action);
    }

    auto* selected = menu.exec(QCursor::pos());
    if (!selected)
        return -1;
    if (selected == copy_link_action) {
        QApplication::clipboard()->setText(link_url);
        return -1;
    }
    if (!action_map.contains(selected))
        return -1;
    return action_map[selected];
}

// window.screen.* backing data in device pixels. On Wayland the compositor hides
// global window positions, so window_x/window_y are whatever Qt reports (~0,0).
ScreenGeometryResult get_screen_geometry(::std::int32_t tab_id)
{
    ScreenGeometryResult result;
    result.valid = false;
    if (servo_shutdown_started())
        return result;
    auto* view = find_view(tab_id);
    if (!view || !view->window())
        return result;
    auto* screen = view->screen();
    if (!screen)
        return result;

    auto dpr = view->devicePixelRatioF();
    auto screen_size = screen->geometry().size();
    auto available_size = screen->availableGeometry().size();
    auto frame = view->window()->frameGeometry();

    result.valid = true;
    result.screen_width = qRound(screen_size.width() * dpr);
    result.screen_height = qRound(screen_size.height() * dpr);
    result.available_width = qRound(available_size.width() * dpr);
    result.available_height = qRound(available_size.height() * dpr);
    result.window_x = qRound(frame.x() * dpr);
    result.window_y = qRound(frame.y() * dpr);
    result.window_width = qRound(frame.width() * dpr);
    result.window_height = qRound(frame.height() * dpr);
    return result;
}

namespace {

// window.moveTo/resizeTo policy: like Firefox and Chrome, page content may
// only move/resize a popup-style window — one with a single tab. The main
// browser window with multiple tabs never honors these requests.
ServoQ::BrowserWindow* window_controllable_by_page(::std::int32_t tab_id)
{
    auto* view = find_view(tab_id);
    if (!view)
        return nullptr;
    auto* browser = dynamic_cast<ServoQ::BrowserWindow*>(view->window());
    if (!browser || browser->tabCount() != 1)
        return nullptr;
    return browser;
}

} // namespace

void request_window_move_to(::std::int32_t tab_id, ::std::int32_t x, ::std::int32_t y)
{
    if (servo_shutdown_started())
        return;
    auto* browser = window_controllable_by_page(tab_id);
    if (!browser)
        return;
    auto dpr = browser->devicePixelRatioF();
    QPoint target(qRound(x / dpr), qRound(y / dpr));
    // Deferred: this is called from inside a Servo delegate callback.
    QTimer::singleShot(0, browser, [browser, target] { browser->move(target); });
}

void request_window_resize_to(::std::int32_t tab_id, ::std::int32_t width, ::std::int32_t height)
{
    if (servo_shutdown_started())
        return;
    auto* browser = window_controllable_by_page(tab_id);
    if (!browser)
        return;
    auto dpr = browser->devicePixelRatioF();
    // Servo requests the OUTER size (with decorations); Qt resizes the client
    // area, so subtract the frame overhead. Clamp to the available screen as
    // the delegate contract suggests, and to a sane minimum.
    QSize outer(qRound(width / dpr), qRound(height / dpr));
    QSize frame_overhead = browser->frameGeometry().size() - browser->geometry().size();
    QSize inner = (outer - frame_overhead).expandedTo(QSize(200, 100));
    if (auto* screen = browser->screen())
        inner = inner.boundedTo(screen->availableGeometry().size());
    QTimer::singleShot(0, browser, [browser, inner] { browser->resize(inner); });
}

// Screenshot result from servoq::take_screenshot. Deep-copies the pixels into
// a QImage, then defers the save dialog so the Servo tick that delivered the
// callback unwinds first.
void notify_screenshot_taken(::std::int32_t tab_id,
                             ::rust::Slice<const ::std::uint8_t> data,
                             ::std::int32_t width,
                             ::std::int32_t height)
{
    if (servo_shutdown_started())
        return;
    auto* window = find_browser_window();
    if (!window)
        return;

    if (data.empty() || width <= 0 || height <= 0) {
        QTimer::singleShot(0, window, [window] {
            QMessageBox::warning(window, QStringLiteral("Screenshot"),
                QStringLiteral("The screenshot could not be captured."));
        });
        return;
    }

    // copy() detaches from the Rust-owned buffer, which is only valid for
    // the duration of this call.
    QImage screenshot(reinterpret_cast<uchar const*>(data.data()),
        static_cast<int>(width), static_cast<int>(height),
        static_cast<qsizetype>(width) * 4, QImage::Format_RGBA8888);
    auto image = screenshot.copy();

    QTimer::singleShot(0, window, [window, image] {
        auto timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HHmmss"));
        auto file_name = QStringLiteral("Screenshot %1.png").arg(timestamp);
        auto pictures_dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (pictures_dir.isEmpty())
            pictures_dir = QDir::homePath();
        auto path = QFileDialog::getSaveFileName(window,
            QStringLiteral("Save Screenshot"),
            pictures_dir + QLatin1Char('/') + file_name,
            QStringLiteral("PNG Image (*.png)"));
        if (path.isEmpty())
            return;
        if (!image.save(path, "PNG"))
            QMessageBox::warning(window, QStringLiteral("Screenshot"),
                QStringLiteral("Could not save the screenshot to %1.").arg(path));
    });
}

// ---- JS evaluation (debug tooling, M3.7) ----------------------------------
// Pending callbacks keyed by request id; filled by
// ServoQ::evaluate_javascript_in_tab, drained by notify_javascript_result.

static QHash<uint64_t, std::function<void(bool, QString const&)>>& g_js_result_handlers()
{
    static QHash<uint64_t, std::function<void(bool, QString const&)>> handlers;
    return handlers;
}

void notify_javascript_result(::std::int32_t tab_id, ::std::uint64_t request_id, bool success, ::rust::Str result)
{
    if (servo_shutdown_started())
        return;
    auto handler = g_js_result_handlers().take(request_id);
    if (handler)
        handler(success, QString::fromUtf8(result.data(), static_cast<qsizetype>(result.size())));
    (void)tab_id;
}

// QClipboard-backed system clipboard for Servo's ClipboardDelegate. Reads and
// writes go through Qt so the whole process shares one clipboard connection
// (Servo's default arboard delegate would open a second Wayland data channel).
void clipboard_clear()
{
    if (auto* clipboard = QGuiApplication::clipboard())
        clipboard->clear();
}

rust::String clipboard_get_text()
{
    if (auto* clipboard = QGuiApplication::clipboard())
        return clipboard->text().toStdString();
    return {};
}

void clipboard_set_text(rust::Str text)
{
    if (auto* clipboard = QGuiApplication::clipboard())
        clipboard->setText(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
}

void show_notification(int32_t /*tab_id*/, rust::Str title, rust::Str body)
{
    static QSystemTrayIcon* s_tray = nullptr;
    if (!s_tray) {
        s_tray = new QSystemTrayIcon(QApplication::windowIcon(), nullptr);
        s_tray->show();
    }
    s_tray->showMessage(
        QString::fromUtf8(title.data(), static_cast<int>(title.size())),
        QString::fromUtf8(body.data(), static_cast<int>(body.size())),
        QSystemTrayIcon::Information, 5000);
}

} // namespace servoq

namespace ServoQ {

// Defined after the servoq namespace so it can use the handler registry that
// notify_javascript_result drains (same translation unit).
void evaluate_javascript_in_tab(int tab_id, QString const& script,
    std::function<void(bool, QString const&)> callback)
{
    static uint64_t s_next_request_id = 1;
    auto request_id = s_next_request_id++;
    servoq::g_js_result_handlers().insert(request_id, std::move(callback));
    servoq::evaluate_javascript(tab_id, request_id, script.toStdString());
}

} // namespace ServoQ
