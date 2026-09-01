#pragma once

// Diagnostic tracing for the side-panel slide (set BLITZVIEW_TRACE_SLIDE=1):
// timestamped stderr lines around the slide — splitter allocation, widget
// geometry events, flow/anim state. Zero overhead when the variable is
// unset; plain fprintf so the message-handler filter cannot swallow it.

#include <QElapsedTimer>
#include <QtGlobal>
#include <cstdio>

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

#define TRACE_SLIDE(fmt, ...) \
    do { \
        if (slideTraceEnabled()) \
            std::fprintf(stderr, "[slide %6lld] " fmt "\n", \
                         (long long)slideTraceMs(), ##__VA_ARGS__); \
    } while (0)
