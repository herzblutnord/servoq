/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Ladybird:
 *   UI/Qt/Tab.cpp (on_request_alert/confirm/prompt, color picker, file picker)
 *   UI/Qt/WebContentView.cpp (select dropdown)
 */
// WebDialogs.cpp
//
// Modal Qt dialogs and menus for web form controls and script dialogs
// (alert/confirm/prompt, <select>, <input type=color>, <input type=file>),
// plus window.close() handling. Called synchronously from Servo's
// show_embedder_control delegate via the CXX bridge, following the
// show_context_menu_sync pattern: the SPINNING guard in servo_engine.rs makes
// the nested Qt event loop of exec() safe, and Servo's script thread is
// blocked awaiting the response — which matches the spec semantics of these
// dialogs.

#include "BrowserWindow.h"
#include "Tab.h"
#include "WebContentView.h"
#include "servo_callbacks.h"

#include <QColorDialog>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWidget>

namespace servoq {

namespace {

// "example.org says" — like Chrome/Firefox, the dialog title carries the page
// origin so web content cannot impersonate browser UI.
QString dialog_title_for_view(ServoQ::WebContentView* view)
{
    if (view && view->tab()) {
        auto host = QUrl(view->tab()->url()).host();
        if (!host.isEmpty())
            return QStringLiteral("%1 says").arg(host);
    }
    return QStringLiteral("This page says");
}

QWidget* dialog_parent_for_view(ServoQ::WebContentView* view)
{
    // Parent to the top-level window, not the Tab/view: the window outlives
    // tab closes that may happen while the modal dialog's nested event loop
    // runs (see the no-parent rationale in show_context_menu_sync).
    return view ? view->window() : nullptr;
}

} // namespace

void show_alert_dialog_sync(::std::int32_t tab_id, ::rust::Str message)
{
    if (servo_shutdown_started())
        return;
    auto* view = ServoQ::WebContentView::findByTabId(tab_id);
    QMessageBox dialog(QMessageBox::Information,
        dialog_title_for_view(view),
        QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())),
        QMessageBox::Ok,
        dialog_parent_for_view(view));
    dialog.exec();
}

bool show_confirm_dialog_sync(::std::int32_t tab_id, ::rust::Str message)
{
    if (servo_shutdown_started())
        return false;
    auto* view = ServoQ::WebContentView::findByTabId(tab_id);
    QMessageBox dialog(QMessageBox::Question,
        dialog_title_for_view(view),
        QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())),
        QMessageBox::Ok | QMessageBox::Cancel,
        dialog_parent_for_view(view));
    return dialog.exec() == QMessageBox::Ok;
}

PromptDialogResult show_prompt_dialog_sync(::std::int32_t tab_id, ::rust::Str message, ::rust::Str default_value)
{
    PromptDialogResult result;
    result.accepted = false;
    if (servo_shutdown_started())
        return result;
    auto* view = ServoQ::WebContentView::findByTabId(tab_id);
    QInputDialog dialog(dialog_parent_for_view(view));
    dialog.setWindowTitle(dialog_title_for_view(view));
    dialog.setLabelText(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
    dialog.setTextValue(QString::fromUtf8(default_value.data(), static_cast<qsizetype>(default_value.size())));
    result.accepted = dialog.exec() == QDialog::Accepted;
    result.value = dialog.textValue().toStdString();
    return result;
}

::std::int32_t show_select_dropdown_sync(::std::int32_t tab_id, ::rust::Str items, ::std::int32_t x, ::std::int32_t y, ::std::int32_t width)
{
    if (servo_shutdown_started())
        return -1;
    auto* view = ServoQ::WebContentView::findByTabId(tab_id);
    if (!view)
        return -1;

    // No parent, like show_context_menu_sync: prevents double-free if the
    // view's Tab is deleted during menu.exec()'s nested event loop.
    QMenu menu;
    QMap<QAction*, int> option_ids;

    auto items_text = QString::fromUtf8(items.data(), static_cast<qsizetype>(items.size()));
    for (auto const& line : items_text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        auto parts = line.split(QLatin1Char('\t'));
        if (parts[0] == QStringLiteral("group") && parts.size() >= 2) {
            auto* subtitle = menu.addAction(parts[1]);
            subtitle->setDisabled(true);
            continue;
        }
        if (parts[0] != QStringLiteral("opt") || parts.size() < 6)
            continue;
        bool ok = false;
        int id = parts[1].toInt(&ok);
        if (!ok)
            continue;
        bool disabled = parts[3] == QStringLiteral("1");
        bool selected = parts[4] == QStringLiteral("1");
        bool in_group = parts[5] == QStringLiteral("1");
        auto label = in_group ? QStringLiteral("    %1").arg(parts[2]) : parts[2];
        auto* action = menu.addAction(label);
        action->setCheckable(true);
        action->setChecked(selected);
        action->setDisabled(disabled);
        option_ids[action] = id;
    }
    if (menu.actions().isEmpty())
        return -1;

    // x/y/width are device pixels relative to the webview origin, which
    // coincides with the view widget origin (see mouse forwarding).
    auto dpr = view->devicePixelRatioF();
    auto anchor = view->mapToGlobal(
        QPoint(static_cast<int>(x / dpr), static_cast<int>(y / dpr)));
    menu.setMinimumWidth(static_cast<int>(width / dpr));

    auto* chosen = menu.exec(anchor);
    return (chosen && option_ids.contains(chosen)) ? option_ids[chosen] : -1;
}

::std::int32_t show_color_picker_sync(::std::int32_t tab_id, ::std::uint8_t red, ::std::uint8_t green, ::std::uint8_t blue)
{
    if (servo_shutdown_started())
        return -1;
    auto* view = ServoQ::WebContentView::findByTabId(tab_id);
    QColorDialog dialog(QColor(red, green, blue), dialog_parent_for_view(view));
    dialog.setWindowTitle(QStringLiteral("Select Color"));
    dialog.setOption(QColorDialog::ShowAlphaChannel, false);
    if (dialog.exec() != QDialog::Accepted)
        return -1;
    auto color = dialog.selectedColor();
    if (!color.isValid())
        return -1;
    return (color.red() << 16) | (color.green() << 8) | color.blue();
}

::rust::String show_file_picker_sync(::std::int32_t tab_id, ::rust::Str filters, bool allow_multiple)
{
    if (servo_shutdown_started())
        return {};
    auto* view = ServoQ::WebContentView::findByTabId(tab_id);
    auto* parent = dialog_parent_for_view(view);

    // Servo FilterPatterns are bare lowercase extensions ("png", "jpg").
    auto filters_text = QString::fromUtf8(filters.data(), static_cast<qsizetype>(filters.size()));
    auto extensions = filters_text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QString filter = QStringLiteral("All Files (*)");
    if (!extensions.isEmpty()) {
        QStringList globs;
        for (auto const& extension : extensions)
            globs.append(QStringLiteral("*.%1").arg(extension));
        filter = QStringLiteral("Supported Files (%1);;All Files (*)").arg(globs.join(QLatin1Char(' ')));
    }

    QStringList paths;
    if (allow_multiple)
        paths = QFileDialog::getOpenFileNames(parent, QStringLiteral("Select Files"), QDir::homePath(), filter);
    else {
        auto path = QFileDialog::getOpenFileName(parent, QStringLiteral("Select File"), QDir::homePath(), filter);
        if (!path.isEmpty())
            paths.append(path);
    }
    return paths.join(QLatin1Char('\n')).toStdString();
}

void notify_webview_close_requested(::std::int32_t tab_id)
{
    if (servo_shutdown_started())
        return;
    auto* view = ServoQ::WebContentView::findByTabId(tab_id);
    if (!view)
        return;
    auto* window = dynamic_cast<ServoQ::BrowserWindow*>(view->window());
    if (!window)
        return;
    // Defer: this is called from inside a Servo delegate callback; closing the
    // tab synchronously would tear down the webview while Servo is mid-tick
    // (same deferred-close rule as TabBar's middle-click close).
    QTimer::singleShot(0, window, [window, tab_id] {
        window->closeTabForController(tab_id);
    });
}

} // namespace servoq
