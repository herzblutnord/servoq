#pragma once

#include <QWidget>

class QAction;
class QLabel;
class QMenu;
class QToolButton;

namespace ServoQ {

class BookmarksBar;
class BrowserWindow;
class FindInPageWidget;
class LocationEdit;
class WebContentPlaceholder;

class Tab final : public QWidget {
public:
    explicit Tab(BrowserWindow* window);

    QString title() const { return m_title; }
    QString url() const { return m_url; }
    QWidget* toolbarContainer() const { return m_toolbar_container; }
    BookmarksBar* bookmarksBar() const { return m_bookmarks_bar; }
    FindInPageWidget* findInPageWidget() const { return m_find_in_page; }

    void setToolbarContainerInTabLayout(bool in_tab_layout);
    void setVerticalTabsEnabled(bool enabled);
    void navigate(QString const& url);
    void focusLocationEditor();
    void showFindInPage();
    void findPrevious();
    void findNext();
    void setStatusText(QString const& status);

private:
    QToolButton* createToolbarButton(QAction* action);
    void buildToolbar();
    void updateFromController();
    void updateTitleFromUrl();

    BrowserWindow* m_window { nullptr };
    QWidget* m_toolbar_container { nullptr };
    QWidget* m_toolbar { nullptr };
    BookmarksBar* m_bookmarks_bar { nullptr };
    LocationEdit* m_location_edit { nullptr };
    WebContentPlaceholder* m_view { nullptr };
    FindInPageWidget* m_find_in_page { nullptr };
    QLabel* m_hover_label { nullptr };
    QToolButton* m_hamburger_button { nullptr };
    QAction* m_back_action { nullptr };
    QAction* m_forward_action { nullptr };
    QAction* m_reload_action { nullptr };
    QAction* m_bookmark_action { nullptr };
    QString m_url { QStringLiteral("about:blank") };
    QString m_title { QStringLiteral("New Tab") };
};

}
