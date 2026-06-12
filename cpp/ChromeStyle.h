/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/ChromeStyle.cpp
 */
#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

namespace ServoQ::ChromeStyle {

bool is_dark(QPalette const& palette);
QColor mix(QColor const& from, QColor const& to, double amount);
QColor chrome_background(QPalette const& palette);
QColor chrome_surface(QPalette const& palette);
QColor chrome_surface_hover(QPalette const& palette);
QColor chrome_surface_pressed(QPalette const& palette);
QColor chrome_control_border(QPalette const& palette);
QColor chrome_active_tab_surface_top(QPalette const& palette);
QColor chrome_active_tab_surface_bottom(QPalette const& palette);
QColor chrome_border(QPalette const& palette);
QColor chrome_window_outline(QPalette const& palette);
QColor chrome_text(QPalette const& palette);
QColor chrome_button_text(QPalette const& palette);
QColor chrome_muted_text(QPalette const& palette);
QColor chrome_accent(QPalette const& palette);
QString style_sheet_color(QColor const& color);
QString application_style_sheet(QPalette const& palette);
QString toolbar_style_sheet(QPalette const& palette);
QString location_edit_style_sheet(QPalette const& palette);
QString bookmarks_bar_style_sheet(QPalette const& palette);
QString find_in_page_style_sheet(QPalette const& palette);
QString tab_widget_style_sheet(QPalette const& palette);
QString web_placeholder_style_sheet(QPalette const& palette);

}
