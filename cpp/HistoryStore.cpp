/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   Libraries/LibWebView/HistoryStore.cpp
 */
#include "HistoryStore.h"

#include <QCoreApplication>
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

static void fill_search_fields(HistoryStore::Entry& e)
{
    e.url_lower = e.url.toLower();
    e.title_lower = e.title.toLower();
    e.host_lower = QUrl(e.url).host().toLower();
}

HistoryStore::HistoryStore()
{
    load();

    m_save_timer = new QTimer(this);
    m_save_timer->setSingleShot(true);
    m_save_timer->setInterval(1000);
    connect(m_save_timer, &QTimer::timeout, this, [this] { save(); });
    if (auto* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this] {
            if (m_save_timer->isActive()) {
                m_save_timer->stop();
                save();
            }
        });
    }
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
        if (!e.url.isEmpty()) {
            fill_search_fields(e);
            m_entries.append(std::move(e));
        }
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

// Coalesce disk writes: visits arrive on every URL/title change (SPAs mutate
// these constantly) and save() serializes all entries and syncs the file to
// disk on the UI thread. One pending write per second is plenty.
void HistoryStore::scheduleSave()
{
    if (m_save_timer && !m_save_timer->isActive())
        m_save_timer->start();
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
        if (!title.isEmpty()) {
            m_entries.first().title = title;
            m_entries.first().title_lower = title.toLower();
        }
        m_entries.first().visited_at = QDateTime::currentDateTime();
        scheduleSave();
        emit changed();
        return;
    }

    Entry e;
    e.url = url;
    e.title = title;
    e.visited_at = QDateTime::currentDateTime();
    fill_search_fields(e);
    m_entries.prepend(std::move(e));

    if (m_entries.size() > MaxHistoryEntries)
        m_entries.resize(MaxHistoryEntries);

    scheduleSave();
    emit changed();
}

void HistoryStore::clearHistory()
{
    m_entries.clear();
    // Deliberately synchronous: clearing history is a privacy action and rare;
    // it must reach disk immediately, not sit behind the debounce timer.
    if (m_save_timer)
        m_save_timer->stop();
    save();
    emit changed();
}

}
