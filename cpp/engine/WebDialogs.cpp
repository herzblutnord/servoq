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
// Modal Qt dialogs/menus for web form controls and script dialogs (alert/confirm/
// prompt, <select>, color, file) plus window.close(). Called synchronously from
// Servo via the CXX bridge; the SPINNING guard (servo_engine.rs) makes exec()'s
// nested event loop safe while Servo's script thread blocks for the response.

#include "ui/BrowserWindow.h"
#include "storage/PermissionStore.h"
#include "ui/Tab.h"
#include "engine/WebContentView.h"
#include "engine/servo_callbacks.h"

#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
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

AuthDialogResult show_authentication_dialog_sync(::std::int32_t tab_id, ::rust::Str url, bool for_proxy)
{
    AuthDialogResult result;
    result.accepted = false;
    if (servo_shutdown_started())
        return result;
    auto* view = ServoQ::WebContentView::findByTabId(tab_id);

    auto request_url = QUrl(QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size())));
    auto origin = request_url.host();
    if (request_url.port() > 0)
        origin += QStringLiteral(":%1").arg(request_url.port());
    if (origin.isEmpty())
        origin = QStringLiteral("This site");

    QDialog dialog(dialog_parent_for_view(view));
    dialog.setWindowTitle(QStringLiteral("Sign in"));
    dialog.setMinimumWidth(380);

    auto* layout = new QVBoxLayout(&dialog);
    auto prompt = for_proxy
        ? QStringLiteral("The proxy %1 requires a username and password.").arg(origin)
        : QStringLiteral("%1 is asking for your username and password.").arg(origin);
    auto* prompt_label = new QLabel(prompt, &dialog);
    prompt_label->setWordWrap(true);
    layout->addWidget(prompt_label);

    // Like Chrome: warn when HTTP Basic credentials would travel unencrypted.
    if (!for_proxy && request_url.scheme() == QStringLiteral("http")) {
        auto* warning = new QLabel(QStringLiteral("Your connection to this site is not private."), &dialog);
        warning->setWordWrap(true);
        warning->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
        layout->addWidget(warning);
    }

    auto* form = new QFormLayout;
    auto* username_edit = new QLineEdit(&dialog);
    auto* password_edit = new QLineEdit(&dialog);
    password_edit->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Username:"), username_edit);
    form->addRow(QStringLiteral("Password:"), password_edit);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Sign in"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    username_edit->setFocus();
    result.accepted = dialog.exec() == QDialog::Accepted;
    if (result.accepted) {
        result.username = username_edit->text().toStdString();
        result.password = password_edit->text().toStdString();
    }
    return result;
}

namespace {

// What the page is asking for, in Chrome's prompt phrasing.
QString permission_request_description(QString const& feature)
{
    if (feature == QStringLiteral("geolocation"))
        return QStringLiteral("Know your location");
    if (feature == QStringLiteral("notifications"))
        return QStringLiteral("Show notifications");
    if (feature == QStringLiteral("push"))
        return QStringLiteral("Receive push messages");
    if (feature == QStringLiteral("midi"))
        return QStringLiteral("Use your MIDI devices");
    if (feature == QStringLiteral("camera"))
        return QStringLiteral("Use your camera");
    if (feature == QStringLiteral("microphone"))
        return QStringLiteral("Use your microphone");
    if (feature == QStringLiteral("speaker-selection"))
        return QStringLiteral("Select audio output devices");
    if (feature == QStringLiteral("device-info"))
        return QStringLiteral("Read device information");
    if (feature == QStringLiteral("background-sync"))
        return QStringLiteral("Sync in the background");
    if (feature == QStringLiteral("bluetooth"))
        return QStringLiteral("Use your Bluetooth devices");
    if (feature == QStringLiteral("persistent-storage"))
        return QStringLiteral("Store data persistently on this device");
    if (feature == QStringLiteral("screen-wake-lock"))
        return QStringLiteral("Keep your screen awake");
    if (feature == QStringLiteral("gamepad"))
        return QStringLiteral("Use your gamepads");
    return QStringLiteral("Use a protected feature (%1)").arg(feature);
}

} // namespace

bool request_permission_sync(::std::int32_t tab_id, ::rust::Str origin, ::rust::Str feature)
{
    if (servo_shutdown_started())
        return false;

    auto origin_text = QString::fromUtf8(origin.data(), static_cast<qsizetype>(origin.size()));
    auto feature_text = QString::fromUtf8(feature.data(), static_cast<qsizetype>(feature.size()));
    if (origin_text.isEmpty() || origin_text == QStringLiteral("null"))
        return false; // Opaque origin: nothing to key a grant on; deny.

    // A previously persisted Allow/Block answers without prompting.
    if (auto stored = ServoQ::PermissionStore::the()->decision(origin_text, feature_text))
        return *stored;

    auto* view = ServoQ::WebContentView::findByTabId(tab_id);
    QMessageBox dialog(QMessageBox::Question,
        QStringLiteral("Permission request"),
        QStringLiteral("%1 wants to:\n\n%2")
            .arg(origin_text, permission_request_description(feature_text)),
        QMessageBox::NoButton,
        dialog_parent_for_view(view));
    auto* allow_button = dialog.addButton(QStringLiteral("Allow"), QMessageBox::AcceptRole);
    auto* block_button = dialog.addButton(QStringLiteral("Block"), QMessageBox::RejectRole);
    auto* not_now_button = dialog.addButton(QStringLiteral("Not Now"), QMessageBox::DestructiveRole);
    dialog.setDefaultButton(allow_button);
    // Esc / closing the window = "Not Now": deny this request only, ask again
    // next time (Chrome's dismiss semantics).
    dialog.setEscapeButton(not_now_button);
    dialog.exec();

    auto* clicked = dialog.clickedButton();
    if (clicked == allow_button || clicked == block_button) {
        bool allow = clicked == allow_button;
        ServoQ::PermissionStore::the()->setDecision(origin_text, feature_text, allow);
        return allow;
    }
    return false;
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
