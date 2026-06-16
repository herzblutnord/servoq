/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Simon Farre <simon.farre.cx@gmail.com>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/BrowserWindow.cpp
 *   Libraries/LibWebView/SearchEngine.cpp
 *   Libraries/LibWebView/Settings.cpp
 *   Libraries/LibWeb/Loader/ContentBlocker.cpp
 */
#include "BrowserWindow.h"
#include "BookmarksBar.h"
#include "FaviconStore.h"
#include "HistoryStore.h"
#include "InternalPageView.h"
#include "MprisManager.h"
#include "NewTabTrace.h"
#include "PermissionStore.h"
#include "SessionStore.h"
#include "TabSearch.h"
#include "ChromeLayout.h"
#include "ChromeStyle.h"
#include "Icon.h"
#include "LocationEdit.h"
#include "Settings.h"
#include "Tab.h"
#include "TabBar.h"
#include "WebContentView.h"
#include "WebViewURL.h"
#include "servo_callbacks.h"
#include "servoq/src/bridge.rs.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QWindow>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QShortcut>
#include <QStatusBar>
#include <QDesktopServices>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QStandardPaths>
#include <QUrl>
#include <QWidgetAction>
#include <QCloseEvent>
#include <QDebug>
#include <memory>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QElapsedTimer>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <optional>

// TEMPORARY DIAGNOSTICS (SERVOQ_DIAG) — defined in WebContentView.cpp.
bool servoq_diag_enabled();
QString servoq_diag_describe(QObject const* o);
void servoq_diag_log(QString const& msg);

namespace ServoQ {

namespace {

bool debug_enabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_DEBUG");
    return v;
}

void debug_log(char const* event, int tab_id, QString const& detail)
{
    if (debug_enabled())
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " tab_id=" << tab_id << " " << detail;
}

// Rate-limit wake-driven ticking to once per kServoTickIntervalMs: Servo can wake
// the loop ~25k×/s and each spin drains everything, so coalesce extra wakes (leave
// the pending flag set) and run one catch-up tick at the interval boundary. No wake
// is dropped, only delayed <1 frame. See docs/DEVIATIONS.md §0.
constexpr qint64 kServoTickIntervalMs = 4;

qint64 servo_tick_clock_ms()
{
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer.elapsed();
}

qint64 g_last_servo_tick_ms = -1000;
bool g_servo_catchup_scheduled = false;

void rate_limited_tick_servo()
{
    qint64 const now = servo_tick_clock_ms();
    qint64 const since = now - g_last_servo_tick_ms;
    if (since >= kServoTickIntervalMs) {
        g_last_servo_tick_ms = now;
        servoq::mark_servo_wake_event_consumed();
        servoq::tick_servo();
        return;
    }
    // Too soon — do NOT consume the wake (leave it pending so background wakes
    // coalesce and stop posting), and ensure a single catch-up tick fires at the
    // interval boundary.
    if (!g_servo_catchup_scheduled) {
        g_servo_catchup_scheduled = true;
        QTimer::singleShot(static_cast<int>(kServoTickIntervalMs - since), qApp, [] {
            g_servo_catchup_scheduled = false;
            g_last_servo_tick_ms = servo_tick_clock_ms();
            servoq::mark_servo_wake_event_consumed();
            servoq::tick_servo();
        });
    }
}

bool is_child_or_self(QObject const* candidate, QObject const* ancestor)
{
    for (auto const* object = candidate; object; object = object->parent()) {
        if (object == ancestor)
            return true;
    }
    return false;
}

bool is_location_completion_popup(QObject const* object)
{
    for (auto const* candidate = object; candidate; candidate = candidate->parent()) {
        if (candidate->objectName() == QStringLiteral("LadybirdAutocompletePopup"))
            return true;
    }
    if (auto const* widget = qobject_cast<QWidget const*>(object)) {
        auto* popup = qobject_cast<QAbstractItemView*>(widget->window());
        return popup && popup->objectName() == QStringLiteral("LadybirdAutocompletePopup");
    }
    return false;
}


}

BrowserWindow::BrowserWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_tabs(new TabWidget(this))
    , m_hamburger_menu(new QMenu(this))
    , m_session_save_timer(new QTimer(this))
{
    // Install event filter on qApp so BrowserWindow::eventFilter() receives the
    // QtEventLoopWaker wake event (QEvent::User+1) posted from Servo's threads.
    qApp->installEventFilter(this);

    // Main-thread jank detector: a 50 ms heartbeat whose gap reveals event-loop
    // stalls the per-second PERF flush can't show (SERVOQ_PERF/SERVOQ_NEWTAB_TRACE).
    if (qEnvironmentVariableIsSet("SERVOQ_PERF") || newtab_trace_enabled()) {
        auto* heartbeat = new QTimer(this);
        heartbeat->setInterval(50);
        heartbeat->setTimerType(Qt::PreciseTimer);
        auto last_beat_ms = std::make_shared<qint64>(newtab_trace_clock_ms());
        connect(heartbeat, &QTimer::timeout, this, [last_beat_ms] {
            qint64 const now = newtab_trace_clock_ms();
            qint64 const gap = now - *last_beat_ms;
            *last_beat_ms = now;
            if (gap > 200)
                qWarning().nospace() << "SERVOQ_JANK main_thread_gap_ms=" << gap
                                     << " last_phase=" << newtab_last_phase();
        });
        heartbeat->start();
    }

    setWindowTitle("ServoQ");
    setWindowIcon(app_icon());
    updateWindowBorder();
    // Keep the floor only as large as the chrome needs to stay usable (toolbar
    // cluster + location bar + hamburger; vertical tab strip). Was 900x640,
    // which felt oversized.
    setMinimumSize(480, 360);
    resize(Settings::the()->last_size());
    if (auto last_position = Settings::the()->last_position(); last_position.has_value())
        move(*last_position);

    createMenus();
    applySettings();
    updateMenuBarVisibility();

    m_session_save_timer->setSingleShot(true);
    m_session_save_timer->setInterval(1000);
    connect(m_session_save_timer, &QTimer::timeout, this, &BrowserWindow::saveSessionState);

    m_tabs->onCurrentChanged = [this](int index) {
        NewTabTraceScope trace_scope("currentChanged");
        if (m_is_restoring_session) {
            m_active_tab = currentTab();
            updateCurrentTabState();
            return;
        }
        auto* next_tab = currentTab();
        {
            NewTabTraceScope scope("deactivate_other_tabs");
            for (int i = 0; i < m_tabs->count(); ++i) {
                auto* tab = m_tabs->tab(i);
                if (tab && tab != next_tab)
                    tab->setActive(false);
            }
        }
        m_active_tab = next_tab;
        if (auto* tab = currentTab()) {
            debug_log("tab_switch", tab->controllerId(), QStringLiteral("active=1"));
            {
                NewTabTraceScope scope("setActive_true", tab->controllerId());
                tab->setActive(true);
            }
            {
                NewTabTraceScope scope("applyControllerState", tab->controllerId());
                tab->applyControllerState();
            }
            // Mirror Ladybird: set Qt focus on the new tab's view so
            // focusInEvent fires → forward_focus(true) → Servo accepts key events.
            if (auto* view = tab->view()) {
                NewTabTraceScope scope("view_setFocus", tab->controllerId());
                view->setFocus(Qt::OtherFocusReason);
            }
        }
        updateCurrentTabState();
        scheduleSessionSave();
        // Defer activateTab so the triggering mouse event unwinds before we
        // reposition the wl_subsurface; doing it synchronously corrupts Qt/Wayland
        // event delivery (§0d).
        QPointer<Tab> target_tab = next_tab;
        QPointer<TabWidget> tabs = m_tabs;
        int serial = ++m_activation_serial;
        if (qEnvironmentVariableIsSet("SERVOQ_DEBUG")) {
            qInfo().nospace()
                << "SERVOQ_DEBUG activation_deferred serial=" << serial
                << " index=" << index
                << " target_tab_id=" << (next_tab ? next_tab->controllerId() : -1)
                << " mouse_buttons=" << static_cast<int>(QApplication::mouseButtons());
        }
        QTimer::singleShot(0, this, [this, tabs, target_tab, serial] {
            if (!tabs || !target_tab)
                return;
            if (serial != m_activation_serial)
                return;
            int idx = tabs->indexOf(target_tab);
            if (idx < 0)
                return;
            NewTabTraceScope scope("deferred_activateTab", target_tab->controllerId());
            tabs->activateTab(idx);
        });
    };
    // Defer the close (via QPointer, not raw index) so the close button's
    // mouseReleaseEvent unwinds first and a tab inserted in between can't cause a
    // stale-index crash.
    m_tabs->onTabCloseRequested = [this](int index) {
        QPointer<Tab> tab = m_tabs->tab(index);
        QTimer::singleShot(0, this, [this, tab] {
            if (!tab)
                return;
            int idx = m_tabs->indexOf(tab);
            if (idx >= 0)
                closeTab(idx);
        });
    };
    m_tabs->onNewTabRequested = [this] {
        newtab_trace_point("new_tab_button_clicked");
        createNewTab();
    };
    m_tabs->onTabsReordered = [this] {
        scheduleSessionSave();
    };
    m_tabs->setNewTabAction(m_new_tab_action);
    setCentralWidget(m_tabs);

    updateChromeStyle();
    loadPersistedClosedTabs();
    createInitialTab();
    applyBrowserChromeCursors(this);
    if (Settings::the()->is_maximized())
        showMaximized();
}

Tab* BrowserWindow::currentTab() const
{
    return m_tabs->currentTab();
}

int BrowserWindow::tabCount() const
{
    return m_tabs ? m_tabs->count() : 0;
}

void BrowserWindow::tabStateChanged(Tab* tab)
{
    auto index = m_tabs->indexOf(tab);
    if (index >= 0) {
        m_tabs->setTabText(index, tab->title());
        m_tabs->tabBar()->setTabIcon(index, tab->tabIcon());
    }
    if (tab == currentTab())
        updateCurrentTabState();
    scheduleSessionSave();
}

bool BrowserWindow::showMenuBar() const
{
    return show_menubar_option_available() && Settings::the()->show_menu_bar();
}

bool BrowserWindow::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        updateChromeStyle();
    return QMainWindow::event(event);
}

void BrowserWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        updateWindowBorder();
        if (m_fullscreen_action)
            m_fullscreen_action->setChecked(isFullScreen());
    }
}

bool BrowserWindow::shouldDrawWindowBorder() const
{
#if defined(Q_OS_MACOS)
    // macOS frameless windows already get rounded corners and a native shadow, so a painted border would clash.
    return false;
#else
    return windowFlags().testFlag(Qt::FramelessWindowHint) && !isFullScreen() && !isMaximized();
#endif
}

void BrowserWindow::updateWindowBorder()
{
    auto border_width = shouldDrawWindowBorder() ? 1 : 0;
    setContentsMargins(border_width, border_width, border_width, border_width);
    update();
}

void BrowserWindow::paintEvent(QPaintEvent* event)
{
    QMainWindow::paintEvent(event);

    if (!shouldDrawWindowBorder())
        return;

    QPainter painter(this);
    auto color = ChromeStyle::chrome_window_outline(palette());
    auto frame = rect();
    painter.fillRect(QRect(frame.left(), frame.top(), frame.width(), 1), color);
    painter.fillRect(QRect(frame.left(), frame.bottom(), frame.width(), 1), color);
    painter.fillRect(QRect(frame.left(), frame.top(), 1, frame.height()), color);
    painter.fillRect(QRect(frame.right(), frame.top(), 1, frame.height()), color);
}

// Intercepts QCoreApplication events posted by QtEventLoopWaker::wake() from
// Servo's background threads. The wake event (User+1) tells the Qt main thread
// that Servo has pending work (font loading complete, frame ready, etc.).
bool BrowserWindow::eventFilter(QObject* obj, QEvent* event)
{
    static constexpr QEvent::Type ServoWakeType = QEvent::Type(QEvent::User + 1);
    if (obj == qApp && event->type() == ServoWakeType) {
        rate_limited_tick_servo();
        return true;
    }
    // TEMPORARY DIAGNOSTICS: app-wide tracer for the text-input investigation.
    // qApp event filters see events for every object, so this shows exactly which
    // QObject a key event is delivered to and what holds focus at click time.
    if (servoq_diag_enabled()) {
        auto t = event->type();
        if (t == QEvent::KeyPress || t == QEvent::KeyRelease) {
            auto* ke = static_cast<QKeyEvent*>(event);
            servoq_diag_log(QStringLiteral("qApp::filter %1 -> receiver=%2 focusWidget=%3 focusWindow=%4 key=0x%5 text='%6'")
                .arg(t == QEvent::KeyPress ? QStringLiteral("KeyPress") : QStringLiteral("KeyRelease"))
                .arg(servoq_diag_describe(obj))
                .arg(servoq_diag_describe(QApplication::focusWidget()))
                .arg(servoq_diag_describe(QGuiApplication::focusWindow()))
                .arg(ke->key(), 0, 16)
                .arg(ke->text()));
        } else if (t == QEvent::MouseButtonPress) {
            servoq_diag_log(QStringLiteral("qApp::filter MousePress -> receiver=%1 focusWidget=%2 focusWindow=%3")
                .arg(servoq_diag_describe(obj))
                .arg(servoq_diag_describe(QApplication::focusWidget()))
                .arg(servoq_diag_describe(QGuiApplication::focusWindow())));
        }
    }
    if (event->type() == QEvent::MouseButtonPress)
        clearLocationEditFocusForMousePress(obj);
    if (event->type() == QEvent::ChildAdded) {
        if (auto* child_event = static_cast<QChildEvent*>(event); child_event->child()) {
            if (auto* widget = qobject_cast<QWidget*>(child_event->child()))
                applyBrowserChromeCursors(widget);
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void BrowserWindow::clearLocationEditFocusForMousePress(QObject* target)
{
    auto* focused = QApplication::focusWidget();
    auto* location_edit = dynamic_cast<LocationEdit*>(focused);
    if (!location_edit)
        return;
    if (is_child_or_self(target, location_edit) || is_location_completion_popup(target))
        return;

    if (debug_enabled()) {
        auto widget_name = target ? target->objectName() : QString {};
        auto class_name = target ? target->metaObject()->className() : "null";
        qInfo().nospace()
            << "SERVOQ_DEBUG focus_clear_location_edit reason=qt_chrome_click widget="
            << class_name << "(" << widget_name << ")";
    }
    location_edit->clearFocus();
}

void BrowserWindow::applyBrowserChromeCursors(QWidget* root)
{
    if (!root)
        return;
    if (auto* button = qobject_cast<QAbstractButton*>(root))
        button->setCursor(Qt::PointingHandCursor);
    for (auto* button : root->findChildren<QAbstractButton*>())
        button->setCursor(Qt::PointingHandCursor);
}

void BrowserWindow::createMenus()
{
    auto* file_menu = menuBar()->addMenu("&File");
    m_new_tab_action = new QAction("New &Tab", this);
    m_new_tab_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::AddTab));
    connect(m_new_tab_action, &QAction::triggered, this, [this] { createNewTab(); });
    file_menu->addAction(m_new_tab_action);
    m_hamburger_menu->addAction(m_new_tab_action);

    m_close_tab_action = new QAction("&Close Current Tab", this);
    m_close_tab_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::Close));
    connect(m_close_tab_action, &QAction::triggered, this, [this] { closeTab(m_tabs->currentIndex()); });
    file_menu->addAction(m_close_tab_action);
    m_hamburger_menu->addAction(m_close_tab_action);

    m_reopen_tab_action = new QAction("Reopen &Closed Tab", this);
    m_reopen_tab_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    m_reopen_tab_action->setEnabled(false);
    connect(m_reopen_tab_action, &QAction::triggered, this, [this] {
        reopenClosedTabAt(m_closed_tabs.size() - 1);
    });
    file_menu->addAction(m_reopen_tab_action);
    m_hamburger_menu->addAction(m_reopen_tab_action);

    auto* open_file_action = new QAction("&Open File…", this);
    open_file_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::Open));
    connect(open_file_action, &QAction::triggered, this, [this] {
        auto* dlg = new QFileDialog(this, QStringLiteral("Open File"), {},
            QStringLiteral("Web files (*.html *.htm *.xhtml *.svg *.xml *.txt *.pdf);;All files (*)"));
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setFileMode(QFileDialog::ExistingFile);
        connect(dlg, &QFileDialog::fileSelected, this, [this](QString const& path) {
            if (auto* tab = currentTab())
                tab->navigate(QUrl::fromLocalFile(path).toString());
        });
        dlg->open();
    });
    file_menu->addAction(open_file_action);
    m_hamburger_menu->addAction(open_file_action);

    auto* add_bookmark_action = new QAction("Add &Bookmark…", this);
    add_bookmark_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(add_bookmark_action, &QAction::triggered, this, [this] {
        if (auto* tab = currentTab(); tab && tab->bookmarksBar())
            tab->bookmarksBar()->showAddBookmarkDialog(tab->title(), tab->url(), tab->faviconBase64Png());
    });
    file_menu->addAction(add_bookmark_action);
    m_hamburger_menu->addAction(add_bookmark_action);

    auto* screenshot_action = new QAction("Take &Screenshot…", this);
    screenshot_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(screenshot_action, &QAction::triggered, this, [this] {
        auto* tab = currentTab();
        if (!tab || tab->isEmptyNewTab())
            return;
        // Asynchronous: Servo waits for the page to be render-stable, then
        // servoq::notify_screenshot_taken prompts for the save location.
        servoq::take_screenshot(tab->controllerId());
    });
    file_menu->addAction(screenshot_action);
    m_hamburger_menu->addAction(screenshot_action);

    file_menu->addSeparator();
    m_hamburger_menu->addSeparator();

    auto* quit_action = new QAction("&Quit", this);
    quit_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::Quit));
    connect(quit_action, &QAction::triggered, qApp, &QApplication::quit);
    file_menu->addAction(quit_action);
    m_hamburger_menu->addAction(quit_action);

    auto* edit_menu = menuBar()->addMenu("&Edit");
    m_find_action = new QAction("&Find in Page...", this);
    m_find_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::Find));
    connect(m_find_action, &QAction::triggered, this, [this] {
        if (auto* tab = currentTab())
            tab->showFindInPage();
    });
    edit_menu->addAction(m_find_action);

    for (auto const& shortcut : QKeySequence::keyBindings(QKeySequence::FindPrevious))
        new QShortcut(shortcut, this, [this] { if (auto* tab = currentTab()) tab->findPrevious(); });
    for (auto const& shortcut : QKeySequence::keyBindings(QKeySequence::FindNext))
        new QShortcut(shortcut, this, [this] { if (auto* tab = currentTab()) tab->findNext(); });

    m_hamburger_menu->addMenu(edit_menu);

    auto* history_menu = menuBar()->addMenu("&History");
    connect(history_menu, &QMenu::aboutToShow, this, [this, history_menu] {
        history_menu->clear();
        auto* show_history_action = history_menu->addAction(QStringLiteral("Show Full History\tCtrl+H"),
            this, [this] { openInternalPage(QStringLiteral("servoq://history")); });
        Q_UNUSED(show_history_action);
        history_menu->addSeparator();
        history_menu->addAction(m_reopen_tab_action);
        auto* recently_closed_menu = history_menu->addMenu(QStringLiteral("Recently Closed Tabs"));
        populateRecentlyClosedTabsMenu(recently_closed_menu);
        history_menu->addSeparator();
        auto* clear_action = history_menu->addAction(QStringLiteral("Clear History"), this, [this] {
            HistoryStore::the()->clearHistory();
        });
        Q_UNUSED(clear_action);
        auto const& entries = HistoryStore::the()->entries();
        if (!entries.isEmpty()) {
            history_menu->addSeparator();
            int count = qMin(entries.size(), 30);
            for (int i = 0; i < count; ++i) {
                auto const& e = entries[i];
                auto label = e.title.isEmpty() ? e.url : QStringLiteral("%1 — %2").arg(e.title, e.url);
                if (label.length() > 80)
                    label = label.left(77) + QStringLiteral("…");
                auto* act = history_menu->addAction(label, this, [this, url = e.url] {
                    createNewTab(url);
                });
                act->setIcon(FaviconStore::the()->iconForUrl(e.url));
                act->setIconVisibleInMenu(true);
                act->setToolTip(e.url);
            }
        }
    });
    m_hamburger_menu->addMenu(history_menu);

    auto* view_menu = menuBar()->addMenu("&View");
    m_toggle_bookmarks_action = new QAction("Toggle &Bookmarks Bar", this);
    m_toggle_bookmarks_action->setCheckable(true);
    m_toggle_bookmarks_action->setChecked(Settings::the()->show_bookmarks_bar());
    connect(m_toggle_bookmarks_action, &QAction::triggered, this, [this](bool visible) {
        Settings::the()->set_show_bookmarks_bar(visible);
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto* tab = m_tabs->tab(i); tab && tab->bookmarksBar())
                tab->bookmarksBar()->setVisible(visible);
        }
    });
    view_menu->addAction(m_toggle_bookmarks_action);

    view_menu->addSeparator();
    auto* tab_layout_group = new QActionGroup(this);
    tab_layout_group->setExclusive(true);

    m_horizontal_tabs_action = new QAction("&Horizontal Tabs", this);
    m_horizontal_tabs_action->setCheckable(true);
    m_horizontal_tabs_action->setChecked(!Settings::the()->vertical_tabs_enabled());
    tab_layout_group->addAction(m_horizontal_tabs_action);
    connect(m_horizontal_tabs_action, &QAction::triggered, this, [this] { setHorizontalTabs(); });
    view_menu->addAction(m_horizontal_tabs_action);

    m_vertical_tabs_collapsed_action = new QAction("Vertical Tabs: &Collapsed", this);
    m_vertical_tabs_collapsed_action->setCheckable(true);
    tab_layout_group->addAction(m_vertical_tabs_collapsed_action);
    connect(m_vertical_tabs_collapsed_action, &QAction::triggered, this, [this] { setVerticalTabsCollapsed(); });
    view_menu->addAction(m_vertical_tabs_collapsed_action);

    m_vertical_tabs_expanded_action = new QAction("Vertical Tabs: &Expanded", this);
    m_vertical_tabs_expanded_action->setCheckable(true);
    tab_layout_group->addAction(m_vertical_tabs_expanded_action);
    connect(m_vertical_tabs_expanded_action, &QAction::triggered, this, [this] { setVerticalTabsExpanded(); });
    view_menu->addAction(m_vertical_tabs_expanded_action);

    m_vertical_tabs_hover_expand_action = new QAction("Expand Vertical Tabs on &Hover", this);
    m_vertical_tabs_hover_expand_action->setCheckable(true);
    m_vertical_tabs_hover_expand_action->setEnabled(false);
    connect(m_vertical_tabs_hover_expand_action, &QAction::toggled, this, [this](bool enabled) {
        setVerticalTabsExpandOnHover(enabled);
    });
    view_menu->addAction(m_vertical_tabs_hover_expand_action);

    view_menu->addSeparator();

    auto* open_next_tab_action = new QAction("Open &Next Tab", this);
    open_next_tab_action->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::Key_PageDown),
        QKeySequence(Qt::CTRL | Qt::Key_Tab),
    });
    view_menu->addAction(open_next_tab_action);
    connect(open_next_tab_action, &QAction::triggered, this, &BrowserWindow::openNextTab);

    auto* open_previous_tab_action = new QAction("Open &Previous Tab", this);
    open_previous_tab_action->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::Key_PageUp),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab),
    });
    view_menu->addAction(open_previous_tab_action);
    connect(open_previous_tab_action, &QAction::triggered, this, &BrowserWindow::openPreviousTab);

    auto* search_tabs_action = new QAction("Search &Tabs…", this);
    search_tabs_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    view_menu->addAction(search_tabs_action);
    connect(search_tabs_action, &QAction::triggered, this, &BrowserWindow::showTabSearch);

    view_menu->addSeparator();
    {
        auto* zoom_in_action = new QAction("Zoom &In", this);
        zoom_in_action->setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_Plus), QKeySequence(Qt::CTRL | Qt::Key_Equal) });
        connect(zoom_in_action, &QAction::triggered, this, [this] {
            if (auto* tab = currentTab()) tab->zoomIn();
        });
        view_menu->addAction(zoom_in_action);

        auto* zoom_out_action = new QAction("Zoom &Out", this);
        zoom_out_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
        connect(zoom_out_action, &QAction::triggered, this, [this] {
            if (auto* tab = currentTab()) tab->zoomOut();
        });
        view_menu->addAction(zoom_out_action);

        auto* reset_zoom_action = new QAction("&Reset Zoom", this);
        reset_zoom_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
        connect(reset_zoom_action, &QAction::triggered, this, [this] {
            if (auto* tab = currentTab()) tab->resetZoom();
        });
        view_menu->addAction(reset_zoom_action);
    }

    // Developer JS console, hidden unless SERVOQ_DEBUG is set.
    if (qEnvironmentVariableIsSet("SERVOQ_DEBUG")) {
        view_menu->addSeparator();
        auto* js_console_action = new QAction(QStringLiteral("Evaluate &JavaScript…"), this);
        js_console_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J));
        connect(js_console_action, &QAction::triggered, this, &BrowserWindow::showJavaScriptConsole);
        view_menu->addAction(js_console_action);
        auto* debug_page_action = new QAction(QStringLiteral("Debug Page"), this);
        connect(debug_page_action, &QAction::triggered, this,
            [this] { openInternalPage(QStringLiteral("servoq://debug")); });
        view_menu->addAction(debug_page_action);
    }

    view_menu->addSeparator();
    m_fullscreen_action = new QAction("&Full Screen", this);
    m_fullscreen_action->setCheckable(true);
    auto fullscreen_shortcuts = QKeySequence::keyBindings(QKeySequence::FullScreen);
    if (!fullscreen_shortcuts.contains(QKeySequence(Qt::Key_F11)))
        fullscreen_shortcuts.append(QKeySequence(Qt::Key_F11));
    m_fullscreen_action->setShortcuts(fullscreen_shortcuts);
    connect(m_fullscreen_action, &QAction::triggered, this, [this](bool checked) {
        setFullscreen(checked);
    });
    view_menu->addAction(m_fullscreen_action);

    if (show_menubar_option_available()) {
        view_menu->addSeparator();
        m_show_menu_bar_action = new QAction("Show Menu Bar", this);
        m_show_menu_bar_action->setCheckable(true);
        m_show_menu_bar_action->setChecked(Settings::the()->show_menu_bar());
        connect(m_show_menu_bar_action, &QAction::toggled, this, [this](bool visible) {
            setShowMenuBar(visible);
        });
        view_menu->addAction(m_show_menu_bar_action);
    }
    m_hamburger_menu->addMenu(view_menu);

    // Browser settings live on the servoq://settings page; this is just a quick
    // entry point to it.
    auto* open_settings_page_action = new QAction(QStringLiteral("&Settings"), this);
    open_settings_page_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    open_settings_page_action->setMenuRole(QAction::PreferencesRole);
    connect(open_settings_page_action, &QAction::triggered, this,
        [this] { openInternalPage(QStringLiteral("servoq://settings")); });
    menuBar()->addAction(open_settings_page_action);
    m_hamburger_menu->addAction(open_settings_page_action);

    auto* help_menu = menuBar()->addMenu("&Help");
    auto* about_action = new QAction("About ServoQ", this);
    about_action->setEnabled(false);
    help_menu->addAction(about_action);
    m_hamburger_menu->addMenu(help_menu);

    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_H), this,
        [this] { openInternalPage(QStringLiteral("servoq://history")); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_J), this,
        [this] { openInternalPage(QStringLiteral("servoq://downloads")); });

    for (int i = 0; i <= 7; ++i) {
        new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)), this, [this, i] {
            if (m_tabs->count() <= 1)
                return;
            m_tabs->setCurrentIndex(std::min(i, m_tabs->count() - 1));
        });
    }
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_9), this, [this] {
        if (m_tabs->count() <= 1)
            return;
        m_tabs->setCurrentIndex(m_tabs->count() - 1);
    });
}

void BrowserWindow::closeEvent(QCloseEvent* event)
{
    if (m_session_save_timer)
        m_session_save_timer->stop();
    Settings::the()->set_last_position(pos());
    Settings::the()->set_last_size(size());
    Settings::the()->set_is_maximized(isMaximized());
    saveSessionState();
    servoq::begin_servo_shutdown();
    QMainWindow::closeEvent(event);
}

void BrowserWindow::createInitialTab()
{
    if (restoreSessionTabs())
        return;

    createNewTab();
    if (auto* tab = currentTab())
        tab->focusLocationEditor();
}

bool BrowserWindow::restoreSessionTabs()
{
    if (!Settings::the()->restore_session_on_startup())
        return false;

    auto entries = SessionStore::the()->tabs();
    if (entries.isEmpty())
        return false;

    m_is_restoring_session = true;
    for (auto const& entry : entries)
        createRestoredSessionTab(entry);

    auto active_index = SessionStore::the()->activeTabIndex();
    active_index = std::clamp(active_index, 0, m_tabs->count() - 1);
    m_tabs->setCurrentIndex(active_index);
    m_is_restoring_session = false;

    if (auto* tab = currentTab()) {
        m_active_tab = tab;
        tab->setActive(true);
        if (tab->isEmptyNewTab())
            tab->focusLocationEditor();
        else if (auto* view = tab->view())
            view->setFocus(Qt::OtherFocusReason);

        QPointer<Tab> target_tab = tab;
        QPointer<TabWidget> tabs = m_tabs;
        int serial = ++m_activation_serial;
        QTimer::singleShot(0, this, [this, tabs, target_tab, serial] {
            if (!tabs || !target_tab)
                return;
            if (serial != m_activation_serial)
                return;
            int idx = tabs->indexOf(target_tab);
            if (idx < 0)
                return;
            NewTabTraceScope scope("restore_activateTab", target_tab->controllerId());
            tabs->activateTab(idx);
        });
    }
    updateCurrentTabState();
    return true;
}

Tab* BrowserWindow::createRestoredSessionTab(SessionTabState const& entry)
{
    auto tab_id = servoq::create_tab();
    auto* tab = new Tab(this, tab_id);
    if (entry.is_empty_new_tab)
        tab->showEmptyNewTab();
    else
        tab->restoreSessionUrl(entry.url);
    // The session is saved with the pinned group first, so restoring in order
    // keeps the group contiguous; no reordering needed here.
    tab->setPinned(entry.pinned);

    auto index = m_tabs->addTab(tab, tab->title());
    // restoreSessionUrl ran before addTab, so its tabStateChanged couldn't
    // reach the tab bar yet; push the cached favicon/title now — restored
    // background tabs get no load events until activated.
    tabStateChanged(tab);
    tab->setHamburgerButtonVisible(!menuBar()->isVisible());
    debug_log("restore_session_tab", tab_id,
        QStringLiteral("index=%1 empty=%2 url=%3")
            .arg(index)
            .arg(entry.is_empty_new_tab ? 1 : 0)
            .arg(entry.url));
    return tab;
}

void BrowserWindow::createNewTab(QString const& url, bool background, bool use_configured_new_tab)
{
    NewTabTraceScope trace_scope("createNewTab");
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral(">>> createNewTab BEGIN url='%1' background=%2 use_configured_new_tab=%3 existing_tab_count=%4")
            .arg(url).arg(background ? 1 : 0).arg(use_configured_new_tab ? 1 : 0).arg(m_tabs->count()));
    auto target_url = url.trimmed();
    if (target_url.isEmpty() && use_configured_new_tab)
        target_url = Settings::the()->new_tab_url().trimmed();
    auto tab_id = servoq::create_tab();
    Tab* tab = nullptr;
    {
        NewTabTraceScope scope("Tab_construction", tab_id);
        tab = new Tab(this, tab_id);
    }
    int index = -1;
    {
        NewTabTraceScope scope("addTab", tab_id);
        index = m_tabs->addTab(tab, tab->title());
    }
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("createNewTab added tab_id=%1 index=%2; setCurrentIndex next").arg(tab_id).arg(index));
    if (!background) {
        NewTabTraceScope scope("setCurrentIndex", tab_id);
        m_tabs->setCurrentIndex(index);
    }
    debug_log("create_tab", tab_id, QStringLiteral("index=%1 active=%2").arg(index).arg(background ? 0 : 1));
    tab->setHamburgerButtonVisible(!menuBar()->isVisible());
    if (target_url.isEmpty()) {
        NewTabTraceScope scope("showEmptyNewTab", tab_id);
        tab->showEmptyNewTab();
        if (!background)
            tab->focusLocationEditor();
    } else {
        NewTabTraceScope scope("navigate", tab_id);
        tab->navigate(target_url);
    }
    {
        NewTabTraceScope scope("updateCurrentTabState", tab_id);
        updateCurrentTabState();
    }
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("<<< createNewTab END tab_id=%1").arg(tab_id));
    scheduleSessionSave();
}

void BrowserWindow::openTabForExistingId(int tab_id)
{
    auto* tab = new Tab(this, tab_id);
    auto index = m_tabs->addTab(tab, tab->title());
    m_tabs->setCurrentIndex(index);
    auto initial_url = QString::fromStdString(std::string(servoq::current_url(tab_id)));
    debug_log("openTabForExistingId", tab_id,
        QStringLiteral("index=%1 initial_url=%2").arg(index).arg(initial_url));
    tab->setHamburgerButtonVisible(!menuBar()->isVisible());
    tab->attachExistingWebView(initial_url);
    updateCurrentTabState();
    scheduleSessionSave();
}

void BrowserWindow::setFullscreen(bool fullscreen)
{
    if (fullscreen) {
        m_was_maximized_before_fullscreen = isMaximized();
        showFullScreen();
    } else {
        if (m_was_maximized_before_fullscreen)
            showMaximized();
        else
            showNormal();
    }
}

void BrowserWindow::closeTab(int index)
{
    if (index < 0 || index >= m_tabs->count())
        return;

    if (m_tabs->count() == 1) {
        close();
        return;
    }
    auto* tab = m_tabs->tab(index);
    auto tab_id = tab ? tab->controllerId() : 0;
    debug_log("close_tab", tab_id, QStringLiteral("index=%1 active=%2").arg(index).arg(tab == currentTab() ? 1 : 0));

    if (tab) {
        rememberClosedTab(*tab);
        if (tab == m_active_tab) {
            tab->setActive(false);
            m_active_tab = nullptr;
        }
    }

    m_tabs->removeTab(index);
    servoq::close_tab(tab_id);
    MprisManager::the()->onTabClosed(tab_id);
    updateCurrentTabState();
    scheduleSessionSave();
}

static constexpr int MaxRecentlyClosedTabs = 25;

void BrowserWindow::rememberClosedTab(Tab const& tab)
{
    // An empty new tab holds nothing to restore; Chrome doesn't add its NTP to
    // the recently-closed list either.
    if (tab.isEmptyNewTab())
        return;
    m_closed_tabs.append({
        tab.url(),
        tab.title(),
        tab.siteIcon(),
        tab.isEmptyNewTab(),
        tab.isPinned(),
    });
    if (m_closed_tabs.size() > MaxRecentlyClosedTabs)
        m_closed_tabs.removeFirst();
    updateRecentlyClosedActions();
    persistClosedTabs();
}

void BrowserWindow::reopenClosedTabAt(int index)
{
    if (index < 0 || index >= m_closed_tabs.size())
        return;

    auto entry = m_closed_tabs.takeAt(index);
    updateRecentlyClosedActions();
    persistClosedTabs();

    if (entry.was_empty_new_tab) {
        createNewTab({}, false, false);
        return;
    }
    createNewTab(entry.url);
    // Closed-while-pinned tabs come back pinned (Chrome behavior). The new tab
    // was appended and selected; pinning moves it into the pinned group.
    if (entry.was_pinned)
        m_tabs->setTabPinned(m_tabs->currentIndex(), true);
}

// Recently-closed tabs survive restarts (Chrome's Tabs_* session log and
// Firefox's sessionstore both do this); icons are re-resolved from the
// favicon DB when the menu is shown.
void BrowserWindow::loadPersistedClosedTabs()
{
    for (auto const& closed : SessionStore::the()->recentlyClosedTabs()) {
        m_closed_tabs.append({
            closed.url,
            closed.title,
            QIcon(),
            closed.was_empty_new_tab,
            closed.was_pinned,
        });
    }
    if (m_closed_tabs.size() > MaxRecentlyClosedTabs)
        m_closed_tabs.remove(0, m_closed_tabs.size() - MaxRecentlyClosedTabs);
    updateRecentlyClosedActions();
}

void BrowserWindow::persistClosedTabs()
{
    QVector<ClosedTabState> closed_tabs;
    closed_tabs.reserve(m_closed_tabs.size());
    for (auto const& entry : m_closed_tabs)
        closed_tabs.append({ entry.url, entry.title, entry.was_empty_new_tab, entry.was_pinned });
    SessionStore::the()->setRecentlyClosedTabs(closed_tabs);
}

void BrowserWindow::reopenAllClosedTabs()
{
    while (!m_closed_tabs.isEmpty())
        reopenClosedTabAt(m_closed_tabs.size() - 1);
}

void BrowserWindow::updateRecentlyClosedActions()
{
    if (m_reopen_tab_action)
        m_reopen_tab_action->setEnabled(!m_closed_tabs.isEmpty());
}

void BrowserWindow::populateRecentlyClosedTabsMenu(QMenu* menu)
{
    menu->clear();

    if (m_closed_tabs.isEmpty()) {
        menu->setEnabled(false);
        auto* empty_action = menu->addAction(QStringLiteral("No Recently Closed Tabs"));
        empty_action->setEnabled(false);
        return;
    }

    menu->setEnabled(true);
    constexpr int MaxMenuLabelWidth = 520;
    for (int i = m_closed_tabs.size() - 1; i >= 0; --i) {
        auto const& entry = m_closed_tabs[i];
        auto title = entry.title.trimmed();
        auto url = entry.url.trimmed();
        if (title.isEmpty())
            title = entry.was_empty_new_tab ? QStringLiteral("New Tab") : url;
        if (title.isEmpty())
            title = QStringLiteral("New Tab");
        auto label = url.isEmpty() || title == url ? title : QStringLiteral("%1 - %2").arg(title, url);

        auto* action = menu->addAction(QFontMetrics(menu->font()).elidedText(label, Qt::ElideRight, MaxMenuLabelWidth), this, [this, i] {
            reopenClosedTabAt(i);
        });
        auto icon = entry.icon;
        if (icon.isNull())
            icon = FaviconStore::the()->iconForUrl(entry.url);
        action->setIcon(icon);
        action->setIconVisibleInMenu(true);
        action->setToolTip(entry.url);
    }

    menu->addSeparator();
    menu->addAction(QStringLiteral("Reopen All Tabs"), this, [this] {
        reopenAllClosedTabs();
    });
    menu->addAction(QStringLiteral("Clear Recently Closed Tabs"), this, [this] {
        m_closed_tabs.clear();
        updateRecentlyClosedActions();
        persistClosedTabs();
    });
}

void BrowserWindow::showTabSearch()
{
    if (!m_tab_search)
        m_tab_search = new TabSearchPopup(this, m_tabs);
    m_tab_search->open();
}

void BrowserWindow::closeTabForController(int controller_id)
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* tab = m_tabs->tab(i);
        if (tab && tab->controllerId() == controller_id) {
            closeTab(i);
            return;
        }
    }
    debug_log("close_requested_unknown_controller", controller_id, {});
}

void BrowserWindow::setHorizontalTabs()
{
    m_tabs->setVerticalTabsEnabled(false);
    m_tabs->setVerticalTabsExpandOnHover(false);
    Settings::the()->set_vertical_tabs_enabled(false);
    Settings::the()->set_vertical_tabs_expanded(false);
    Settings::the()->set_vertical_tabs_expand_on_hover(false);
    if (m_horizontal_tabs_action)
        m_horizontal_tabs_action->setChecked(true);
    if (m_vertical_tabs_hover_expand_action) {
        m_vertical_tabs_hover_expand_action->setChecked(false);
        m_vertical_tabs_hover_expand_action->setEnabled(false);
    }
}

void BrowserWindow::setVerticalTabsCollapsed()
{
    m_tabs->setVerticalTabsEnabled(true);
    m_tabs->setVerticalTabsExpanded(false);
    Settings::the()->set_vertical_tabs_enabled(true);
    Settings::the()->set_vertical_tabs_expanded(false);
    if (m_vertical_tabs_collapsed_action)
        m_vertical_tabs_collapsed_action->setChecked(true);
    if (m_vertical_tabs_hover_expand_action)
        m_vertical_tabs_hover_expand_action->setEnabled(true);
}

void BrowserWindow::setVerticalTabsExpanded()
{
    m_tabs->setVerticalTabsEnabled(true);
    m_tabs->setVerticalTabsExpanded(true);
    Settings::the()->set_vertical_tabs_enabled(true);
    Settings::the()->set_vertical_tabs_expanded(true);
    if (m_vertical_tabs_expanded_action)
        m_vertical_tabs_expanded_action->setChecked(true);
    if (m_vertical_tabs_hover_expand_action)
        m_vertical_tabs_hover_expand_action->setEnabled(true);
}

void BrowserWindow::setVerticalTabsExpandOnHover(bool enabled)
{
    m_tabs->setVerticalTabsExpandOnHover(enabled);
    Settings::the()->set_vertical_tabs_expand_on_hover(enabled);
}

void BrowserWindow::toggleVerticalTabsExpanded()
{
    if (!Settings::the()->vertical_tabs_enabled() || !Settings::the()->vertical_tabs_expanded())
        setVerticalTabsExpanded();
    else
        setVerticalTabsCollapsed();

    // Update the icon on all tabs since setVerticalTabsExpanded/Collapsed only calls
    // setVerticalTabsEnabled(true) which is a no-op when already enabled.
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* tab = m_tabs->tab(i))
            tab->updateToggleVerticalTabsIcon();
    }
}

void BrowserWindow::openNextTab()
{
    if (m_tabs->count() <= 1)
        return;
    auto next_index = m_tabs->currentIndex() + 1;
    if (next_index >= m_tabs->count())
        next_index = 0;
    m_tabs->setCurrentIndex(next_index);
}

void BrowserWindow::openPreviousTab()
{
    if (m_tabs->count() <= 1)
        return;
    auto next_index = m_tabs->currentIndex() - 1;
    if (next_index < 0)
        next_index = m_tabs->count() - 1;
    m_tabs->setCurrentIndex(next_index);
}

void BrowserWindow::setShowMenuBar(bool visible)
{
    Settings::the()->set_show_menu_bar(visible && show_menubar_option_available());
    if (m_show_menu_bar_action && m_show_menu_bar_action->isChecked() != Settings::the()->show_menu_bar())
        m_show_menu_bar_action->setChecked(Settings::the()->show_menu_bar());
    updateMenuBarVisibility();
}

void BrowserWindow::updateMenuBarVisibility()
{
    auto show_menu_bar = showMenuBar();
    menuBar()->setVisible(show_menu_bar);
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* tab = m_tabs->tab(i))
            tab->setHamburgerButtonVisible(!show_menu_bar);
    }
}

void BrowserWindow::applySettings()
{
    servoq::set_experimental_features_enabled(Settings::the()->experimental_features_enabled());

    auto vertical_tabs_enabled = Settings::the()->vertical_tabs_enabled();
    auto vertical_tabs_expanded = Settings::the()->vertical_tabs_expanded();
    m_tabs->setVerticalTabsEnabled(vertical_tabs_enabled);
    m_tabs->setVerticalTabsExpanded(vertical_tabs_expanded);
    m_tabs->setVerticalTabsExpandOnHover(Settings::the()->vertical_tabs_expand_on_hover());
    if (m_horizontal_tabs_action)
        m_horizontal_tabs_action->setChecked(!vertical_tabs_enabled);
    if (m_vertical_tabs_collapsed_action)
        m_vertical_tabs_collapsed_action->setChecked(vertical_tabs_enabled && !vertical_tabs_expanded);
    if (m_vertical_tabs_expanded_action)
        m_vertical_tabs_expanded_action->setChecked(vertical_tabs_enabled && vertical_tabs_expanded);
    if (m_vertical_tabs_hover_expand_action) {
        m_vertical_tabs_hover_expand_action->setEnabled(vertical_tabs_enabled);
        m_vertical_tabs_hover_expand_action->setChecked(Settings::the()->vertical_tabs_expand_on_hover());
    }
    if (m_toggle_bookmarks_action)
        m_toggle_bookmarks_action->setChecked(Settings::the()->show_bookmarks_bar());
    refreshHomeButtons();
}

void BrowserWindow::onSettingsChangedFromPage()
{
    // The servoq://settings page wrote a value directly to Settings; re-apply
    // everything chrome-side so the change is reflected live (tab layout,
    // toolbar buttons, bookmarks bar, menu bar, home button).
    applySettings();
    if (m_show_menu_bar_action)
        m_show_menu_bar_action->setChecked(Settings::the()->show_menu_bar());
    updateMenuBarVisibility();
    refreshBookmarksBars();
    // Keep the persisted session in sync with the "continue where you left off"
    // toggle: enabling it snapshots the current tabs, disabling it clears the
    // stored session (the side effect the old menu checkbox had).
    if (Settings::the()->restore_session_on_startup())
        saveSessionState();
    else
        SessionStore::the()->clearTabs();
}

void BrowserWindow::openInternalPage(QString const& url)
{
    auto* tab = currentTab();
    // Open in the current tab when it is empty/already an internal page,
    // otherwise a new tab — matching Chrome's chrome:// behavior.
    if (tab && (tab->isEmptyNewTab() || tab->isInternalPage()))
        tab->navigate(url);
    else
        createNewTab(url);
}

// Developer JS console (SERVOQ_DEBUG only): non-modal dialog that evaluates
// the entered script in the current tab via the M3.7 bridge and prints the
// JSON result or error. The future servoq://debug page (M4.4) replaces this.
void BrowserWindow::showJavaScriptConsole()
{
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Evaluate JavaScript"));
    dialog->resize(600, 440);

    auto* layout = new QVBoxLayout(dialog);
    auto* input = new QPlainTextEdit(dialog);
    input->setPlaceholderText(QStringLiteral("JavaScript to evaluate in the current tab…"));
    layout->addWidget(input, 1);

    auto* run_button = new QPushButton(QStringLiteral("Run (Ctrl+Enter)"), dialog);
    layout->addWidget(run_button);

    auto* output = new QPlainTextEdit(dialog);
    output->setReadOnly(true);
    output->setPlaceholderText(QStringLiteral("Result"));
    layout->addWidget(output, 1);

    auto run_script = [this, input, output] {
        auto* tab = currentTab();
        if (!tab || tab->isEmptyNewTab()) {
            output->setPlainText(QStringLiteral("No page in the current tab."));
            return;
        }
        auto script = input->toPlainText();
        if (script.trimmed().isEmpty())
            return;
        output->setPlainText(QStringLiteral("Evaluating…"));
        QPointer<QPlainTextEdit> result_view(output);
        evaluate_javascript_in_tab(tab->controllerId(), script,
            [result_view](bool success, QString const& result) {
                if (!result_view)
                    return; // Console closed before the page answered.
                result_view->setPlainText(success
                        ? result
                        : QStringLiteral("Error: %1").arg(result));
            });
    };
    connect(run_button, &QPushButton::clicked, dialog, run_script);
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), dialog, run_script);

    input->setFocus();
    dialog->show();
}


void BrowserWindow::refreshHomeButtons()
{
    auto visible = Settings::the()->show_home_button();
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* tab = m_tabs->tab(i))
            tab->setHomeButtonVisible(visible);
    }
}

void BrowserWindow::scheduleSessionSave()
{
    if (m_is_restoring_session || !Settings::the()->restore_session_on_startup())
        return;
    if (m_session_save_timer)
        m_session_save_timer->start();
}

void BrowserWindow::saveSessionState()
{
    if (!Settings::the()->restore_session_on_startup()) {
        SessionStore::the()->clearTabs();
        return;
    }

    QVector<SessionTabState> tabs;
    tabs.reserve(m_tabs->count());
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* tab = m_tabs->tab(i);
        if (!tab)
            continue;
        tabs.append({
            tab->isEmptyNewTab() ? QStringLiteral("about:blank") : tab->url(),
            tab->isEmptyNewTab(),
            tab->isPinned(),
        });
    }

    if (tabs.isEmpty()) {
        SessionStore::the()->clearTabs();
        return;
    }

    SessionStore::the()->setTabs(tabs, m_tabs->currentIndex());
}

void BrowserWindow::refreshBookmarksBars()
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* tab = m_tabs->tab(i); tab && tab->bookmarksBar())
            tab->bookmarksBar()->rebuild();
    }
}

void BrowserWindow::updateCurrentTabState()
{
    auto* tab = currentTab();
    if (!tab)
        return;
    setWindowTitle(QStringLiteral("%1 - ServoQ").arg(tab->title()));
    // Status/link-hover text is shown by Tab's in-view m_hover_label
    // Reference BrowserWindow does not route link hover to statusBar().
}

void BrowserWindow::updateChromeStyle()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    qApp->setStyleSheet(ChromeStyle::application_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

void BrowserWindow::wheelEvent(QWheelEvent* event)
{
    if (!currentTab())
        return;

    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        if (event->angleDelta().y() > 0)
            currentTab()->zoomIn();
        else if (event->angleDelta().y() < 0)
            currentTab()->zoomOut();
    }
}

void BrowserWindow::closeTabFromContextMenu(int index)
{
    closeTab(index);
}

// Bulk closes skip pinned tabs, like Chrome's "Close other tabs".
void BrowserWindow::closeOtherTabs(int keep_index)
{
    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        auto* tab = m_tabs->tab(i);
        if (i != keep_index && tab && !tab->isPinned())
            closeTab(i);
    }
}

void BrowserWindow::closeTabsToRight(int from_index)
{
    for (int i = m_tabs->count() - 1; i > from_index; --i) {
        auto* tab = m_tabs->tab(i);
        if (tab && !tab->isPinned())
            closeTab(i);
    }
}

}
