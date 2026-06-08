/*
 * Copyright (c) 2026-present, the Ladybird developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   Libraries/LibWebView/HistoryStore.cpp
 */
#include "HistoryStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

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

QList<HistoryStore::AutocompleteSuggestion> HistoryStore::autocompleteSuggestions(QString const& query, int limit) const
{
    struct Candidate {
        AutocompleteSuggestion suggestion;
        int score { 100 };
    };

    auto needle = query.trimmed().toLower();
    if (needle.isEmpty() || limit <= 0)
        return {};

    QList<Candidate> candidates;
    QSet<QString> seen_urls;
    for (auto const& entry : m_entries) {
        if (seen_urls.contains(entry.url))
            continue;
        seen_urls.insert(entry.url);

        auto url_lower = entry.url.toLower();
        auto title_lower = entry.title.toLower();
        auto host_lower = QUrl(entry.url).host().toLower();

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

        candidates.append({ { entry.url, entry.title }, score });
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b) {
        return a.score < b.score;
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
