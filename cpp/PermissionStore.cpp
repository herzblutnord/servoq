#include "PermissionStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace ServoQ {

static PermissionStore* s_instance = nullptr;

PermissionStore* PermissionStore::the()
{
    if (!s_instance)
        s_instance = new PermissionStore();
    return s_instance;
}

PermissionStore::PermissionStore()
{
    load();
}

QString PermissionStore::storePath()
{
    auto dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/permissions.json");
}

void PermissionStore::load()
{
    QFile file(storePath());
    if (!file.open(QIODevice::ReadOnly))
        return;

    auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;

    auto root = doc.object();
    for (auto origin_it = root.begin(); origin_it != root.end(); ++origin_it) {
        auto features = origin_it.value().toObject();
        QHash<QString, bool> decisions;
        for (auto feature_it = features.begin(); feature_it != features.end(); ++feature_it) {
            auto value = feature_it.value().toString();
            if (value == QStringLiteral("allow"))
                decisions.insert(feature_it.key(), true);
            else if (value == QStringLiteral("block"))
                decisions.insert(feature_it.key(), false);
        }
        if (!decisions.isEmpty())
            m_decisions.insert(origin_it.key(), decisions);
    }
}

void PermissionStore::save() const
{
    QJsonObject root;
    for (auto origin_it = m_decisions.constBegin(); origin_it != m_decisions.constEnd(); ++origin_it) {
        QJsonObject features;
        for (auto feature_it = origin_it.value().constBegin(); feature_it != origin_it.value().constEnd(); ++feature_it)
            features.insert(feature_it.key(), feature_it.value() ? QStringLiteral("allow") : QStringLiteral("block"));
        root.insert(origin_it.key(), features);
    }

    QSaveFile file(storePath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

std::optional<bool> PermissionStore::decision(QString const& origin, QString const& feature) const
{
    auto origin_it = m_decisions.constFind(origin);
    if (origin_it == m_decisions.constEnd())
        return std::nullopt;
    auto feature_it = origin_it->constFind(feature);
    if (feature_it == origin_it->constEnd())
        return std::nullopt;
    return *feature_it;
}

void PermissionStore::setDecision(QString const& origin, QString const& feature, bool allow)
{
    m_decisions[origin][feature] = allow;
    save();
}

void PermissionStore::clearAll()
{
    m_decisions.clear();
    save();
}

}
