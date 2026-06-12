/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/ChromeStyle.cpp
 */
#include "ChromeStyle.h"

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

QColor chrome_surface_recessed(QPalette const& palette)
{
    if (is_dark(palette))
        return chrome_background(palette).lighter(108);
    return mix(chrome_background(palette), QColor(150, 150, 152), 0.42); // material_color_anchors(false).recessed = QColor(150,150,152)
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

QColor chrome_control_border(QPalette const& palette)
{
    if (is_dark(palette))
        return mix(control_hover(palette), QColor(150, 155, 162), 0.42);
    return mix(chrome_border(palette), QColor(95, 96, 98), 0.18);
}

QColor chrome_active_tab_surface_top(QPalette const& palette)
{
    if (!is_dark(palette))
        return QColor(255, 255, 255);
    return mix(chrome_background(palette), QColor(255, 255, 255), 0.22);
}

QColor chrome_active_tab_surface_bottom(QPalette const& palette)
{
    if (!is_dark(palette))
        return QColor(251, 251, 251);
    return mix(chrome_background(palette), QColor(255, 255, 255), 0.20);
}

QColor chrome_border(QPalette const& palette)
{
    return mix(is_dark(palette) ? chrome_surface(palette) : chrome_background(palette), is_dark(palette) ? QColor(150, 155, 162) : QColor(95, 96, 98), 0.22);
}

QColor chrome_window_outline(QPalette const& palette)
{
    if (is_dark(palette))
        return chrome_border(palette);

    // The window outline has to hold up against arbitrary backdrops behind the window, not just our own chrome
    // surfaces, so in light mode it is mixed further toward the border anchor than chrome_border().
    return mix(chrome_background(palette), QColor(95, 96, 98), 0.5); // material_color_anchors(false).border = QColor(95,96,98)
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
    auto dark = is_dark(palette);
    auto surface_color = chrome_surface(palette);
    auto focus_color = dark ? mix(surface_color, QColor(255, 255, 255), 0.035) : surface_color;
    auto border_color = dark ? mix(chrome_background(palette), chrome_border(palette), 0.36) : chrome_border(palette);
    auto focus_border_color = mix(chrome_border(palette), chrome_accent(palette), dark ? 0.50 : 0.54);

    auto surface       = style_sheet_color(surface_color);
    auto hover         = style_sheet_color(surface_color);
    auto focus         = style_sheet_color(focus_color);
    auto border        = style_sheet_color(border_color);
    auto hover_border  = style_sheet_color(border_color);
    auto focus_border  = style_sheet_color(focus_border_color);
    auto text          = style_sheet_color(dark ? mix(chrome_text(palette), QColor(255, 255, 255), 0.08) : chrome_text(palette));
    auto placeholder   = style_sheet_color(mix(chrome_muted_text(palette), surface_color, dark ? 0.46 : 0.34));
    auto selection     = style_sheet_color(chrome_accent(palette));
    auto selection_text= style_sheet_color(palette.color(QPalette::HighlightedText));

    // "Not secure" pill colors
    auto not_secure_text       = style_sheet_color(dark ? QColor(224, 142, 136) : QColor(144, 62, 56));
    auto not_secure_background = style_sheet_color(dark ? mix(surface_color, QColor(102, 52, 48), 0.28) : QColor(246, 235, 233));
    auto not_secure_hover      = style_sheet_color(dark ? mix(surface_color, QColor(104, 55, 51), 0.34) : QColor(242, 226, 223));
    auto not_secure_pressed    = style_sheet_color(dark ? mix(surface_color, QColor(112, 60, 55), 0.40) : QColor(236, 215, 211));
    auto not_secure_border     = style_sheet_color(dark ? mix(QColor(92, 48, 45), chrome_border(palette), 0.52) : QColor(224, 203, 199));

    // Zoom indicator pill colors
    auto surf_recessed = chrome_surface_recessed(palette);
    auto zoom_text        = style_sheet_color(chrome_muted_text(palette));
    auto zoom_background  = style_sheet_color(dark ? mix(surface_color, surf_recessed, 0.28) : mix(surface_color, surf_recessed, 0.14));
    auto zoom_hover       = style_sheet_color(dark ? mix(surface_color, surf_recessed, 0.36) : mix(surface_color, surf_recessed, 0.20));
    auto zoom_pressed     = style_sheet_color(dark ? mix(surface_color, surf_recessed, 0.44) : mix(surface_color, surf_recessed, 0.28));
    auto zoom_border      = style_sheet_color(dark ? mix(chrome_border(palette), surface_color, 0.38) : mix(chrome_border(palette), surface_color, 0.54));

    return QStringLiteral(R"(
QLineEdit#LadybirdLocationEdit {
    color: %4;
    background: %1;
    border: 1px solid %2;
    border-radius: 16px;
    min-height: 32px;
    padding: 0 16px;
    selection-background-color: %6;
    selection-color: %7;
    placeholder-text-color: %5;
}
QLineEdit#LadybirdLocationEdit:hover { background: %8; border-color: %9; }
QLineEdit#LadybirdLocationEdit:focus { background: %3; border-color: %19; }
QLineEdit#LadybirdLocationEdit:disabled { color: %5; border-color: %2; }
QToolButton#LadybirdLocationIcon { background: transparent; border: 0; padding: 0; }
QToolButton#LadybirdLocationIcon[notSecure="true"] {
    color: %10;
    background: %11;
    border: 1px solid %14;
    border-radius: 10px;
    padding: 0 7px;
    font-weight: 500;
}
QToolButton#LadybirdLocationIcon[notSecure="true"]:hover { background: %12; }
QToolButton#LadybirdLocationIcon[notSecure="true"]:pressed { background: %13; }
QToolButton#LadybirdLocationZoomIndicator {
    color: %15;
    background: %16;
    border: 1px solid %20;
    border-radius: 10px;
    padding: 0 7px;
    font-weight: 500;
}
QToolButton#LadybirdLocationZoomIndicator:hover { background: %17; }
QToolButton#LadybirdLocationZoomIndicator:pressed { background: %18; }
QToolButton#LadybirdLocationAction { background: transparent; border: 0; margin: 1px; padding: 0; }
)").arg(
        surface,            // %1
        border,             // %2
        focus,              // %3
        text,               // %4
        placeholder,        // %5
        selection,          // %6
        selection_text,     // %7
        hover,              // %8
        hover_border,       // %9
        not_secure_text,    // %10
        not_secure_background, // %11
        not_secure_hover,   // %12
        not_secure_pressed, // %13
        not_secure_border,  // %14
        zoom_text,          // %15
        zoom_background,    // %16
        zoom_hover,         // %17
        zoom_pressed,       // %18
        focus_border,       // %19
        zoom_border         // %20
    );
}

QString bookmarks_bar_style_sheet(QPalette const& palette)
{
    auto hover = style_sheet_color(control_hover(palette));             // chrome_control_surface_hover
    auto pressed = style_sheet_color(control_pressed(palette));         // chrome_control_surface_pressed
    auto ctrl_border = style_sheet_color(chrome_control_border(palette));
    auto text = style_sheet_color(chrome_button_text(palette));
    return QStringLiteral(R"(
QToolBar#LadybirdBookmarksBar { color: %3; border: 0; padding: 1px 4px; spacing: 3px; }
QToolBar#LadybirdBookmarksBar QToolButton { color: %3; background: transparent; border: 1px solid transparent; border-radius: 7px; }
QToolBar#LadybirdBookmarksBar QToolButton:hover { background: %1; border-color: %4; }
QToolBar#LadybirdBookmarksBar QToolButton:pressed, QToolBar#LadybirdBookmarksBar QToolButton:checked { background: %2; border-color: %4; }
)").arg(hover, pressed, text, ctrl_border);
}

QString find_in_page_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto surface = style_sheet_color(chrome_surface(palette));
    auto hover = style_sheet_color(control_hover(palette));
    auto pressed = style_sheet_color(control_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto ctrl_border = style_sheet_color(chrome_control_border(palette));
    auto accent = style_sheet_color(chrome_accent(palette));
    auto text = style_sheet_color(chrome_text(palette));
    auto muted = style_sheet_color(chrome_muted_text(palette));
    return QStringLiteral(R"(
QWidget#LadybirdFindInPageBar { background: %1; border-top: 1px solid %5; }
QWidget#LadybirdFindInPageBar QLineEdit { color: %8; background: %2; border: 1px solid %5; border-radius: 8px; min-height: 26px; padding: 2px 9px; selection-background-color: %7; }
QWidget#LadybirdFindInPageBar QLineEdit:focus { border-color: %7; }
QWidget#LadybirdFindInPageBar QPushButton { color: %8; background: transparent; border: 1px solid transparent; border-radius: 7px; min-width: 30px; min-height: 26px; }
QWidget#LadybirdFindInPageBar QPushButton:hover { background: %3; border-color: %6; }
QWidget#LadybirdFindInPageBar QPushButton:pressed { background: %4; border-color: %6; }
QWidget#LadybirdFindInPageBar QCheckBox, QWidget#LadybirdFindInPageBar QLabel { color: %9; }
)").arg(background, surface, hover, pressed, border, ctrl_border, accent, text, muted);
}

QString tab_widget_style_sheet(QPalette const& palette)
{
    auto dark = is_dark(palette);
    auto chrome_bg = chrome_background(palette);
    auto background = style_sheet_color(chrome_bg);
    auto hover = style_sheet_color(control_hover(palette));                                          // chrome_control_surface_hover
    auto pressed = style_sheet_color(control_pressed(palette));                                      // chrome_control_surface_pressed
    auto ctrl_border = style_sheet_color(chrome_control_border(palette));
    auto text = style_sheet_color(chrome_button_text(palette));
    // Destructive close button colors
    auto close_hover = style_sheet_color(QColor(196, 43, 28));
    auto close_text = style_sheet_color(QColor(255, 255, 255));
    auto strip_sep = style_sheet_color(chrome_border(palette));
    // Sidebar separator: mix of background and border — less prominent than strip
    auto sidebar_sep = style_sheet_color(mix(chrome_bg, chrome_border(palette), dark ? 0.44 : 0.58));
    auto sidebar_sep_hover = style_sheet_color(mix(chrome_bg, chrome_border(palette), dark ? 0.64 : 0.76));
    auto vtab_active_bg = style_sheet_color(chrome_active_tab_surface_top(palette));

    return QStringLiteral(R"(
QWidget#LadybirdTabStrip { color: %4; background: %1; border: 0; border-bottom: 1px solid %8; }
QWidget#LadybirdVerticalTabBar { color: %4; background: %1; border-right: 1px solid %9; }
QWidget#LadybirdVerticalTabBar[hovered="true"], QWidget#LadybirdVerticalTabBar[active="true"] { border-right: 1px solid %10; }
QWidget#LadybirdVerticalTabsResizeHandle { background: transparent; border: 0; }
QWidget#LadybirdVerticalTabsContentSeparator { background: %8; border: 0; min-height: 1px; max-height: 1px; }
QTabBar::tab { color: %4; background: transparent; border: 1px solid transparent; border-radius: 10px; min-width: 128px; max-width: 240px; height: 32px; margin: 5px 3px 4px 3px; padding: 0 12px; }
QTabBar::tab:hover { background: %2; border-color: %3; }
QTabBar::tab:selected { background: %11; border-color: %3; }
QTabBar::close-button { margin-left: 6px; }
QToolButton#LadybirdNewTabButton { color: %4; background: transparent; border: 1px solid transparent; border-radius: 16px; min-width: 30px; min-height: 30px; padding: 0; }
QToolButton#LadybirdNewTabButton[verticalTabsButton="false"]:hover { background: %2; border-color: %3; }
QToolButton#LadybirdNewTabButton[verticalTabsButton="false"]:pressed { background: %12; border-color: %3; }
QToolButton#LadybirdNewTabButton[verticalTabsExpanded="true"] { border-radius: 11px; padding-left: 8px; text-align: left; }
QToolButton#LadybirdWindowButton, QToolButton#LadybirdCloseWindowButton { color: %4; background: transparent; border: 0; border-radius: 0; min-width: 40px; min-height: 40px; padding: 0; }
QToolButton#LadybirdWindowButton:hover { background: %2; }
QToolButton#LadybirdWindowButton:pressed { background: %12; }
QToolButton#LadybirdCloseWindowButton:hover { color: %6; background: %5; }
QToolButton#LadybirdCloseWindowButton:pressed { color: %6; background: %5; }
)").arg(background,        // %1
        hover,             // %2
        ctrl_border,       // %3
        text,              // %4
        close_hover,       // %5
        close_text,        // %6
        // %7 absent from stylesheet — no arg; next arg fills %8 (lowest remaining).
        // Qt variadic arg() replaces lowest %N each step; a QString() here would
        // shift %8 to empty string and leave 'pressed' with no placeholder.
        strip_sep,         // %8
        sidebar_sep,       // %9
        sidebar_sep_hover) // %10
     .arg(vtab_active_bg,  // %11
          pressed);        // %12
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
