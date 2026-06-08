/*
 * Copyright (c) 2026, Ladybird Browser Initiative and contributors
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
    QString favicon_base64_png;
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

// Root-level bookmark store: a flat list of root BookmarkItems plus BookmarkFolders,
// with a separate mixed root order so folders/bookmarks persist in one toolbar list.
// Each folder contains its own BookmarkItems. One level of nesting only.
// Storage: QStandardPaths::AppDataLocation + "/bookmarks.json", written with QSaveFile.
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

    void addBookmark(QString const& title, QString const& url, QString const& folder_id = {});
    void addFolder(QString const& title);
    void editBookmark(QString const& id, QString const& title, QString const& url);
    bool updateFavicon(QString const& url, QString const& favicon_base64_png);
    void editFolder(QString const& id, QString const& title);
    void removeBookmark(QString const& id);
    void removeFolder(QString const& id);
    void toggleBookmark(QString const& title, QString const& url);
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
