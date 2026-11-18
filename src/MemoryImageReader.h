#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QString>
#include <memory>
#include "MediaReadGate.h"

// QImageReader's handler probe (Qt 6, createReadHandlerHelper) runs under a
// process-wide static mutex and performs the device's FIRST reads while
// holding it. Probing straight from a file therefore serialises EVERY
// QImageReader in the process — including the decode pool's in-memory
// probes — behind one cold disk read at a time (traced on a saturated disk:
// six decode threads in futex_do_wait for seconds while loader threads sat
// in folio_wait_bit, extraction throughput pinned at ~10/s regardless of
// worker count). So the file is read OUTSIDE the lock and the reader works
// on memory; the probe then costs microseconds. Files above the cap fall
// back to the path reader (rare — a cap-sized decode is slow anyway).
inline constexpr qint64 kMaxInMemoryImageBytes = 128 * 1024 * 1024;

struct MemoryImageReader
{
    QByteArray bytes;
    QBuffer buffer;
    std::unique_ptr<QImageReader> reader;
    qint64 readMs = 0;      // wall time of the file read (diagnostics)
    qint64 waitMs = 0;      // time queued for a MediaReadSlot (0 if ungated)

    enum class Gate { None, Background, Urgent };

    // gate: None on the UI thread (viewer — it must never queue behind
    // workers); Background for prefetch; Urgent for the visible wave.
    explicit MemoryImageReader(const QString& path, Gate gate = Gate::Background)
    {
        {
            std::unique_ptr<MediaReadSlot> slot;
            if (gate != Gate::None) {
                slot = std::make_unique<MediaReadSlot>(gate == Gate::Urgent);
                waitMs = slot->waitMs();
            }
            QElapsedTimer timer;
            timer.start();
            QFile f(path);
            if (f.open(QIODevice::ReadOnly) && f.size() > 0 && f.size() <= kMaxInMemoryImageBytes)
                bytes = f.readAll();
            readMs = timer.elapsed();
        }
        if (!bytes.isEmpty()) {
            buffer.setData(bytes);
            buffer.open(QIODevice::ReadOnly);
            reader = std::make_unique<QImageReader>(
                &buffer, QFileInfo(path).suffix().toLower().toLatin1());
        } else {
            reader = std::make_unique<QImageReader>(path);
        }
    }
};
