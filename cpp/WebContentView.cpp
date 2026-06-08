// WebContentView.cpp
//
// Implements the Qt widget that hosts the Servo engine (or shows a placeholder
// when the engine is disabled / before the first frame arrives).
//
// Ladybird reference for the EVENT SURFACE (structure, not engine calls):
//   vendor/reference-ladybird/UI/Qt/WebContentView.cpp
// Specific line citations appear inline below.

#include "BrowserWindow.h"
#include "BookmarkStore.h"
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
#include <QImage>
#include <QKeyEvent>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QDebug>
#include <QResizeEvent>
#include <QStyleHints>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <QtGui/qpa/qplatformwindow_p.h>

#include <atomic>
#include <chrono>
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
        requestUpdate();
    }

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::UpdateRequest) {
            qt_perf_stats().qwindow_update_requests++;
            if (g_servo_shutting_down().load(std::memory_order_acquire))
                return true;
            if (m_owner && m_owner->waylandRendererActive() && m_owner->takeWaylandPresentPending()) {
                qt_perf_stats().qwindow_presents++;
                m_owner->m_wayland_present_in_progress = true;
                servoq::present_wayland_webview(m_owner->tabId());
                m_owner->m_wayland_present_in_progress = false;
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
        if (m_owner && !g_servo_shutting_down().load(std::memory_order_acquire))
            m_owner->forwardWindowMouseButton(0, qtMouseButtonToServo(event->button()), event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_owner && !g_servo_shutting_down().load(std::memory_order_acquire))
            m_owner->forwardWindowMouseButton(1, qtMouseButtonToServo(event->button()), event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        mousePressEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (!m_owner || g_servo_shutting_down().load(std::memory_order_acquire))
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
};

WebContentView::WebContentView(QWidget* parent)
    : QWidget(parent)
    , m_engine_tick_timer(new QTimer(this))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    // No placeholder widget — the view is shown immediately. Before the first
    // Servo frame arrives the widget simply shows a blank background.
    // [ladybird: Tab.cpp:158] — m_view added directly to tab_layout, no stacked widget.

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
    // [ladybird: WebContentView.cpp:100-105] defer after Qt color-scheme change.
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
        if (g_wayland_window())
            g_wayland_window()->setOwner(nullptr);
        if (g_wayland_container())
            g_wayland_container()->hide();
    }
    if (m_tab_id != 0) {
        debug_log("close_webview", m_tab_id);
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
    // No-op: kept for Tab.cpp call-site compatibility; no placeholder to update.
}

void WebContentView::setStatus(QString const& /*status*/)
{
    // No-op: kept for call-site compatibility; no placeholder to update.
}

void WebContentView::setInitialUrl(QString const& url)
{
    m_initial_url = url.isEmpty() ? QStringLiteral("about:blank") : url;
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
        g_wayland_container() = QWidget::createWindowContainer(g_wayland_window(), this);
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
        g_wayland_owner() = this;
        m_wayland_window->setOwner(this);
        m_wayland_container->setParent(this);
        m_wayland_container->setGeometry(rect());
        if (perf_enabled()) {
            qInfo().nospace()
                << "SERVOQ_PERF wayland_surface_count=1 window_rendering_context_instances=1 "
                << "new-tab-path=shared-qt-surface-attached tab_id=" << m_tab_id
                << " webview_id=" << m_tab_id;
        }
    }
    return true;
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

    m_wayland_container->setGeometry(rect());
    m_wayland_container->show();
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

// [ladybird: WebContentView crash signal] — Servo crashed; paint the reason inline.
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
    if (g_wayland_owner() != this)
        return;
    qt_perf_stats().qwindow_present_requests++;
    if (m_wayland_present_in_progress) {
        m_wayland_dirty_after_present = true;
        qt_perf_stats().qwindow_present_requests_coalesced++;
        maybe_log_qt_perf();
        return;
    }
    if (m_wayland_present_pending) {
        qt_perf_stats().qwindow_present_requests_coalesced++;
        maybe_log_qt_perf();
        return;
    }
    m_wayland_present_pending = true;
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
void WebContentView::startEngineIfNeeded()
{
    if (m_tab_id == 0 || m_webview_created)
        return;
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
            return;
        }
        use_software();
        return;
    }

    if (mode == RendererMode::Auto) {
        if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
            if (perf_enabled())
                qInfo() << "[servoq] auto renderer: Qt is not running on Wayland; using software renderer";
        } else if (startWaylandRendererIfPossible(pw, ph, dpr, /*allow_software_gl=*/false)) {
            // Hardware Wayland renderer selected.
            debug_log("create_wayland_webview", m_tab_id, QSize(pw, ph), dpr);
            notifyThemeChange();
            return;
        }
        // Wayland unavailable or software GL detected — fall through to software.
    }

    // RendererMode::Software, or auto fallback.
    use_software();
}

void WebContentView::forwardResizeToEngine()
{
    if (m_tab_id == 0 || !m_webview_created || g_servo_shutting_down().load(std::memory_order_acquire))
        return;
    qreal dpr = devicePixelRatioF();
    int pw = qMax(1, static_cast<int>(width() * dpr));
    int ph = qMax(1, static_cast<int>(height() * dpr));
    if (m_wayland_container)
        m_wayland_container->setGeometry(rect());
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
// between frame delivery and the repaint. [ladybird: WebContentView.cpp:676-708]
void WebContentView::paintEvent(QPaintEvent*)
{
    if (m_wayland_renderer_active)
        return;
    qt_perf_stats().software_paints++;
    if (!m_frame.isNull() && !m_crashed) {
        m_frame.setDevicePixelRatio(devicePixelRatioF()); // [ladybird: WebContentView.cpp:690]
        QPainter painter(this);
        painter.drawImage(QPoint(0, 0), m_frame);         // [ladybird: WebContentView.cpp:696]
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

// action 0 = Down, 1 = Up  (maps to MouseButtonAction::Down/Up in servo_engine.rs)
// button 0 = Left, 1 = Middle, 2 = Right

void WebContentView::mousePressEvent(QMouseEvent* event)
{
    if (g_servo_shutting_down().load(std::memory_order_acquire))
        return;
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
    forwardResizeToEngine();
    if (m_wayland_renderer_active && m_wayland_container)
        attachSharedWaylandWindow();
    if (m_wayland_renderer_active && m_wayland_container)
        m_wayland_container->show();
    if (m_webview_created && !m_wayland_renderer_active)
        m_engine_tick_timer->start();
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::set_webview_active(m_tab_id, true); // [ladybird: WebContentView.cpp:774-778]
}

void WebContentView::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    debug_log("hide", m_tab_id);
    m_engine_tick_timer->stop();
    if (m_wayland_container && g_wayland_owner() == this)
        m_wayland_container->hide();
    if (!g_servo_shutting_down().load(std::memory_order_acquire))
        servoq::set_webview_active(m_tab_id, false); // [ladybird: WebContentView.cpp:780-783]
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
    // [ladybird: WebContentView.cpp:712-714] — DPR change when window moves between monitors
    if (ev->type() == QEvent::DevicePixelRatioChange) {
        forwardResizeToEngine(); // sends new physical size + new hidpi scale factor
    }
    if (ev->type() == QEvent::PaletteChange || ev->type() == QEvent::ApplicationPaletteChange || ev->type() == QEvent::ThemeChange) {
        // [ladybird: WebContentView.cpp:990-994] defer palette/theme work until Qt settles.
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
        view->tab()->on_favicon_change({});
        return;
    }
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
    view->setCursor(QCursor(static_cast<Qt::CursorShape>(cursor_shape)));
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
    if (auto* window = find_browser_window())
        window->openTabForExistingId(static_cast<int>(tab_id));
}

::std::int32_t show_context_menu_sync(::std::int32_t tab_id, ::rust::Str items_str)
{
    if (servo_shutdown_started())
        return -1;
    auto* view = find_view(tab_id);
    if (!view)
        return -1;

    auto items_text = QString::fromUtf8(items_str.data(), static_cast<qsizetype>(items_str.size()));
    auto lines = items_text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    QMenu menu(view);
    QMap<QAction*, int> action_map;

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
        auto label = parts[1];
        bool enabled = (parts[2].trimmed() != QStringLiteral("false"));
        auto* act = menu.addAction(label);
        act->setEnabled(enabled);
        action_map[act] = action_id;
    }

    auto* selected = menu.exec(QCursor::pos());
    if (!selected || !action_map.contains(selected))
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
