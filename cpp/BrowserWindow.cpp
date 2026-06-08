#include "BrowserWindow.h"
#include "BookmarksBar.h"
#include "HistoryStore.h"
#include "ChromeLayout.h"
#include "ChromeStyle.h"
#include "Icon.h"
#include "Settings.h"
#include "Tab.h"
#include "TabBar.h"
#include "servo_callbacks.h"
#include "servoq/src/bridge.rs.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QFileDialog>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QShortcut>
#include <QStatusBar>
#include <QDesktopServices>
#include <QMessageBox>
#include <QStandardPaths>
#include <QUrl>
#include <QWidgetAction>
#include <QCloseEvent>
#include <QDebug>
#include <QWheelEvent>

#include <algorithm>

namespace ServoQ {

namespace {

bool debug_enabled()
{
    return qEnvironmentVariableIsSet("SERVOQ_DEBUG");
}

void debug_log(char const* event, int tab_id, QString const& detail)
{
    if (debug_enabled())
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " tab_id=" << tab_id << " " << detail;
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

    setWindowTitle("ServoQ");
    setWindowIcon(app_icon());
    setMinimumSize(900, 640);
    resize(Settings::the()->last_size());
    if (auto last_position = Settings::the()->last_position(); last_position.has_value())
        move(*last_position);

    createMenus();
    applySettings();
    updateMenuBarVisibility();

    m_tabs->onCurrentChanged = [this](int) {
        auto* previous_tab = m_active_tab;
        auto* next_tab = currentTab();
        if (previous_tab && previous_tab != next_tab) {
            // [ladybird: BrowserWindow.cpp:696-702] inactive tab becomes hidden.
            previous_tab->setActive(false);
        }
        m_active_tab = next_tab;
        if (auto* tab = currentTab()) {
            debug_log("tab_switch", tab->controllerId(), QStringLiteral("active=1"));
            // [ladybird: BrowserWindow.cpp:701-702] active tab becomes visible.
            tab->setActive(true);
            tab->applyControllerState();
        }
        updateCurrentTabState();
    };
    m_tabs->onTabCloseRequested = [this](int index) { closeTab(index); };
    m_tabs->onNewTabRequested = [this] { createNewTab(); };
    m_tabs->setNewTabAction(m_new_tab_action);
    setCentralWidget(m_tabs);

    updateChromeStyle();
    createInitialTab();
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
        servoq::mark_servo_wake_event_consumed();
        servoq::tick_servo();
        return true;
    }
    return QMainWindow::eventFilter(obj, event);
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

    // [ladybird: BrowserWindow.cpp — reopen last closed tab, Ctrl+Shift+T]
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

    // [ladybird: BrowserWindow.cpp ~line 260 — Ctrl+O open local file]
    auto* open_file_action = new QAction("&Open File…", this);
    open_file_action->setShortcuts(QKeySequence::keyBindings(QKeySequence::Open));
    connect(open_file_action, &QAction::triggered, this, [this] {
        if (auto* tab = currentTab()) {
            // [ladybird: BrowserWindow.cpp:263] filter matches common web file types
            auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Open File"), {},
                QStringLiteral("Web files (*.html *.htm *.xhtml *.svg *.xml *.txt *.pdf);;All files (*)"));
            if (!path.isEmpty())
                tab->navigate(QUrl::fromLocalFile(path).toString());
        }
    });
    file_menu->addAction(open_file_action);
    m_hamburger_menu->addAction(open_file_action);

    // [ladybird: BrowserWindow.cpp:360 — Ctrl+D add bookmark]
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

    // [ladybird: BrowserWindow.cpp:311-323]
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

    // [ladybird: BrowserWindow.cpp:334-356]
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

    // [ladybird: BrowserWindow.cpp:358-360] — zoom submenu added to View menu
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
    auto* content_blocking_action = new QWidgetAction(this);
    auto* content_blocking_checkbox = new QCheckBox("Block trackers and ads", this);
    content_blocking_checkbox->setChecked(Settings::the()->content_blocking_enabled());
    connect(content_blocking_checkbox, &QCheckBox::toggled, this, [](bool enabled) {
        Settings::the()->set_content_blocking_enabled(enabled);
    });
    content_blocking_action->setDefaultWidget(content_blocking_checkbox);
    settings_menu->addAction(content_blocking_action);

    // Search engine selector
    auto* search_engine_action = new QWidgetAction(this);
    auto* search_engine_widget = new QWidget(this);
    auto* search_engine_layout = new QHBoxLayout(search_engine_widget);
    search_engine_layout->setContentsMargins(16, 4, 8, 4);
    search_engine_layout->addWidget(new QLabel("Search engine:", search_engine_widget));
    auto* search_combo = new QComboBox(search_engine_widget);
    search_combo->addItems({ "DuckDuckGo", "Google", "Yandex" });
    search_combo->setCurrentText(Settings::the()->search_engine_name());
    connect(search_combo, &QComboBox::currentTextChanged, this, [](QString const& name) {
        Settings::the()->set_search_engine_name(name);
    });
    search_engine_layout->addWidget(search_combo);
    search_engine_action->setDefaultWidget(search_engine_widget);
    settings_menu->addAction(search_engine_action);

    settings_menu->addSeparator();

    auto* blocklist_action = new QAction(QStringLiteral("Custom filter list…"), this);
    connect(blocklist_action, &QAction::triggered, this, [] {
        auto config_dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        auto blocklist_path = config_dir + QStringLiteral("/blocklist.txt");
        QDesktopServices::openUrl(QUrl::fromLocalFile(blocklist_path));
        QMessageBox::information(nullptr,
            QStringLiteral("Custom filter list"),
            QStringLiteral("Place EasyList-compatible rules in:\n%1\n\nRestart ServoQ to apply changes.").arg(blocklist_path));
    });
    settings_menu->addAction(blocklist_action);

    m_hamburger_menu->addMenu(settings_menu);

    auto* help_menu = menuBar()->addMenu("&Help");
    auto* about_action = new QAction("About ServoQ", this);
    about_action->setEnabled(false);
    help_menu->addAction(about_action);
    m_hamburger_menu->addMenu(help_menu);

    // [ladybird: BrowserWindow.cpp:447-461]
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
    createNewTab(QStringLiteral("about:blank"));
    if (auto* tab = currentTab())
        tab->focusLocationEditor();
}

void BrowserWindow::createNewTab(QString const& url)
{
    auto tab_id = servoq::create_tab();
    auto* tab = new Tab(this, tab_id);
    auto index = m_tabs->addTab(tab, tab->title());
    m_tabs->setCurrentIndex(index);
    debug_log("create_tab", tab_id, QStringLiteral("index=%1 active=1").arg(index));
    tab->setHamburgerButtonVisible(!menuBar()->isVisible());
    if (url != QStringLiteral("about:blank"))
        tab->navigate(url);
    updateCurrentTabState();
}

void BrowserWindow::openTabForExistingId(int tab_id)
{
    auto* tab = new Tab(this, tab_id);
    auto index = m_tabs->addTab(tab, tab->title());
    m_tabs->setCurrentIndex(index);
    debug_log("open_tab_for_existing_id", tab_id, QStringLiteral("index=%1").arg(index));
    tab->setHamburgerButtonVisible(!menuBar()->isVisible());
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

    // [ladybird: BrowserWindow.cpp reopen last closed tab] push before removal, cap at 10
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

void BrowserWindow::toggleVerticalTabsExpanded() // [ladybird: Tab.cpp:176,213 — application.toggle_vertical_tabs_expanded_action()]
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

void BrowserWindow::openNextTab() // [ladybird: BrowserWindow.cpp:1065-1073]
{
    if (m_tabs->count() <= 1)
        return;
    auto next_index = m_tabs->currentIndex() + 1;
    if (next_index >= m_tabs->count())
        next_index = 0;
    m_tabs->setCurrentIndex(next_index);
}

void BrowserWindow::openPreviousTab() // [ladybird: BrowserWindow.cpp:1076-1084]
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
    // Status/link-hover text is shown by Tab's in-view m_hover_label [ladybird: Tab.cpp:748-763].
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

// [ladybird: BrowserWindow.cpp:1365-1376]
void BrowserWindow::wheelEvent(QWheelEvent* event)
{
    if (!currentTab())
        return;

    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        if (event->angleDelta().y() > 0)
            currentTab()->zoomIn();   // [ladybird: BrowserWindow.cpp:1372]
        else if (event->angleDelta().y() < 0)
            currentTab()->zoomOut();  // [ladybird: BrowserWindow.cpp:1374]
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
