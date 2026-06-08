#pragma once

#include <QMainWindow>
#include <QPair>
#include <QString>
#include <QVector>

class QAction;
class QCloseEvent;
class QMenu;
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
    void toggleVerticalTabsExpanded(); // [ladybird: Tab.cpp:176,213]
    void openNextTab();                // [ladybird: BrowserWindow.cpp:1065]
    void openPreviousTab();            // [ladybird: BrowserWindow.cpp:1076]
    void createNewTab(QString const& url = {});
    void openTabForExistingId(int tab_id);
    void setFullscreen(bool fullscreen);
    void closeTabFromContextMenu(int index);
    void closeOtherTabs(int keep_index);
    void closeTabsToRight(int from_index);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override; // [ladybird: BrowserWindow.cpp:1365]

private:
    void createMenus();
    void createInitialTab();
    void closeTab(int index);
    void setHorizontalTabs();
    void setVerticalTabsCollapsed();
    void setVerticalTabsExpanded();
    void setVerticalTabsExpandOnHover(bool enabled);
    void setShowMenuBar(bool visible);
    void updateMenuBarVisibility();
    void applySettings();
    void refreshBookmarksBars();
    void updateCurrentTabState();
    void updateChromeStyle();

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
    QAction* m_reopen_tab_action { nullptr }; // [ladybird: BrowserWindow.cpp reopen last closed tab]
    QVector<QPair<QString, QString>> m_closed_tabs; // url, title stack; capped at 10
    bool m_is_updating_chrome_style { false };
    bool m_was_maximized_before_fullscreen { false };
};

}
