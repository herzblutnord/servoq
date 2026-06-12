/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Simon Farre <simon.farre.cx@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/BrowserWindow.cpp
 */
#pragma once

#include <QMainWindow>
#include <QIcon>
#include <QString>
#include <QVector>

class QAction;
class QCloseEvent;
class QMenu;
class QPaintEvent;
class QWheelEvent;

namespace ServoQ {

class Tab;
class TabWidget;

class BrowserWindow final : public QMainWindow {
public:
    explicit BrowserWindow(QWidget* parent = nullptr);

    QMenu* hamburgerMenu() const { return m_hamburger_menu; }
    Tab* currentTab() const;
    void tabStateChanged(Tab* tab);
    bool showMenuBar() const;
    void toggleVerticalTabsExpanded();
    void openNextTab();
    void openPreviousTab();
    void createNewTab(QString const& url = {}, bool background = false, bool use_configured_new_tab = true);
    void openTabForExistingId(int tab_id);
    void setFullscreen(bool fullscreen);
    void closeTabFromContextMenu(int index);
    void closeOtherTabs(int keep_index);
    void closeTabsToRight(int from_index);
    // window.close() from web content (servoq::notify_webview_close_requested).
    void closeTabForController(int controller_id);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct ClosedTabEntry {
        QString url;
        QString title;
        QIcon icon;
        bool was_empty_new_tab { false };
    };

    void createMenus();
    void createInitialTab();
    void clearLocationEditFocusForMousePress(QObject* target);
    void applyBrowserChromeCursors(QWidget* root);
    void closeTab(int index);
    void setHorizontalTabs();
    void setVerticalTabsCollapsed();
    void setVerticalTabsExpanded();
    void setVerticalTabsExpandOnHover(bool enabled);
    void setShowMenuBar(bool visible);
    void updateMenuBarVisibility();
    void applySettings();
    void refreshBookmarksBars();
    void refreshHomeButtons();
    void updateCurrentTabState();
    void updateChromeStyle();
    void rememberClosedTab(Tab const& tab);
    void reopenClosedTabAt(int index);
    void reopenAllClosedTabs();
    void updateRecentlyClosedActions();
    void populateRecentlyClosedTabsMenu(QMenu* menu);
    void showHomeAndNewTabSettingsDialog();
    bool shouldDrawWindowBorder() const;
    void updateWindowBorder();

    TabWidget* m_tabs { nullptr };
    Tab* m_active_tab { nullptr };
    QMenu* m_hamburger_menu { nullptr };
    QAction* m_new_tab_action { nullptr };
    QAction* m_close_tab_action { nullptr };
    QAction* m_find_action { nullptr };
    QAction* m_horizontal_tabs_action { nullptr };
    QAction* m_vertical_tabs_collapsed_action { nullptr };
    QAction* m_vertical_tabs_expanded_action { nullptr };
    QAction* m_vertical_tabs_hover_expand_action { nullptr };
    QAction* m_toggle_bookmarks_action { nullptr };
    QAction* m_show_menu_bar_action { nullptr };
    QAction* m_reopen_tab_action { nullptr };
    QAction* m_fullscreen_action { nullptr };
    QVector<ClosedTabEntry> m_closed_tabs; // oldest to newest; capped at 10
    bool m_is_updating_chrome_style { false };
    bool m_was_maximized_before_fullscreen { false };
    int m_activation_serial { 0 };
};

}
