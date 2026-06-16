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

// Session persistence in its own session.json (not QSettings, which would rewrite
// the whole INI per debounce window). Open tabs are saved only while "continue
// where you left off" is on; the recently-closed list is always saved (Ctrl+Shift+T).
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
