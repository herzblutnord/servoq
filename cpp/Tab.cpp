/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Simon Farre <simon.farre.cx@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/Tab.cpp
 *   UI/Qt/BrowserWindow.cpp
 */
#include "Tab.h"
#include "BookmarksBar.h"
#include "BookmarkStore.h"
#include "HistoryStore.h"
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
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace ServoQ {

namespace {
constexpr int ToolbarHorizontalMargin = 12;
constexpr int ToolbarVerticalMargin = 2;
constexpr int ToolbarSidebarToggleNavigationGap = 8;

bool debug_enabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_DEBUG");
    return v;
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

    // Wire BookmarksBar URL-open callbacks
    m_bookmarks_bar->setOpenUrlCallback([this](QString const& url) { navigate(url); });
    m_bookmarks_bar->setOpenUrlInNewTabCallback([window](QString const& url) {
        if (window) window->createNewTab(url);
    });

    buildToolbar();

    m_find_in_page->hide();
    tab_layout->addWidget(m_toolbar_container);
    tab_layout->addWidget(m_view, 1);
    tab_layout->addWidget(m_find_in_page);

    m_hover_label->setContentsMargins(4, 2, 4, 2);
    m_hover_label->setAutoFillBackground(true);
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
    if (m_sidebar_toggle_spacer)
        m_sidebar_toggle_spacer->changeSize(enabled ? ToolbarSidebarToggleNavigationGap : 0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    updateToggleVerticalTabsIcon();
    m_toolbar->layout()->invalidate();
}

void Tab::setHamburgerButtonVisible(bool visible)
{
    m_hamburger_button->setVisible(visible);
}

void Tab::setActive(bool active)
{
    servoq::set_webview_active(m_controller_id, active);
}

void Tab::showEmptyNewTab()
{
    m_is_empty_new_tab = true;
    m_url = QStringLiteral("about:blank");
    m_title = QStringLiteral("New Tab");
    m_favicon = {};
    m_view->setInitialUrl(m_url);
    m_view->setEmptyNewTab(true);
    m_location_edit->setUrl(m_url);
    set_loading(false);
    refreshBookmarkIcon();
    update_tab_title();
}

void Tab::attachExistingWebView(QString const& initial_url)
{
    auto normalized_url = initial_url.trimmed().isEmpty() ? QStringLiteral("about:blank") : initial_url.trimmed();
    m_is_empty_new_tab = false;
    m_url = normalized_url;
    m_view->setEmptyNewTab(false);
    m_view->setInitialUrl(normalized_url);
    m_location_edit->setUrl(m_url);
    debug_log("attach_existing_webview", m_controller_id,
        QStringLiteral("empty=false initial_url=%1").arg(normalized_url));
    m_view->ensureEngineStarted();
    applyControllerState();
}

void Tab::navigate(QString const& url)
{
    auto normalized_url = url.trimmed().isEmpty() ? QStringLiteral("about:blank") : url.trimmed();
    m_is_empty_new_tab = false;
    // Queue URL so create_webview (called from showEvent) uses it if the engine
    // WebView has not been created yet.
    auto is_active = m_window && m_window->currentTab() == this;
    debug_log("final_url_to_servo", m_controller_id, QStringLiteral("url=%1 active=%2").arg(normalized_url).arg(is_active ? 1 : 0));
    m_view->setEmptyNewTab(false);
    m_view->setInitialUrl(normalized_url);
    on_load_start(normalized_url);
    bool created_now = m_view->ensureEngineStarted();
    if (!created_now)
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

// Servo 0.2.0 also clamps internally, so our outer clamp matches the stricter bound.
static constexpr float ZoomStep = 0.1f;
static constexpr float ZoomMin  = 0.1f;
static constexpr float ZoomMax  = 10.0f;

static float round_zoom(float v) {
    return std::round(v * 10.0f) / 10.0f;
}

void Tab::zoomIn()
{
    auto current = servoq::page_zoom(m_controller_id);
    auto next = std::min(round_zoom(current + ZoomStep), ZoomMax);
    servoq::set_page_zoom(m_controller_id, next);
    updateZoomAction();
}

void Tab::zoomOut()
{
    auto current = servoq::page_zoom(m_controller_id);
    auto next = std::max(round_zoom(current - ZoomStep), ZoomMin);
    servoq::set_page_zoom(m_controller_id, next);
    updateZoomAction();
}

void Tab::resetZoom()
{
    servoq::set_page_zoom(m_controller_id, 1.0f);
    updateZoomAction();
}

void Tab::updateZoomAction()
{
    if (!m_reset_zoom_action)
        return;
    auto zoom = servoq::page_zoom(m_controller_id);
    // Visible only when zoom != 1.0
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
    if (m_is_empty_new_tab) {
        m_location_edit->setUrl(m_url);
        m_view->setUrl(m_url);
        set_loading(false);
        m_back_action->setEnabled(false);
        m_forward_action->setEnabled(false);
        m_find_in_page->setVisible(false);
        m_bookmarks_bar->setVisible(Settings::the()->show_bookmarks_bar());
        refreshBookmarkIcon();
        return;
    }

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
    HistoryStore::the()->recordVisit(m_url, m_title);
}

void Tab::on_title_change(QString const& title)
{
    m_title = title.isEmpty() ? QStringLiteral("New Tab") : title;
    update_tab_title();
    // Update title of most-recent history entry for current URL
    HistoryStore::the()->recordVisit(m_url, m_title);
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

void Tab::on_history_changed(QStringList const& urls, int current)
{
    m_history_urls = urls;
    m_history_current = current;
}

void Tab::on_link_hover(QString const& url)
{
    if (url.isEmpty()) {
        m_hovered_link_url.clear();
        setStatusText({});
        m_hover_label->hide();
        return;
    }
    auto parsed = QUrl(url);
    m_hovered_link_url = (parsed.isValid() && !parsed.scheme().isEmpty()) ? url : QString();
    m_hover_label->setText(url);
    setStatusText(url);
    updateHoverLabel();
    m_hover_label->show();
}

bool Tab::openHoveredLinkInNewTab()
{
    if (m_hovered_link_url.isEmpty()) {
        debug_log("middle_click_link", m_controller_id, QStringLiteral("ignored reason=no_hovered_link"));
        return false;
    }
    debug_log("middle_click_link", m_controller_id,
        QStringLiteral("hovered_url=%1 action=open_new_tab").arg(m_hovered_link_url));
    if (m_window)
        m_window->createNewTab(m_hovered_link_url, /*background=*/true);
    return m_window != nullptr;
}

// tab, eliding text to half the tab width. If the mouse is over the label, shift it
// to the right side so it doesn't obscure the hovered link.
void Tab::updateHoverLabel()
{
    auto elided = QFontMetrics(m_hover_label->font()).elidedText(
        m_hover_label->text(), Qt::ElideRight, width() / 2 - 10);
    m_hover_label->setText(elided);
    m_hover_label->resize(m_hover_label->sizeHint());

    int label_y = height() - m_hover_label->height();
    if (m_find_in_page->isVisible())
        label_y -= m_find_in_page->height();

    int label_x = 0;
    if (m_hover_label->underMouse() && m_hover_label->x() == 0)
        label_x = width() / 2 + (width() / 2 - m_hover_label->width());

    m_hover_label->move(label_x, label_y);
    m_hover_label->raise();
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

    // Toggle vertical tabs expanded/collapsed — always present in toolbar
    m_toggle_vertical_tabs_action = new QAction(this);
    m_toggle_vertical_tabs_action->setToolTip(QStringLiteral("Toggle Sidebar"));
    updateToggleVerticalTabsIcon();
    connect(m_toggle_vertical_tabs_action, &QAction::triggered, this, [this] {
        if (m_window)
            m_window->toggleVerticalTabsExpanded();
    });
    navigation_layout->addWidget(createToolbarButton(m_toggle_vertical_tabs_action));
    m_sidebar_toggle_spacer = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    navigation_layout->addItem(m_sidebar_toggle_spacer);

    m_back_action = new QAction("‹", this);
    m_back_action->setToolTip("Back");
    m_back_action->setShortcut(QKeySequence::Back);
    m_forward_action = new QAction("›", this);
    m_forward_action->setToolTip("Forward");
    m_forward_action->setShortcut(QKeySequence::Forward);
    m_reload_action = new QAction("↻", this);
    m_reload_action->setToolTip("Reload");
    m_reload_action->setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_R), QKeySequence(Qt::Key_F5) });
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
        BookmarkStore::the()->toggleBookmark(m_title, m_url);
        refreshBookmarkIcon();
        applyControllerState();
    });

    auto* back_btn = createToolbarButton(m_back_action);
    back_btn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(back_btn, &QToolButton::customContextMenuRequested, this, [this, back_btn](QPoint const& pos) {
        if (m_history_urls.isEmpty())
            return;
        auto* menu = new QMenu;
        menu->setAttribute(Qt::WA_DeleteOnClose);
        int start = m_history_current - 1;
        for (int i = start; i >= 0; --i) {
            auto* act = menu->addAction(m_history_urls[i]);
            int steps = m_history_current - i;
            connect(act, &QAction::triggered, this, [this, steps] {
                for (int s = 0; s < steps; ++s)
                    servoq::go_back(m_controller_id);
                applyControllerState();
            });
        }
        if (!menu->isEmpty())
            menu->popup(back_btn->mapToGlobal(pos));
        else
            delete menu;
    });

    auto* forward_btn = createToolbarButton(m_forward_action);
    forward_btn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(forward_btn, &QToolButton::customContextMenuRequested, this, [this, forward_btn](QPoint const& pos) {
        if (m_history_urls.isEmpty())
            return;
        auto* menu = new QMenu;
        menu->setAttribute(Qt::WA_DeleteOnClose);
        int start = m_history_current + 1;
        for (int i = start; i < m_history_urls.size(); ++i) {
            auto* act = menu->addAction(m_history_urls[i]);
            int steps = i - m_history_current;
            connect(act, &QAction::triggered, this, [this, steps] {
                for (int s = 0; s < steps; ++s)
                    servoq::go_forward(m_controller_id);
                applyControllerState();
            });
        }
        if (!menu->isEmpty())
            menu->popup(forward_btn->mapToGlobal(pos));
        else
            delete menu;
    });

    navigation_layout->addWidget(back_btn);
    navigation_layout->addWidget(forward_btn);
    navigation_layout->addWidget(createToolbarButton(m_reload_action));

    m_reset_zoom_action = new QAction(this);
    m_reset_zoom_action->setToolTip("Reset zoom");
    m_reset_zoom_action->setVisible(false);
    connect(m_reset_zoom_action, &QAction::triggered, this, &Tab::resetZoom);

    m_location_edit->setTrailingAction(m_bookmark_action);
    m_location_edit->setZoomAction(m_reset_zoom_action);
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
    location_layout->setContentsMargins(32, 0, 32, 0);
    location_layout->setSpacing(0);
    location_layout->addWidget(m_location_edit);

    toolbar_layout->addWidget(navigation_cluster, 0, Qt::AlignTop);
    toolbar_layout->addWidget(location_container, 1);
    toolbar_layout->addWidget(m_hamburger_button, 0, Qt::AlignTop);
}

void Tab::updateToggleVerticalTabsIcon()
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
    auto is_internal_page = m_url == QStringLiteral("about:blank");
    m_bookmark_action->setEnabled(!is_internal_page);
    m_bookmark_action->setIcon(create_chrome_icon(
        !is_internal_page && BookmarkStore::the()->hasBookmark(m_url) ? ChromeIcon::StarFilled : ChromeIcon::Star,
        palette()));
}

}
