/*
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/Settings.h
 *   Libraries/LibWebView/Settings.cpp
 */

#pragma once

#include <QList>
#include <QPoint>
#include <QSettings>
#include <QSize>
#include <QStringList>
#include <optional>

namespace ServoQ {

struct SearchEngineDefinition {
    QString name;
    QString query_url;
    bool custom { false };
};

class Settings final : public QObject {
public:
    Settings(Settings const&) = delete;
    Settings& operator=(Settings const&) = delete;

    static Settings* the()
    {
        static Settings instance;
        return &instance;
    }

    QString directory() const;

    std::optional<QPoint> last_position() const;
    void set_last_position(QPoint const& last_position);

    QSize last_size() const;
    void set_last_size(QSize const& last_size);

    bool is_maximized() const;
    void set_is_maximized(bool is_maximized);

    bool vertical_tabs_enabled() const;
    void set_vertical_tabs_enabled(bool enabled);

    bool vertical_tabs_expanded() const;
    void set_vertical_tabs_expanded(bool expanded);

    bool vertical_tabs_expand_on_hover() const;
    void set_vertical_tabs_expand_on_hover(bool expand_on_hover);

    bool show_menu_bar() const;
    void set_show_menu_bar(bool show_menu_bar);

    bool show_bookmarks_bar() const;
    void set_show_bookmarks_bar(bool show_bookmarks_bar);

    bool content_blocking_enabled() const;
    void set_content_blocking_enabled(bool enabled);
    QStringList content_blocking_allowlist_hosts() const;
    bool content_blocking_disabled_for_host(QString const& host) const;
    void set_content_blocking_disabled_for_host(QString const& host, bool disabled);

    QString search_engine_name() const;
    void set_search_engine_name(QString const& name);
    QList<SearchEngineDefinition> search_engines() const;
    QStringList search_engine_names() const;
    bool is_custom_search_engine(QString const& name) const;
    bool add_custom_search_engine(QString const& name, QString const& query_url);
    void remove_custom_search_engine(QString const& name);
    QString search_url_for_query(QString const& query) const;

    QStringList bookmarks() const;
    void set_bookmarks(QStringList const& bookmarks);
    bool has_bookmark(QString const& url) const;
    void toggle_bookmark(QString const& title, QString const& url);

private:
    Settings();

    QSettings m_qsettings;
};

}
