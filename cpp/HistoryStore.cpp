#include "HistoryStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace ServoQ {

static constexpr int MaxHistoryEntries = 1000;

static HistoryStore* s_instance = nullptr;

HistoryStore* HistoryStore::the()
{
    if (!s_instance)
        s_instance = new HistoryStore();
    return s_instance;
}

HistoryStore::HistoryStore()
{
    load();
}

QString HistoryStore::storePath()
{
    auto dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/history.json");
}

void HistoryStore::load()
{
    QFile file(storePath());
    if (!file.open(QIODevice::ReadOnly))
        return;

    auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;

    m_entries.clear();
    for (auto const& val : doc.object()[QStringLiteral("entries")].toArray()) {
        auto obj = val.toObject();
        Entry e;
        e.url = obj[QStringLiteral("url")].toString();
        e.title = obj[QStringLiteral("title")].toString();
        e.visited_at = QDateTime::fromSecsSinceEpoch(
            static_cast<qint64>(obj[QStringLiteral("ts")].toDouble()));
        if (!e.url.isEmpty())
            m_entries.append(std::move(e));
    }
}

void HistoryStore::save()
{
    QJsonArray arr;
    for (auto const& e : m_entries) {
        QJsonObject obj;
        obj[QStringLiteral("url")] = e.url;
        obj[QStringLiteral("title")] = e.title;
        obj[QStringLiteral("ts")] = static_cast<double>(e.visited_at.toSecsSinceEpoch());
        arr.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("entries")] = arr;

    QSaveFile file(storePath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void HistoryStore::recordVisit(QString const& url, QString const& title)
{
    if (url.isEmpty() || url == QStringLiteral("about:blank"))
        return;

    // Update title on existing most-recent entry for the same URL
    if (!m_entries.isEmpty() && m_entries.first().url == url) {
        if (!title.isEmpty())
            m_entries.first().title = title;
        m_entries.first().visited_at = QDateTime::currentDateTime();
        save();
        emit changed();
        return;
    }

    Entry e;
    e.url = url;
    e.title = title;
    e.visited_at = QDateTime::currentDateTime();
    m_entries.prepend(std::move(e));

    if (m_entries.size() > MaxHistoryEntries)
        m_entries.resize(MaxHistoryEntries);

    save();
    emit changed();
}

void HistoryStore::clearHistory()
{
    m_entries.clear();
    save();
    emit changed();
}

}
