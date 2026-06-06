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

namespace ServoQ {
namespace {

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
