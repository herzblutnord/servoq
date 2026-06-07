#include "Tab.h"
#include "BookmarksBar.h"
#include "BookmarkStore.h"
#include "ChromeLayout.h"
#include "BrowserWindow.h"
#include "ChromeStyle.h"
#include "FindInPageWidget.h"
#include "Icon.h"
#include "LocationEdit.h"
#include "Settings.h"
#include "WebContentView.h"
#include "WebViewURL.h"
#include "servoq/src/bridge.rs.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QShortcut>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace ServoQ {

namespace {
constexpr int ToolbarHorizontalMargin = 12;                  // [ladybird: Tab.cpp:91]
constexpr int ToolbarVerticalMargin = 2;                     // [ladybird: Tab.cpp:92]
constexpr int ToolbarSidebarToggleNavigationGap = 8;         // [ladybird: Tab.cpp:94]

bool debug_enabled()
{
    return qEnvironmentVariableIsSet("SERVOQ_DEBUG");
}

void debug_log(char const* event, int tab_id, QString const& detail = {})
{
    if (!debug_enabled())
        return;
    if (detail.isEmpty())
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " tab_id=" << tab_id;
    else
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " tab_id=" << tab_id << " " << detail;
}
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

    // Wire BookmarksBar URL-open callbacks — [ladybird: BookmarksBar.cpp:223-225]
    m_bookmarks_bar->setOpenUrlCallback([this](QString const& url) { navigate(url); });
    m_bookmarks_bar->setOpenUrlInNewTabCallback([this, window](QString const& url) {
        if (window) window->createNewTab(url);
    });

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
    if (m_sidebar_toggle_spacer) // [ladybird: Tab.cpp:584-585]
        m_sidebar_toggle_spacer->changeSize(enabled ? ToolbarSidebarToggleNavigationGap : 0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    updateToggleVerticalTabsIcon();
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
    auto is_active = m_window && m_window->currentTab() == this;
    debug_log("final_url_to_servo", m_controller_id, QStringLiteral("url=%1 active=%2").arg(normalized_url).arg(is_active ? 1 : 0));
    m_view->setInitialUrl(normalized_url);
    on_load_start(normalized_url);
    servoq::load_url(m_controller_id, normalized_url.toStdString());
    applyControllerState();
    on_load_finish();
}

void Tab::location_edit_return_pressed()
{
    auto text = m_location_edit->text();
    if (text.isEmpty())
        return;

    auto ctrl_held = QApplication::keyboardModifiers() & Qt::ControlModifier;
    auto append_tld = ctrl_held ? WebViewURL::AppendTLD::Yes : WebViewURL::AppendTLD::No;
    auto url = WebViewURL::sanitize_url(text, append_tld);
    m_location_edit->setUrl(url);
    if (url.has_value()) {
        navigate(*url);
    } else {
        setStatusText(QStringLiteral("Navigation error: %1").arg(text.trimmed()));
    }
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
    if (m_window)
        m_window->tabStateChanged(this);
}

// [ladybird: BrowserWindow.cpp:1372] — zoom_in step = 0.1, clamped to [0.1, 10.0]
// Servo 0.2.0 also clamps internally, so our outer clamp matches the stricter bound.
static constexpr float ZoomStep = 0.1f;
static constexpr float ZoomMin  = 0.1f;
static constexpr float ZoomMax  = 10.0f;

static float round_zoom(float v) {
    return std::round(v * 10.0f) / 10.0f;
}

void Tab::zoomIn() // [ladybird: BrowserWindow.cpp:1372 — view().zoom_in()]
{
    auto current = servoq::page_zoom(m_controller_id);
    auto next = std::min(round_zoom(current + ZoomStep), ZoomMax);
    servoq::set_page_zoom(m_controller_id, next);
    updateZoomAction();
}

void Tab::zoomOut() // [ladybird: BrowserWindow.cpp:1374 — view().zoom_out()]
{
    auto current = servoq::page_zoom(m_controller_id);
    auto next = std::max(round_zoom(current - ZoomStep), ZoomMin);
    servoq::set_page_zoom(m_controller_id, next);
    updateZoomAction();
}

void Tab::resetZoom() // [ladybird: Tab.cpp:233 — view().reset_zoom_action()]
{
    servoq::set_page_zoom(m_controller_id, 1.0f);
    updateZoomAction();
}

void Tab::updateZoomAction()
{
    if (!m_reset_zoom_action)
        return;
    auto zoom = servoq::page_zoom(m_controller_id);
    // Visible only when zoom != 1.0 — [ladybird: LocationEdit.cpp:684]
    bool at_default = (std::abs(zoom - 1.0f) < 0.005f);
    m_reset_zoom_action->setVisible(!at_default);
    m_reset_zoom_action->setText(at_default ? QString() : QStringLiteral("%1%").arg(qRound(zoom * 100)));
    m_reset_zoom_action->setToolTip(at_default ? QString() : QStringLiteral("Reset zoom to 100%"));
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
        setStatusText({});
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

    // Toggle vertical tabs expanded/collapsed — always present in toolbar [ladybird: Tab.cpp:176,213-215]
    m_toggle_vertical_tabs_action = new QAction(this);
    m_toggle_vertical_tabs_action->setToolTip(QStringLiteral("Toggle Sidebar"));
    updateToggleVerticalTabsIcon();
    connect(m_toggle_vertical_tabs_action, &QAction::triggered, this, [this] {
        if (m_window)
            m_window->toggleVerticalTabsExpanded();
    });
    navigation_layout->addWidget(createToolbarButton(m_toggle_vertical_tabs_action)); // [ladybird: Tab.cpp:213]
    m_sidebar_toggle_spacer = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum); // [ladybird: Tab.cpp:214]
    navigation_layout->addItem(m_sidebar_toggle_spacer);                                        // [ladybird: Tab.cpp:215]

    m_back_action = new QAction("‹", this);
    m_back_action->setToolTip("Back");
    m_back_action->setShortcut(QKeySequence::Back);   // [ladybird: Menu.cpp:172]
    m_forward_action = new QAction("›", this);
    m_forward_action->setToolTip("Forward");
    m_forward_action->setShortcut(QKeySequence::Forward); // [ladybird: Menu.cpp:177]
    m_reload_action = new QAction("↻", this);
    m_reload_action->setToolTip("Reload");
    m_reload_action->setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_R), QKeySequence(Qt::Key_F5) }); // [ladybird: Menu.cpp:182]
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
        BookmarkStore::the()->toggleBookmark(m_title, m_url); // [ladybird: Tab.cpp:BookmarkStore]
        refreshBookmarkIcon();
        applyControllerState();
    });

    navigation_layout->addWidget(createToolbarButton(m_back_action));    // [ladybird: Tab.cpp:216]
    navigation_layout->addWidget(createToolbarButton(m_forward_action)); // [ladybird: Tab.cpp:217]
    navigation_layout->addWidget(createToolbarButton(m_reload_action));  // [ladybird: Tab.cpp:218]

    m_reset_zoom_action = new QAction(this); // [ladybird: Tab.cpp:233 — view().reset_zoom_action()]
    m_reset_zoom_action->setToolTip("Reset zoom");
    m_reset_zoom_action->setVisible(false);
    connect(m_reset_zoom_action, &QAction::triggered, this, &Tab::resetZoom);

    m_location_edit->setTrailingAction(m_bookmark_action);
    m_location_edit->setZoomAction(m_reset_zoom_action); // [ladybird: Tab.cpp:233]
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

void Tab::updateToggleVerticalTabsIcon() // [ladybird: Tab.cpp:794-797]
{
    if (!m_toggle_vertical_tabs_action)
        return;
    bool expanded = Settings::the()->vertical_tabs_expanded();
    m_toggle_vertical_tabs_action->setIcon(create_chrome_icon(
        expanded ? ChromeIcon::VerticalTabBarCollapse : ChromeIcon::VerticalTabBarExpand,
        palette()));
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
    m_bookmark_action->setIcon(create_chrome_icon(
        BookmarkStore::the()->hasBookmark(m_url) ? ChromeIcon::StarFilled : ChromeIcon::Star,
        palette()));
}

}

