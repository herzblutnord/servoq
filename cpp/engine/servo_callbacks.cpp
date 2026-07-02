// servoq:: callback implementations (declared in servo_callbacks.h): the
// Rust→C++ half of the CXX bridge. Routes engine notifications to the widget
// layer via WebContentView::findByTabId.

#include "engine/servo_callbacks.h"
#include "DebugFlags.h"
#include "engine/Favicon.h"
#include "engine/QtPerfStats.h"
#include "engine/WebContentView.h"
#include "storage/Settings.h"
#include "ui/BrowserWindow.h"
#include "ui/InternalPageView.h"
#include "ui/Tab.h"
#include "servoq/src/bridge.rs.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QScreen>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWindow>

#include <atomic>
#include <functional>

namespace servoq {

using ServoQ::debug_log;

namespace {

std::atomic_bool& g_servo_wake_pending()
{
    static std::atomic_bool s_pending { false };
    return s_pending;
}

std::atomic_bool& g_servo_shutting_down()
{
    static std::atomic_bool s_shutting_down { false };
    return s_shutting_down;
}

ServoQ::WebContentView* find_view(::std::int32_t tab_id)
{
    return ServoQ::WebContentView::findByTabId(static_cast<int>(tab_id));
}

ServoQ::BrowserWindow* find_browser_window()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = dynamic_cast<ServoQ::BrowserWindow*>(widget))
            return window;
    }
    return nullptr;
}

char const* cursor_code_name(int code)
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

Qt::CursorShape cursor_shape_from_servoq_code(int code)
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

char const* qt_cursor_name(Qt::CursorShape shape)
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

// Pending JS-evaluation callbacks keyed by request id; filled by
// ServoQ::evaluate_javascript_in_tab, drained by notify_javascript_result.
QHash<uint64_t, std::function<void(bool, QString const&)>>& g_js_result_handlers()
{
    static QHash<uint64_t, std::function<void(bool, QString const&)>> handlers;
    return handlers;
}

} // namespace

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
    for (auto* view : ServoQ::WebContentView::allViews()) {
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
    ServoQ::qt_perf_stats().wake_events++;
    if (!g_servo_wake_pending().compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ServoQ::qt_perf_stats().wake_events_coalesced++;
        ServoQ::maybe_log_qt_perf();
        return;
    }

    static constexpr QEvent::Type ServoWakeType = QEvent::Type(QEvent::User + 1);
    QCoreApplication::postEvent(qApp, new QEvent(ServoWakeType));
    ServoQ::maybe_log_qt_perf();
}

void mark_servo_wake_event_consumed()
{
    g_servo_wake_pending().store(false, std::memory_order_release);
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
        debug_log("favicon", tab_id,
            QStringLiteral("page_url=%1 source=servo decoded_output=empty action=clear").arg(view->tab()->url()));
        view->tab()->on_favicon_change({});
        return;
    }
    debug_log("favicon", tab_id,
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
    if (auto* container = ServoQ::WebContentView::sharedWaylandContainer()) {
        container->setCursor(qt_cursor);
        applied_container = true;
    }
    if (auto* window = ServoQ::WebContentView::sharedWaylandWindow()) {
        window->setCursor(qt_cursor);
        applied_window = true;
    }
    if (ServoQ::debug_enabled()) {
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
    Q_UNUSED(tab_id); // the save dialog is window-global, not per tab
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

void evaluate_javascript_in_tab(int tab_id, QString const& script,
    std::function<void(bool, QString const&)> callback)
{
    static uint64_t s_next_request_id = 1;
    auto request_id = s_next_request_id++;
    servoq::g_js_result_handlers().insert(request_id, std::move(callback));
    servoq::evaluate_javascript(tab_id, request_id, script.toStdString());
}

} // namespace ServoQ
