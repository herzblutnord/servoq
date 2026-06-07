#include "BrowserWindow.h"
#include "BookmarksBar.h"
#include "ChromeLayout.h"
#include "ChromeStyle.h"
#include "Icon.h"
#include "Settings.h"
#include "Tab.h"
#include "TabBar.h"
#include "servoq/src/bridge.rs.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QCloseEvent>
#include <QDebug>

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
        if (auto* tab = currentTab()) {
            debug_log("tab_switch", tab->controllerId(), QStringLiteral("active=1"));
            tab->applyControllerState();
        }
        updateCurrentTabState();
    };
    m_tabs->onTabCloseRequested = [this](int index) { closeTab(index); };
    m_tabs->onNewTabRequested = [this] { createNewTab(); };
    m_tabs->setNewTabAction(m_new_tab_action);
    setCentralWidget(m_tabs);

    statusBar()->showMessage("Servo renderer placeholder is idle");
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
    m_hamburger_menu->addMenu(edit_menu);

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

    auto* help_menu = menuBar()->addMenu("&Help");
    auto* about_action = new QAction("About ServoQ", this);
    about_action->setEnabled(false);
    help_menu->addAction(about_action);
    m_hamburger_menu->addMenu(help_menu);
}

void BrowserWindow::closeEvent(QCloseEvent* event)
{
    Settings::the()->set_last_position(pos());
    Settings::the()->set_last_size(size());
    Settings::the()->set_is_maximized(isMaximized());
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
    statusBar()->showMessage(QString::fromStdString(std::string(servoq::status_text(tab->controllerId()))));
}

void BrowserWindow::updateChromeStyle()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    qApp->setStyleSheet(ChromeStyle::application_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

}
