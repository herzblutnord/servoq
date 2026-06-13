/*
 * Copyright (c) 2026, ServoQ contributors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Internal "servoq://" pages (M4.4): native Qt views shown in place of web
 * content for shell pages — settings (M4.3), history (M4.1), downloads, and a
 * debug page (shell state + console panel). One InternalPageView is hosted per
 * Tab in a QStackedWidget alongside the WebContentView; navigating to a
 * servoq:// URL releases the shared Servo surface and shows this widget.
 */
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>

class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QVBoxLayout;

namespace ServoQ {

// Process-wide ring buffer of page console messages, fed by
// servoq::notify_console_message while a debug page is open. Capture is
// reference-counted: the engine only forwards messages while at least one
// debug page is consuming them.
class ConsoleLog final : public QObject {
    Q_OBJECT
public:
    struct Message {
        int tab_id;
        int level; // 0 Log, 1 Debug, 2 Info, 3 Warn, 4 Error, 5 Trace
        QString text;
        QString time;
    };

    static ConsoleLog* the();

    void append(int tab_id, int level, QString const& text);
    QList<Message> const& messages() const { return m_messages; }
    void clear();

    // Reference-counted enable of engine-side console forwarding.
    void addConsumer();
    void removeConsumer();

signals:
    void appended(ServoQ::ConsoleLog::Message const& message);
    void cleared();

private:
    ConsoleLog() = default;

    QList<Message> m_messages;
    int m_consumers { 0 };
};

class InternalPageView final : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Unknown, Settings, History, Downloads, Debug };

    explicit InternalPageView(QWidget* parent = nullptr);
    ~InternalPageView() override;

    static bool isInternalUrl(QString const& url);
    static Kind kindForUrl(QString const& url);
    static QString titleForUrl(QString const& url);

    // Build and display the page for the given servoq:// URL.
    void showUrl(QString const& url);
    Kind currentKind() const { return m_kind; }

    // Open a normal URL in this tab / a new tab.
    std::function<void(QString const&)> onNavigate;
    std::function<void(QString const&)> onOpenInNewTab;
    // Ask the owning window to re-apply settings after a change on the page.
    std::function<void()> onSettingsChanged;

private:
    void clearContent();
    QWidget* makeScrollHost(QVBoxLayout*& out_layout);

    void buildSettingsPage();
    void buildHistoryPage();
    void buildDownloadsPage();
    void buildDebugPage();

    void refreshHistoryList(QString const& filter);
    void setConsoleConsuming(bool consuming);

    Kind m_kind { Kind::Unknown };
    QVBoxLayout* m_root_layout { nullptr };
    QWidget* m_content { nullptr };

    // History page widgets (valid only while the history page is shown).
    QLineEdit* m_history_search { nullptr };
    QListWidget* m_history_list { nullptr };

    bool m_console_consuming { false };
};

}
