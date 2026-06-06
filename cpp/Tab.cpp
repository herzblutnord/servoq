#include "Tab.h"
#include "BookmarksBar.h"
#include "ChromeLayout.h"
#include "BrowserWindow.h"
#include "ChromeStyle.h"
#include "FindInPageWidget.h"
#include "Icon.h"
#include "LocationEdit.h"
#include "WebContentPlaceholder.h"
#include "servoq/src/bridge.rs.h"

#include <QAction>
#include <QBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>

namespace ServoQ {

namespace {
constexpr int ToolbarHorizontalMargin = 12;
constexpr int ToolbarVerticalMargin = 2;
}

Tab::Tab(BrowserWindow* window)
    : QWidget(window)
    , m_window(window)
    , m_toolbar_container(new QWidget(this))
    , m_toolbar(new QWidget(m_toolbar_container))
    , m_bookmarks_bar(new BookmarksBar(m_toolbar_container))
    , m_location_edit(new LocationEdit(m_toolbar))
    , m_view(new WebContentPlaceholder(this))
    , m_find_in_page(new FindInPageWidget(this))
    , m_hover_label(new QLabel(this))
{
    auto* tab_layout = new QVBoxLayout(this);
    tab_layout->setContentsMargins(0, 0, 0, 0);
    tab_layout->setSpacing(0);

    m_toolbar_container->setObjectName("LadybirdToolbarContainer");
    auto* toolbar_container_layout = new QVBoxLayout(m_toolbar_container);
    toolbar_container_layout->setContentsMargins(0, 0, 0, 0);
    toolbar_container_layout->setSpacing(0);

    m_toolbar->setObjectName("LadybirdNavigationToolbar");
    m_toolbar->setFixedHeight(browser_chrome_layout_policy().toolbar_height);
    toolbar_container_layout->addWidget(m_toolbar);
    toolbar_container_layout->addWidget(m_bookmarks_bar);

    buildToolbar();

    m_find_in_page->hide();
    tab_layout->addWidget(m_toolbar_container);
    tab_layout->addWidget(m_view, 1);
    tab_layout->addWidget(m_find_in_page);

    m_hover_label->setContentsMargins(5, 2, 5, 2);
    m_hover_label->hide();

    new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this, [this] { focusLocationEditor(); });
    new QShortcut(QKeySequence(QStringLiteral("Alt+D")), this, [this] { focusLocationEditor(); });
    new QShortcut(QKeySequence::Find, this, [this] { showFindInPage(); });

    navigate(QStringLiteral("about:blank"));
}

void Tab::setToolbarContainerInTabLayout(bool in_tab_layout)
{
    auto* tab_layout = qobject_cast<QBoxLayout*>(layout());
    if (!tab_layout)
        return;

    auto index = tab_layout->indexOf(m_toolbar_container);
    if (in_tab_layout) {
        if (index == -1) {
            m_toolbar_container->setParent(this);
            tab_layout->insertWidget(0, m_toolbar_container);
        }
        m_toolbar_container->show();
        return;
    }

    if (index != -1) {
        tab_layout->removeWidget(m_toolbar_container);
        m_toolbar_container->hide();
    }
}

void Tab::setVerticalTabsEnabled(bool enabled)
{
    m_toolbar->setProperty("verticalTabsEnabled", enabled);
    m_toolbar->layout()->invalidate();
}

void Tab::setHamburgerButtonVisible(bool visible)
{
    m_hamburger_button->setVisible(visible);
}

void Tab::navigate(QString const& url)
{
    m_url = url.trimmed().isEmpty() ? QStringLiteral("about:blank") : url.trimmed();
    servoq::load_url(m_url.toStdString());
    updateFromController();
}

void Tab::focusLocationEditor()
{
    m_location_edit->setFocus();
    m_location_edit->selectAll();
}

void Tab::showFindInPage()
{
    m_find_in_page->show();
    m_find_in_page->setFocus();
}

void Tab::findPrevious()
{
    showFindInPage();
}

void Tab::findNext()
{
    showFindInPage();
}

void Tab::setStatusText(QString const& status)
{
    m_view->setStatus(status);
}

QToolButton* Tab::createToolbarButton(QAction* action)
{
    auto* button = new QToolButton(m_toolbar);
    button->setDefaultAction(action);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(36, 36);
    return button;
}

void Tab::buildToolbar()
{
    m_toolbar_container->setStyleSheet(ChromeStyle::toolbar_style_sheet(palette()));

    auto* toolbar_layout = new QHBoxLayout(m_toolbar);
    toolbar_layout->setContentsMargins(ToolbarHorizontalMargin, ToolbarVerticalMargin, ToolbarHorizontalMargin, ToolbarVerticalMargin);
    toolbar_layout->setSpacing(0);

    auto* navigation_cluster = new QWidget(m_toolbar);
    auto* navigation_layout = new QHBoxLayout(navigation_cluster);
    navigation_layout->setContentsMargins(0, 0, 0, 0);
    navigation_layout->setSpacing(2);

    m_back_action = new QAction("‹", this);
    m_back_action->setToolTip("Back");
    m_forward_action = new QAction("›", this);
    m_forward_action->setToolTip("Forward");
    m_reload_action = new QAction("↻", this);
    m_reload_action->setToolTip("Reload");
    m_bookmark_action = new QAction("☆", this);
    m_bookmark_action->setToolTip("Bookmark this page");

    m_back_action->setIcon(create_chrome_icon(ChromeIcon::Back, palette()));
    m_forward_action->setIcon(create_chrome_icon(ChromeIcon::Forward, palette()));
    m_reload_action->setIcon(create_chrome_icon(ChromeIcon::Reload, palette()));
    m_bookmark_action->setIcon(create_chrome_icon(ChromeIcon::Star, palette()));

    connect(m_back_action, &QAction::triggered, this, [this] { servoq::go_back(); updateFromController(); });
    connect(m_forward_action, &QAction::triggered, this, [this] { servoq::go_forward(); updateFromController(); });
    connect(m_reload_action, &QAction::triggered, this, [this] { servoq::reload(); updateFromController(); });
    connect(m_bookmark_action, &QAction::triggered, this, [this] { servoq::toggle_bookmark(); updateFromController(); });

    navigation_layout->addWidget(createToolbarButton(m_back_action));
    navigation_layout->addWidget(createToolbarButton(m_forward_action));
    navigation_layout->addWidget(createToolbarButton(m_reload_action));

    m_location_edit->setTrailingAction(m_bookmark_action);
    connect(m_location_edit, &QLineEdit::returnPressed, this, [this] {
        auto url = m_location_edit->text().trimmed();
        navigate(url.isEmpty() ? QStringLiteral("about:blank") : url);
    });

    m_hamburger_button = new QToolButton(m_toolbar);
    m_hamburger_button->setText("Show &Menu");
    m_hamburger_button->setToolTip("Show Menu");
    m_hamburger_button->setIcon(create_chrome_icon(ChromeIcon::Menu, palette()));
    m_hamburger_button->setIconSize({ 20, 20 });
    m_hamburger_button->setFixedSize(36, 36);
    m_hamburger_button->setAutoRaise(true);
    m_hamburger_button->setFocusPolicy(Qt::NoFocus);
    m_hamburger_button->setPopupMode(QToolButton::InstantPopup);
    m_hamburger_button->setMenu(m_window->hamburgerMenu());

    auto* location_container = new QWidget(m_toolbar);
    auto* location_layout = new QHBoxLayout(location_container);
    location_layout->setContentsMargins(0, 0, 32, 0);
    location_layout->setSpacing(0);
    location_layout->addWidget(m_location_edit);

    toolbar_layout->addWidget(navigation_cluster, 0, Qt::AlignTop);
    toolbar_layout->addWidget(location_container, 1);
    toolbar_layout->addWidget(m_hamburger_button, 0, Qt::AlignTop);
}

void Tab::updateFromController()
{
    m_url = QString::fromStdString(std::string(servoq::current_url()));
    m_title = QString::fromStdString(std::string(servoq::title()));
    auto status = QString::fromStdString(std::string(servoq::status_text()));
    m_location_edit->setUrl(m_url);
    m_view->setUrl(m_url);
    m_view->setStatus(status);
    m_bookmarks_bar->setVisible(servoq::bookmarks_bar_visible());
    m_find_in_page->setVisible(servoq::find_bar_visible());
    m_back_action->setEnabled(servoq::can_go_back());
    m_forward_action->setEnabled(servoq::can_go_forward());
    if (m_window)
        m_window->tabStateChanged(this);
}

}
