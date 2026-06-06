#pragma once

#include <QMainWindow>

class QAction;
class QMenu;

namespace ServoQ {

class Tab;
class TabWidget;

class BrowserWindow final : public QMainWindow {
public:
    explicit BrowserWindow(QWidget* parent = nullptr);

    QMenu* hamburgerMenu() const { return m_hamburger_menu; }
    Tab* currentTab() const;
    void tabStateChanged(Tab* tab);

protected:
    bool event(QEvent* event) override;

private:
    void createMenus();
    void createInitialTab();
    void createNewTab(QString const& url = QStringLiteral("about:blank"));
    void closeTab(int index);
    void setHorizontalTabs();
    void setVerticalTabsCollapsed();
    void setVerticalTabsExpanded();
    void setVerticalTabsExpandOnHover(bool enabled);
    void updateCurrentTabState();
    void updateChromeStyle();

    TabWidget* m_tabs { nullptr };
    QMenu* m_hamburger_menu { nullptr };
    QAction* m_new_tab_action { nullptr };
    QAction* m_close_tab_action { nullptr };
    QAction* m_find_action { nullptr };
    QAction* m_horizontal_tabs_action { nullptr };
    QAction* m_vertical_tabs_collapsed_action { nullptr };
    QAction* m_vertical_tabs_expanded_action { nullptr };
    QAction* m_vertical_tabs_hover_expand_action { nullptr };
    bool m_is_updating_chrome_style { false };
};

}
