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

// Persistent browsing history: a `urls` table (per-URL visit_count/last_visit)
// plus a `visits` table. Per-keystroke autocomplete is served from an in-memory
// index so DB size never affects typing latency.
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
    // Writes are coalesced behind a debounce timer (one SQLite transaction) and
    // flushed on quit — no synchronous disk I/O per navigation (docs/DEVIATIONS.md §0e).
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
