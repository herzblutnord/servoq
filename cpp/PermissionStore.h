#pragma once

#include <QHash>
#include <QString>
#include <optional>

namespace ServoQ {

// Per-origin web permission decisions (permissions.json), modeled on the
// content-settings exceptions Chrome keeps in its profile Preferences file:
// an explicit Allow or Block from the permission prompt is remembered for
// that origin permanently; dismissing the prompt decides once and stores
// nothing. Keyed by (origin, feature), e.g. ("https://example.com",
// "notifications"). Saves are synchronous — decisions are rare, explicit
// user actions, not navigation-event traffic (see docs/STORAGE.md rules).
class PermissionStore {
public:
    static PermissionStore* the();

    // Stored decision for origin+feature, or nullopt if never decided.
    std::optional<bool> decision(QString const& origin, QString const& feature) const;
    void setDecision(QString const& origin, QString const& feature, bool allow);
    void clearAll();
    bool isEmpty() const { return m_decisions.isEmpty(); }

private:
    PermissionStore();
    void load();
    void save() const;
    static QString storePath();

    // origin -> (feature -> allow)
    QHash<QString, QHash<QString, bool>> m_decisions;
};

}
