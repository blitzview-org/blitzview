#pragma once

#include <atomic>

// Set once the application decided to quit (QCoreApplication::aboutToQuit).
// Long-running jobs on the GLOBAL thread pool — the MetadataCache disk flush
// and the ThumbnailDiskCache trim — poll this and bail out: Qt's
// QCoreApplication destructor waits for that pool, so a trim walking a
// 100 000-entry cache directory keeps the PROCESS alive long after the last
// window is gone. Both jobs are pure cache maintenance and simply resume on
// the next start.
inline std::atomic<bool>& appShuttingDownFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}

inline bool appShuttingDown()
{
    return appShuttingDownFlag().load(std::memory_order_relaxed);
}

inline void setAppShuttingDown()
{
    appShuttingDownFlag().store(true, std::memory_order_relaxed);
}
