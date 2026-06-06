#include "Tab.h"
#include "BookmarksBar.h"
#include "ChromeLayout.h"
#include "BrowserWindow.h"
#include "ChromeStyle.h"
#include "FindInPageWidget.h"
#include "Icon.h"
#include "LocationEdit.h"
#include "Settings.h"
#include "WebContentView.h"
#include "servoq/src/bridge.rs.h"

#include <QAction>
#include <QBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QShortcut>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace ServoQ {

namespace {
constexpr int ToolbarHorizontalMargin = 12;
constexpr int ToolbarVerticalMargin = 2;
}

Tab::Tab(BrowserWindow* window, int controller_id)
    : QWidget(window)
    , m_window(window)
    , m_controller_id(controller_id)
    , m_toolbar_container(new QWidget(this))
    , m_toolbar(new QWidget(m_toolbar_container))
    , m_bookmarks_bar(new BookmarksBar(m_toolbar_container))
    , m_location_edit(new LocationEdit(m_toolbar))
    , m_view(new WebContentView(this))
    , m_find_in_page(new FindInPageWidget(this))
    , m_hover_label(new QLabel(this))
    , m_loading_animation_timer(new QTimer(this))
{
    // Wire WebContentView to this Tab so delegate callbacks can reach on_* handlers.
    m_view->setTab(this);
    m_view->setTabId(m_controller_id);

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
    m_loading_animation_timer->setInterval(80);
    connect(m_loading_animation_timer, &QTimer::timeout, this, [this] {
        m_loading_animation_frame = (m_loading_animation_frame + 1) % 12;
        if (m_window)
            m_window->tabStateChanged(this);
    });
    m_find_in_page->onShown = [this] {
        servoq::show_find_in_page(m_controller_id);
    };
    m_find_in_page->onHidden = [this] {
        servoq::hide_find_in_page(m_controller_id);
    };

    new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this, [this] { focusLocationEditor(); });
    new QShortcut(QKeySequence(QStringLiteral("Alt+D")), this, [this] { focusLocationEditor(); });
    new QShortcut(QKeySequence::Find, this, [this] { showFindInPage(); });

    applyControllerState();
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
    auto normalized_url = url.trimmed().isEmpty() ? QStringLiteral("about:blank") : url.trimmed();
    // Queue URL so create_webview (called from showEvent) uses it if the engine
    // WebView has not been created yet.
    m_view->setInitialUrl(normalized_url);
    on_load_start(normalized_url);
    servoq::load_url(m_controller_id, normalized_url.toStdString());
    applyControllerState();
    on_load_finish();
}

void Tab::location_edit_return_pressed()
{
    auto text = m_location_edit->text().trimmed();
    if (text.isEmpty())
        return;
    navigate(text);
    m_view->setFocus();
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

QIcon Tab::tabIcon() const
{
    if (m_is_loading)
        return loading_spinner_icon(palette(), m_loading_animation_frame);
    return m_favicon.isNull() ? create_chrome_icon(ChromeIcon::Globe, palette()) : m_favicon;
}

void Tab::applyControllerState()
{
    auto url = QString::fromStdString(std::string(servoq::current_url(m_controller_id)));
    auto title = QString::fromStdString(std::string(servoq::title(m_controller_id)));
    on_url_change(url);
    on_title_change(title);
    set_loading(servoq::loading(m_controller_id));
    auto status = QString::fromStdString(std::string(servoq::status_text(m_controller_id)));
    setStatusText(status);
    m_back_action->setEnabled(servoq::can_go_back(m_controller_id));
    m_forward_action->setEnabled(servoq::can_go_forward(m_controller_id));
    m_find_in_page->setVisible(servoq::find_bar_visible(m_controller_id));
    m_bookmarks_bar->setVisible(Settings::the()->show_bookmarks_bar());
    refreshBookmarkIcon();
}

void Tab::on_url_change(QString const& url)
{
    m_url = url.isEmpty() ? QStringLiteral("about:blank") : url;
    m_location_edit->setUrl(m_url);
    m_view->setUrl(m_url); // keeps placeholder label in sync
}

void Tab::on_title_change(QString const& title)
{
    m_title = title.isEmpty() ? QStringLiteral("New Tab") : title;
    update_tab_title();
}

void Tab::on_load_start(QString const& url)
{
    m_title = url;
    update_tab_title();
    on_favicon_change({});
    set_loading(true);
    m_location_edit->setUrl(url);
}

void Tab::on_load_finish()
{
    set_loading(false);
}

void Tab::on_favicon_change(QIcon const& icon)
{
    m_favicon = icon.isNull() ? create_chrome_icon(ChromeIcon::Globe, palette()) : icon;
    if (m_window)
        m_window->tabStateChanged(this);
}

void Tab::on_link_hover(QString const& url)
{
    if (url.isEmpty()) {
        m_hover_label->hide();
        return;
    }
    m_hover_label->setText(url);
    setStatusText(url);
    m_hover_label->show();
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

    connect(m_back_action, &QAction::triggered, this, [this] { servoq::go_back(m_controller_id); applyControllerState(); });
    connect(m_forward_action, &QAction::triggered, this, [this] { servoq::go_forward(m_controller_id); applyControllerState(); });
    connect(m_reload_action, &QAction::triggered, this, [this] {
        on_load_start(m_url);
        servoq::reload(m_controller_id);
        applyControllerState();
        on_load_finish();
    });
    connect(m_bookmark_action, &QAction::triggered, this, [this] {
        servoq::toggle_bookmark(m_controller_id);
        Settings::the()->toggle_bookmark(m_title, m_url);
        refreshBookmarksBar();
        refreshBookmarkIcon();
        applyControllerState();
    });

    navigation_layout->addWidget(createToolbarButton(m_back_action));
    navigation_layout->addWidget(createToolbarButton(m_forward_action));
    navigation_layout->addWidget(createToolbarButton(m_reload_action));

    m_location_edit->setTrailingAction(m_bookmark_action);
    connect(m_location_edit, &QLineEdit::returnPressed, this, &Tab::location_edit_return_pressed);

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

void Tab::update_tab_title()
{
    if (m_window)
        m_window->tabStateChanged(this);
}

void Tab::set_loading(bool is_loading)
{
    if (m_is_loading == is_loading)
        return;
    m_is_loading = is_loading;
    m_loading_animation_frame = 0;
    if (m_is_loading)
        m_loading_animation_timer->start();
    else
        m_loading_animation_timer->stop();
    m_reload_action->setIcon(create_chrome_icon(m_is_loading ? ChromeIcon::Stop : ChromeIcon::Reload, palette()));
    if (m_window)
        m_window->tabStateChanged(this);
}

void Tab::refreshBookmarkIcon()
{
    m_bookmark_action->setIcon(create_chrome_icon(Settings::the()->has_bookmark(m_url) ? ChromeIcon::StarFilled : ChromeIcon::Star, palette()));
}

void Tab::refreshBookmarksBar()
{
    for (auto* toolbar : window()->findChildren<QToolBar*>()) {
        if (auto* bar = dynamic_cast<BookmarksBar*>(toolbar))
            bar->rebuild();
    }
}

}
