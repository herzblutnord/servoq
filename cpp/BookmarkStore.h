/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   Libraries/LibWebView/BookmarkStore.cpp
 */
#pragma once

#include <QList>
#include <QObject>
#include <QString>

namespace ServoQ {

struct BookmarkItem {
    QString id;
    QString title;
    QString url;
    QString folder_id; // empty = root level
};

struct BookmarkFolder {
    QString id;
    QString title;
    QList<BookmarkItem> items;
};

struct BookmarkRootEntry {
    QString id;
    QString type; // "bookmark" or "folder"
};

// Bookmark store: root BookmarkItems + BookmarkFolders in one mixed toolbar order,
// one level of nesting. Stored in bookmarks.json (QSaveFile).
class BookmarkStore : public QObject {
    Q_OBJECT
public:
    static BookmarkStore* the();

    // Root-level items (bookmarks not in any folder)
    QList<BookmarkItem> const& rootBookmarks() const { return m_root_bookmarks; }
    QList<BookmarkFolder> const& folders() const { return m_folders; }
    QList<BookmarkRootEntry> const& rootItems() const { return m_root_items; }

    bool hasBookmark(QString const& url) const;
    BookmarkItem const* findBookmarkByUrl(QString const& url) const;
    BookmarkItem const* findRootBookmark(QString const& id) const;
    BookmarkFolder const* findFolder(QString const& id) const;

    // The favicon parameter (PNG as base64) is stored in FaviconStore, not in
    // the bookmark itself; bookmark icons are looked up there by URL.
    void addBookmark(QString const& title, QString const& url, QString const& folder_id = {}, QString const& favicon_base64_png = {});
    void addFolder(QString const& title);
    void editBookmark(QString const& id, QString const& title, QString const& url);
    void editFolder(QString const& id, QString const& title);
    void removeBookmark(QString const& id);
    void removeFolder(QString const& id);
    void toggleBookmark(QString const& title, QString const& url, QString const& favicon_base64_png = {});
    void moveRootItem(QString const& id, int to_index);
    bool moveBookmarkToFolder(QString const& bookmark_id, QString const& folder_id, int to_index = -1);
    bool moveBookmarkToRoot(QString const& bookmark_id, int root_index = -1);
    bool moveBookmarkWithinFolder(QString const& folder_id, QString const& bookmark_id, int to_index);

    // Legacy-compatible flat list "title\turl" for Settings compatibility
    QStringList toFlatList() const;
    void importFlatList(QStringList const& items);

signals:
    void changed();

private:
    BookmarkStore();
    void load();
    void save();
    static QString storePath();
    static QString generateId();
    void removeRootEntry(QString const& id);
    void reconcileRootOrder();

    QList<BookmarkItem> m_root_bookmarks;
    QList<BookmarkFolder> m_folders;
    QList<BookmarkRootEntry> m_root_items;
};

}
