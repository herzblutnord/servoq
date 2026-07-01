#pragma once

#include <QSqlDatabase>
#include <QString>

namespace ServoQ {

// Opens/creates a SQLite DB in the app data dir, configured for UI-thread use
// (WAL, synchronous=NORMAL, busy timeout). Returns an invalid QSqlDatabase on
// failure; callers must check isOpen() and degrade to in-memory.
QSqlDatabase open_storage_database(QString const& file_name, QString const& connection_name);

}
