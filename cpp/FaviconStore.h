#pragma once

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QTimer>

namespace ServoQ {

// Persistent favicon cache, modeled on Chromium's Favicons database: PNG
// bitmaps in SQLite, mapped from the page's host. Serves icons for restored
// session tabs, the bookmarks bar, history/recently-closed menus, and the
// URL-bar autocomplete — anywhere an icon is needed before (or without) the
// page being loaded in a live tab.
//
// Mapping is per host rather than per page URL: ServoQ re-probes the favicon
// on every page load anyway, so the cache only needs to answer "what does
// this site's icon look like" for chrome UI.
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
