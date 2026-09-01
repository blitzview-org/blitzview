#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <atomic>
#include <optional>

#include "MediaMetadata.h"

// Thread-safe metadata cache: in-memory hash backed by the .bvm entries in
// ThumbnailDiskCache. Filled by ExifToolService (and by get() pulling disk
// entries into memory); read by the model and the details panel.
class MetadataCache
{
public:
    static MetadataCache& instance();

    // Memory first, then disk (a hit is promoted into memory).
    std::optional<MediaMetadata> get(const QString& filePath);

    // Memory-only lookup — no disk I/O, safe for hot paths.
    std::optional<MediaMetadata> peek(const QString& filePath) const;

    void put(const QString& filePath, const MediaMetadata& meta);

    // Drops in-memory entries (after a metadata write: the file's mtime
    // changed, so the disk entry misses automatically and a fresh read runs).
    void remove(const QStringList& filePaths);

    // Every tag ever seen by put(), persisted across sessions — offered as
    // completion choices when adding tags.
    QStringList knownTags() const;

private:
    void collectTags(const QStringList& tags);
    void scheduleFlush();
    MetadataCache() = default;
    MetadataCache(const MetadataCache&) = delete;
    MetadataCache& operator=(const MetadataCache&) = delete;

    mutable QMutex               m_mutex;
    QHash<QString, MediaMetadata> m_entries;
    QSet<QString>                m_knownTags;
    bool                         m_knownTagsLoaded = false;

    // put() persists to disk ASYNCHRONOUSLY: entries queue here and a
    // QtConcurrent worker drains the queue. A metadata batch of 100 files
    // costs ~0.5 s of QSaveFile writes — done synchronously that blocked
    // the GUI thread for the whole batch (ExifToolService::handleResult
    // runs there). RAM stays the source of truth; the disk tier is only a
    // warm-start optimization, so a write lost to a crash merely re-queries
    // exiftool next session.
    QList<QPair<QString, MediaMetadata>> m_pendingWrites;
    std::atomic<bool>            m_flushRunning{false};
};
