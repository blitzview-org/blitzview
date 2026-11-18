#pragma once

// Diagnostic tracing for the side-panel slide (set BLITZVIEW_TRACE_SLIDE=1):
// timestamped stderr lines around the slide — splitter allocation, widget
// geometry events, flow/anim state. Zero overhead when the variable is
// unset; plain fprintf so the message-handler filter cannot swallow it.

#include <QElapsedTimer>
#include <QtGlobal>
#include <cstdio>
#ifdef Q_OS_LINUX
#include <sys/resource.h>
#endif

inline bool slideTraceEnabled()
{
    static const bool on =
        qEnvironmentVariableIntValue("BLITZVIEW_TRACE_SLIDE") > 0;
    return on;
}

inline qint64 slideTraceMs()
{
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer.elapsed();
}

// Per-thread resource snapshot for worker-side trace lines: lets a trace
// tell CPU work (cpu ≈ wall) from a blocked/starved thread (cpu ≪ wall —
// majflt = swapped-in pages, nivcsw = involuntary preemptions). Linux only;
// zeros elsewhere.
struct SlideTraceRusage {
    long long cpuMs = 0;
    long      majflt = 0;
    long      minflt = 0;
    long      nivcsw = 0;

    static SlideTraceRusage now()
    {
        SlideTraceRusage r;
#ifdef Q_OS_LINUX
        struct rusage u;
        if (getrusage(RUSAGE_THREAD, &u) == 0) {
            r.cpuMs = (long long)u.ru_utime.tv_sec * 1000 + u.ru_utime.tv_usec / 1000
                    + (long long)u.ru_stime.tv_sec * 1000 + u.ru_stime.tv_usec / 1000;
            r.majflt = u.ru_majflt;
            r.minflt = u.ru_minflt;
            r.nivcsw = u.ru_nivcsw;
        }
#endif
        return r;
    }
    SlideTraceRusage since(const SlideTraceRusage& start) const
    {
        SlideTraceRusage d;
        d.cpuMs = cpuMs - start.cpuMs;
        d.majflt = majflt - start.majflt;
        d.minflt = minflt - start.minflt;
        d.nivcsw = nivcsw - start.nivcsw;
        return d;
    }
};

#define TRACE_SLIDE(fmt, ...) \
    do { \
        if (slideTraceEnabled()) \
            std::fprintf(stderr, "[slide %6lld] " fmt "\n", \
                         (long long)slideTraceMs(), ##__VA_ARGS__); \
    } while (0)
