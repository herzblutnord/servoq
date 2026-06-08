/*
 * Copyright (c) 2022-2026, Ladybird Browser Initiative and contributors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/BookmarksBar.h
 *   Libraries/LibWebView/BookmarkStore.cpp
 */
#pragma once

#include <QPoint>
#include <QString>
#include <QToolBar>
#include <functional>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;

namespace ServoQ {

class BookmarksBar final : public QToolBar {
    Q_OBJECT
public:
    explicit BookmarksBar(QWidget* parent = nullptr);
    void rebuild();

    void setOpenUrlCallback(std::function<void(QString const&)> cb)          { m_open_url_callback = std::move(cb); }
    void setOpenUrlInNewTabCallback(std::function<void(QString const&)> cb)  { m_open_url_in_new_tab_callback = std::move(cb); }

    // Called by BrowserWindow on Ctrl+D
    void showAddBookmarkDialog(QString const& title = {}, QString const& url = {});

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void updateChromeStyle();
    void showAddBookmarkDialog(QString const& folder_id, QString const& prefill_title, QString const& prefill_url);
    void showEditBookmarkDialog(QString const& id);
    void showEditFolderDialog(QString const& id);
    void showNewFolderDialog();
    int insertionIndicatorX(QPoint const& drop_pos) const;
    void hideDropIndicator();

    std::function<void(QString const&)> m_open_url_callback;
    std::function<void(QString const&)> m_open_url_in_new_tab_callback;
    bool m_is_updating_chrome_style { false };
    QString m_drag_source_id;
    QString m_drag_source_type;
    QString m_drag_source_folder_id;
    QPoint m_drag_start_pos;
    QWidget* m_drop_indicator { nullptr };
};

}
