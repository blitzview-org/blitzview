#include "MetadataCache.h"
#include "AppSettings.h"
#include "ThumbnailDiskCache.h"
#include "SlideTrace.h"
#include "AppShutdown.h"

#include <QMutexLocker>
#include <QtConcurrentRun>

MetadataCache& MetadataCache::instance()
{
    static MetadataCache cache;
    return cache;
}

std::optional<MediaMetadata> MetadataCache::get(const QString& filePath)
{
    {
        QMutexLocker lk(&m_mutex);
        auto it = m_entries.constFind(filePath);
        if (it != m_entries.constEnd())
            return *it;
    }

    auto disk = ThumbnailDiskCache::instance().readMeta(filePath);
    if (disk) {
        QMutexLocker lk(&m_mutex);
        m_entries.insert(filePath, *disk);
        collectTags(disk->tags);
    }
    return disk;
}

std::optional<MediaMetadata> MetadataCache::peek(const QString& filePath) const
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.constFind(filePath);
    if (it != m_entries.constEnd())
        return *it;
    return std::nullopt;
}

void MetadataCache::put(const QString& filePath, const MediaMetadata& meta)
{
    {
        QMutexLocker lk(&m_mutex);
        m_entries.insert(filePath, meta);
        collectTags(meta.tags);
        m_pendingWrites.append({filePath, meta});
    }
    scheduleFlush();
}

// Drain m_pendingWrites on a pool thread (see the member comment — the
// synchronous per-file QSaveFile writes blocked the GUI thread for ~0.5 s
// per exiftool batch). The emptiness check and the running-flag reset
// happen under the same lock as put()'s append, so no entry can slip
// between "queue looks empty" and "worker exits".
void MetadataCache::scheduleFlush()
{
    if (m_flushRunning.exchange(true))
        return;

    auto future = QtConcurrent::run([this]() {
        for (;;) {
            QList<QPair<QString, MediaMetadata>> batch;
            {
                QMutexLocker lk(&m_mutex);
                if (m_pendingWrites.isEmpty() || appShuttingDown()) {
                    m_flushRunning.store(false);
                    return;
                }
                batch = std::move(m_pendingWrites);
                m_pendingWrites.clear();
            }
            const qint64 t0 = slideTraceMs();
            int written = 0;
            for (const auto& [path, meta] : std::as_const(batch)) {
                // Quitting: drop the rest of the queue instead of holding the
                // process open — these entries are re-read on the next start.
                if (appShuttingDown())
                    break;
                ThumbnailDiskCache::instance().writeMeta(path, meta);
                ++written;
            }
            TRACE_SLIDE("meta flush n=%d/%d dur=%lldms", written,
                        int(batch.size()), (long long)(slideTraceMs() - t0));
        }
    });
    Q_UNUSED(future);
}

void MetadataCache::remove(const QStringList& filePaths)
{
    QMutexLocker lk(&m_mutex);
    for (const QString& fp : filePaths)
        m_entries.remove(fp);

    // Also drop queued disk writes for these paths: remove() runs after a
    // metadata edit changed the file's mtime — a late flush would persist
    // the PRE-edit values under the POST-edit disk key.
    const QSet<QString> removed(filePaths.cbegin(), filePaths.cend());
    m_pendingWrites.removeIf([&removed](const QPair<QString, MediaMetadata>& p) {
        return removed.contains(p.first);
    });
}

QStringList MetadataCache::knownTags() const
{
    QMutexLocker lk(&m_mutex);
    if (!m_knownTagsLoaded) {
        auto* self = const_cast<MetadataCache*>(this);
        const QStringList stored = AppSettings::knownTags();
        for (const QString& t : stored)
            self->m_knownTags.insert(t);
        self->m_knownTagsLoaded = true;
    }
    QStringList out(m_knownTags.cbegin(), m_knownTags.cend());
    out.sort(Qt::CaseInsensitive);
    return out;
}

// Caller holds m_mutex
void MetadataCache::collectTags(const QStringList& tags)
{
    if (tags.isEmpty())
        return;
    if (!m_knownTagsLoaded) {
        const QStringList stored = AppSettings::knownTags();
        for (const QString& t : stored)
            m_knownTags.insert(t);
        m_knownTagsLoaded = true;
    }
    bool changed = false;
    for (const QString& t : tags)
        if (!m_knownTags.contains(t)) {
            m_knownTags.insert(t);
            changed = true;
        }
    if (changed)
        AppSettings::setKnownTags(QStringList(m_knownTags.cbegin(), m_knownTags.cend()));
}
