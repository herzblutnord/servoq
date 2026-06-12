#include "StorageDb.h"

#include <QDebug>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

namespace ServoQ {

QSqlDatabase open_storage_database(QString const& file_name, QString const& connection_name)
{
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        qWarning() << "ServoQ storage: QSQLITE driver unavailable; " << file_name << "will not persist";
        return {};
    }

    auto dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);

    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
    db.setDatabaseName(dir + QLatin1Char('/') + file_name);
    if (!db.open()) {
        qWarning() << "ServoQ storage: cannot open" << file_name << ":" << db.lastError().text();
        return db;
    }

    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=250"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    return db;
}

}
