#include "SessionStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace ServoQ {

static SessionStore* s_instance = nullptr;

SessionStore* SessionStore::the()
{
    if (!s_instance)
        s_instance = new SessionStore();
    return s_instance;
}

SessionStore::SessionStore()
{
    load();
}

QString SessionStore::storePath()
{
    auto dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/session.json");
}

void SessionStore::load()
{
    QFile file(storePath());
    if (!file.open(QIODevice::ReadOnly)) {
        migrateLegacyQSettingsSession();
        return;
    }

    auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;
    auto root = doc.object();

    for (auto const& value : root[QStringLiteral("tabs")].toArray()) {
        auto object = value.toObject();
        auto is_empty = object[QStringLiteral("empty")].toBool(false);
        auto url = object[QStringLiteral("url")].toString().trimmed();
        if (!is_empty && url.isEmpty())
            continue;
        m_tabs.append({
            is_empty ? QStringLiteral("about:blank") : url,
            is_empty,
            object[QStringLiteral("pinned")].toBool(false),
        });
    }
    m_active_tab_index = root[QStringLiteral("active_tab")].toInt(0);

    for (auto const& value : root[QStringLiteral("closed_tabs")].toArray()) {
        auto object = value.toObject();
        m_closed_tabs.append({
            object[QStringLiteral("url")].toString(),
            object[QStringLiteral("title")].toString(),
            object[QStringLiteral("empty")].toBool(false),
            object[QStringLiteral("pinned")].toBool(false),
        });
    }
}

// Earlier builds stored the session as a JSON string blob inside the QSettings
// INI. Import it once, then remove the keys.
void SessionStore::migrateLegacyQSettingsSession()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
        QStringLiteral("ServoQ"), QStringLiteral("ServoQ"));
    auto raw_json = settings.value(QStringLiteral("session/tabs_json")).toString().toUtf8();
    if (raw_json.isEmpty())
        return;

    auto document = QJsonDocument::fromJson(raw_json);
    if (document.isArray()) {
        for (auto const& value : document.array()) {
            auto object = value.toObject();
            auto is_empty = object.value(QStringLiteral("empty")).toBool(false);
            auto url = object.value(QStringLiteral("url")).toString().trimmed();
            if (!is_empty && url.isEmpty())
                continue;
            m_tabs.append({ is_empty ? QStringLiteral("about:blank") : url, is_empty, false });
        }
        m_active_tab_index = settings.value(QStringLiteral("session/active_tab_index"), 0).toInt();
    }
    settings.remove(QStringLiteral("session/tabs_json"));
    settings.remove(QStringLiteral("session/active_tab_index"));
    if (!m_tabs.isEmpty())
        save();
}

void SessionStore::save()
{
    QJsonArray tabs;
    for (auto const& tab : m_tabs) {
        QJsonObject object;
        object.insert(QStringLiteral("url"), tab.url);
        if (tab.is_empty_new_tab)
            object.insert(QStringLiteral("empty"), true);
        if (tab.pinned)
            object.insert(QStringLiteral("pinned"), true);
        tabs.append(object);
    }

    QJsonArray closed_tabs;
    for (auto const& closed : m_closed_tabs) {
        QJsonObject object;
        object.insert(QStringLiteral("url"), closed.url);
        if (!closed.title.isEmpty())
            object.insert(QStringLiteral("title"), closed.title);
        if (closed.was_empty_new_tab)
            object.insert(QStringLiteral("empty"), true);
        if (closed.was_pinned)
            object.insert(QStringLiteral("pinned"), true);
        closed_tabs.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("tabs"), tabs);
    root.insert(QStringLiteral("active_tab"), m_active_tab_index);
    root.insert(QStringLiteral("closed_tabs"), closed_tabs);

    QSaveFile file(storePath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void SessionStore::setTabs(QVector<SessionTabState> const& tabs, int active_tab_index)
{
    m_tabs = tabs;
    m_active_tab_index = active_tab_index;
    save();
}

void SessionStore::clearTabs()
{
    if (m_tabs.isEmpty() && m_active_tab_index == 0)
        return;
    m_tabs.clear();
    m_active_tab_index = 0;
    save();
}

void SessionStore::setRecentlyClosedTabs(QVector<ClosedTabState> const& closed_tabs)
{
    m_closed_tabs = closed_tabs;
    save();
}

}
