// Shared gates for the SERVOQ_* env vars, cached once per process — env reads
// take a process-global lock contended on hot paths (docs/DEVIATIONS.md §0c).

#pragma once

#include <QDebug>
#include <QObject>
#include <QString>

namespace ServoQ {

inline bool debug_enabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_DEBUG");
    return v;
}

inline bool perf_enabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_PERF");
    return v;
}

inline void debug_log(char const* event, int tab_id, QString const& detail = {})
{
    if (!debug_enabled())
        return;
    if (detail.isEmpty())
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " tab_id=" << tab_id;
    else
        qInfo().nospace() << "SERVOQ_DEBUG " << event << " tab_id=" << tab_id << " " << detail;
}

} // namespace ServoQ

// Opt-in low-noise event tracing (SERVOQ_DIAG). Global names kept from the
// original per-TU forward declarations.

inline bool servoq_diag_enabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_DIAG");
    return v;
}

inline QString servoq_diag_describe(QObject const* o)
{
    if (!o)
        return QStringLiteral("<null>");
    auto name = o->objectName();
    return QStringLiteral("%1{%2}")
        .arg(QString::fromUtf8(o->metaObject()->className()))
        .arg(name.isEmpty() ? QStringLiteral("-") : name);
}

inline void servoq_diag_log(QString const& msg)
{
    if (servoq_diag_enabled())
        qInfo().noquote().nospace() << "SERVOQ_DIAG " << msg;
}
