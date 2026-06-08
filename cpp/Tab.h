#pragma once

#include <QStringList>
#include <QWidget>
#include <QIcon>

class QAction;
class QLabel;
class QMenu;
class QSpacerItem;
class QTimer;
class QToolButton;

namespace ServoQ {

class BookmarksBar;
class BrowserWindow;
class FindInPageWidget;
class LocationEdit;
class WebContentView;

class Tab final : public QWidget {
public:
    explicit Tab(BrowserWindow* window, int controller_id);

    QString title() const { return m_title; }
    QString url() const { return m_url; }
    QIcon tabIcon() const;
    int controllerId() const { return m_controller_id; }
    QWidget* toolbarContainer() const { return m_toolbar_container; }
    BookmarksBar* bookmarksBar() const { return m_bookmarks_bar; }
    FindInPageWidget* findInPageWidget() const { return m_find_in_page; }
    LocationEdit* locationEdit() const { return m_location_edit; }

    void setToolbarContainerInTabLayout(bool in_tab_layout);
    void setVerticalTabsEnabled(bool enabled);
    void setHamburgerButtonVisible(bool visible);
    void setActive(bool active);
    void showEmptyNewTab();
    void attachExistingWebView(QString const& initial_url = {});
    void updateToggleVerticalTabsIcon(); // [ladybird: Tab.cpp:794-797]
    void navigate(QString const& url);
    void location_edit_return_pressed();
    void focusLocationEditor();
    void showFindInPage();
    void findPrevious();
    void findNext();
    void setStatusText(QString const& status);
    void applyControllerState();

    // Page zoom — [ladybird: BrowserWindow.cpp:1372-1374]
    void zoomIn();
    void zoomOut();
    void resetZoom();

    void on_url_change(QString const& url);
    void on_title_change(QString const& title);
    void on_load_start(QString const& url);
    void on_load_finish();
    void on_favicon_change(QIcon const& icon = {});
    void on_link_hover(QString const& url);
    void on_history_changed(QStringList const& urls, int current);

private:
    QToolButton* createToolbarButton(QAction* action);
    void buildToolbar();
    void update_tab_title();
    void set_loading(bool is_loading);
    void refreshBookmarkIcon();
    void updateZoomAction(); // [ladybird: Tab.cpp:233 — reset_zoom_action text/visibility]
    void updateHoverLabel(); // [ladybird: Tab.cpp:748-763] — positions label at bottom-left

    BrowserWindow* m_window { nullptr };
    int m_controller_id { 0 };
    QWidget* m_toolbar_container { nullptr };
    QWidget* m_toolbar { nullptr };
    BookmarksBar* m_bookmarks_bar { nullptr };
    LocationEdit* m_location_edit { nullptr };
    WebContentView* m_view { nullptr };
    FindInPageWidget* m_find_in_page { nullptr };
    QLabel* m_hover_label { nullptr };
    QToolButton* m_hamburger_button { nullptr };
    QAction* m_toggle_vertical_tabs_action { nullptr }; // [ladybird: Tab.h:149]
    QSpacerItem* m_sidebar_toggle_spacer { nullptr };   // [ladybird: Tab.cpp:214]
    QAction* m_back_action { nullptr };
    QAction* m_forward_action { nullptr };
    QAction* m_reload_action { nullptr };
    QStringList m_history_urls;
    int m_history_current { -1 };
    QAction* m_bookmark_action { nullptr };
    QAction* m_reset_zoom_action { nullptr }; // [ladybird: Tab.cpp:233 — view().reset_zoom_action()]
    QTimer* m_loading_animation_timer { nullptr };
    QIcon m_favicon;
    QString m_url { QStringLiteral("about:blank") };
    QString m_title { QStringLiteral("New Tab") };
    bool m_is_loading { false };
    bool m_is_empty_new_tab { true };
    int m_loading_animation_frame { 0 };
};

}
