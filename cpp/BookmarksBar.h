#pragma once

#include <QToolBar>
#include <functional>

namespace ServoQ {

class BookmarksBar final : public QToolBar {
    Q_OBJECT
public:
    explicit BookmarksBar(QWidget* parent = nullptr);
    void rebuild(); // [ladybird: BookmarksBar.cpp:205-273]

    void setOpenUrlCallback(std::function<void(QString const&)> cb)          { m_open_url_callback = std::move(cb); }
    void setOpenUrlInNewTabCallback(std::function<void(QString const&)> cb)  { m_open_url_in_new_tab_callback = std::move(cb); }

    // Called by BrowserWindow on Ctrl+D — [ladybird: BrowserWindow.cpp:360]
    void showAddBookmarkDialog(QString const& title = {}, QString const& url = {});

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    void updateChromeStyle();
    void showAddBookmarkDialog(QString const& folder_id, QString const& prefill_title, QString const& prefill_url);
    void showEditBookmarkDialog(QString const& id);
    void showEditFolderDialog(QString const& id);
    void showNewFolderDialog();

    std::function<void(QString const&)> m_open_url_callback;
    std::function<void(QString const&)> m_open_url_in_new_tab_callback;
    bool m_is_updating_chrome_style { false };
};

}
