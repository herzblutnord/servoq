#include "BrowserWindow.h"
#include "BookmarksBar.h"
#include "ChromeStyle.h"
#include "Icon.h"
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

namespace ServoQ {

BrowserWindow::BrowserWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_tabs(new TabWidget(this))
    , m_hamburger_menu(new QMenu(this))
{
    setWindowTitle("ServoQ");
    setWindowIcon(app_icon());
    setMinimumSize(900, 640);
    resize(1180, 780);

    createMenus();

    m_tabs->onCurrentChanged = [this](int) { updateCurrentTabState(); };
    m_tabs->onTabCloseRequested = [this](int index) { closeTab(index); };
    m_tabs->onNewTabRequested = [this] { createNewTab(); };
    m_tabs->setNewTabAction(m_new_tab_action);
    setCentralWidget(m_tabs);

    statusBar()->showMessage("Servo renderer placeholder is idle");
    updateChromeStyle();
    createInitialTab();
}

Tab* BrowserWindow::currentTab() const
{
    return m_tabs->currentTab();
}

void BrowserWindow::tabStateChanged(Tab* tab)
{
    auto index = m_tabs->indexOf(tab);
    if (index >= 0)
        m_tabs->setTabText(index, tab->title());
    if (tab == currentTab())
        updateCurrentTabState();
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
    auto* toggle_bookmarks_action = new QAction("Toggle &Bookmarks Bar", this);
    toggle_bookmarks_action->setCheckable(true);
    toggle_bookmarks_action->setChecked(true);
    connect(toggle_bookmarks_action, &QAction::triggered, this, [this](bool visible) {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto* tab = m_tabs->tab(i); tab && tab->bookmarksBar())
                tab->bookmarksBar()->setVisible(visible);
        }
    });
    view_menu->addAction(toggle_bookmarks_action);

    view_menu->addSeparator();
    auto* tab_layout_group = new QActionGroup(this);
    tab_layout_group->setExclusive(true);

    m_horizontal_tabs_action = new QAction("&Horizontal Tabs", this);
    m_horizontal_tabs_action->setCheckable(true);
    m_horizontal_tabs_action->setChecked(true);
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
    m_hamburger_menu->addMenu(view_menu);

    auto* help_menu = menuBar()->addMenu("&Help");
    auto* about_action = new QAction("About ServoQ", this);
    about_action->setEnabled(false);
    help_menu->addAction(about_action);
    m_hamburger_menu->addMenu(help_menu);
}

void BrowserWindow::createInitialTab()
{
    createNewTab(QStringLiteral("about:blank"));
    if (auto* tab = currentTab())
        tab->focusLocationEditor();
}

void BrowserWindow::createNewTab(QString const& url)
{
    servoq::new_tab();
    auto* tab = new Tab(this);
    auto index = m_tabs->addTab(tab, tab->title());
    m_tabs->setCurrentIndex(index);
    if (url != QStringLiteral("about:blank"))
        tab->navigate(url);
    updateCurrentTabState();
}

void BrowserWindow::closeTab(int index)
{
    if (index < 0 || index >= m_tabs->count())
        return;

    servoq::close_tab(index);
    if (m_tabs->count() == 1) {
        close();
        return;
    }
    m_tabs->removeTab(index);
    updateCurrentTabState();
}

void BrowserWindow::setHorizontalTabs()
{
    m_tabs->setVerticalTabsEnabled(false);
    m_tabs->setVerticalTabsExpandOnHover(false);
    if (m_horizontal_tabs_action)
        m_horizontal_tabs_action->setChecked(true);
    if (m_vertical_tabs_hover_expand_action)
        m_vertical_tabs_hover_expand_action->setEnabled(false);
}

void BrowserWindow::setVerticalTabsCollapsed()
{
    m_tabs->setVerticalTabsEnabled(true);
    m_tabs->setVerticalTabsExpanded(false);
    if (m_vertical_tabs_collapsed_action)
        m_vertical_tabs_collapsed_action->setChecked(true);
    if (m_vertical_tabs_hover_expand_action)
        m_vertical_tabs_hover_expand_action->setEnabled(true);
}

void BrowserWindow::setVerticalTabsExpanded()
{
    m_tabs->setVerticalTabsEnabled(true);
    m_tabs->setVerticalTabsExpanded(true);
    if (m_vertical_tabs_expanded_action)
        m_vertical_tabs_expanded_action->setChecked(true);
    if (m_vertical_tabs_hover_expand_action)
        m_vertical_tabs_hover_expand_action->setEnabled(false);
}

void BrowserWindow::setVerticalTabsExpandOnHover(bool enabled)
{
    m_tabs->setVerticalTabsExpandOnHover(enabled);
}

void BrowserWindow::updateCurrentTabState()
{
    auto* tab = currentTab();
    if (!tab)
        return;
    setWindowTitle(QStringLiteral("%1 - ServoQ").arg(tab->title()));
    statusBar()->showMessage(QString::fromStdString(std::string(servoq::status_text())));
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
