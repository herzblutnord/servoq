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
#include <QString>
#include <QTimer>

namespace ServoQ {

class HistoryStore : public QObject {
    Q_OBJECT
public:
    static HistoryStore* the();

    struct Entry {
        QString url;
        QString title;
        QDateTime visited_at;
        // Precomputed at insert/load time: autocompleteSuggestions() scans every
        // entry per keystroke, and parsing a QUrl + lowering three strings for
        // up to 1000 entries on each keypress cost milliseconds on the UI thread.
        QString url_lower;
        QString title_lower;
        QString host_lower;
    };

    struct AutocompleteSuggestion {
        QString url;
        QString title;
    };

    QList<Entry> const& entries() const { return m_entries; }
    QList<AutocompleteSuggestion> autocompleteSuggestions(QString const& query, int limit = 8) const;
    void recordVisit(QString const& url, QString const& title = {});
    void clearHistory();

signals:
    void changed();

private:
    HistoryStore();
    void load();
    void save();
    void scheduleSave();
    static QString storePath();

    QList<Entry> m_entries; // most recent first, capped at 1000
    // Persisting rewrites the whole store and QSaveFile::commit() syncs to disk;
    // doing that synchronously on every URL/title change stalled the UI thread.
    // Writes are coalesced behind this timer and flushed on quit.
    QTimer* m_save_timer { nullptr };
};

}
