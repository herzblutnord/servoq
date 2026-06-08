/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/WebContentView.cpp
 */
// WebContentView.cpp
//
// Implements the Qt widget that hosts the Servo engine (or shows a placeholder
// when the engine is disabled / before the first frame arrives).
// Specific line citations appear inline below.

#include "BrowserWindow.h"
#include "BookmarkStore.h"
#include "ChromeStyle.h"
#include "WebContentView.h"
#include "Settings.h"
#include "Tab.h"
#include "servo_callbacks.h"
#include "servoq/src/bridge.rs.h"

#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QSystemTrayIcon>
#include <QCursor>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QDebug>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <QtGui/qpa/qplatformwindow_p.h>
#include <QClipboard>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

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

static bool debug_enabled()
{
    return qEnvironmentVariableIsSet("SERVOQ_DEBUG");
}

static bool perf_enabled()
{
    return qEnvironmentVariableIsSet("SERVOQ_PERF");
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
};

static QtPerfStats& qt_perf_stats()
{
    static QtPerfStats stats;
    return stats;
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

static QHash<int, int>& g_favicon_generations()
{
    static QHash<int, int> s_generations;
    return s_generations;
}

static QNetworkAccessManager& favicon_network_manager()
{
    static QNetworkAccessManager manager;
    return manager;
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
                debug_log("wayland_present_skipped_hidden", m_owner->tabId());
                maybe_log_qt_perf();
                return true;
            }
            if (m_owner->waylandRendererActive() && m_owner->takeWaylandPresentPending()) {
                qt_perf_stats().qwindow_presents++;
                auto owner_generation = g_wayland_owner_generation();
                m_owner->m_wayland_present_in_progress = true;
                debug_log("wayland_present_enter", m_owner->tabId(),
                    QStringLiteral("owner_generation=%1").arg(owner_generation));
                servoq::present_wayland_webview(m_owner->tabId());
                m_owner->m_wayland_present_in_progress = false;
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
                    m_owner->requestWaylandRepaint();
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
            m_owner->requestWaylandRepaint();
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
        if (m_owner && !g_servo_shutting_down().load(std::memory_order_acquire))
            servoq::forward_focus(m_owner->tabId(), true);
    }

    void focusOutEvent(QFocusEvent*) override
    {
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

WebContentView::WebContentView(QWidget* parent)
    : QWidget(parent)
    , m_engine_tick_timer(new QTimer(this))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    // No child placeholder widget. Empty new tabs and crash states are painted
    // directly by this widget; real pages are handed to Servo on first navigation.

    // Fallback timer: spins servo.spin_event_loop() at 5 Hz when the tab is visible.
    // With QtEventLoopWaker installed, Servo background threads (paint, font, layout)
    // wake the Qt event loop immediately via QCoreApplication::postEvent(). This timer
    // is kept only as a safety net in case a wake event is dropped or coalesced.
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
        g_favicon_generations().remove(m_tab_id);
        g_view_registry().remove(m_tab_id);
        if (!g_servo_shutting_down().load(std::memory_order_acquire))
            servoq::close_webview(m_tab_id);
    }
}

void WebContentView::setTab(Tab* tab)
{
    m_tab = tab;
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
        if (m_wayland_container && g_wayland_owner() == this)
            m_wayland_container->hide();
        m_engine_tick_timer->stop();
    }
    update();
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

bool WebContentView::attachSharedWaylandWindow()
{
    if (!waylandRendererRequested())
        return false;
    if (!g_wayland_window()) {
        g_wayland_window() = new ServoWaylandContentWindow(this);
        // Use the Tab's QStackedWidget as a stable parent so we never need to
        // call setParent() on the container during tab switches. Reparenting a
        // createWindowContainer widget on Wayland reconfigures the wl_subsurface,
        // which can leave the EGL surface in a state where eglSwapBuffers blocks
        // indefinitely on the next tab switch — causing the UI to freeze.
        QWidget* stable_parent = (m_tab && m_tab->parentWidget()) ? m_tab->parentWidget() : this;
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
        auto* previous_owner = g_wayland_owner();
        if (previous_owner && previous_owner != this) {
            previous_owner->m_wayland_present_pending = false;
            previous_owner->m_wayland_dirty_after_present = false;
            previous_owner->m_wayland_present_in_progress = false;
        }
        g_wayland_owner() = this;
        ++g_wayland_owner_generation();
        m_wayland_window->setOwner(this);
        // Do NOT call setParent() — the container lives under a stable parent
        // (the QStackedWidget) for the lifetime of the app. Just reposition it.
        updateContainerGeometry();
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
            QStringLiteral("previous=%1 owner_generation=%2")
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

void WebContentView::requestWaylandRepaint()
{
    if (!m_wayland_renderer_active || !m_wayland_window || g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    if (!isVisible()) {
        debug_log("wayland_present_skipped_request_hidden_view", m_tab_id);
        return;
    }
    if (g_wayland_owner() != this) {
        debug_log("wayland_present_skipped_request_inactive_owner", m_tab_id,
            QStringLiteral("current_owner=%1").arg(g_wayland_owner() ? g_wayland_owner()->tabId() : 0));
        return;
    }
    qt_perf_stats().qwindow_present_requests++;
    if (m_wayland_present_in_progress) {
        m_wayland_dirty_after_present = true;
        debug_log("wayland_present_deferred_in_progress", m_tab_id);
        qt_perf_stats().qwindow_present_requests_coalesced++;
        maybe_log_qt_perf();
        return;
    }
    if (m_wayland_present_pending) {
        debug_log("wayland_present_coalesced_pending", m_tab_id);
        qt_perf_stats().qwindow_present_requests_coalesced++;
        maybe_log_qt_perf();
        return;
    }
    m_wayland_present_pending = true;
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

// Deferred to showEvent so width()/height() carry real layout-assigned values.
bool WebContentView::startEngineIfNeeded()
{
    if (m_tab_id == 0 || m_webview_created)
        return false;
    if (m_empty_new_tab)
        return false;
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
    if (m_wayland_container && g_wayland_owner() == this)
        updateContainerGeometry();
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
        requestWaylandRepaint();
    else
        update();
}

// paintEvent — mirrors Ladybird WebContentView::paintEvent (vendor line 676-708).
// Use Qt's idiomatic HiDPI path: set devicePixelRatio on the image so Qt maps
// physical-pixel image dimensions to the widget's logical rect automatically.
// This avoids the painter.scale(1/dpr) trick which breaks when DPR changes
// between frame delivery and the repaint.
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
    servoq::forward_mouse_button(m_tab_id, action, button, x, y);
}

void WebContentView::forwardWindowMouseButton(int action, int button, QMouseEvent* ev)
{
    if (button < 0 || g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    qreal dpr = m_wayland_window ? m_wayland_window->devicePixelRatio() : devicePixelRatioF();
    float x = static_cast<float>(ev->position().x() * dpr);
    float y = static_cast<float>(ev->position().y() * dpr);
    servoq::forward_mouse_button(m_tab_id, action, button, x, y);
}

void WebContentView::takeFocusFromContentClick()
{
    if (auto* focus_widget = QApplication::focusWidget()) {
        if (focus_widget != this && focus_widget != m_wayland_container)
            focus_widget->clearFocus();
    }
    setFocus(Qt::MouseFocusReason);
    if (m_wayland_container && g_wayland_owner() == this)
        m_wayland_container->setFocus(Qt::MouseFocusReason);
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::forward_focus(m_tab_id, true);
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
    if (g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    auto text = event->text();
    uint32_t key_char = text.isEmpty() ? 0u : static_cast<uint32_t>(text[0].unicode());
    servoq::forward_key(m_tab_id, false, key_char,
                        static_cast<int32_t>(event->key()),
                        static_cast<uint32_t>(event->modifiers()));
}

// showEvent / hideEvent — mirrors Ladybird (vendor lines 774-783).
void WebContentView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    debug_log("show", m_tab_id);
    startEngineIfNeeded();
    if (m_wayland_renderer_active && m_wayland_container) {
        attachSharedWaylandWindow(); // transfers ownership, updates geometry without setParent
        m_wayland_container->show();
        m_wayland_container->raise();
    } else if (!m_wayland_renderer_active && g_wayland_container()
               && g_wayland_owner() == nullptr && g_wayland_container()->isVisible()) {
        // Non-Wayland tab taking over; the container was kept visible but off-screen
        // (to avoid unmapping the wl_surface). Now truly hide it so the software-rendered
        // or placeholder content is visible without the Wayland overlay on top.
        g_wayland_container()->hide();
    }
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
    if (m_wayland_container && g_wayland_owner() == this) {
        m_wayland_present_pending = false;
        m_wayland_dirty_after_present = false;
        m_wayland_present_in_progress = false;
        // Move the container off-screen instead of hiding it. Hiding a
        // createWindowContainer widget causes Qt/Wayland to unmap the embedded
        // wl_surface. The first eglSwapBuffers() after remapping blocks waiting
        // for a compositor frame callback that may never arrive for a freshly-
        // remapped surface, freezing the Qt main thread indefinitely.
        // Keeping the surface mapped (just off-screen) avoids this race.
        // The incoming tab's showEvent will either reposition it (Wayland tab)
        // or truly hide it (non-Wayland tab, see showEvent above).
        auto* parent = m_wayland_container->parentWidget();
        int off = parent ? parent->width() : width();
        m_wayland_container->move(-off, 0);
        if (m_wayland_window)
            m_wayland_window->setOwner(nullptr);
        g_wayland_owner() = nullptr;
        ++g_wayland_owner_generation();
        if (perf_enabled()) {
            qInfo().nospace()
                << "SERVOQ_PERF wayland_surface_count=1 window_rendering_context_instances=1 "
                << "tab_switch_path=shared-qt-surface-offscreen previous_tab_id=" << m_tab_id
                << " active_tab_id=0 webview_id=" << m_tab_id
                << " owner_generation=" << g_wayland_owner_generation();
        }
        debug_log("wayland_owner_cleared", m_tab_id,
            QStringLiteral("owner_generation=%1").arg(g_wayland_owner_generation()));
    }
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::set_webview_active(m_tab_id, false);
}

// focusInEvent / focusOutEvent — mirrors Ladybird (vendor lines 646-652).
void WebContentView::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::forward_focus(m_tab_id, true);
}

void WebContentView::focusOutEvent(QFocusEvent* event)
{
    QWidget::focusOutEvent(event);
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::forward_focus(m_tab_id, false);
}

// event() — ensures Tab key reaches keyPressEvent instead of Qt focus navigation;
// handles DevicePixelRatioChange for multi-monitor DPI transitions;
// mirrors Ladybird WebContentView::event() (vendor 964-1009).
bool WebContentView::event(QEvent* ev)
{
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
    if (ev->type() == QEvent::PaletteChange || ev->type() == QEvent::ApplicationPaletteChange || ev->type() == QEvent::ThemeChange) {
        QTimer::singleShot(0, this, [this] {
            notifyThemeChange();
            update();
        });
    }
    return QWidget::event(ev);
}

} // namespace ServoQ

// ────────────────────────────────────────────────────────────────────────────
// servoq::* callback implementations
// Declared in servo_callbacks.h; called from Rust via the CXX bridge.
// They use g_view_registry() defined in this same translation unit.
// ────────────────────────────────────────────────────────────────────────────

namespace servoq {

static ServoQ::WebContentView* find_view(::std::int32_t tab_id)
{
    return g_view_registry().value(static_cast<int>(tab_id), nullptr);
}

static QString http_header_value(QNetworkReply* reply, QByteArray const& header)
{
    return QString::fromLatin1(reply->rawHeader(header).trimmed());
}

static QNetworkRequest favicon_request(QUrl const& url)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ServoQ/0.1"));
    request.setTransferTimeout(15000);
    return request;
}

static bool bytes_look_like_svg(QByteArray const& bytes)
{
    auto prefix = bytes.left(512).trimmed().toLower();
    return prefix.startsWith("<svg") || (prefix.startsWith("<?xml") && prefix.contains("<svg"));
}

static QString detect_favicon_format(QUrl const& favicon_url, QString const& content_type, QByteArray const& bytes)
{
    auto path = favicon_url.path().toLower();
    auto mime = content_type.toLower();
    if (path.endsWith(QStringLiteral(".svg")) || mime.contains(QStringLiteral("image/svg+xml")) || bytes_look_like_svg(bytes))
        return QStringLiteral("svg");
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    auto format = reader.format();
    if (!format.isEmpty())
        return QString::fromLatin1(format).toLower();
    if (path.endsWith(QStringLiteral(".ico")))
        return QStringLiteral("ico");
    if (path.endsWith(QStringLiteral(".png")))
        return QStringLiteral("png");
    if (path.endsWith(QStringLiteral(".jpg")) || path.endsWith(QStringLiteral(".jpeg")))
        return QStringLiteral("jpeg");
    return QStringLiteral("unknown");
}

static std::optional<QImage> decode_favicon_bytes(int tab_id, QUrl const& page_url, QUrl const& favicon_url, QString const& content_type, QByteArray const& bytes)
{
    auto format = detect_favicon_format(favicon_url, content_type, bytes);
    debug_log_favicon(tab_id,
        QStringLiteral("page_url=%1 favicon_url=%2 mime=%3 input_bytes=%4 detected_input_format=%5")
            .arg(page_url.toString(), favicon_url.toString(), content_type.isEmpty() ? QStringLiteral("<none>") : content_type)
            .arg(bytes.size())
            .arg(format));

    if (format == QStringLiteral("svg")) {
        QSvgRenderer renderer(bytes);
        if (!renderer.isValid()) {
            debug_log_favicon(tab_id,
                QStringLiteral("page_url=%1 favicon_url=%2 decode_failure=invalid_svg").arg(page_url.toString(), favicon_url.toString()));
            return {};
        }

        static constexpr int FaviconBitmapSize = 64;
        QImage image(FaviconBitmapSize, FaviconBitmapSize, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.render(&painter, QRectF(0, 0, FaviconBitmapSize, FaviconBitmapSize));
        painter.end();
        debug_log_favicon(tab_id,
            QStringLiteral("page_url=%1 favicon_url=%2 decoded_output=%3x%4")
                .arg(page_url.toString(), favicon_url.toString())
                .arg(image.width())
                .arg(image.height()));
        return image;
    }

    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        debug_log_favicon(tab_id,
            QStringLiteral("page_url=%1 favicon_url=%2 decode_failure=buffer_open_failed").arg(page_url.toString(), favicon_url.toString()));
        return {};
    }

    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) {
        debug_log_favicon(tab_id,
            QStringLiteral("page_url=%1 favicon_url=%2 decode_failure=%3")
                .arg(page_url.toString(), favicon_url.toString(), reader.errorString()));
        return {};
    }

    auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
    debug_log_favicon(tab_id,
        QStringLiteral("page_url=%1 favicon_url=%2 decoded_output=%3x%4")
            .arg(page_url.toString(), favicon_url.toString())
            .arg(rgba.width())
            .arg(rgba.height()));
    return rgba;
}

static void apply_decoded_favicon(ServoQ::WebContentView* view, QImage const& image)
{
    if (!view || !view->tab())
        return;
    auto copy = image.convertToFormat(QImage::Format_RGBA8888);
    view->tab()->on_favicon_change(QIcon(QPixmap::fromImage(copy)));

    QByteArray png_bytes;
    QBuffer buffer(&png_bytes);
    if (buffer.open(QIODevice::WriteOnly) && copy.save(&buffer, "PNG")) {
        auto url = view->tab()->url();
        auto favicon = QString::fromLatin1(png_bytes.toBase64());
        auto changed = ServoQ::BookmarkStore::the()->updateFavicon(url, favicon);
        debug_log_favicon(view->tabId(),
            QStringLiteral("page_url=%1 storage=png_base64 png_bytes=%2 bookmark_updated=%3")
                .arg(url)
                .arg(png_bytes.size())
                .arg(changed ? 1 : 0));
    }
}

static QString extract_html_attr(QString const& tag, QString const& attr)
{
    QRegularExpression re(QStringLiteral("\\b%1\\s*=\\s*(['\"])(.*?)\\1").arg(QRegularExpression::escape(attr)),
        QRegularExpression::CaseInsensitiveOption);
    auto match = re.match(tag);
    if (match.hasMatch())
        return match.captured(2);

    QRegularExpression unquoted(QStringLiteral("\\b%1\\s*=\\s*([^\\s>]+)").arg(QRegularExpression::escape(attr)),
        QRegularExpression::CaseInsensitiveOption);
    match = unquoted.match(tag);
    return match.hasMatch() ? match.captured(1) : QString {};
}

static QUrl favicon_url_from_html(QUrl const& page_url, QByteArray const& html)
{
    auto text = QString::fromUtf8(html);
    QRegularExpression link_re(QStringLiteral("<link\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    auto it = link_re.globalMatch(text);
    while (it.hasNext()) {
        auto tag = it.next().captured(0);
        auto rel = extract_html_attr(tag, QStringLiteral("rel")).toLower();
        if (!rel.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).contains(QStringLiteral("icon")))
            continue;
        auto href = extract_html_attr(tag, QStringLiteral("href"));
        if (!href.isEmpty())
            return page_url.resolved(QUrl(href));
    }
    return page_url.resolved(QUrl(QStringLiteral("/favicon.ico")));
}

static void fetch_favicon_bytes(int tab_id, int generation, QUrl const& page_url, QUrl const& favicon_url)
{
    debug_log_favicon(tab_id,
        QStringLiteral("page_url=%1 favicon_url=%2 fetch=icon").arg(page_url.toString(), favicon_url.toString()));
    auto* reply = favicon_network_manager().get(favicon_request(favicon_url));
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, tab_id, generation, page_url, favicon_url] {
        reply->deleteLater();
        if (g_favicon_generations().value(tab_id) != generation) {
            debug_log_favicon(tab_id,
                QStringLiteral("page_url=%1 favicon_url=%2 skipped=stale_generation").arg(page_url.toString(), favicon_url.toString()));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            debug_log_favicon(tab_id,
                QStringLiteral("page_url=%1 favicon_url=%2 decode_failure=network_error:%3")
                    .arg(page_url.toString(), favicon_url.toString(), reply->errorString()));
            return;
        }
        auto content_type = http_header_value(reply, "content-type");
        auto bytes = reply->readAll();
        auto decoded = decode_favicon_bytes(tab_id, page_url, favicon_url, content_type, bytes);
        if (!decoded.has_value())
            return;
        auto* view = find_view(tab_id);
        if (!view || !view->tab() || QUrl(view->tab()->url()) != page_url) {
            debug_log_favicon(tab_id,
                QStringLiteral("page_url=%1 favicon_url=%2 skipped=stale_page").arg(page_url.toString(), favicon_url.toString()));
            return;
        }
        apply_decoded_favicon(view, decoded.value());
    });
}

static void start_favicon_probe(ServoQ::WebContentView* view)
{
    if (!view || !view->tab())
        return;
    QUrl page_url(view->tab()->url());
    if (!page_url.isValid() || (page_url.scheme() != QStringLiteral("http") && page_url.scheme() != QStringLiteral("https")))
        return;

    auto tab_id = view->tabId();
    auto generation = g_favicon_generations().value(tab_id) + 1;
    g_favicon_generations().insert(tab_id, generation);
    debug_log_favicon(tab_id, QStringLiteral("page_url=%1 fetch=html generation=%2").arg(page_url.toString()).arg(generation));

    auto* reply = favicon_network_manager().get(favicon_request(page_url));
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, tab_id, generation, page_url] {
        reply->deleteLater();
        if (g_favicon_generations().value(tab_id) != generation) {
            debug_log_favicon(tab_id, QStringLiteral("page_url=%1 skipped=stale_html_generation").arg(page_url.toString()));
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            debug_log_favicon(tab_id,
                QStringLiteral("page_url=%1 html_fetch_failure=%2 fallback=/favicon.ico")
                    .arg(page_url.toString(), reply->errorString()));
            fetch_favicon_bytes(tab_id, generation, page_url, page_url.resolved(QUrl(QStringLiteral("/favicon.ico"))));
            return;
        }

        auto content_type = http_header_value(reply, "content-type");
        auto html = reply->readAll();
        auto favicon_url = favicon_url_from_html(page_url, html);
        debug_log_favicon(tab_id,
            QStringLiteral("page_url=%1 html_mime=%2 html_bytes=%3 favicon_url=%4")
                .arg(page_url.toString(), content_type.isEmpty() ? QStringLiteral("<none>") : content_type)
                .arg(html.size())
                .arg(favicon_url.toString()));
        fetch_favicon_bytes(tab_id, generation, page_url, favicon_url);
    });
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
            view->requestWaylandRepaint();
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
    // C1: skip frames for hidden tabs. Servo shares one event loop across all
    // tabs; set_throttled(true) is advisory and may not take effect immediately.
    // Without this guard a tab that was hidden still delivers frames, wasting
    // CPU and potentially racing with the visible tab's render state.
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
    start_favicon_probe(view);
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
        view->requestWaylandRepaint();
}

// Called from Rust's QtEventLoopWaker::wake() from Servo background threads.
// Posts a custom event to qApp; BrowserWindow::eventFilter intercepts it and
// calls tick_servo() to spin Servo's event loop on the Qt main thread.
// QCoreApplication::postEvent() is documented as thread-safe.
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
    auto copy = img.copy();
    view->tab()->on_favicon_change(QIcon(QPixmap::fromImage(copy)));

    QByteArray png_bytes;
    QBuffer buffer(&png_bytes);
    if (buffer.open(QIODevice::WriteOnly) && copy.save(&buffer, "PNG")) {
        auto url = view->tab()->url();
        auto favicon = QString::fromLatin1(png_bytes.toBase64());
        auto changed = ServoQ::BookmarkStore::the()->updateFavicon(url, favicon);
        if (perf_enabled()) {
            qInfo().nospace()
                << "SERVOQ_PERF favicon target_tab_id=" << tab_id
                << " tab_id=" << tab_id
                << " webview_id=" << tab_id
                << " url=" << url
                << " bookmark_updated=" << changed;
        }
    }
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

    QMenu menu(view);
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
