#pragma once

#include <QString>
#include <QVector>

namespace ServoQ {

struct SessionTabState {
    QString url;
    bool is_empty_new_tab { false };
    bool pinned { false };
};

struct ClosedTabState {
    QString url;
    QString title;
    bool was_empty_new_tab { false };
    bool was_pinned { false };
};

// Session persistence in its own file (session.json), like Firefox's
// sessionstore and Chromium's Sessions/ directory — NOT inside QSettings:
// session state changes on every navigation/tab event, and storing it in the
// settings INI rewrote the whole config file once per debounce window.
//
// Open tabs are only written while "continue where you left off" is enabled
// (and cleared when it is turned off); the recently-closed list is always
// persisted so Ctrl+Shift+T works across restarts, as in Chrome/Firefox.
class SessionStore {
public:
    static SessionStore* the();

    QVector<SessionTabState> tabs() const { return m_tabs; }
    int activeTabIndex() const { return m_active_tab_index; }
    void setTabs(QVector<SessionTabState> const& tabs, int active_tab_index);
    void clearTabs();

    QVector<ClosedTabState> recentlyClosedTabs() const { return m_closed_tabs; }
    void setRecentlyClosedTabs(QVector<ClosedTabState> const& closed_tabs);

private:
    SessionStore();
    void load();
    void migrateLegacyQSettingsSession();
    void save();
    static QString storePath();

    QVector<SessionTabState> m_tabs;
    QVector<ClosedTabState> m_closed_tabs; // oldest to newest
    int m_active_tab_index { 0 };
};

}
