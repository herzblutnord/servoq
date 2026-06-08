#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

namespace ServoQ {

class HistoryStore : public QObject {
    Q_OBJECT
public:
    static HistoryStore* the();

    struct Entry {
        QString url;
        QString title;
        QDateTime visited_at;
    };

    QList<Entry> const& entries() const { return m_entries; }
    void recordVisit(QString const& url, QString const& title = {});
    void clearHistory();

signals:
    void changed();

private:
    HistoryStore();
    void load();
    void save();
    static QString storePath();

    QList<Entry> m_entries; // most recent first, capped at 1000
};

}
