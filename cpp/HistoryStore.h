/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   Libraries/LibWebView/HistoryStore.cpp
 */
#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QTimer>

namespace ServoQ {

// Persistent browsing history, modeled on the Chromium/Firefox split:
// a `urls` table (one row per known URL, with visit_count/last_visit_time)
// plus a `visits` table (one row per visit). All queries that run per
// keystroke (URL-bar autocomplete) are served from an in-memory index of the
// most recently visited URLs, so the DB size never affects typing latency.
class HistoryStore : public QObject {
    Q_OBJECT
public:
    static HistoryStore* the();

    struct Entry {
        QString url;
        QString title;
        QDateTime visited_at;
        int visit_count { 1 };
        // Precomputed at insert/load time: autocompleteSuggestions() scans every
        // entry per keystroke, and parsing a QUrl + lowering three strings per
        // entry on each keypress cost milliseconds on the UI thread.
        QString url_lower;
        QString title_lower;
        QString host_lower;
    };

    struct AutocompleteSuggestion {
        QString url;
        QString title;
    };

    // Unique URLs, most recently visited first (in-memory index, capped).
    QList<Entry> const& entries() const { return m_entries; }
    QList<AutocompleteSuggestion> autocompleteSuggestions(QString const& query, int limit = 8) const;
    void recordVisit(QString const& url, QString const& title = {});
    // Remove a single URL (and its visits) — used by the history page's
    // per-entry delete. Synchronous like clearHistory (an explicit user action).
    void removeUrl(QString const& url);
    void clearHistory();

signals:
    void changed();

private:
    HistoryStore();
    void openDatabase();
    void migrateLegacyJson();
    void loadIndex();
    void expireOldVisits();
    // Persisting goes through SQLite in one transaction per debounce window.
    // Visits arrive on every URL/title change (SPAs mutate these constantly),
    // so writes are coalesced behind a timer and flushed on quit — no
    // synchronous disk I/O on the UI thread per navigation event.
    void scheduleFlush();
    void flushPendingWrites();

    struct PendingWrite {
        enum class Kind { Visit, TitleUpdate } kind { Kind::Visit };
        QString url;
        QString title;
        qint64 time_secs { 0 };
    };

    QSqlDatabase m_db;
    QList<Entry> m_entries; // most recent first, capped at the index size
    QList<PendingWrite> m_pending_writes;
    QTimer* m_flush_timer { nullptr };
};

}
