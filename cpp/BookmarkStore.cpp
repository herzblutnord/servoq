#include "BookmarkStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace ServoQ {

static BookmarkStore* s_instance = nullptr;
static QString const BookmarkType = QStringLiteral("bookmark");
static QString const FolderType = QStringLiteral("folder");

BookmarkStore* BookmarkStore::the()
{
    if (!s_instance)
        s_instance = new BookmarkStore();
    return s_instance;
}

BookmarkStore::BookmarkStore()
{
    load();
}

QString BookmarkStore::storePath()
{
    auto dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/bookmarks.json");
}

QString BookmarkStore::generateId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void BookmarkStore::load()
{
    QFile file(storePath());
    if (!file.open(QIODevice::ReadOnly))
        return;

    auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;

    auto root = doc.object();
    auto items = root[QStringLiteral("items")].toArray();

    m_root_bookmarks.clear();
    m_folders.clear();
    m_root_items.clear();

    for (auto const& item_val : items) {
        auto item = item_val.toObject();
        auto type = item[QStringLiteral("type")].toString();

        if (type == BookmarkType) {
            auto bookmark = BookmarkItem {
                item[QStringLiteral("id")].toString(generateId()),
                item[QStringLiteral("title")].toString(),
                item[QStringLiteral("url")].toString(),
                {},
                item[QStringLiteral("favicon")].toString(),
            };
            m_root_items.append({ bookmark.id, BookmarkType });
            m_root_bookmarks.append(bookmark);
        } else if (type == FolderType) {
            BookmarkFolder folder;
            folder.id = item[QStringLiteral("id")].toString(generateId());
            folder.title = item[QStringLiteral("title")].toString();

            for (auto const& child_val : item[QStringLiteral("items")].toArray()) {
                auto child = child_val.toObject();
                folder.items.append(BookmarkItem {
                    child[QStringLiteral("id")].toString(generateId()),
                    child[QStringLiteral("title")].toString(),
                    child[QStringLiteral("url")].toString(),
                    folder.id,
                    child[QStringLiteral("favicon")].toString(),
                });
            }
            m_root_items.append({ folder.id, FolderType });
            m_folders.append(folder);
        }
    }
    reconcileRootOrder();
}

void BookmarkStore::save()
{
    QJsonArray items;

    auto append_bookmark = [&items](BookmarkItem const& bm) {
        QJsonObject obj;
        obj[QStringLiteral("type")]  = BookmarkType;
        obj[QStringLiteral("id")]    = bm.id;
        obj[QStringLiteral("title")] = bm.title;
        obj[QStringLiteral("url")]   = bm.url;
        if (!bm.favicon_base64_png.isEmpty())
            obj[QStringLiteral("favicon")] = bm.favicon_base64_png;
        items.append(obj);
    };

    auto append_folder = [&items](BookmarkFolder const& folder) {
        QJsonObject obj;
        obj[QStringLiteral("type")]  = FolderType;
        obj[QStringLiteral("id")]    = folder.id;
        obj[QStringLiteral("title")] = folder.title;

        QJsonArray children;
        for (auto const& child : folder.items) {
            QJsonObject child_obj;
            child_obj[QStringLiteral("type")]  = BookmarkType;
            child_obj[QStringLiteral("id")]    = child.id;
            child_obj[QStringLiteral("title")] = child.title;
            child_obj[QStringLiteral("url")]   = child.url;
            if (!child.favicon_base64_png.isEmpty())
                child_obj[QStringLiteral("favicon")] = child.favicon_base64_png;
            children.append(child_obj);
        }
        obj[QStringLiteral("items")] = children;
        items.append(obj);
    };

    for (auto const& entry : m_root_items) {
        if (entry.type == BookmarkType) {
            if (auto const* bm = findRootBookmark(entry.id))
                append_bookmark(*bm);
        } else if (entry.type == FolderType) {
            if (auto const* folder = findFolder(entry.id))
                append_folder(*folder);
        }
    }

    QJsonObject root;
    root[QStringLiteral("items")] = items;

    QSaveFile file(storePath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
        file.commit();
    }
}

bool BookmarkStore::hasBookmark(QString const& url) const
{
    for (auto const& bm : m_root_bookmarks)
        if (bm.url == url) return true;
    for (auto const& folder : m_folders)
        for (auto const& bm : folder.items)
            if (bm.url == url) return true;
    return false;
}

BookmarkItem const* BookmarkStore::findBookmarkByUrl(QString const& url) const
{
    for (auto const& bm : m_root_bookmarks)
        if (bm.url == url) return &bm;
    for (auto const& folder : m_folders)
        for (auto const& bm : folder.items)
            if (bm.url == url) return &bm;
    return nullptr;
}

BookmarkItem const* BookmarkStore::findRootBookmark(QString const& id) const
{
    for (auto const& bm : m_root_bookmarks)
        if (bm.id == id) return &bm;
    return nullptr;
}

BookmarkFolder const* BookmarkStore::findFolder(QString const& id) const
{
    for (auto const& folder : m_folders)
        if (folder.id == id) return &folder;
    return nullptr;
}

void BookmarkStore::removeRootEntry(QString const& id)
{
    for (int i = 0; i < m_root_items.size(); ++i) {
        if (m_root_items[i].id == id) {
            m_root_items.removeAt(i);
            return;
        }
    }
}

void BookmarkStore::reconcileRootOrder()
{
    QList<BookmarkRootEntry> reconciled;
    auto append_if_valid = [this, &reconciled](BookmarkRootEntry const& entry) {
        if (entry.type == BookmarkType && findRootBookmark(entry.id))
            reconciled.append(entry);
        else if (entry.type == FolderType && findFolder(entry.id))
            reconciled.append(entry);
    };

    for (auto const& entry : m_root_items)
        append_if_valid(entry);

    auto contains_id = [&reconciled](QString const& id) {
        for (auto const& entry : reconciled) {
            if (entry.id == id)
                return true;
        }
        return false;
    };

    for (auto const& bm : m_root_bookmarks) {
        if (!contains_id(bm.id))
            reconciled.append({ bm.id, BookmarkType });
    }
    for (auto const& folder : m_folders) {
        if (!contains_id(folder.id))
            reconciled.append({ folder.id, FolderType });
    }

    m_root_items = reconciled;
}

void BookmarkStore::addBookmark(QString const& title, QString const& url, QString const& folder_id)
{
    if (folder_id.isEmpty()) {
        BookmarkItem item { generateId(), title, url, {}, {} };
        m_root_items.append({ item.id, BookmarkType });
        m_root_bookmarks.append(item);
    } else {
        for (auto& folder : m_folders) {
            if (folder.id == folder_id) {
                folder.items.append(BookmarkItem { generateId(), title, url, folder_id, {} });
                break;
            }
        }
    }
    save();
    emit changed();
}

void BookmarkStore::addFolder(QString const& title)
{
    BookmarkFolder folder { generateId(), title, {} };
    m_root_items.append({ folder.id, FolderType });
    m_folders.append(folder);
    save();
    emit changed();
}

void BookmarkStore::editBookmark(QString const& id, QString const& title, QString const& url)
{
    for (auto& bm : m_root_bookmarks) {
        if (bm.id == id) { bm.title = title; bm.url = url; save(); emit changed(); return; }
    }
    for (auto& folder : m_folders) {
        for (auto& bm : folder.items) {
            if (bm.id == id) { bm.title = title; bm.url = url; save(); emit changed(); return; }
        }
    }
}

bool BookmarkStore::updateFavicon(QString const& url, QString const& favicon_base64_png)
{
    bool did_change = false;
    for (auto& bm : m_root_bookmarks) {
        if (bm.url == url && bm.favicon_base64_png != favicon_base64_png) {
            bm.favicon_base64_png = favicon_base64_png;
            did_change = true;
        }
    }
    for (auto& folder : m_folders) {
        for (auto& bm : folder.items) {
            if (bm.url == url && bm.favicon_base64_png != favicon_base64_png) {
                bm.favicon_base64_png = favicon_base64_png;
                did_change = true;
            }
        }
    }
    if (!did_change)
        return false;
    save();
    emit changed();
    return true;
}

void BookmarkStore::editFolder(QString const& id, QString const& title)
{
    for (auto& folder : m_folders) {
        if (folder.id == id) { folder.title = title; save(); emit changed(); return; }
    }
}

void BookmarkStore::removeBookmark(QString const& id)
{
    for (int i = 0; i < m_root_bookmarks.size(); ++i) {
        if (m_root_bookmarks[i].id == id) { m_root_bookmarks.removeAt(i); removeRootEntry(id); save(); emit changed(); return; }
    }
    for (auto& folder : m_folders) {
        for (int i = 0; i < folder.items.size(); ++i) {
            if (folder.items[i].id == id) { folder.items.removeAt(i); save(); emit changed(); return; }
        }
    }
}

void BookmarkStore::removeFolder(QString const& id)
{
    for (int i = 0; i < m_folders.size(); ++i) {
        if (m_folders[i].id == id) { m_folders.removeAt(i); removeRootEntry(id); save(); emit changed(); return; }
    }
}

void BookmarkStore::toggleBookmark(QString const& title, QString const& url)
{
    for (int i = 0; i < m_root_bookmarks.size(); ++i) {
        if (m_root_bookmarks[i].url == url) {
            auto id = m_root_bookmarks[i].id;
            m_root_bookmarks.removeAt(i);
            removeRootEntry(id);
            save();
            emit changed();
            return;
        }
    }
    // Check folders too
    for (auto& folder : m_folders) {
        for (int i = 0; i < folder.items.size(); ++i) {
            if (folder.items[i].url == url) {
                folder.items.removeAt(i);
                save();
                emit changed();
                return;
            }
        }
    }
    // Not found — add to root
    addBookmark(title, url, {});
}

void BookmarkStore::moveRootItem(QString const& id, int to_index)
{
    reconcileRootOrder();
    int from_index = -1;
    for (int i = 0; i < m_root_items.size(); ++i) {
        if (m_root_items[i].id == id) {
            from_index = i;
            break;
        }
    }
    if (from_index < 0) return;
    if (to_index < 0 || to_index > m_root_items.size()) return;
    if (from_index == to_index) return;
    m_root_items.move(from_index, to_index < from_index ? to_index : to_index - 1);
    save();
    emit changed();
}

QStringList BookmarkStore::toFlatList() const
{
    QStringList list;
    for (auto const& bm : m_root_bookmarks)
        list.append(bm.title + QStringLiteral("\t") + bm.url);
    for (auto const& folder : m_folders)
        for (auto const& bm : folder.items)
            list.append(bm.title + QStringLiteral("\t") + bm.url);
    return list;
}

void BookmarkStore::importFlatList(QStringList const& items)
{
    m_root_bookmarks.clear();
    m_folders.clear();
    m_root_items.clear();
    for (auto const& entry : items) {
        auto parts = entry.split(QStringLiteral("\t"));
        if (parts.size() >= 2) {
            BookmarkItem item { generateId(), parts.first(), parts.last(), {}, {} };
            m_root_items.append({ item.id, BookmarkType });
            m_root_bookmarks.append(item);
        }
    }
    save();
}

}
