#include "storage/FaviconStore.h"
#include "storage/StorageDb.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QPixmap>
#include <QSqlQuery>
#include <QUrl>

namespace ServoQ {

static FaviconStore* s_instance = nullptr;

FaviconStore* FaviconStore::the()
{
    if (!s_instance)
        s_instance = new FaviconStore();
    return s_instance;
}

FaviconStore::FaviconStore()
{
    openDatabase();

    m_flush_timer = new QTimer(this);
    m_flush_timer->setSingleShot(true);
    m_flush_timer->setInterval(1000);
    connect(m_flush_timer, &QTimer::timeout, this, [this] { flushPendingWrites(); });
    if (auto* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this] {
            m_flush_timer->stop();
            flushPendingWrites();
        });
    }
}

void FaviconStore::openDatabase()
{
    m_db = open_storage_database(QStringLiteral("favicons.db"), QStringLiteral("servoq_favicons"));
    if (!m_db.isOpen())
        return;

    QSqlQuery query(m_db);
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS icons ("
        "  host TEXT PRIMARY KEY,"
        "  png BLOB NOT NULL,"
        "  updated_at INTEGER NOT NULL)"));
}

QString FaviconStore::hostKeyForUrl(QString const& url)
{
    return QUrl(url).host().toLower();
}

QIcon FaviconStore::iconForUrl(QString const& url)
{
    auto host = hostKeyForUrl(url);
    if (host.isEmpty())
        return {};

    auto cached = m_icon_cache.constFind(host);
    if (cached != m_icon_cache.constEnd())
        return *cached;

    QIcon icon;
    if (m_db.isOpen()) {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("SELECT png FROM icons WHERE host = ?"));
        query.addBindValue(host);
        if (query.exec() && query.next()) {
            QPixmap pixmap;
            if (pixmap.loadFromData(query.value(0).toByteArray(), "PNG"))
                icon = QIcon(pixmap);
        }
    }
    m_icon_cache.insert(host, icon);
    return icon;
}

void FaviconStore::storeIcon(QString const& url, QByteArray const& png_bytes)
{
    auto host = hostKeyForUrl(url);
    if (host.isEmpty() || png_bytes.isEmpty())
        return;

    QPixmap pixmap;
    if (!pixmap.loadFromData(png_bytes, "PNG"))
        return;

    m_icon_cache.insert(host, QIcon(pixmap));
    m_pending_writes.insert(host, png_bytes);
    scheduleFlush();
    emit iconsChanged(host);
}

void FaviconStore::importLegacyIcon(QString const& url, QByteArray const& png_bytes)
{
    auto host = hostKeyForUrl(url);
    if (host.isEmpty() || png_bytes.isEmpty())
        return;
    if (!iconForUrl(url).isNull())
        return;

    QPixmap pixmap;
    if (!pixmap.loadFromData(png_bytes, "PNG"))
        return;
    m_icon_cache.insert(host, QIcon(pixmap));
    m_pending_writes.insert(host, png_bytes);
    scheduleFlush();
}

void FaviconStore::clearIcons()
{
    m_icon_cache.clear();
    m_pending_writes.clear();
    if (m_flush_timer)
        m_flush_timer->stop();
    if (m_db.isOpen()) {
        QSqlQuery query(m_db);
        query.exec(QStringLiteral("DELETE FROM icons"));
        query.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    }
    emit iconsChanged({});
}

void FaviconStore::scheduleFlush()
{
    if (m_flush_timer && !m_flush_timer->isActive())
        m_flush_timer->start();
}

void FaviconStore::flushPendingWrites()
{
    if (m_pending_writes.isEmpty() || !m_db.isOpen()) {
        m_pending_writes.clear();
        return;
    }
    auto writes = std::move(m_pending_writes);
    m_pending_writes.clear();

    m_db.transaction();
    QSqlQuery upsert(m_db);
    upsert.prepare(QStringLiteral(
        "INSERT INTO icons (host, png, updated_at) VALUES (?, ?, ?) "
        "ON CONFLICT(host) DO UPDATE SET png = excluded.png, updated_at = excluded.updated_at"));
    auto now = QDateTime::currentSecsSinceEpoch();
    for (auto it = writes.constBegin(); it != writes.constEnd(); ++it) {
        upsert.addBindValue(it.key());
        upsert.addBindValue(it.value());
        upsert.addBindValue(now);
        upsert.exec();
    }
    m_db.commit();
}

}
