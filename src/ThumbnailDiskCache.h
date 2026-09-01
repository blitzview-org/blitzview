#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>
#include <atomic>
#include <optional>

#include "MediaMetadata.h"

// Persistent thumbnail cache — third tier below the in-memory ThumbnailCache.
// Stores the already-compressed thumbnail JPEGs produced by the loader workers.
//
// Entries are keyed by a hash of (absolute path, mtime, file size): a changed
// media file automatically misses, and its stale entry ages out via the
// size-based LRU trim. On a read hit the entry file's mtime is refreshed so
// the trim removes least-recently-used entries first.
//
// All methods are thread-safe. read/write are called from loader worker
// threads; the settings UI calls the maintenance methods.
class ThumbnailDiskCache
{
public:
    static ThumbnailDiskCache& instance();

    void setEnabled(bool enabled);
    bool isEnabled() const;

    // Total size limit in bytes. 0 = unlimited.
    void   setMaxBytes(qint64 bytes);
    qint64 maxBytes() const;

    struct Entry {
        QByteArray smallThumb;       // EXIF/low-res JPEG
        QByteArray largeThumb;       // full-decode JPEG
        int        largeSizeKey = 0;
        QSize      oriented;         // display-oriented image dimensions (invalid = unknown)
    };

    Entry read(const QString& mediaPath);
    void writeSmall(const QString& mediaPath, const QByteArray& jpeg,
                    const QSize& oriented);
    void writeLarge(const QString& mediaPath, const QByteArray& jpeg,
                    int sizeKey, const QSize& oriented);

    // Media metadata entries (.bvm) — same (path, mtime, size) key scheme,
    // covered by the same LRU trim. Own format version, independent of the
    // thumbnail entry version.
    std::optional<MediaMetadata> readMeta(const QString& mediaPath);
    void writeMeta(const QString& mediaPath, const MediaMetadata& meta);

    // Maintenance / settings UI
    QString cacheDir() const { return m_dir; }
    qint64  computeCurrentBytes() const;   // synchronous scan — call off the UI thread
    void    clear();
    void    trimIfNeeded();                // async LRU trim to maxBytes; no-op when unlimited

private:
    ThumbnailDiskCache();
    ThumbnailDiskCache(const ThumbnailDiskCache&) = delete;
    ThumbnailDiskCache& operator=(const ThumbnailDiskCache&) = delete;

    // "<cacheDir>/<hh>/<hash>" for the media file's current (mtime, size);
    // empty when the file cannot be stat'ed or the cache is disabled.
    QString entryBase(const QString& mediaPath) const;
    void writeFile(const QString& filePath, const QByteArray& jpeg,
                   int sizeKey, const QSize& oriented);
    void trimNow();

    QString             m_dir;
    std::atomic<bool>   m_enabled{true};
    std::atomic<qint64> m_maxBytes{0};
    std::atomic<qint64> m_bytesSinceTrim{0};
    std::atomic<bool>   m_trimRunning{false};
};
