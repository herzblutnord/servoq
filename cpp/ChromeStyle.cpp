#include "ChromeStyle.h"

// Chrome roles and widget object names are based on Ladybird's Qt UI structure.
// No Ladybird assets are embedded; colors and styles here are ServoQ-local approximations.
// Some selectors and proportions are adapted from Ladybird UI/Qt/ChromeStyle.cpp.
//
// Copyright (c) 2026-present, the Ladybird developers.
//
// SPDX-License-Identifier: BSD-2-Clause

#include <QGuiApplication>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#    include <QStyleHints>
#endif

namespace ServoQ::ChromeStyle {
namespace {

bool color_is_dark(QColor const& color)
{
    return color.lightness() < 128;
}

bool palette_is_dark(QPalette const& palette)
{
    return color_is_dark(palette.color(QPalette::Window));
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
bool palette_roles_match_color_scheme(QPalette const& palette, bool dark)
{
    return color_is_dark(palette.color(QPalette::Window)) == dark
        && color_is_dark(palette.color(QPalette::Base)) == dark
        && color_is_dark(palette.color(QPalette::Text)) != dark
        && color_is_dark(palette.color(QPalette::ButtonText)) != dark;
}
#endif

bool palette_matches_current_color_scheme(QPalette const& palette)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    auto color_scheme = QGuiApplication::styleHints()->colorScheme();
    if (color_scheme == Qt::ColorScheme::Dark)
        return palette_roles_match_color_scheme(palette, true);
    if (color_scheme == Qt::ColorScheme::Light)
        return palette_roles_match_color_scheme(palette, false);
#endif
    Q_UNUSED(palette);
    return true;
}

QColor chrome_window(QPalette const& palette)
{
    if (palette_matches_current_color_scheme(palette))
        return palette.color(QPalette::Window);
    return is_dark(palette) ? QColor(24, 25, 28) : QColor(245, 245, 246);
}

QColor chrome_base(QPalette const& palette)
{
    if (palette_matches_current_color_scheme(palette))
        return palette.color(QPalette::Base);
    return is_dark(palette) ? QColor(22, 23, 26) : QColor(255, 255, 255);
}

QColor control_hover(QPalette const& palette)
{
    return mix(chrome_surface(palette), is_dark(palette) ? QColor(57, 61, 66) : QColor(229, 229, 230), is_dark(palette) ? 0.82 : 0.62);
}

QColor control_pressed(QPalette const& palette)
{
    return mix(chrome_surface(palette), is_dark(palette) ? QColor(70, 74, 80) : QColor(219, 220, 221), is_dark(palette) ? 0.86 : 0.66);
}

} // namespace

bool is_dark(QPalette const& palette)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    auto color_scheme = QGuiApplication::styleHints()->colorScheme();
    if (color_scheme != Qt::ColorScheme::Unknown)
        return color_scheme == Qt::ColorScheme::Dark;
#endif
    return palette_is_dark(palette);
}

QColor mix(QColor const& from, QColor const& to, double amount)
{
    auto channel = [&](int a, int b) { return static_cast<int>(a + (b - a) * amount); };
    return { channel(from.red(), to.red()), channel(from.green(), to.green()), channel(from.blue(), to.blue()) };
}

QColor chrome_background(QPalette const& palette)
{
    return mix(chrome_window(palette), is_dark(palette) ? QColor(13, 15, 18) : QColor(236, 236, 237), is_dark(palette) ? 0.34 : 0.68);
}

QColor chrome_surface(QPalette const& palette)
{
    return mix(chrome_base(palette), is_dark(palette) ? QColor(34, 36, 40) : QColor(255, 255, 255), is_dark(palette) ? 0.64 : 0.72);
}

QColor chrome_surface_hover(QPalette const& palette)
{
    return mix(chrome_surface(palette), is_dark(palette) ? QColor(57, 61, 66) : QColor(229, 229, 230), is_dark(palette) ? 0.34 : 0.52);
}

QColor chrome_surface_pressed(QPalette const& palette)
{
    return mix(chrome_surface(palette), is_dark(palette) ? QColor(70, 74, 80) : QColor(219, 220, 221), is_dark(palette) ? 0.48 : 0.56);
}

QColor chrome_border(QPalette const& palette)
{
    return mix(is_dark(palette) ? chrome_surface(palette) : chrome_background(palette), is_dark(palette) ? QColor(150, 155, 162) : QColor(95, 96, 98), 0.22);
}

QColor chrome_text(QPalette const& palette)
{
    if (palette_matches_current_color_scheme(palette))
        return palette.color(QPalette::Text);
    return is_dark(palette) ? QColor(238, 241, 246) : QColor(24, 29, 36);
}

QColor chrome_button_text(QPalette const& palette)
{
    if (palette_matches_current_color_scheme(palette))
        return palette.color(QPalette::ButtonText);
    return chrome_text(palette);
}

QColor chrome_muted_text(QPalette const& palette)
{
    if (!palette_matches_current_color_scheme(palette))
        return is_dark(palette) ? QColor(154, 163, 176) : QColor(98, 108, 122);
    if (palette.color(QPalette::PlaceholderText).isValid())
        return palette.color(QPalette::PlaceholderText);
    return palette.color(QPalette::Disabled, QPalette::Text);
}

QColor chrome_accent(QPalette const& palette)
{
    return palette.color(QPalette::Highlight);
}

QString style_sheet_color(QColor const& color)
{
    return QStringLiteral("rgb(%1, %2, %3)").arg(color.red()).arg(color.green()).arg(color.blue());
}

QString application_style_sheet(QPalette const& palette)
{
    auto surface = style_sheet_color(chrome_surface(palette));
    auto hover = style_sheet_color(control_hover(palette));
    auto pressed = style_sheet_color(control_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(chrome_text(palette));
    auto muted = style_sheet_color(chrome_muted_text(palette));
    return QStringLiteral(R"(
QMainWindow { background: %1; color: %5; }
QMenu { color: %5; background: %1; border: 1px solid %4; border-radius: 7px; padding: 5px; }
QMenu::item { color: %5; background: transparent; border-radius: 5px; min-height: 20px; padding: 5px 14px; }
QMenu::item:selected { background: %2; }
QMenu::item:pressed { background: %3; }
QMenu::item:disabled { color: %6; }
QMenu::separator { background: %4; height: 1px; margin: 5px 8px; }
QStatusBar { background: %1; border-top: 1px solid %4; color: %6; }
)").arg(surface, hover, pressed, border, text, muted);
}

QString toolbar_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto hover = style_sheet_color(control_hover(palette));
    auto pressed = style_sheet_color(control_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(chrome_text(palette));
    auto muted = style_sheet_color(chrome_muted_text(palette));
    return QStringLiteral(R"(
QWidget#LadybirdToolbarContainer { background: %1; border-bottom: 1px solid %4; }
QWidget#LadybirdNavigationToolbar { background: %1; }
QWidget#LadybirdNavigationToolbar QToolButton { color: %5; background: transparent; border: 1px solid transparent; border-radius: 17px; min-width: 34px; min-height: 34px; margin: 1px 0; padding: 0; }
QWidget#LadybirdNavigationToolbar QToolButton:hover { background: %2; border-color: %4; }
QWidget#LadybirdNavigationToolbar QToolButton:pressed { background: %3; border-color: %4; }
QWidget#LadybirdNavigationToolbar QToolButton:disabled { color: %6; background: transparent; border-color: transparent; }
QWidget#LadybirdNavigationToolbar QToolButton::menu-indicator { image: none; }
)").arg(background, hover, pressed, border, text, muted);
}

QString location_edit_style_sheet(QPalette const& palette)
{
    auto surface = style_sheet_color(chrome_surface(palette));
    auto text = style_sheet_color(chrome_text(palette));
    auto muted = style_sheet_color(chrome_muted_text(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto accent = style_sheet_color(chrome_accent(palette));
    return QStringLiteral(R"(
QLineEdit#LadybirdLocationEdit { background: %1; color: %2; border: 1px solid %4; border-radius: 16px; padding: 5px 36px 5px 34px; selection-background-color: %5; }
QLineEdit#LadybirdLocationEdit:focus { border-color: %5; }
QLineEdit#LadybirdLocationEdit:placeholder { color: %3; }
QToolButton#LadybirdLocationIcon, QToolButton#LadybirdLocationAction { background: transparent; border: 0; }
)").arg(surface, text, muted, border, accent);
}

QString bookmarks_bar_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto hover = style_sheet_color(control_hover(palette));
    auto text = style_sheet_color(chrome_text(palette));
    auto border = style_sheet_color(chrome_border(palette));
    return QStringLiteral(R"(
QToolBar#LadybirdBookmarksBar { background: %1; border: 0; border-bottom: 1px solid %4; spacing: 3px; padding: 3px 10px; }
QToolBar#LadybirdBookmarksBar QToolButton { color: %3; background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 3px 7px; }
QToolBar#LadybirdBookmarksBar QToolButton:hover { background: %2; border-color: %4; }
)").arg(background, hover, text, border);
}

QString find_in_page_style_sheet(QPalette const& palette)
{
    auto surface = style_sheet_color(chrome_surface(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(chrome_text(palette));
    return QStringLiteral(R"(
QWidget#LadybirdFindInPageBar { background: %1; border-top: 1px solid %2; color: %3; }
QWidget#LadybirdFindInPageBar QPushButton { min-width: 30px; min-height: 26px; border: 0; border-radius: 5px; background: transparent; }
QWidget#LadybirdFindInPageBar QPushButton:hover { background: %2; }
)").arg(surface, border, text);
}

QString tab_widget_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto surface = style_sheet_color(chrome_surface(palette));
    auto hover = style_sheet_color(chrome_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(chrome_text(palette));
    return QStringLiteral(R"(
QWidget#LadybirdTabStrip { background: %1; border: 0; border-bottom: 1px solid %4; }
QWidget#LadybirdVerticalTabBar { color: %5; background: %1; border-right: 1px solid %4; }
QWidget#LadybirdVerticalTabBar[hovered="true"], QWidget#LadybirdVerticalTabBar[active="true"] { border-right: 1px solid %5; }
QWidget#LadybirdVerticalTabsResizeHandle { background: transparent; border: 0; }
QWidget#LadybirdVerticalTabsContentSeparator { background: %4; border: 0; min-height: 1px; max-height: 1px; }
QTabBar::tab { color: %5; background: transparent; border: 1px solid transparent; border-radius: 10px; min-width: 128px; max-width: 240px; height: 32px; margin: 5px 3px 4px 3px; padding: 0 12px; }
QTabBar::tab:hover { background: %3; }
QTabBar::tab:selected { background: %2; border-color: %4; }
QTabBar::close-button { margin-left: 6px; }
QToolButton#LadybirdNewTabButton { color: %5; background: transparent; border: 1px solid transparent; border-radius: 16px; min-width: 30px; min-height: 30px; padding: 0; }
QToolButton#LadybirdNewTabButton[verticalTabsButton="false"]:hover { background: %3; border-color: %4; }
QToolButton#LadybirdNewTabButton[verticalTabsButton="false"]:pressed { background: %6; border-color: %4; }
QToolButton#LadybirdNewTabButton[verticalTabsExpanded="true"] { border-radius: 11px; padding-left: 8px; text-align: left; }
)").arg(background, surface, hover, border, text, pressed);
}

QString web_placeholder_style_sheet(QPalette const& palette)
{
    auto surface = style_sheet_color(mix(chrome_surface(palette), chrome_background(palette), 0.55));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(chrome_text(palette));
    auto muted = style_sheet_color(chrome_muted_text(palette));
    return QStringLiteral(R"(
QWidget#ServoQWebContentPlaceholder { background: %1; color: %3; }
QLabel#ServoQPlaceholderTitle { color: %3; font-size: 20px; font-weight: 600; }
QLabel#ServoQPlaceholderSubtitle { color: %4; }
QFrame#ServoQPlaceholderCard { background: rgba(255, 255, 255, 20); border: 1px solid %2; border-radius: 14px; }
)").arg(surface, border, text, muted);
}

}
