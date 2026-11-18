#pragma once

#include <QElapsedTimer>
#include <QMutex>
#include <QWaitCondition>
#include <QtGlobal>

// Bounds how many loader workers READ media files (the user's photos and
// videos) at the same time. Decoding stays fully parallel across all
// workers; only the storage access is funnelled through a few slots.
//
// Why: with 12 workers each reading its own file, a cold library on a slow
// medium was traced at 550 ms per 3 MB file (5 MB/s per worker, ~30 MB/s in
// total) and 300 ms for a 64 KB EXIF header read — the signature of a disk
// (or network share) servicing 12 interleaved streams: constant seeking,
// readahead defeated. Two sequential readers get the same medium to stream
// at its real rate (traced after: 46 ms per file, 63 MB/s per reader), while
// an SSD loses nothing measurable. BLITZVIEW_READ_SLOTS overrides the
// default of 2.
//
// Urgency: the gate is NOT first-come-first-served. The visible-priority
// wave takes URGENT slots and is served before any waiting background
// reader (EXIF prefetch, neighbourhood prefetch); traced without this, a
// visible item queued 932 ms behind prefetch readers that had arrived
// earlier. Background readers only proceed while no urgent reader waits.
//
// Videos have their own single slot: an FFmpeg extraction holds the slot
// for its whole run (0.3–1 s on the USB disk, it keeps reading the
// container), and one video in a screenful otherwise halves the photo
// readers' throughput for that long.
//
// Hold a MediaReadSlot ONLY around the file access itself, never around the
// decode. Never take one on the UI thread (the viewer reads ungated).
class MediaReadGate
{
public:
    enum class Kind { Image, Video };

    static MediaReadGate& instance()
    {
        static MediaReadGate gate;
        return gate;
    }
    static MediaReadGate& videoInstance()
    {
        static MediaReadGate gate(1);
        return gate;
    }

    int slotCount() const { return m_slots; }

    void acquire(bool urgent)
    {
        QMutexLocker lk(&m_mutex);
        if (urgent)
            ++m_urgentWaiting;
        while (m_free <= 0 || (!urgent && m_urgentWaiting > 0))
            m_cond.wait(&m_mutex);
        if (urgent)
            --m_urgentWaiting;
        --m_free;
    }

    void release()
    {
        QMutexLocker lk(&m_mutex);
        ++m_free;
        m_cond.wakeAll();
    }

private:
    MediaReadGate()
    {
        bool ok = false;
        const int v = qEnvironmentVariableIntValue("BLITZVIEW_READ_SLOTS", &ok);
        m_slots = ok ? qBound(1, v, 64) : 2;
        m_free = m_slots;
    }
    // NOT named `slots`: that is a Qt keyword macro (expands to nothing),
    // which silently turned m_free(slots) into m_free() — a gate with zero
    // slots and every video reader waiting forever.
    explicit MediaReadGate(int count) : m_slots(count), m_free(count) {}

    QMutex         m_mutex;
    QWaitCondition m_cond;
    int            m_slots = 2;
    int            m_free = 2;
    int            m_urgentWaiting = 0;
};

class MediaReadSlot
{
public:
    explicit MediaReadSlot(bool urgent = false,
                           MediaReadGate::Kind kind = MediaReadGate::Kind::Image)
        : m_gate(kind == MediaReadGate::Kind::Video ? MediaReadGate::videoInstance()
                                                     : MediaReadGate::instance())
    {
        QElapsedTimer t;
        t.start();
        m_gate.acquire(urgent);
        m_waitMs = t.elapsed();
    }
    ~MediaReadSlot() { m_gate.release(); }
    MediaReadSlot(const MediaReadSlot&) = delete;
    MediaReadSlot& operator=(const MediaReadSlot&) = delete;

    qint64 waitMs() const { return m_waitMs; }   // time spent queued for a slot

private:
    MediaReadGate& m_gate;
    qint64 m_waitMs = 0;
};
