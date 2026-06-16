#pragma once

#include <QHash>
#include <QString>
#include <optional>

namespace ServoQ {

// Per-origin permission decisions (permissions.json), keyed by (origin, feature).
// An explicit Allow/Block is remembered permanently; dismissing stores nothing.
// Saves are synchronous (rare, explicit actions — see docs/STORAGE.md).
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
