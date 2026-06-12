/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Simon Farre <simon.farre.cx@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
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
#include "HistoryStore.h"
#include "NewTabTrace.h"
#include "ChromeLayout.h"
#include "ChromeStyle.h"
#include "Icon.h"
#include "LocationEdit.h"
#include "Settings.h"
#include "Tab.h"
#include "TabBar.h"
#include "WebContentView.h"
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
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QShortcut>
#include <QStatusBar>
#include <QDesktopServices>
#include <QMessageBox>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QUrl>
#include <QWidgetAction>
#include <QCloseEvent>
#include <QDebug>
#include <memory>
#include <QPointer>
#include <QElapsedTimer>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

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

// Rate-limited wake-driven Servo ticking.
//
// During a page load Servo's background threads can wake the Qt loop ~25k times a
// second. Because the wake-pending flag was cleared *before* each tick, every wake
// posted a fresh event and span the event loop again — and since
// servo.spin_event_loop() drains ALL pending work in a single call, the 2nd..Nth
// spins of a burst are near-no-ops that still burned ~50% of a core (measured via
// SERVOQ_PERF: ticks≈25k/s, tick_ms≈500/s) and starved Qt's own paint/input.
//
// Here we tick at most once per kServoTickIntervalMs. If a wake arrives sooner we
// leave the wake-pending flag SET — so further background wakes coalesce instead of
// posting new events — and schedule exactly one catch-up tick at the interval
// boundary. Because spin drains everything, this is correctness-preserving: no wake
// is ever dropped, it is at most delayed by kServoTickIntervalMs (well under one
// frame). All state is touched only on the Qt main thread (eventFilter + the
// singleShot callback). servoq::tick_servo() is itself a no-op once shutdown has
// started, so the catch-up timer needs no extra guard.
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
{
    // Install event filter on qApp so BrowserWindow::eventFilter() receives the
    // QtEventLoopWaker wake event (QEvent::User+1) posted from Servo's threads.
    qApp->installEventFilter(this);

    // Main-thread jank detector (SERVOQ_PERF or SERVOQ_NEWTAB_TRACE): a 50 ms
    // heartbeat whose gap reveals event-loop stalls that the per-second PERF
    // flush cannot show (a blocked thread prints nothing until it unblocks).
    // newtab_last_phase() names the section that was running when it stalled.
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
    setMinimumSize(900, 640);
    resize(Settings::the()->last_size());
    if (auto last_position = Settings::the()->last_position(); last_position.has_value())
        move(*last_position);

    createMenus();
    applySettings();
    updateMenuBarVisibility();

    m_tabs->onCurrentChanged = [this](int index) {
        NewTabTraceScope trace_scope("currentChanged");
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
        // Defer activateTab so any mouse event that triggered the tab switch
        // fully unwinds before we show/hide/reposition the native Wayland
        // wl_subsurface. Doing this synchronously corrupts Qt/Wayland event
        // delivery and leaves the container in an unmapped state.
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
    // Defer tab close to the next event-loop spin via QPointer so the close-button's
    // mouseReleaseEvent fully unwinds before we remove anything. Capturing a QPointer
    // rather than the raw index prevents a stale-index crash if another tab is
    // inserted (e.g. via request_open_tab_for_id during a nested servo tick) between
    // the moment the close button is clicked and when the deferred lambda runs.
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
    m_tabs->setNewTabAction(m_new_tab_action);
    setCentralWidget(m_tabs);

    updateChromeStyle();
    createInitialTab();
    applyBrowserChromeCursors(this);
    if (Settings::the()->is_maximized())
        showMaximized();
}

Tab* BrowserWindow::currentTab() const
{
    return m_tabs->currentTab();
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

    m_reopen_tab_action = new QAction("&Reopen Last Tab", this);
    m_reopen_tab_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    m_reopen_tab_action->setEnabled(false); // disabled until a tab is closed
    connect(m_reopen_tab_action, &QAction::triggered, this, [this] {
        if (m_closed_tabs.isEmpty()) return;
        auto [url, title] = m_closed_tabs.takeLast(); // pop from top of stack
        createNewTab(url);
        m_reopen_tab_action->setEnabled(!m_closed_tabs.isEmpty());
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
            tab->bookmarksBar()->showAddBookmarkDialog(tab->title(), tab->url());
    });
    file_menu->addAction(add_bookmark_action);
    m_hamburger_menu->addAction(add_bookmark_action);

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

    auto* settings_menu = menuBar()->addMenu("&Settings");
    auto* experimental_action = new QWidgetAction(this);
    auto* experimental_checkbox = new QCheckBox("Experimental Web Platform Features", this);
    experimental_checkbox->setChecked(Settings::the()->experimental_features_enabled());
    connect(experimental_checkbox, &QCheckBox::toggled, this, [](bool enabled) {
        Settings::the()->set_experimental_features_enabled(enabled);
        servoq::set_experimental_features_enabled(enabled);
    });
    experimental_action->setDefaultWidget(experimental_checkbox);
    settings_menu->addAction(experimental_action);

    auto* content_blocking_action = new QWidgetAction(this);
    auto* content_blocking_checkbox = new QCheckBox("Block trackers and ads", this);
    content_blocking_checkbox->setChecked(Settings::the()->content_blocking_enabled());
    connect(content_blocking_checkbox, &QCheckBox::toggled, this, [](bool enabled) {
        Settings::the()->set_content_blocking_enabled(enabled);
    });
    content_blocking_action->setDefaultWidget(content_blocking_checkbox);
    settings_menu->addAction(content_blocking_action);

    auto current_content_blocking_host = [this] {
        auto* tab = currentTab();
        if (!tab)
            return QString {};
        return QUrl(tab->url()).host().toLower();
    };

    auto* site_blocking_action = new QAction(this);
    connect(settings_menu, &QMenu::aboutToShow, this, [site_blocking_action, current_content_blocking_host] {
        auto host = current_content_blocking_host();
        site_blocking_action->setEnabled(!host.isEmpty());
        if (host.isEmpty()) {
            site_blocking_action->setText(QStringLiteral("Content Blocking for This Site"));
            return;
        }
        bool disabled = Settings::the()->content_blocking_disabled_for_host(host);
        site_blocking_action->setText(disabled
            ? QStringLiteral("Enable Blocking for %1").arg(host)
            : QStringLiteral("Disable Blocking for %1").arg(host));
    });
    connect(site_blocking_action, &QAction::triggered, this, [current_content_blocking_host] {
        auto host = current_content_blocking_host();
        if (host.isEmpty())
            return;
        bool disabled = Settings::the()->content_blocking_disabled_for_host(host);
        Settings::the()->set_content_blocking_disabled_for_host(host, !disabled);
    });
    settings_menu->addAction(site_blocking_action);

    // Search engine selector
    auto* search_engine_action = new QWidgetAction(this);
    auto* search_engine_widget = new QWidget(this);
    auto* search_engine_layout = new QHBoxLayout(search_engine_widget);
    search_engine_layout->setContentsMargins(16, 4, 8, 4);
    search_engine_layout->addWidget(new QLabel("Search engine:", search_engine_widget));
    auto* search_combo = new QComboBox(search_engine_widget);
    auto refresh_search_combo = [search_combo] {
        auto selected = Settings::the()->search_engine_name();
        search_combo->blockSignals(true);
        search_combo->clear();
        search_combo->addItems(Settings::the()->search_engine_names());
        auto index = search_combo->findText(selected);
        search_combo->setCurrentIndex(index >= 0 ? index : search_combo->findText(QStringLiteral("DuckDuckGo")));
        search_combo->blockSignals(false);
    };
    refresh_search_combo();
    search_combo->setCurrentText(Settings::the()->search_engine_name());
    connect(search_combo, &QComboBox::currentTextChanged, this, [](QString const& name) {
        Settings::the()->set_search_engine_name(name);
    });
    search_engine_layout->addWidget(search_combo);
    search_engine_action->setDefaultWidget(search_engine_widget);
    settings_menu->addAction(search_engine_action);

    auto* add_search_engine_action = new QAction(QStringLiteral("Add Custom Search Engine…"), this);
    connect(add_search_engine_action, &QAction::triggered, this, [this, search_combo, refresh_search_combo] {
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Add Search Engine"));
        auto* layout = new QFormLayout(dialog);
        auto* name_edit = new QLineEdit(dialog);
        auto* template_edit = new QLineEdit(dialog);
        template_edit->setPlaceholderText(QStringLiteral("https://example.com/search?q=%s"));
        layout->addRow(QStringLiteral("Name:"), name_edit);
        layout->addRow(QStringLiteral("Query URL:"), template_edit);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
        layout->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(dialog, &QDialog::accepted, this, [this, dialog, name_edit, template_edit, search_combo, refresh_search_combo] {
            if (!Settings::the()->add_custom_search_engine(name_edit->text(), template_edit->text())) {
                QMessageBox::warning(this, QStringLiteral("Search Engine"),
                    QStringLiteral("Custom search engines need a unique name and a query URL containing %s."));
                return;
            }
            Settings::the()->set_search_engine_name(name_edit->text().trimmed());
            refresh_search_combo();
            search_combo->setCurrentText(name_edit->text().trimmed());
        });
        dialog->open();
    });
    settings_menu->addAction(add_search_engine_action);

    auto* remove_search_engine_action = new QAction(QStringLiteral("Remove Current Custom Search Engine"), this);
    connect(settings_menu, &QMenu::aboutToShow, this, [search_combo, remove_search_engine_action] {
        remove_search_engine_action->setEnabled(Settings::the()->is_custom_search_engine(search_combo->currentText()));
    });
    connect(remove_search_engine_action, &QAction::triggered, this, [search_combo, refresh_search_combo] {
        auto name = search_combo->currentText();
        if (!Settings::the()->is_custom_search_engine(name))
            return;
        Settings::the()->remove_custom_search_engine(name);
        refresh_search_combo();
    });
    settings_menu->addAction(remove_search_engine_action);

    settings_menu->addSeparator();

    auto* blocklist_action = new QAction(QStringLiteral("Custom filter list…"), this);
    connect(blocklist_action, &QAction::triggered, this, [] {
        auto blocklist_path = QString::fromStdString(std::string(servoq::user_blocklist_path()));
        QFileInfo info(blocklist_path);
        QDir().mkpath(info.absolutePath());
        if (!info.exists()) {
            QFile file(blocklist_path);
            if (file.open(QIODevice::WriteOnly))
                file.close();
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(blocklist_path));
        QMessageBox::information(nullptr,
            QStringLiteral("Custom filter list"),
            QStringLiteral("Place EasyList-compatible rules in:\n%1\n\nUse Reload Filter Lists to apply changes.").arg(blocklist_path));
    });
    settings_menu->addAction(blocklist_action);

    auto* reload_filter_lists_action = new QAction(QStringLiteral("Reload Filter Lists"), this);
    connect(reload_filter_lists_action, &QAction::triggered, this, [this] {
        bool ok = servoq::reload_blocklists();
        QMessageBox::information(this, QStringLiteral("Filter Lists"),
            ok ? QStringLiteral("Filter lists reloaded.") : QStringLiteral("Filter lists could not be reloaded."));
    });
    settings_menu->addAction(reload_filter_lists_action);

    m_hamburger_menu->addMenu(settings_menu);

    auto* help_menu = menuBar()->addMenu("&Help");
    auto* about_action = new QAction("About ServoQ", this);
    about_action->setEnabled(false);
    help_menu->addAction(about_action);
    m_hamburger_menu->addMenu(help_menu);

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
    Settings::the()->set_last_position(pos());
    Settings::the()->set_last_size(size());
    Settings::the()->set_is_maximized(isMaximized());
    servoq::begin_servo_shutdown();
    QMainWindow::closeEvent(event);
}

void BrowserWindow::createInitialTab()
{
    createNewTab();
    if (auto* tab = currentTab())
        tab->focusLocationEditor();
}

void BrowserWindow::createNewTab(QString const& url, bool background)
{
    NewTabTraceScope trace_scope("createNewTab");
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral(">>> createNewTab BEGIN url='%1' background=%2 existing_tab_count=%3")
            .arg(url).arg(background ? 1 : 0).arg(m_tabs->count()));
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
    if (url.trimmed().isEmpty()) {
        NewTabTraceScope scope("showEmptyNewTab", tab_id);
        tab->showEmptyNewTab();
        tab->focusLocationEditor();
    } else {
        NewTabTraceScope scope("navigate", tab_id);
        tab->navigate(url);
    }
    {
        NewTabTraceScope scope("updateCurrentTabState", tab_id);
        updateCurrentTabState();
    }
    if (servoq_diag_enabled())
        servoq_diag_log(QStringLiteral("<<< createNewTab END tab_id=%1").arg(tab_id));
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
        m_closed_tabs.append({ tab->url(), tab->title() });
        if (m_closed_tabs.size() > 10)
            m_closed_tabs.removeFirst();
        if (m_reopen_tab_action)
            m_reopen_tab_action->setEnabled(true);
        if (tab == m_active_tab) {
            tab->setActive(false);
            m_active_tab = nullptr;
        }
    }

    m_tabs->removeTab(index);
    servoq::close_tab(tab_id);
    updateCurrentTabState();
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

void BrowserWindow::closeOtherTabs(int keep_index)
{
    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        if (i != keep_index)
            closeTab(i);
    }
}

void BrowserWindow::closeTabsToRight(int from_index)
{
    for (int i = m_tabs->count() - 1; i > from_index; --i)
        closeTab(i);
}

}
