#pragma once

#include <QSqlDatabase>
#include <QString>

namespace ServoQ {

// Opens (or creates) a SQLite database in the app data directory, configured
// for UI-thread use: WAL journaling so commits append to the log instead of
// fsyncing the main file (per-navigation writes must never block the UI
// thread), synchronous=NORMAL, and a busy timeout so a stale -wal/-shm lock
// from a crashed process doesn't error out immediately.
//
// Returns an invalid QSqlDatabase if the driver is missing or the file cannot
// be opened; callers must check isOpen() and degrade to in-memory operation.
QSqlDatabase open_storage_database(QString const& file_name, QString const& connection_name);

}
