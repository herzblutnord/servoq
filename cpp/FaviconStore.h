#pragma once

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QTimer>

namespace ServoQ {

// Persistent favicon cache: PNG bitmaps in SQLite, keyed per host (re-probed on
// every load anyway). Serves icons for chrome UI (session tabs, bookmarks bar,
// history menus, URL-bar autocomplete) before/without a live tab.
class FaviconStore : public QObject {
    Q_OBJECT
public:
    static FaviconStore* the();

    // Returns the cached icon for the page URL's host, or a null QIcon.
    QIcon iconForUrl(QString const& url);
    // Stores PNG bytes for the page URL's host. Writes are debounced and
    // batched into one transaction; lookups see the new icon immediately.
    void storeIcon(QString const& url, QByteArray const& png_bytes);
    // Legacy import path (bookmarks.json used to embed base64 PNGs): stores
    // only if no icon exists for the host yet.
    void importLegacyIcon(QString const& url, QByteArray const& png_bytes);
    void clearIcons();

signals:
    // Emitted with the affected host; empty host means "everything changed"
    // (e.g. the store was cleared). Listeners showing icons for specific URLs
    // should only refresh when the host matches, not on every page load.
    void iconsChanged(QString const& host);

private:
    FaviconStore();
    void openDatabase();
    void scheduleFlush();
    void flushPendingWrites();
    static QString hostKeyForUrl(QString const& url);

    QSqlDatabase m_db;
    // In-memory icon cache; negative results are cached as null QIcons so
    // repeated misses don't re-query the DB.
    QHash<QString, QIcon> m_icon_cache;
    QHash<QString, QByteArray> m_pending_writes; // host -> PNG bytes
    QTimer* m_flush_timer { nullptr };
};

}
