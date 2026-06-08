/*
 * Adapted from Ladybird UI/Qt/Settings.cpp.
 *
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Settings.h"

#include <QFileInfo>
#include <QUrl>

namespace ServoQ {
namespace {

QList<SearchEngineDefinition> builtin_search_engines()
{
    // Mirrors Ladybird LibWebView/SearchEngine.cpp built-ins.
    return {
        { QStringLiteral("Bing"), QStringLiteral("https://www.bing.com/search?q=%s"), false },
        { QStringLiteral("Brave"), QStringLiteral("https://search.brave.com/search?q=%s"), false },
        { QStringLiteral("DuckDuckGo"), QStringLiteral("https://duckduckgo.com/?q=%s"), false },
        { QStringLiteral("Ecosia"), QStringLiteral("https://ecosia.org/search?q=%s"), false },
        { QStringLiteral("Google"), QStringLiteral("https://www.google.com/search?q=%s"), false },
        { QStringLiteral("Kagi"), QStringLiteral("https://kagi.com/search?q=%s"), false },
        { QStringLiteral("Mojeek"), QStringLiteral("https://www.mojeek.com/search?q=%s"), false },
        { QStringLiteral("Startpage"), QStringLiteral("https://startpage.com/search?q=%s"), false },
        { QStringLiteral("Yahoo"), QStringLiteral("https://search.yahoo.com/search?p=%s"), false },
        { QStringLiteral("Yandex"), QStringLiteral("https://yandex.com/search/?text=%s"), false },
    };
}

QString custom_engine_entry(SearchEngineDefinition const& engine)
{
    return engine.name + QStringLiteral("\t") + engine.query_url;
}

std::optional<SearchEngineDefinition> parse_custom_engine_entry(QString const& entry)
{
    auto tab = entry.indexOf(QLatin1Char('\t'));
    if (tab <= 0)
        return std::nullopt;
    auto name = entry.left(tab).trimmed();
    auto query_url = entry.mid(tab + 1).trimmed();
    if (name.isEmpty() || !query_url.contains(QStringLiteral("%s")))
        return std::nullopt;
    return SearchEngineDefinition { name, query_url, true };
}

QString bookmark_entry(QString const& title, QString const& url)
{
    return title + QStringLiteral("\t") + url;
}

QString bookmark_url(QString const& entry)
{
    auto parts = entry.split(QStringLiteral("\t"));
    return parts.isEmpty() ? QString {} : parts.last();
}

}

Settings::Settings()
    : m_qsettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ServoQ"), QStringLiteral("ServoQ"), this)
{
}

QString Settings::directory() const
{
    return QFileInfo(m_qsettings.fileName()).absolutePath();
}

std::optional<QPoint> Settings::last_position() const
{
    if (m_qsettings.contains(QStringLiteral("window/last_position")))
        return m_qsettings.value(QStringLiteral("window/last_position"), QPoint()).toPoint();
    return {};
}

void Settings::set_last_position(QPoint const& last_position)
{
    m_qsettings.setValue(QStringLiteral("window/last_position"), last_position);
}

QSize Settings::last_size() const
{
    return m_qsettings.value(QStringLiteral("window/last_size"), QSize(800, 600)).toSize();
}

void Settings::set_last_size(QSize const& last_size)
{
    m_qsettings.setValue(QStringLiteral("window/last_size"), last_size);
}

bool Settings::is_maximized() const
{
    return m_qsettings.value(QStringLiteral("window/is_maximized"), QVariant(false)).toBool();
}

void Settings::set_is_maximized(bool is_maximized)
{
    m_qsettings.setValue(QStringLiteral("window/is_maximized"), is_maximized);
}

bool Settings::vertical_tabs_enabled() const
{
    return m_qsettings.value(QStringLiteral("tabs/vertical_tabs_enabled"), QVariant(false)).toBool();
}

void Settings::set_vertical_tabs_enabled(bool enabled)
{
    m_qsettings.setValue(QStringLiteral("tabs/vertical_tabs_enabled"), enabled);
}

bool Settings::vertical_tabs_expanded() const
{
    return m_qsettings.value(QStringLiteral("tabs/vertical_tabs_expanded"), QVariant(true)).toBool();
}

void Settings::set_vertical_tabs_expanded(bool expanded)
{
    m_qsettings.setValue(QStringLiteral("tabs/vertical_tabs_expanded"), expanded);
}

bool Settings::vertical_tabs_expand_on_hover() const
{
    return m_qsettings.value(QStringLiteral("tabs/vertical_tabs_expand_on_hover"), QVariant(false)).toBool();
}

void Settings::set_vertical_tabs_expand_on_hover(bool expand_on_hover)
{
    m_qsettings.setValue(QStringLiteral("tabs/vertical_tabs_expand_on_hover"), expand_on_hover);
}

bool Settings::show_menu_bar() const
{
    return m_qsettings.value(QStringLiteral("chrome/show_menu_bar"), QVariant(false)).toBool();
}

void Settings::set_show_menu_bar(bool show_menu_bar)
{
    m_qsettings.setValue(QStringLiteral("chrome/show_menu_bar"), show_menu_bar);
}

bool Settings::show_bookmarks_bar() const
{
    return m_qsettings.value(QStringLiteral("chrome/show_bookmarks_bar"), QVariant(true)).toBool();
}

void Settings::set_show_bookmarks_bar(bool show_bookmarks_bar)
{
    m_qsettings.setValue(QStringLiteral("chrome/show_bookmarks_bar"), show_bookmarks_bar);
}

bool Settings::content_blocking_enabled() const
{
    return m_qsettings.value(QStringLiteral("content_blocking/enabled"), QVariant(true)).toBool();
}

void Settings::set_content_blocking_enabled(bool enabled)
{
    m_qsettings.setValue(QStringLiteral("content_blocking/enabled"), enabled);
}

QStringList Settings::content_blocking_allowlist_hosts() const
{
    return m_qsettings.value(QStringLiteral("content_blocking/allowlist_hosts"), QStringList {}).toStringList();
}

bool Settings::content_blocking_disabled_for_host(QString const& host) const
{
    auto normalized_host = host.trimmed().toLower();
    if (normalized_host.isEmpty())
        return false;
    return content_blocking_allowlist_hosts().contains(normalized_host);
}

void Settings::set_content_blocking_disabled_for_host(QString const& host, bool disabled)
{
    auto normalized_host = host.trimmed().toLower();
    if (normalized_host.isEmpty())
        return;

    auto hosts = content_blocking_allowlist_hosts();
    hosts.removeDuplicates();
    if (disabled) {
        if (!hosts.contains(normalized_host))
            hosts.append(normalized_host);
    } else {
        hosts.removeAll(normalized_host);
    }
    m_qsettings.setValue(QStringLiteral("content_blocking/allowlist_hosts"), hosts);
}

QString Settings::search_engine_name() const
{
    return m_qsettings.value(QStringLiteral("search/engine"), QStringLiteral("DuckDuckGo")).toString();
}

void Settings::set_search_engine_name(QString const& name)
{
    m_qsettings.setValue(QStringLiteral("search/engine"), name);
}

QList<SearchEngineDefinition> Settings::search_engines() const
{
    auto engines = builtin_search_engines();
    auto custom_entries = m_qsettings.value(QStringLiteral("search/custom_engines"), QStringList {}).toStringList();
    for (auto const& entry : custom_entries) {
        auto custom_engine = parse_custom_engine_entry(entry);
        if (!custom_engine.has_value())
            continue;
        bool duplicate = false;
        for (auto const& engine : engines) {
            if (engine.name.compare(custom_engine->name, Qt::CaseInsensitive) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            engines.append(*custom_engine);
    }
    return engines;
}

QStringList Settings::search_engine_names() const
{
    QStringList names;
    for (auto const& engine : search_engines())
        names.append(engine.name);
    return names;
}

bool Settings::is_custom_search_engine(QString const& name) const
{
    for (auto const& engine : search_engines()) {
        if (engine.custom && engine.name == name)
            return true;
    }
    return false;
}

bool Settings::add_custom_search_engine(QString const& name, QString const& query_url)
{
    auto normalized_name = name.trimmed();
    auto normalized_query_url = query_url.trimmed();
    if (normalized_name.isEmpty() || !normalized_query_url.contains(QStringLiteral("%s")))
        return false;

    for (auto const& engine : search_engines()) {
        if (engine.name.compare(normalized_name, Qt::CaseInsensitive) == 0)
            return false;
    }

    auto entries = m_qsettings.value(QStringLiteral("search/custom_engines"), QStringList {}).toStringList();
    entries.append(custom_engine_entry({ normalized_name, normalized_query_url, true }));
    m_qsettings.setValue(QStringLiteral("search/custom_engines"), entries);
    return true;
}

void Settings::remove_custom_search_engine(QString const& name)
{
    auto entries = m_qsettings.value(QStringLiteral("search/custom_engines"), QStringList {}).toStringList();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        auto custom_engine = parse_custom_engine_entry(entries[i]);
        if (custom_engine.has_value() && custom_engine->name == name) {
            entries.removeAt(i);
            break;
        }
    }
    m_qsettings.setValue(QStringLiteral("search/custom_engines"), entries);
    if (search_engine_name() == name)
        set_search_engine_name(QStringLiteral("DuckDuckGo"));
}

QString Settings::search_url_for_query(QString const& query) const
{
    auto encoded = QString::fromUtf8(QUrl::toPercentEncoding(query));
    auto selected_engine = search_engine_name();
    QString query_template;
    for (auto const& engine : search_engines()) {
        if (engine.name == selected_engine) {
            query_template = engine.query_url;
            break;
        }
    }
    if (query_template.isEmpty()) {
        for (auto const& engine : builtin_search_engines()) {
            if (engine.name == QStringLiteral("DuckDuckGo")) {
                query_template = engine.query_url;
                break;
            }
        }
    }
    return query_template.replace(QStringLiteral("%s"), encoded);
}

QStringList Settings::bookmarks() const
{
    return m_qsettings.value(QStringLiteral("bookmarks/items"), QStringList {
        bookmark_entry(QStringLiteral("Ladybird"), QStringLiteral("https://ladybird.org/")),
        bookmark_entry(QStringLiteral("Servo"), QStringLiteral("https://servo.org/")),
        bookmark_entry(QStringLiteral("Qt"), QStringLiteral("https://www.qt.io/")),
    }).toStringList();
}

void Settings::set_bookmarks(QStringList const& bookmarks)
{
    m_qsettings.setValue(QStringLiteral("bookmarks/items"), bookmarks);
}

bool Settings::has_bookmark(QString const& url) const
{
    for (auto const& entry : bookmarks()) {
        if (bookmark_url(entry) == url)
            return true;
    }
    return false;
}

void Settings::toggle_bookmark(QString const& title, QString const& url)
{
    auto items = bookmarks();
    for (qsizetype i = 0; i < items.size(); ++i) {
        if (bookmark_url(items.at(i)) == url) {
            items.removeAt(i);
            set_bookmarks(items);
            return;
        }
    }
    items.append(bookmark_entry(title.isEmpty() ? url : title, url));
    set_bookmarks(items);
}

}
