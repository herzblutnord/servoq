// Opt-in instrumentation for tab-activation stalls (cheap enough to leave in):
//  * SERVOQ_NEWTAB_TRACE — timestamped ENTER/LEAVE lines per step, so a freeze
//    can be pinned to one synchronous section.
//  * SERVOQ_JANK — traced scopes warn at >200 ms ("slow_phase"), and a 50 ms
//    heartbeat warns on event-loop stalls outside traced scopes ("main_thread_gap_ms").
// Needed because the per-second SERVOQ_PERF flush is invisible while the main
// thread is blocked.

#pragma once

#include <QDebug>
#include <QElapsedTimer>
#include <QtGlobal>

namespace ServoQ {

inline bool newtab_trace_enabled()
{
    static bool const v = qEnvironmentVariableIsSet("SERVOQ_NEWTAB_TRACE");
    return v;
}

// Monotonic ms since first use (main-thread only).
inline qint64 newtab_trace_clock_ms()
{
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer.elapsed();
}

// Most recently entered phase (main-thread only) — read by the heartbeat jank
// detector so an event-loop gap can name the section that was running.
inline char const*& newtab_last_phase()
{
    static char const* s_phase = "idle";
    return s_phase;
}

// One-shot event marker. tab_id < 0 means "not tab-specific".
inline void newtab_trace_point(char const* phase, int tab_id = -1)
{
    newtab_last_phase() = phase;
    if (!newtab_trace_enabled())
        return;
    auto out = qInfo().nospace();
    out << "SERVOQ_NEWTAB t=" << newtab_trace_clock_ms() << "ms POINT " << phase;
    if (tab_id >= 0)
        out << " tab_id=" << tab_id;
}

class NewTabTraceScope {
public:
    explicit NewTabTraceScope(char const* phase, int tab_id = -1)
        : m_phase(phase)
        , m_tab_id(tab_id)
        , m_previous_phase(newtab_last_phase())
        , m_start_ms(newtab_trace_clock_ms())
    {
        newtab_last_phase() = phase;
        if (newtab_trace_enabled()) {
            auto out = qInfo().nospace();
            out << "SERVOQ_NEWTAB t=" << m_start_ms << "ms ENTER " << m_phase;
            if (m_tab_id >= 0)
                out << " tab_id=" << m_tab_id;
        }
    }

    ~NewTabTraceScope()
    {
        qint64 const end_ms = newtab_trace_clock_ms();
        qint64 const elapsed = end_ms - m_start_ms;
        if (newtab_trace_enabled()) {
            auto out = qInfo().nospace();
            out << "SERVOQ_NEWTAB t=" << end_ms << "ms LEAVE " << m_phase << " dt=" << elapsed << "ms";
            if (m_tab_id >= 0)
                out << " tab_id=" << m_tab_id;
        }
        // Always-on: a traced section blocking the main thread names itself
        // even when tracing is off (this is how the freeze gets attributed in
        // a normal SERVOQ_PERF run).
        if (elapsed > 200) {
            qWarning().nospace() << "SERVOQ_JANK slow_phase=" << m_phase
                                 << " dt_ms=" << elapsed
                                 << (m_tab_id >= 0 ? QString::number(m_tab_id).prepend(" tab_id=") : QString());
        }
        newtab_last_phase() = m_previous_phase;
    }

    NewTabTraceScope(NewTabTraceScope const&) = delete;
    NewTabTraceScope& operator=(NewTabTraceScope const&) = delete;

private:
    char const* m_phase;
    int m_tab_id;
    char const* m_previous_phase;
    qint64 m_start_ms;
};

} // namespace ServoQ
