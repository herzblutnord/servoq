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

#include "ui/BrowserWindow.h"
#include "DebugFlags.h"
#include "storage/BookmarkStore.h"
#include "ui/ChromeStyle.h"
#include "engine/Favicon.h"
#include "ui/InternalPageView.h"
#include "engine/NewTabTrace.h"
#include "engine/QtPerfStats.h"
#include "engine/WebContentView.h"
#include "storage/Settings.h"
#include "ui/Tab.h"
#include "engine/servo_callbacks.h"
#include "servoq/src/bridge.rs.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QImage>
#include <QInputDevice>
#include <QKeyEvent>
#include <QMap>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QDebug>
#include <QResizeEvent>
#include <QStyleHints>
#include <QTabletEvent>
#include <QTimer>
#include <QTouchEvent>
#include <QWheelEvent>
#include <QStackedWidget>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <QtGui/qpa/qplatformwindow_p.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ServoQ {

namespace {

// Maps tab_id -> WebContentView* so the servo_callbacks.cpp FFI layer can locate
// its widget (via WebContentView::findByTabId).
QMap<int, WebContentView*>& g_view_registry()
{
    static QMap<int, WebContentView*> s_registry;
    return s_registry;
}

enum class RendererMode { Auto, Software, WaylandWindow };

RendererMode parse_renderer_mode()
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

RendererMode g_renderer_mode()
{
    static RendererMode mode = parse_renderer_mode();
    return mode;
}

// Monotonic ms clock for the present duty-cycle cap (main-thread only).
qint64 present_clock_ms()
{
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer.elapsed();
}

// Cap presents at the display refresh rate; faster is wasted main-thread work.
qint64 min_present_interval_ms()
{
    qreal hz = 60.0;
    if (auto* screen = QGuiApplication::primaryScreen()) {
        if (screen->refreshRate() > 1.0)
            hz = screen->refreshRate();
    }
    return qMax<qint64>(1, static_cast<qint64>(1000.0 / hz));
}

ServoWaylandContentWindow*& g_wayland_window()
{
    static ServoWaylandContentWindow* s_window { nullptr };
    return s_window;
}

QWidget*& g_wayland_container()
{
    static QWidget* s_container { nullptr };
    return s_container;
}

WebContentView*& g_wayland_owner()
{
    static WebContentView* s_owner { nullptr };
    return s_owner;
}

int& g_wayland_owner_generation()
{
    static int s_generation { 0 };
    return s_generation;
}

// Commit the toplevel wl_surface directly so pending subsurface moves apply now
// and Qt's frame callback fires, un-stalling chrome repaints (docs/DEVIATIONS.md
// §0d). Safe from here: Qt stages/commits its own toplevel state on this thread.
// SERVOQ_NO_DIRECT_WL_COMMIT=1 disables for diagnosis.
bool direct_wl_commit_disabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_NO_DIRECT_WL_COMMIT");
    return v;
}

void commit_toplevel_wl_surface(QWidget* widget_in_window, char const* trace_marker)
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
    newtab_trace_point(trace_marker);
}

void park_shared_wayland_container()
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
    newtab_trace_point("park_shared_wayland_container_toplevel_update_queued");
    // The queued update above is NOT sufficient on its own: Qt's repaint can be
    // stalled on a starved frame callback (see commit_toplevel_wl_surface).
    commit_toplevel_wl_surface(container, "park_toplevel_wl_surface_committed");
}

void debug_log(char const* event, int tab_id, QSize const& size, qreal dpr)
{
    if (debug_enabled()) {
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " tab_id=" << tab_id
                          << " physical=" << size.width() << "x" << size.height()
                          << " dpr=" << dpr;
    }
}

bool is_using_dark_system_theme(QWidget const& widget)
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

} // namespace

// ────────────────────────────────────────────────────────────────────────────
// ServoQ::WebContentView implementation
// ────────────────────────────────────────────────────────────────────────────

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
        if (m_owner && !servoq::servo_shutdown_started()) {
            if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
                || event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
                if (m_owner->forwardTouchEvent(static_cast<QTouchEvent*>(event), devicePixelRatio()))
                    return true;
            }
            if (event->type() == QEvent::TabletPress || event->type() == QEvent::TabletMove
                || event->type() == QEvent::TabletRelease) {
                if (m_owner->forwardTabletEvent(static_cast<QTabletEvent*>(event), devicePixelRatio()))
                    return true;
            }
        }
        // Touchpad pinch arrives on the embedded QWindow (it has pointer
        // focus while the cursor is over the page). Mirrors Ladybird's
        // ZoomNativeGesture handling; Servo applies the magnification.
        if (event->type() == QEvent::NativeGesture) {
            auto const& gesture = *static_cast<QNativeGestureEvent const*>(event);
            if (gesture.gestureType() == Qt::ZoomNativeGesture && m_owner
                && !servoq::servo_shutdown_started()) {
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
            if (servoq::servo_shutdown_started())
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
        if (isExposed() && m_owner && m_owner->waylandRendererActive() && !servoq::servo_shutdown_started())
            m_owner->requestWaylandRepaint(WebContentView::PresentRequestReason::Expose);
        maybe_log_qt_perf();
    }

    void resizeEvent(QResizeEvent*) override
    {
        if (m_owner && !servoq::servo_shutdown_started())
            m_owner->forwardResizeToEngine();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!m_owner || servoq::servo_shutdown_started())
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
        if (m_owner && !servoq::servo_shutdown_started()) {
            m_owner->takeFocusFromContentClick();
            m_owner->forwardWindowMouseButton(0, qtMouseButtonToServo(event->button()), event);
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_owner && !servoq::servo_shutdown_started()) {
            m_owner->forwardWindowMouseButton(1, qtMouseButtonToServo(event->button()), event);
            if (m_owner->handleMiddleClickLinkFallback(event))
                event->accept();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        // QWindow already sends the second press before this event.
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (!m_owner || servoq::servo_shutdown_started())
            return;
        if (m_owner->handleCtrlWheelZoom(event))
            return;
        double dx = 0.0;
        double dy = 0.0;
        auto pixel_delta = -event->pixelDelta();
        auto* device = event->pointingDevice();
        if (!pixel_delta.isNull() && device && device->type() == QInputDevice::DeviceType::TouchPad) {
            dx = pixel_delta.x();
            dy = pixel_delta.y();
        } else if (auto angle_delta = -event->angleDelta(); !angle_delta.isNull()) {
            double step_x = static_cast<double>(angle_delta.x()) / 120.0
                            * static_cast<double>(QApplication::wheelScrollLines());
            double step_y = static_cast<double>(angle_delta.y()) / 120.0
                            * static_cast<double>(QApplication::wheelScrollLines());
            static constexpr double scroll_step_size = 40.0;
            dx = step_x * scroll_step_size;
            dy = step_y * scroll_step_size;
        } else {
            dx = pixel_delta.x();
            dy = pixel_delta.y();
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
        if (!m_owner || servoq::servo_shutdown_started())
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
        if (!m_owner || servoq::servo_shutdown_started())
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
        if (m_owner && !servoq::servo_shutdown_started())
            servoq::forward_focus(m_owner->tabId(), true);
    }

    void focusOutEvent(QFocusEvent*) override
    {
        if (servoq_diag_enabled())
            servoq_diag_log(QStringLiteral("SWCW::focusOutEvent owner_tab=%1").arg(m_owner ? m_owner->tabId() : 0));
        if (m_owner && !servoq::servo_shutdown_started())
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
    setAttribute(Qt::WA_AcceptTouchEvents, true);

    // Empty new tabs and crash states are painted directly by this widget; real
    // pages are handed to Servo on first navigation.

    // Fallback tick timer: a safety net in case a Servo wake event is dropped or
    // coalesced (QtEventLoopWaker normally wakes the loop immediately).
    m_engine_tick_timer->setInterval(200);
    m_engine_tick_timer->setSingleShot(false);
    connect(m_engine_tick_timer, &QTimer::timeout, this, [this] {
        if (servoq::servo_shutdown_started())
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
        if (!servoq::servo_shutdown_started())
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

// static
QList<WebContentView*> WebContentView::allViews()
{
    return g_view_registry().values();
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

// static
QWindow* WebContentView::sharedWaylandWindow()
{
    return g_wayland_window();
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
    if (debug_enabled()) {
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

void WebContentView::receiveFrameBytes(uint8_t const* bytes, int width, int height)
{
    if (m_wayland_renderer_active || servoq::servo_shutdown_started())
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
    if (!m_wayland_renderer_active || !m_wayland_window || servoq::servo_shutdown_started()) {
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
    if (!m_webview_created || servoq::servo_shutdown_started())
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
    if (m_tab_id == 0 || !m_webview_created || servoq::servo_shutdown_started())
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

bool WebContentView::forwardTouchEvent(QTouchEvent* event, qreal dpr)
{
    if (!m_webview_created || m_tab_id == 0)
        return false;

    for (auto const& point : event->points())
        m_active_touch_points[point.id()] = point.position();

    if (event->type() == QEvent::TouchCancel) {
        for (auto it = m_active_touch_points.cbegin(); it != m_active_touch_points.cend(); ++it) {
            servoq::forward_touch(m_tab_id, 3, it.key(),
                static_cast<float>(it.value().x() * dpr),
                static_cast<float>(it.value().y() * dpr), 1);
        }
        m_active_touch_points.clear();
        event->accept();
        return true;
    }

    bool forwarded = false;
    for (auto const& point : event->points()) {
        int event_type = -1;
        switch (point.state()) {
        case QEventPoint::State::Pressed:
            event_type = 0;
            break;
        case QEventPoint::State::Updated:
            event_type = 1;
            break;
        case QEventPoint::State::Released:
            event_type = 2;
            break;
        case QEventPoint::State::Stationary:
        case QEventPoint::State::Unknown:
            break;
        }
        if (event_type < 0)
            continue;
        servoq::forward_touch(m_tab_id, event_type, point.id(),
            static_cast<float>(point.position().x() * dpr),
            static_cast<float>(point.position().y() * dpr), 1);
        if (event_type == 2)
            m_active_touch_points.remove(point.id());
        forwarded = true;
    }
    if (forwarded)
        event->accept();
    return forwarded;
}

bool WebContentView::forwardTabletEvent(QTabletEvent* event, qreal dpr)
{
    if (!m_webview_created || m_tab_id == 0)
        return false;
    int event_type = -1;
    if (event->type() == QEvent::TabletPress) {
        event_type = 0;
        m_pen_active = true;
    } else if (event->type() == QEvent::TabletMove && m_pen_active) {
        event_type = 1;
    } else if (event->type() == QEvent::TabletRelease && m_pen_active) {
        event_type = 2;
        m_pen_active = false;
    }
    if (event_type < 0)
        return false;
    servoq::forward_touch(m_tab_id, event_type, 0,
        static_cast<float>(event->position().x() * dpr),
        static_cast<float>(event->position().y() * dpr), 0);
    event->accept();
    return true;
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
    if (servoq::servo_shutdown_started())
        return;
    qreal dpr = devicePixelRatioF();
    float x = static_cast<float>(event->position().x() * dpr);
    float y = static_cast<float>(event->position().y() * dpr);
    servoq::forward_mouse_move(m_tab_id, x, y);
    QWidget::mouseMoveEvent(event);
}

void WebContentView::forwardMouseButton(int action, int button, QMouseEvent* ev)
{
    if (button < 0 || servoq::servo_shutdown_started())
        return;
    qreal dpr = devicePixelRatioF();
    float x = static_cast<float>(ev->position().x() * dpr);
    float y = static_cast<float>(ev->position().y() * dpr);
    servoq::forward_mouse_button(m_tab_id, action, button, x, y,
        static_cast<uint32_t>(ev->modifiers()));
}

void WebContentView::forwardWindowMouseButton(int action, int button, QMouseEvent* ev)
{
    if (button < 0 || servoq::servo_shutdown_started())
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
    if (!servoq::servo_shutdown_started())
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
    if (servoq::servo_shutdown_started())
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
    if (servoq::servo_shutdown_started())
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
    if (servoq::servo_shutdown_started())
        return;
    if (handleCtrlWheelZoom(event))
        return;
    double dx = 0.0;
    double dy = 0.0;

    auto pixel_delta = -event->pixelDelta();
    auto* device = event->pointingDevice();
    if (!pixel_delta.isNull() && device && device->type() == QInputDevice::DeviceType::TouchPad) {
        dx = pixel_delta.x();
        dy = pixel_delta.y();
    } else if (auto angle_delta = -event->angleDelta(); !angle_delta.isNull()) {
        double step_x = static_cast<double>(angle_delta.x()) / 120.0
                        * static_cast<double>(QApplication::wheelScrollLines());
        double step_y = static_cast<double>(angle_delta.y()) / 120.0
                        * static_cast<double>(QApplication::wheelScrollLines());
        static constexpr double scroll_step_size = 40.0;
        dx = step_x * scroll_step_size;
        dy = step_y * scroll_step_size;
    } else {
        dx = pixel_delta.x();
        dy = pixel_delta.y();
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
    if (servoq::servo_shutdown_started())
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
    if (servoq::servo_shutdown_started())
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
    if (!servoq::servo_shutdown_started())
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
    if (!servoq::servo_shutdown_started())
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
    if (!servoq::servo_shutdown_started())
        servoq::forward_focus(m_tab_id, true);
}

void WebContentView::focusOutEvent(QFocusEvent* event)
{
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("WCV::focusOutEvent tab_id=%1 reason=%2 -> forward_focus(false)")
            .arg(m_tab_id).arg(static_cast<int>(event->reason())));
    QWidget::focusOutEvent(event);
    if (!servoq::servo_shutdown_started())
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
    if (servoq::servo_shutdown_started())
        return QWidget::event(ev);
    if (ev->type() == QEvent::KeyPress) {
        keyPressEvent(static_cast<QKeyEvent*>(ev));
        return true;
    }
    if (ev->type() == QEvent::KeyRelease) {
        keyReleaseEvent(static_cast<QKeyEvent*>(ev));
        return true;
    }
    if (ev->type() == QEvent::TouchBegin || ev->type() == QEvent::TouchUpdate
        || ev->type() == QEvent::TouchEnd || ev->type() == QEvent::TouchCancel) {
        if (forwardTouchEvent(static_cast<QTouchEvent*>(ev), devicePixelRatioF()))
            return true;
    }
    if (ev->type() == QEvent::TabletPress || ev->type() == QEvent::TabletMove
        || ev->type() == QEvent::TabletRelease) {
        if (forwardTabletEvent(static_cast<QTabletEvent*>(ev), devicePixelRatioF()))
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
