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

    for (auto const& item_val : items) {
        auto item = item_val.toObject();
        auto type = item[QStringLiteral("type")].toString();

        if (type == QStringLiteral("bookmark")) {
            m_root_bookmarks.append(BookmarkItem {
                item[QStringLiteral("id")].toString(generateId()),
                item[QStringLiteral("title")].toString(),
                item[QStringLiteral("url")].toString(),
                {},
            });
        } else if (type == QStringLiteral("folder")) {
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
                });
            }
            m_folders.append(folder);
        }
    }
}

void BookmarkStore::save()
{
    QJsonArray items;

    for (auto const& bm : m_root_bookmarks) {
        QJsonObject obj;
        obj[QStringLiteral("type")]  = QStringLiteral("bookmark");
        obj[QStringLiteral("id")]    = bm.id;
        obj[QStringLiteral("title")] = bm.title;
        obj[QStringLiteral("url")]   = bm.url;
        items.append(obj);
    }

    for (auto const& folder : m_folders) {
        QJsonObject obj;
        obj[QStringLiteral("type")]  = QStringLiteral("folder");
        obj[QStringLiteral("id")]    = folder.id;
        obj[QStringLiteral("title")] = folder.title;

        QJsonArray children;
        for (auto const& child : folder.items) {
            QJsonObject child_obj;
            child_obj[QStringLiteral("type")]  = QStringLiteral("bookmark");
            child_obj[QStringLiteral("id")]    = child.id;
            child_obj[QStringLiteral("title")] = child.title;
            child_obj[QStringLiteral("url")]   = child.url;
            children.append(child_obj);
        }
        obj[QStringLiteral("items")] = children;
        items.append(obj);
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

BookmarkFolder const* BookmarkStore::findFolder(QString const& id) const
{
    for (auto const& folder : m_folders)
        if (folder.id == id) return &folder;
    return nullptr;
}

void BookmarkStore::addBookmark(QString const& title, QString const& url, QString const& folder_id)
{
    if (folder_id.isEmpty()) {
        m_root_bookmarks.append(BookmarkItem { generateId(), title, url, {} });
    } else {
        for (auto& folder : m_folders) {
            if (folder.id == folder_id) {
                folder.items.append(BookmarkItem { generateId(), title, url, folder_id });
                break;
            }
        }
    }
    save();
    emit changed();
}

void BookmarkStore::addFolder(QString const& title)
{
    m_folders.append(BookmarkFolder { generateId(), title, {} });
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

void BookmarkStore::editFolder(QString const& id, QString const& title)
{
    for (auto& folder : m_folders) {
        if (folder.id == id) { folder.title = title; save(); emit changed(); return; }
    }
}

void BookmarkStore::removeBookmark(QString const& id)
{
    for (int i = 0; i < m_root_bookmarks.size(); ++i) {
        if (m_root_bookmarks[i].id == id) { m_root_bookmarks.removeAt(i); save(); emit changed(); return; }
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
        if (m_folders[i].id == id) { m_folders.removeAt(i); save(); emit changed(); return; }
    }
}

void BookmarkStore::toggleBookmark(QString const& title, QString const& url)
{
    for (int i = 0; i < m_root_bookmarks.size(); ++i) {
        if (m_root_bookmarks[i].url == url) {
            m_root_bookmarks.removeAt(i);
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
    for (auto const& entry : items) {
        auto parts = entry.split(QStringLiteral("\t"));
        if (parts.size() >= 2) {
            m_root_bookmarks.append(BookmarkItem {
                generateId(), parts.first(), parts.last(), {}
            });
        }
    }
    save();
}

}
