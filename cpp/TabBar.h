/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/TabBar.h
 */
#pragma once

#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <functional>

#include "Icon.h"
#include "ChromeLayout.h"

class QAction;
class QBoxLayout;
class QContextMenuEvent;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QToolButton;
class QVariantAnimation;

namespace ServoQ {

class Tab;
class TabWidget;

enum class TabLayout : std::uint8_t {
    Horizontal,
    VerticalCollapsed,
    VerticalExpanded,
};

class TabBar final : public QTabBar {
public:
    explicit TabBar(TabWidget* parent);

    void setAvailableWidth(int width);
    void set_available_width(int width) { setAvailableWidth(width); }

    TabLayout tabLayout() const { return m_tab_layout; }
    TabLayout tab_layout() const { return m_tab_layout; }
    void setTabLayout(TabLayout layout);
    void set_tab_layout(TabLayout layout) { setTabLayout(layout); }
    void refreshTabLayout();
    void refresh_tab_layout() { refreshTabLayout(); }

protected:
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    QSize tabSizeHint(int index) const override;
    void resizeEvent(QResizeEvent* event) override;
    void tabLayoutChange() override;
    void paintEvent(QPaintEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    QRect visualTabRect(int index) const;
    int tabIndexAt(QPoint const& position) const;
    int insertionIndexAt(QPoint const& position) const;
    int dropIndicatorIndexForInsertionIndex(int insertion_index) const;
    QPixmap renderTabDragPixmap(int index) const;
    void startTabDrag(int index);
    void startHoverAnimation(int tab_index, qreal target_progress);
    void setHoveredTabIndex(int index);
    void updateTabButtonGeometry();
    QSize verticalSizeHint(int tab_count) const;
    int maxVerticalScrollOffset() const;
    void setVerticalScrollOffset(int offset);
    void ensureTabVisible(int index);

    TabLayout m_tab_layout { TabLayout::Horizontal };
    int m_available_width { 0 };
    int m_hovered_tab_index { -1 };
    int m_hover_animation_tab_index { -1 };
    int m_vertical_scroll_offset { 0 };
    qreal m_hover_progress { 0.0 };
    int m_drop_indicator_index { -1 };
    QVariantAnimation* m_hover_animation { nullptr };
    QPoint m_drag_start_position;
    int m_pressed_tab_index { -1 };
};

class TabWidget final : public QWidget {
public:
    explicit TabWidget(QWidget* parent = nullptr);

    int addTab(Tab* tab, QString const& label);
    void removeTab(int index);
    int count() const { return m_tab_bar->count(); }
    int currentIndex() const { return m_tab_bar->currentIndex(); }
    void setCurrentIndex(int index) { m_tab_bar->setCurrentIndex(index); }
    Tab* currentTab() const;
    Tab* tab(int index) const;
    int indexOf(Tab* tab) const;
    void setTabText(int index, QString const& text) { m_tab_bar->setTabText(index, text); }
    void setNewTabAction(QAction* action);
    void set_new_tab_action(QAction* action) { setNewTabAction(action); }
    TabBar* tabBar() const { return m_tab_bar; }
    TabBar* tab_bar() const { return m_tab_bar; }

    void setVerticalTabsEnabled(bool enabled);
    void set_vertical_tabs_enabled(bool enabled) { setVerticalTabsEnabled(enabled); }
    void setVerticalTabsExpanded(bool expanded);
    void set_vertical_tabs_expanded(bool expanded) { setVerticalTabsExpanded(expanded); }
    void setVerticalTabsExpandOnHover(bool expand_on_hover);
    void set_vertical_tabs_expand_on_hover(bool expand_on_hover) { setVerticalTabsExpandOnHover(expand_on_hover); }
    void setTabLayout(TabLayout layout);
    void set_tab_layout(TabLayout layout) { setTabLayout(layout); }
    void refreshTabLayout();
    void refresh_tab_layout() { refreshTabLayout(); }

    std::function<void(int)> onCurrentChanged;
    std::function<void(int)> onTabCloseRequested;
    std::function<void()> onNewTabRequested;

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    TabLayout currentTabLayout() const;
    bool verticalTabsEffectivelyExpanded() const;
    bool canExpandVerticalTabsOnHover() const;
    bool cursorIsOverVerticalTabs() const;
    int verticalTabsLayoutWidth() const;
    int currentVerticalTabsWidth() const;
    int verticalTabsTabWidth() const;

    void clearLayout(QBoxLayout* layout);
    void rebuildLayout();
    void rebuildLayoutForHorizontalTabs();
    void rebuildLayoutForVerticalTabs();
    void rebuildPageColumn();
    void updateTabLayout();
    void updateTabChromeVisibility();
    void updateVerticalTabsButtonLayout();
    void updateVerticalTabsOverlayGeometry();
    void updateVerticalTabsResizeHandle();
    void updateVerticalTabsSeparator();
    void applyVerticalTabsExpandedWidth(int width);
    void setResizeHandleProperty(char const* property, bool enabled);
    void setVerticalTabsHoverExpanded(bool expanded);
    void deferUpdateVerticalTabsHoverExpanded();
    void updateVerticalTabsHoverExpanded();
    void recreateIcons();
    void updateChromeStyle();
    QToolButton* createWindowButton(ChromeIcon icon, QString const& tooltip);

    TabBar* m_tab_bar { nullptr };
    QStackedWidget* m_stack { nullptr };
    QStackedWidget* m_toolbar_container { nullptr };
    QWidget* m_page_column { nullptr };
    QWidget* m_tab_bar_row { nullptr };
    QWidget* m_vertical_tabs_content { nullptr };
    QWidget* m_vertical_tabs_reserved_space { nullptr };
    QWidget* m_vertical_tab_bar_column { nullptr };
    QWidget* m_vertical_tabs_resize_handle { nullptr };
    QWidget* m_vertical_tabs_separator { nullptr };
    QWidget* m_window_controls { nullptr };
    QToolButton* m_minimize_window_button { nullptr };
    QToolButton* m_maximize_window_button { nullptr };
    QToolButton* m_close_window_button { nullptr };
    QToolButton* m_new_tab_button { nullptr };
    QTimer* m_vertical_tabs_hover_collapse_timer { nullptr };
    QBoxLayout* m_main_layout { nullptr };
    QBoxLayout* m_tab_bar_row_layout { nullptr };
    QBoxLayout* m_page_column_layout { nullptr };
    QBoxLayout* m_vertical_tabs_content_layout { nullptr };
    QBoxLayout* m_vertical_tab_bar_column_layout { nullptr };
    bool m_is_updating_chrome_style { false };
    bool m_vertical_tabs_enabled { false };
    bool m_vertical_tabs_expanded { true };
    bool m_vertical_tabs_expand_on_hover { false };
    bool m_vertical_tabs_hover_expanded { false };
    bool m_is_resizing_vertical_tabs { false };
    bool m_main_window_filter_installed { false };
    int m_vertical_tabs_expanded_width { browser_chrome_layout_policy().expanded_sidebar_width };
    int m_vertical_tabs_resize_start_global_x { 0 };
    int m_vertical_tabs_resize_start_width { browser_chrome_layout_policy().expanded_sidebar_width };
};

}
