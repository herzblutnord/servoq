/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/Icon.h
 */

#pragma once

#include <array>

#include <QIcon>
#include <QSize>
#include <QString>

class QPalette;

namespace ServoQ {

enum class ChromeIcon {
    Back,
    Forward,
    Home,
    Reload,
    Stop,
    NewTab,
    Close,
    TabClose,
    Menu,
    Star,
    StarFilled,
    Search,
    Pin,
    Globe,
    Folder,
    Volume,
    VolumeMuted,
    ChevronUp,
    ChevronDown,
    VerticalTabBarCollapse,
    VerticalTabBarExpand,
    WindowMinimize,
    WindowMaximize,
    WindowRestore,
    WindowClose,
};

constexpr inline auto ICON_DEVICE_PIXEL_RATIOS = std::array { 1, 2, 3 };

QIcon load_icon_from_uri(QString const&);
QIcon app_icon();
QIcon create_chrome_icon(ChromeIcon, QPalette const&);
QIcon loading_spinner_icon(QPalette const& palette, int frame);

QSize physical_size_for_device_pixel_ratio(QSize size, qreal device_pixel_ratio);

}
