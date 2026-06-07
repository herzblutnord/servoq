// WebContentView.cpp
//
// Implements the Qt widget that hosts the Servo engine (or shows a placeholder
// when the engine is disabled / before the first frame arrives).
//
// Ladybird reference for the EVENT SURFACE (structure, not engine calls):
//   vendor/reference-ladybird/UI/Qt/WebContentView.cpp
// Specific line citations appear inline below.

#include "WebContentView.h"
#include "Settings.h"
#include "Tab.h"
#include "servo_callbacks.h"
#include "servoq/src/bridge.rs.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMap>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QDebug>
#include <QResizeEvent>
#include <QStyleHints>
#include <QTimer>
#include <QWheelEvent>

#include <cstring>

// ── Global registry (file-local, accessible to both namespace blocks below) ──
// Maps tab_id -> WebContentView* so Rust-side callbacks can locate their widget.

static QMap<int, ServoQ::WebContentView*>& g_view_registry()
{
    static QMap<int, ServoQ::WebContentView*> s_registry;
    return s_registry;
}

static bool debug_enabled()
{
    return qEnvironmentVariableIsSet("SERVOQ_DEBUG");
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
    if (m_tab_id != 0) {
        debug_log("close_webview", m_tab_id);
        g_view_registry().remove(m_tab_id);
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

void WebContentView::receiveFrame(QImage const& frame)
{
    debug_log("deliver_frame_target", m_tab_id, frame.size(), devicePixelRatioF());
    m_frame = frame;
    m_crashed = false;
    update();
}

void WebContentView::receiveFrameBytes(uint8_t const* bytes, int width, int height)
{
    debug_log("deliver_frame_target", m_tab_id, QSize(width, height), devicePixelRatioF());
    if (m_frame.size() != QSize(width, height) || m_frame.format() != QImage::Format_RGBA8888)
        m_frame = QImage(width, height, QImage::Format_RGBA8888);

    auto const byte_count = qsizetype(width) * qsizetype(height) * 4;
    std::memcpy(m_frame.bits(), bytes, static_cast<size_t>(byte_count));
    m_crashed = false;
    update();
}

// [ladybird: WebContentView crash signal] — Servo crashed; paint the reason inline.
// No placeholder widget: crash state is tracked via m_crashed + m_crash_reason and
// drawn directly in paintEvent so there is no QWidget child to manage.
void WebContentView::receiveWebViewCrash(QString const& reason)
{
    m_engine_tick_timer->stop();
    m_frame = {};
    m_crashed = true;
    m_crash_reason = reason;
    update();
}

void WebContentView::receiveRequestBlocked(QString const& url)
{
    emit request_blocked(url);
}

void WebContentView::notifyThemeChange()
{
    if (!m_webview_created)
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

    debug_log("create_webview", m_tab_id, QSize(pw, ph), dpr);
    auto url_std = m_initial_url.toStdString();
    servoq::create_webview(m_tab_id, url_std.c_str(), pw, ph, static_cast<float>(dpr));
    notifyThemeChange();
}

void WebContentView::forwardResizeToEngine()
{
    if (m_tab_id == 0 || !m_webview_created)
        return;
    qreal dpr = devicePixelRatioF();
    int pw = qMax(1, static_cast<int>(width() * dpr));
    int ph = qMax(1, static_cast<int>(height() * dpr));
    debug_log("resize", m_tab_id, QSize(pw, ph), dpr);
    servoq::forward_resize(m_tab_id, pw, ph, static_cast<float>(dpr));
}

// paintEvent — mirrors Ladybird WebContentView::paintEvent (vendor line 676-708).
// Use Qt's idiomatic HiDPI path: set devicePixelRatio on the image so Qt maps
// physical-pixel image dimensions to the widget's logical rect automatically.
// This avoids the painter.scale(1/dpr) trick which breaks when DPR changes
// between frame delivery and the repaint. [ladybird: WebContentView.cpp:676-708]
void WebContentView::paintEvent(QPaintEvent*)
{
    if (!m_frame.isNull() && !m_crashed) {
        m_frame.setDevicePixelRatio(devicePixelRatioF()); // [ladybird: WebContentView.cpp:690]
        QPainter painter(this);
        painter.drawImage(QPoint(0, 0), m_frame);         // [ladybird: WebContentView.cpp:696]
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
    qreal dpr = devicePixelRatioF();
    float x = static_cast<float>(event->position().x() * dpr);
    float y = static_cast<float>(event->position().y() * dpr);
    servoq::forward_mouse_move(m_tab_id, x, y);
    QWidget::mouseMoveEvent(event);
}

void WebContentView::forwardMouseButton(int action, int button, QMouseEvent* ev)
{
    qreal dpr = devicePixelRatioF();
    float x = static_cast<float>(ev->position().x() * dpr);
    float y = static_cast<float>(ev->position().y() * dpr);
    servoq::forward_mouse_button(m_tab_id, action, button, x, y);
}

// action 0 = Down, 1 = Up  (maps to MouseButtonAction::Down/Up in servo_engine.rs)
// button 0 = Left, 1 = Middle, 2 = Right

void WebContentView::mousePressEvent(QMouseEvent* event)
{
    int button = -1;
    if (event->button() == Qt::LeftButton)        button = 0;
    else if (event->button() == Qt::MiddleButton) button = 1;
    else if (event->button() == Qt::RightButton)  button = 2;
    if (button >= 0)
        forwardMouseButton(0, button, event);
}

void WebContentView::mouseReleaseEvent(QMouseEvent* event)
{
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
    auto text = event->text();
    uint32_t key_char = text.isEmpty() ? 0u : static_cast<uint32_t>(text[0].unicode());
    servoq::forward_key(m_tab_id, true, key_char,
                        static_cast<int32_t>(event->key()),
                        static_cast<uint32_t>(event->modifiers()));
}

void WebContentView::keyReleaseEvent(QKeyEvent* event)
{
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
    if (m_webview_created)
        m_engine_tick_timer->start();
    servoq::set_webview_active(m_tab_id, true); // [ladybird: WebContentView.cpp:774-778]
}

void WebContentView::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    debug_log("hide", m_tab_id);
    m_engine_tick_timer->stop();
    servoq::set_webview_active(m_tab_id, false); // [ladybird: WebContentView.cpp:780-783]
}

// focusInEvent / focusOutEvent — mirrors Ladybird (vendor lines 646-652).
void WebContentView::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    servoq::forward_focus(m_tab_id, true);
}

void WebContentView::focusOutEvent(QFocusEvent* event)
{
    QWidget::focusOutEvent(event);
    servoq::forward_focus(m_tab_id, false);
}

// event() — ensures Tab key reaches keyPressEvent instead of Qt focus navigation;
// handles DevicePixelRatioChange for multi-monitor DPI transitions;
// mirrors Ladybird WebContentView::event() (vendor 964-1009).
bool WebContentView::event(QEvent* ev)
{
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
        update();                // repaint stale frame with updated DPR until new frame arrives
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

void deliver_frame(::std::int32_t tab_id,
                   ::rust::Slice<const ::std::uint8_t> bytes,
                   ::std::int32_t width,
                   ::std::int32_t height)
{
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

// Called from Rust's QtEventLoopWaker::wake() from Servo background threads.
// Posts a custom event to qApp; BrowserWindow::eventFilter intercepts it and
// calls tick_servo() to spin Servo's event loop on the Qt main thread.
// QCoreApplication::postEvent() is documented as thread-safe.
void servoq_wake_event_loop()
{
    static constexpr QEvent::Type ServoWakeType = QEvent::Type(QEvent::User + 1);
    QCoreApplication::postEvent(qApp, new QEvent(ServoWakeType));
}

} // namespace servoq
