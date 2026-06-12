/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   Libraries/LibWebView/HistoryStore.cpp
 */
#include "HistoryStore.h"
#include "StorageDb.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace ServoQ {

// Size of the in-memory autocomplete/menu index. The DB itself is unbounded
// (like Firefox places.sqlite); only the per-keystroke scan is capped.
static constexpr int AutocompleteIndexSize = 4000;
// Individual visit rows older than this are expired at startup (Chromium
// expires history at 90 days). The urls rows — what autocomplete and the
// history menu use — are kept indefinitely; they are tiny.
static constexpr qint64 VisitExpiryDays = 90;

static HistoryStore* s_instance = nullptr;

HistoryStore* HistoryStore::the()
{
    if (!s_instance)
        s_instance = new HistoryStore();
    return s_instance;
}

static void fill_search_fields(HistoryStore::Entry& e)
{
    e.url_lower = e.url.toLower();
    e.title_lower = e.title.toLower();
    e.host_lower = QUrl(e.url).host().toLower();
}

HistoryStore::HistoryStore()
{
    openDatabase();
    migrateLegacyJson();
    loadIndex();
    expireOldVisits();

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

void HistoryStore::openDatabase()
{
    m_db = open_storage_database(QStringLiteral("history.db"), QStringLiteral("servoq_history"));
    if (!m_db.isOpen())
        return;

    QSqlQuery query(m_db);
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS urls ("
        "  id INTEGER PRIMARY KEY,"
        "  url TEXT NOT NULL UNIQUE,"
        "  title TEXT NOT NULL DEFAULT '',"
        "  visit_count INTEGER NOT NULL DEFAULT 0,"
        "  last_visit_time INTEGER NOT NULL DEFAULT 0)"));
    query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS urls_by_last_visit ON urls(last_visit_time)"));
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS visits ("
        "  id INTEGER PRIMARY KEY,"
        "  url_id INTEGER NOT NULL REFERENCES urls(id) ON DELETE CASCADE,"
        "  visit_time INTEGER NOT NULL)"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS visits_by_url ON visits(url_id)"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS visits_by_time ON visits(visit_time)"));
}

// One-time import of the legacy capped JSON store into the urls/visits tables.
// The JSON file is renamed (not deleted) so a downgrade still has its data.
void HistoryStore::migrateLegacyJson()
{
    if (!m_db.isOpen())
        return;

    auto legacy_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/history.json");
    QFile file(legacy_path);
    if (!file.exists())
        return;

    QSqlQuery count_query(m_db);
    if (count_query.exec(QStringLiteral("SELECT COUNT(*) FROM urls")) && count_query.next()
        && count_query.value(0).toLongLong() > 0) {
        // DB already has data; don't double-import, just retire the file.
        QFile::rename(legacy_path, legacy_path + QStringLiteral(".imported"));
        return;
    }

    if (!file.open(QIODevice::ReadOnly))
        return;
    auto doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject())
        return;

    m_db.transaction();
    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT INTO urls (url, title, visit_count, last_visit_time) VALUES (?, ?, 1, ?) "
        "ON CONFLICT(url) DO UPDATE SET"
        "  visit_count = visit_count + 1,"
        "  last_visit_time = MAX(last_visit_time, excluded.last_visit_time),"
        "  title = CASE WHEN excluded.title != '' THEN excluded.title ELSE urls.title END"));
    QSqlQuery insert_visit(m_db);
    insert_visit.prepare(QStringLiteral(
        "INSERT INTO visits (url_id, visit_time) "
        "SELECT id, ? FROM urls WHERE url = ?"));

    // Legacy entries are most-recent-first; iterate backwards so the upserted
    // titles/last_visit_time end on the most recent values.
    auto entries = doc.object()[QStringLiteral("entries")].toArray();
    for (auto i = entries.size(); i-- > 0;) {
        auto obj = entries[i].toObject();
        auto url = obj[QStringLiteral("url")].toString();
        if (url.isEmpty())
            continue;
        auto ts = static_cast<qint64>(obj[QStringLiteral("ts")].toDouble());
        insert.addBindValue(url);
        insert.addBindValue(obj[QStringLiteral("title")].toString());
        insert.addBindValue(ts);
        insert.exec();
        insert_visit.addBindValue(ts);
        insert_visit.addBindValue(url);
        insert_visit.exec();
    }
    m_db.commit();

    QFile::rename(legacy_path, legacy_path + QStringLiteral(".imported"));
}

void HistoryStore::loadIndex()
{
    m_entries.clear();
    if (!m_db.isOpen())
        return;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT url, title, visit_count, last_visit_time FROM urls "
        "ORDER BY last_visit_time DESC LIMIT ?"));
    query.addBindValue(AutocompleteIndexSize);
    if (!query.exec())
        return;
    while (query.next()) {
        Entry e;
        e.url = query.value(0).toString();
        e.title = query.value(1).toString();
        e.visit_count = query.value(2).toInt();
        e.visited_at = QDateTime::fromSecsSinceEpoch(query.value(3).toLongLong());
        fill_search_fields(e);
        m_entries.append(std::move(e));
    }
}

void HistoryStore::expireOldVisits()
{
    if (!m_db.isOpen())
        return;
    // Startup-time housekeeping (indexed single DELETE), kept off the
    // navigation hot path. urls rows stay; only the bulky visit log expires.
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM visits WHERE visit_time < ?"));
    query.addBindValue(QDateTime::currentSecsSinceEpoch() - VisitExpiryDays * 86400);
    query.exec();
}

void HistoryStore::scheduleFlush()
{
    if (m_flush_timer && !m_flush_timer->isActive())
        m_flush_timer->start();
}

void HistoryStore::flushPendingWrites()
{
    if (m_pending_writes.isEmpty())
        return;
    auto writes = std::move(m_pending_writes);
    m_pending_writes.clear();
    if (!m_db.isOpen())
        return;

    m_db.transaction();
    QSqlQuery upsert(m_db);
    upsert.prepare(QStringLiteral(
        "INSERT INTO urls (url, title, visit_count, last_visit_time) VALUES (?, ?, 1, ?) "
        "ON CONFLICT(url) DO UPDATE SET"
        "  visit_count = visit_count + 1,"
        "  last_visit_time = excluded.last_visit_time,"
        "  title = CASE WHEN excluded.title != '' THEN excluded.title ELSE urls.title END"));
    QSqlQuery insert_visit(m_db);
    insert_visit.prepare(QStringLiteral(
        "INSERT INTO visits (url_id, visit_time) SELECT id, ? FROM urls WHERE url = ?"));
    QSqlQuery update_title(m_db);
    update_title.prepare(QStringLiteral(
        "UPDATE urls SET title = ?, last_visit_time = MAX(last_visit_time, ?) WHERE url = ?"));

    for (auto const& write : writes) {
        if (write.kind == PendingWrite::Kind::Visit) {
            upsert.addBindValue(write.url);
            upsert.addBindValue(write.title);
            upsert.addBindValue(write.time_secs);
            upsert.exec();
            insert_visit.addBindValue(write.time_secs);
            insert_visit.addBindValue(write.url);
            insert_visit.exec();
        } else {
            update_title.addBindValue(write.title);
            update_title.addBindValue(write.time_secs);
            update_title.addBindValue(write.url);
            update_title.exec();
        }
    }
    m_db.commit();
}

QList<HistoryStore::AutocompleteSuggestion> HistoryStore::autocompleteSuggestions(QString const& query, int limit) const
{
    struct Candidate {
        AutocompleteSuggestion suggestion;
        int score { 100 };
        int visit_count { 0 };
    };

    auto needle = query.trimmed().toLower();
    if (needle.isEmpty() || limit <= 0)
        return {};

    QList<Candidate> candidates;
    for (auto const& entry : m_entries) {
        auto const& url_lower = entry.url_lower;
        auto const& title_lower = entry.title_lower;
        auto const& host_lower = entry.host_lower;

        int score = 100;
        if (host_lower.startsWith(needle))
            score = 0;
        else if (url_lower.startsWith(needle))
            score = 1;
        else if (title_lower.startsWith(needle))
            score = 2;
        else if (host_lower.contains(needle))
            score = 3;
        else if (url_lower.contains(needle))
            score = 4;
        else if (title_lower.contains(needle))
            score = 5;
        else
            continue;

        candidates.append({ { entry.url, entry.title }, score, entry.visit_count });
    }

    // Score buckets first; within a bucket frequently visited URLs win, and the
    // stable sort keeps the recency order from m_entries as the tiebreaker.
    std::stable_sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b) {
        if (a.score != b.score)
            return a.score < b.score;
        return a.visit_count > b.visit_count;
    });

    QList<AutocompleteSuggestion> suggestions;
    for (auto const& candidate : candidates) {
        if (suggestions.size() >= limit)
            break;
        suggestions.append(candidate.suggestion);
    }
    return suggestions;
}

void HistoryStore::recordVisit(QString const& url, QString const& title)
{
    if (url.isEmpty() || url == QStringLiteral("about:blank"))
        return;

    auto now = QDateTime::currentDateTime();

    // Same URL as the most recent entry: a title/SPA-state refresh, not a new
    // visit. Update in place without bumping the visit count.
    if (!m_entries.isEmpty() && m_entries.first().url == url) {
        auto& front = m_entries.first();
        if (!title.isEmpty()) {
            front.title = title;
            front.title_lower = title.toLower();
        }
        front.visited_at = now;
        m_pending_writes.append({ PendingWrite::Kind::TitleUpdate, url, front.title, now.toSecsSinceEpoch() });
        scheduleFlush();
        emit changed();
        return;
    }

    // Unique-URL index: pull an existing entry to the front instead of
    // duplicating it.
    int existing_index = -1;
    for (int i = 1; i < m_entries.size(); ++i) {
        if (m_entries[i].url == url) {
            existing_index = i;
            break;
        }
    }

    if (existing_index >= 0) {
        auto e = m_entries.takeAt(existing_index);
        if (!title.isEmpty()) {
            e.title = title;
            e.title_lower = title.toLower();
        }
        e.visited_at = now;
        ++e.visit_count;
        m_entries.prepend(std::move(e));
    } else {
        Entry e;
        e.url = url;
        e.title = title;
        e.visited_at = now;
        fill_search_fields(e);
        m_entries.prepend(std::move(e));
        if (m_entries.size() > AutocompleteIndexSize)
            m_entries.resize(AutocompleteIndexSize);
    }

    m_pending_writes.append({ PendingWrite::Kind::Visit, url, title, now.toSecsSinceEpoch() });
    scheduleFlush();
    emit changed();
}

void HistoryStore::clearHistory()
{
    m_entries.clear();
    m_pending_writes.clear();
    if (m_flush_timer)
        m_flush_timer->stop();
    // Deliberately synchronous: clearing history is a privacy action and rare;
    // it must reach disk immediately. The checkpoint truncates the WAL so the
    // deleted rows don't linger in the log file.
    if (m_db.isOpen()) {
        QSqlQuery query(m_db);
        query.exec(QStringLiteral("DELETE FROM visits"));
        query.exec(QStringLiteral("DELETE FROM urls"));
        query.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    }
    emit changed();
}

}
