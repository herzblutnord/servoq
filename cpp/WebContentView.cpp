// WebContentView.cpp
//
// Implements the Qt widget that hosts the Servo engine (or shows a placeholder
// when the engine is disabled / before the first frame arrives).
//
// Ladybird reference for the EVENT SURFACE (structure, not engine calls):
//   vendor/reference-ladybird/UI/Qt/WebContentView.cpp
// Specific line citations appear inline below.

#include "WebContentView.h"
#include "Tab.h"
#include "WebContentPlaceholder.h"
#include "servo_callbacks.h"
#include "servoq/src/bridge.rs.h"

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMap>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

// ── Global registry (file-local, accessible to both namespace blocks below) ──
// Maps tab_id -> WebContentView* so Rust-side callbacks can locate their widget.

static QMap<int, ServoQ::WebContentView*>& g_view_registry()
{
    static QMap<int, ServoQ::WebContentView*> s_registry;
    return s_registry;
}

// ────────────────────────────────────────────────────────────────────────────
// ServoQ::WebContentView implementation
// ────────────────────────────────────────────────────────────────────────────

namespace ServoQ {

WebContentView::WebContentView(QWidget* parent)
    : QWidget(parent)
    , m_placeholder(new WebContentPlaceholder(this))
    , m_engine_tick_timer(new QTimer(this))
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    // Placeholder fills the entire widget until the first Servo frame arrives.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_placeholder);

    // Engine tick timer pumps servo.spin_event_loop() at ~60 fps.
    // Started in showEvent after create_webview; paused in hideEvent.
    m_engine_tick_timer->setInterval(16);
    m_engine_tick_timer->setSingleShot(false);
    connect(m_engine_tick_timer, &QTimer::timeout, this, [this] {
        servoq::tick_webview(m_tab_id);
    });
}

WebContentView::~WebContentView()
{
    if (m_tab_id != 0) {
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
    if (m_tab_id != 0)
        g_view_registry().insert(m_tab_id, this);
}

void WebContentView::setUrl(QString const& url)
{
    m_placeholder->setUrl(url);
}

void WebContentView::setStatus(QString const& status)
{
    m_placeholder->setStatus(status);
}

void WebContentView::setInitialUrl(QString const& url)
{
    m_initial_url = url.isEmpty() ? QStringLiteral("about:blank") : url;
}

void WebContentView::receiveFrame(QImage const& frame)
{
    m_frame = frame;
    if (m_placeholder_visible) {
        m_placeholder->hide();
        m_placeholder_visible = false;
    }
    update();
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

    auto url_std = m_initial_url.toStdString();
    servoq::create_webview(m_tab_id, url_std.c_str(), pw, ph, static_cast<float>(dpr));
}

// paintEvent — mirrors Ladybird WebContentView::paintEvent (vendor line 676-708):
//   painter.scale(1/dpr, 1/dpr); painter.drawImage(QPoint(0,0), bitmap, srcRect);
// When no frame is available the placeholder child paints itself automatically.
void WebContentView::paintEvent(QPaintEvent*)
{
    if (!m_frame.isNull()) {
        QPainter painter(this);
        qreal dpr = devicePixelRatioF();
        painter.scale(1.0 / dpr, 1.0 / dpr);
        painter.drawImage(QPoint(0, 0), m_frame);
        return;
    }
    // No frame yet: placeholder child widget handles its own painting.
}

// resizeEvent — mirrors Ladybird WebContentView::resizeEvent (vendor line 710-714):
//   WebContentViewBase::resizeEvent(event); update_viewport_size();
void WebContentView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_tab_id == 0 || !m_webview_created)
        return;
    qreal dpr = devicePixelRatioF();
    int pw = qMax(1, static_cast<int>(width() * dpr));
    int ph = qMax(1, static_cast<int>(height() * dpr));
    servoq::forward_resize(m_tab_id, pw, ph, static_cast<float>(dpr));
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
    startEngineIfNeeded();
    if (m_webview_created)
        m_engine_tick_timer->start();
}

void WebContentView::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_engine_tick_timer->stop();
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
    if (!view)
        return;
    // Wrap the Rust slice in a QImage, then copy before the slice is released.
    QImage image(bytes.data(), width, height,
                 static_cast<qsizetype>(width) * 4,
                 QImage::Format_RGBA8888);
    view->receiveFrame(image.copy());
}

void notify_url_changed(::std::int32_t tab_id, ::rust::Str url)
{
    auto* view = find_view(tab_id);
    if (!view || !view->tab())
        return;
    view->tab()->on_url_change(
        QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size())));
}

void notify_title_changed(::std::int32_t tab_id, ::rust::Str title)
{
    auto* view = find_view(tab_id);
    if (!view || !view->tab())
        return;
    view->tab()->on_title_change(
        QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size())));
}

void notify_load_started(::std::int32_t tab_id, ::rust::Str url)
{
    auto* view = find_view(tab_id);
    if (!view || !view->tab())
        return;
    view->tab()->on_load_start(
        QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size())));
}

void notify_load_finished(::std::int32_t tab_id)
{
    auto* view = find_view(tab_id);
    if (!view || !view->tab())
        return;
    view->tab()->on_load_finish();
}

void notify_status_changed(::std::int32_t tab_id, ::rust::Str text)
{
    auto* view = find_view(tab_id);
    if (!view || !view->tab())
        return;
    view->tab()->on_link_hover(
        QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
}

} // namespace servoq
