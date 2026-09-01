#include "ThumbnailLoader.h"
#include "ThumbnailCache.h"
#include "ThumbnailDiskCache.h"
#include "ExifThumbExtractor.h"
#include "FfmpegThumbExtractor.h"
#include "SlideTrace.h"

#include <QImage>
#include <QImageReader>
#include <QMutexLocker>
#include <QRunnable>
#include <QPainter>
#include <QThread>
#include <QFileInfo>
#include <QMetaObject>
#include <QBuffer>
#include <QtGlobal>

namespace {
constexpr int kJpegQuality = 85;

// Geometry jitter (scrollbar gutter appearing, panel width, ±px window
// resizes) moves the width-derived icon size by a few px. Extracting at
// the EXACT size would key the RAM/disk caches on that jitter — a 1 px
// difference between the startup and the settled viewport re-extracts a
// whole directory. Extraction sizes are therefore quantized UP to the
// next multiple of 32: every size within a bucket shares one cache entry,
// and the display decode scales down to the exact size anyway, so quality
// is unaffected. Model-side checks keep comparing against the exact
// display key (quantized >= exact always holds).
int quantizeExtractionEdge(int px)
{
    return ((qMax(1, px) + 31) / 32) * 32;
}

QSize quantizeExtractionSize(const QSize& s)
{
    const int edge = quantizeExtractionEdge(qMax(s.width(), s.height()));
    return QSize(edge, edge);
}

// Apply EXIF orientation transform to an image
QImage applyExifOrientation(const QImage& src, int orientation)
{
    if (orientation <= 1 || orientation > 8 || src.isNull())
        return src;

    QTransform t;
    switch (orientation) {
    case 2: t.scale(-1, 1); break;
    case 3: t.rotate(180); break;
    case 4: t.scale(1, -1); break;
    case 5: t.rotate(90); t.scale(-1, 1); break;
    case 6: t.rotate(90); break;
    case 7: t.rotate(-90); t.scale(-1, 1); break;
    case 8: t.rotate(-90); break;
    }
    return src.transformed(t);
}

} // namespace

class ChunkThumbnailTask : public QObject, public QRunnable
{
    Q_OBJECT
public:
    // size: EXTRACTION size (quantized); acceptKey: the exact display need —
    // an existing entry >= acceptKey is good enough to show and must count
    // as a hit even when it is below the quantized extraction size (legacy
    // exact-size entries stay valid forever, no transitional re-extraction).
    ChunkThumbnailTask(quint64 epoch,
                       QStringList paths,
                       QList<bool> isVideos,
                       const QSize& size,
                       int acceptKey,
                       ThumbnailLoader* loader,
                       bool exifOnly = false)
        : m_epoch(epoch)
        , m_paths(std::move(paths))
        , m_isVideos(std::move(isVideos))
        , m_size(size)
        , m_acceptKey(acceptKey > 0 ? acceptKey : size.width())
        , m_loader(loader)
        , m_exifOnly(exifOnly)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QStringList readyPaths;
        QStringList failedPaths;
        auto& cache = ThumbnailCache::instance();

        for (int i = 0; i < m_paths.size(); ++i) {
            // A cancelled epoch stops FUTURE work — results already produced
            // still get reported below: the items are in the RAM cache, and
            // silently dropping the notification leaves the model unaware
            // of them (visible cells then stay white until some later
            // request happens to touch the same paths — user-visible as
            // stragglers filling in ~100 ms late).
            if (!m_loader->isEpochCurrent(m_epoch))
                break;

            const QString& path = m_paths.at(i);
            const bool isVideo = m_isVideos.at(i);

            if (m_exifOnly) {
                // EXIF prefetch: extract embedded thumbnail (JPEG only)
                const QString pendingKey = path + QLatin1String("|exif");

                if (cache.hasSmall(path)) {
                    m_loader->removePending(pendingKey);
                    readyPaths.append(path);
                    continue;
                }

                // Disk tier: reuse thumbs persisted in a previous session
                {
                    ThumbnailDiskCache::Entry de = ThumbnailDiskCache::instance().read(path);
                    TRACE_SLIDE("loader exif disk %s small=%d large=%d(key %d)",
                                qUtf8Printable(QFileInfo(path).fileName()),
                                !de.smallThumb.isEmpty(), !de.largeThumb.isEmpty(),
                                de.largeSizeKey);
                    if (!de.smallThumb.isEmpty() || !de.largeThumb.isEmpty()) {
                        if (de.oriented.isValid() && !de.oriented.isEmpty())
                            cache.setOrientedSize(path, de.oriented.width(), de.oriented.height());
                        if (!de.smallThumb.isEmpty())
                            cache.putSmall(path, de.smallThumb);
                        if (!de.largeThumb.isEmpty())
                            cache.putLarge(path, de.largeThumb, de.largeSizeKey);
                        m_loader->removePending(pendingKey);
                        readyPaths.append(path);
                        continue;
                    }
                }

                if (!isVideo) {
                    const qint64 t0 = slideTraceMs();
                    ExifThumbResult exif = extractExifThumbnail(path);
                    TRACE_SLIDE("loader exif extract %s ok=%d dur=%lldms",
                                qUtf8Printable(QFileInfo(path).fileName()),
                                !exif.data.isEmpty(),
                                (long long)(slideTraceMs() - t0));
                    if (!exif.data.isEmpty()) {
                        QImage img;
                        img.loadFromData(exif.data, "JPEG");
                        if (!img.isNull()) {
                            QSize actualSize(exif.imageWidth, exif.imageHeight);
                            bool hasActualSize = actualSize.isValid()
                                                     && actualSize.width() > 0
                                                     && actualSize.height() > 0;

                            // Fallback: if SOF parser missed the dimensions (e.g.
                            // large XMP/ICC segments push SOF beyond 64 KB),
                            // use QImageReader which only reads headers.
                            if (!hasActualSize) {
                                QImageReader sizeReader(path);
                                const QSize sz = sizeReader.size();
                                if (sz.isValid() && sz.width() > 0 && sz.height() > 0) {
                                    actualSize = sz;
                                    hasActualSize = true;
                                    // QImageReader returns pre-orientation dims
                                    // (same as SOF), so orientation handling below
                                    // stays correct.
                                }
                            }

                            if (hasActualSize) {
                                const qreal targetAr = qreal(actualSize.width()) / actualSize.height();
                                const qreal thumbAr  = qreal(img.width()) / img.height();
                                if (qAbs(targetAr - thumbAr) / qMax(targetAr, thumbAr) > 0.02) {
                                    int cw = img.width(), ch = img.height();
                                    if (thumbAr > targetAr)
                                        cw = qRound(img.height() * targetAr);
                                    else
                                        ch = qRound(img.width() / targetAr);
                                    img = img.copy((img.width() - cw) / 2,
                                                   (img.height() - ch) / 2, cw, ch);
                                }
                            }
                            img = applyExifOrientation(img, exif.orientation);

                            // Compute oriented actual dimensions and store in cache
                            QSize orientedActual;
                            if (hasActualSize) {
                                orientedActual = actualSize;
                                if (exif.orientation >= 5 && exif.orientation <= 8)
                                    orientedActual.transpose();
                                cache.setOrientedSize(path, orientedActual.width(), orientedActual.height());
                            }

                            // ALWAYS scale to target size -- even when actual dims
                            // are unknown, use the (cropped+oriented) thumb's own
                            // aspect ratio so the cached JPEG fits in m_size.
                            if (hasActualSize && orientedActual.isValid()) {
                                const QSize targetDims = orientedActual.scaled(m_size, Qt::KeepAspectRatio);
                                if (targetDims.isValid() && !targetDims.isEmpty())
                                    img = img.scaled(targetDims, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                            } else {
                                // No actual dims: scale using thumb's own aspect ratio
                                const QSize targetDims = img.size().scaled(m_size, Qt::KeepAspectRatio);
                                if (targetDims.isValid() && !targetDims.isEmpty())
                                    img = img.scaled(targetDims, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                            }

                            QByteArray jpegData;
                            QBuffer buffer(&jpegData);
                            buffer.open(QIODevice::WriteOnly);
                            img.save(&buffer, "JPEG", kJpegQuality);
                            cache.putSmall(path, jpegData);
                            ThumbnailDiskCache::instance().writeSmall(path, jpegData, orientedActual);
                            readyPaths.append(path);
                        }
                    }
                }

                m_loader->removePending(pendingKey);
            } else {
                // Full decode at target size
                const int sizeKey = m_size.width();
                const QString pendingKey = path + QLatin1Char('|') + QString::number(sizeKey);

                if (cache.hasLarge(path) && cache.largeSize(path) >= m_acceptKey) {
                    m_loader->removePending(pendingKey);
                    readyPaths.append(path);
                    continue;
                }

                // Disk tier: only a persisted large thumb at sufficient size
                // avoids the full decode
                {
                    ThumbnailDiskCache::Entry de = ThumbnailDiskCache::instance().read(path);
                    TRACE_SLIDE("loader full disk %s large=%d(key %d) req=%d accept=%d %s",
                                qUtf8Printable(QFileInfo(path).fileName()),
                                !de.largeThumb.isEmpty(), de.largeSizeKey, sizeKey,
                                m_acceptKey,
                                (!de.largeThumb.isEmpty() && de.largeSizeKey >= m_acceptKey)
                                    ? "HIT" : "MISS");
                    if (!de.largeThumb.isEmpty() && de.largeSizeKey >= m_acceptKey) {
                        if (de.oriented.isValid() && !de.oriented.isEmpty())
                            cache.setOrientedSize(path, de.oriented.width(), de.oriented.height());
                        cache.putLarge(path, de.largeThumb, de.largeSizeKey);
                        m_loader->removePending(pendingKey);
                        readyPaths.append(path);
                        continue;
                    }
                    // Large miss, but a persisted small (or a too-small large)
                    // still gives an INSTANT preview while the full extraction
                    // runs — matters most for videos, whose extraction takes
                    // 100s of ms and which have no EXIF-thumb stage.
                    if (!cache.has(path)) {
                        if (de.oriented.isValid() && !de.oriented.isEmpty())
                            cache.setOrientedSize(path, de.oriented.width(), de.oriented.height());
                        if (!de.smallThumb.isEmpty())
                            cache.putSmall(path, de.smallThumb);
                        else if (!de.largeThumb.isEmpty())
                            cache.putLarge(path, de.largeThumb, de.largeSizeKey);
                        if (!de.smallThumb.isEmpty() || !de.largeThumb.isEmpty())
                            m_loader->notifyBatchReady({path});
                    }
                }

                QImage resultImg;
                QSize orientedFull; // oriented full-image dimensions
                const qint64 t0 = slideTraceMs();
                if (isVideo) {
                    resultImg = loadVideoThumb(path);
                } else {
                    resultImg = loadImageThumb(path, &orientedFull);
                }
                TRACE_SLIDE("loader full extract %s video=%d ok=%d key=%d dur=%lldms",
                            qUtf8Printable(QFileInfo(path).fileName()), isVideo,
                            !resultImg.isNull(), sizeKey,
                            (long long)(slideTraceMs() - t0));
                if (!resultImg.isNull()) {
                    // Store oriented full-image dimensions for consistent sizing
                    if (orientedFull.isValid() && orientedFull.width() > 0 && orientedFull.height() > 0)
                        cache.setOrientedSize(path, orientedFull.width(), orientedFull.height());

                    QByteArray jpegData;
                    QBuffer buffer(&jpegData);
                    buffer.open(QIODevice::WriteOnly);
                    resultImg.save(&buffer, "JPEG", kJpegQuality);
                    cache.putLarge(path, jpegData, sizeKey);
                    ThumbnailDiskCache::instance().writeLarge(path, jpegData, sizeKey, orientedFull);

                    // Videos have no EXIF-thumb stage — derive the small tier
                    // from this decode so future sessions/zooms get an instant
                    // preview even when the large entry misses the size check.
                    if (isVideo && !cache.hasSmall(path)) {
                        QImage smallImg = resultImg;
                        if (smallImg.width() > 256 || smallImg.height() > 256)
                            smallImg = smallImg.scaled(256, 256, Qt::KeepAspectRatio,
                                                       Qt::SmoothTransformation);
                        QByteArray smallJpeg;
                        QBuffer smallBuffer(&smallJpeg);
                        smallBuffer.open(QIODevice::WriteOnly);
                        smallImg.save(&smallBuffer, "JPEG", kJpegQuality);
                        cache.putSmall(path, smallJpeg);
                        ThumbnailDiskCache::instance().writeSmall(path, smallJpeg, orientedFull);
                    }
                    readyPaths.append(path);
                } else {
                    failedPaths.append(path);
                }

                m_loader->removePending(pendingKey);
            }
        }

        // Deliver even under a stale epoch (see the break above). Stale-path
        // notifications are harmless: receivers re-check the caches, and
        // paths from an unloaded directory resolve to row -1.
        if (!readyPaths.isEmpty())
            m_loader->notifyBatchReady(readyPaths);
        if (!failedPaths.isEmpty())
            m_loader->notifyBatchFailed(failedPaths);
    }

private:
    QImage loadImageThumb(const QString& path, QSize* orientedFull = nullptr) const
    {
        QImageReader reader(path);
        reader.setAutoTransform(true);

        const QSize srcSize = reader.size();
        if (srcSize.isValid() && srcSize.width() > 0 && srcSize.height() > 0) {
            // Compute oriented full-image dimensions.
            // QImageReader::size() returns pre-transform dims. We need to
            // account for EXIF orientation (same transform as autoTransform).
            if (orientedFull) {
                QSize oriented = srcSize;
                const int orient = reader.transformation();
                // Qt::TransformationRotate90 (and combinations) swap width/height
                if (orient & QImageIOHandler::TransformationRotate90)
                    oriented.transpose();
                *orientedFull = oriented;
            }

            QSize decodeSize = srcSize;
            decodeSize.scale(m_size, Qt::KeepAspectRatio);
            if (decodeSize.isValid())
                reader.setScaledSize(decodeSize);
        }

        QImage img = reader.read();
        if (img.isNull())
            return {};

        const Qt::TransformationMode mode =
            (m_size.width() <= 128) ? Qt::FastTransformation : Qt::SmoothTransformation;
        return img.scaled(m_size, Qt::KeepAspectRatio, mode);
    }

    QImage loadVideoThumb(const QString& path) const
    {
        QImage img = extractVideoThumbnail(path, m_size);
        if (img.isNull())
            return {};
        return img;
    }

    quint64 m_epoch = 0;
    QStringList m_paths;
    QList<bool> m_isVideos;
    QSize m_size;
    int m_acceptKey = 0;
    ThumbnailLoader* m_loader = nullptr;
    bool m_exifOnly = false;
};

#include "ThumbnailLoader.moc"

ThumbnailLoader::ThumbnailLoader(QObject* parent) : QObject(parent)
{
    bool hasConfiguredWorkers = false;
    const int configuredWorkers = qEnvironmentVariableIntValue("BLITZVIEW_THUMB_WORKERS", &hasConfiguredWorkers);
    const int idealWorkers = qMax(1, QThread::idealThreadCount());
    m_workerCount = hasConfiguredWorkers ? configuredWorkers : idealWorkers;
    m_workerCount = qBound(1, m_workerCount, 32);

    m_pool.setMaxThreadCount(m_workerCount);
    m_maxPendingJobs = m_workerCount * 32;

    connect(this, &ThumbnailLoader::thumbnailReady,
            this, &ThumbnailLoader::onThumbnailCompleted,
            Qt::QueuedConnection);
}

ThumbnailLoader::~ThumbnailLoader()
{
    const qint64 t0 = slideTraceMs();
    cancelAll();
    m_pool.waitForDone();
    TRACE_SLIDE("shutdown loaderPool dur=%lldms", (long long)(slideTraceMs() - t0));
}

void ThumbnailLoader::setThumbnailSize(const QSize& size)
{
    m_thumbSize = quantizeExtractionSize(size);
}

int ThumbnailLoader::quantizedSizeKey(int px)
{
    return quantizeExtractionEdge(px);
}

bool ThumbnailLoader::isEpochCurrent(quint64 epoch) const
{
    return m_cancelEpoch.load(std::memory_order_relaxed) == epoch;
}

bool ThumbnailLoader::requestChunk(const QStringList& filePaths,
                                   const QList<bool>& isVideos,
                                   int priority,
                                   const QSize& size,
                                   bool forceQueue)
{
    if (filePaths.isEmpty() || filePaths.size() != isVideos.size())
        return true;

    // acceptKey: the exact display need (hit test); requestSize: the
    // quantized extraction size (what gets created on a miss).
    const int acceptKey = size.isValid() ? size.width() : m_thumbSize.width();
    const QSize requestSize = size.isValid() ? quantizeExtractionSize(size)
                                             : m_thumbSize;
    const int sizeKey = requestSize.width();

    // First pass: filter out items already cached at sufficient size
    auto& cache = ThumbnailCache::instance();
    QStringList uncachedPaths;
    QList<bool> uncachedIsVideos;
    QStringList alreadyCachedPaths;

    for (int i = 0; i < filePaths.size(); ++i) {
        const QString& fp = filePaths.at(i);
        if (cache.hasLarge(fp) && cache.largeSize(fp) >= acceptKey) {
            alreadyCachedPaths.append(fp);
        } else {
            uncachedPaths.append(fp);
            uncachedIsVideos.append(isVideos.at(i));
        }
    }

    // Notify about already-cached items so they get decoded
    if (!alreadyCachedPaths.isEmpty()) {
        QMetaObject::invokeMethod(this,
            [this, alreadyCachedPaths]() {
                emit thumbnailReady(alreadyCachedPaths);
            }, Qt::QueuedConnection);
    }

    if (uncachedPaths.isEmpty())
        return true;

    // Second pass: filter out pending items
    QStringList taskPaths;
    QList<bool> taskIsVideos;

    {
        QMutexLocker lk(&m_mutex);
        int available = forceQueue ? std::numeric_limits<int>::max()
                                   : qMax(0, m_maxPendingJobs - static_cast<int>(m_pending.size()));

        for (int i = 0; i < uncachedPaths.size(); ++i) {
            const QString key = uncachedPaths.at(i) + QLatin1Char('|') + QString::number(sizeKey);

            if (m_pending.contains(key))
                continue;

            if (!forceQueue && available <= 0)
                return false;

            m_pending.insert(key);
            taskPaths.append(uncachedPaths.at(i));
            taskIsVideos.append(uncachedIsVideos.at(i));
            if (!forceQueue)
                --available;
        }
    }

    if (taskPaths.isEmpty())
        return true;

    const int workerSlices = qMax(1, qMin(m_workerCount, taskPaths.size()));
    const int basePerSlice = taskPaths.size() / workerSlices;
    const int extra = taskPaths.size() % workerSlices;

    const quint64 epoch = m_cancelEpoch.load(std::memory_order_relaxed);

    int offset = 0;
    for (int slice = 0; slice < workerSlices; ++slice) {
        const int count = basePerSlice + (slice < extra ? 1 : 0);
        if (count <= 0)
            continue;

        auto* task = new ChunkThumbnailTask(epoch,
            taskPaths.mid(offset, count),
            taskIsVideos.mid(offset, count),
            requestSize, acceptKey, this);
        task->setAutoDelete(true);
        m_pool.start(task, priority - slice);
        offset += count;
    }

    return true;
}

bool ThumbnailLoader::requestExifPrefetch(const QStringList& filePaths,
                                          const QList<bool>& isVideos,
                                          int priority)
{
    if (filePaths.isEmpty())
        return true;

    auto& cache = ThumbnailCache::instance();

    QStringList taskPaths;
    QList<bool> taskIsVideos;
    QStringList alreadyCachedPaths;

    {
        QMutexLocker lk(&m_mutex);
        int available = qMax(0, m_maxPendingJobs - static_cast<int>(m_pending.size()));

        for (int i = 0; i < filePaths.size(); ++i) {
            const QString& fp = filePaths.at(i);
            const bool isVideo = isVideos.at(i);

            if (isVideo) continue;

            const QString key = fp + QLatin1String("|exif");
            if (m_pending.contains(key))
                continue;

            if (cache.hasSmall(fp)) {
                alreadyCachedPaths.append(fp);
                continue;
            }

            if (available <= 0)
                continue;

            m_pending.insert(key);
            taskPaths.append(fp);
            taskIsVideos.append(false);
            --available;
        }
    }

    if (!alreadyCachedPaths.isEmpty()) {
        QMetaObject::invokeMethod(this,
            [this, alreadyCachedPaths]() {
                emit thumbnailReady(alreadyCachedPaths);
            }, Qt::QueuedConnection);
    }

    if (taskPaths.isEmpty())
        return !alreadyCachedPaths.isEmpty();

    const int workerSlices = qMax(1, qMin(m_workerCount, taskPaths.size()));
    const int basePerSlice = taskPaths.size() / workerSlices;
    const int extra = taskPaths.size() % workerSlices;

    const quint64 epoch = m_cancelEpoch.load(std::memory_order_relaxed);

    int offset = 0;
    for (int slice = 0; slice < workerSlices; ++slice) {
        const int count = basePerSlice + (slice < extra ? 1 : 0);
        if (count <= 0)
            continue;

        auto* task = new ChunkThumbnailTask(epoch,
            taskPaths.mid(offset, count),
            taskIsVideos.mid(offset, count),
            m_thumbSize, 0, this, true /*exifOnly*/);
        task->setAutoDelete(true);
        m_pool.start(task, priority - slice);
        offset += count;
    }

    return true;
}

void ThumbnailLoader::cancelAll()
{
    m_cancelEpoch.fetch_add(1, std::memory_order_relaxed);
    QMutexLocker lk(&m_mutex);
    m_pending.clear();
    m_pool.clear();
}

void ThumbnailLoader::cancelPending()
{
    m_cancelEpoch.fetch_add(1, std::memory_order_relaxed);
    QMutexLocker lk(&m_mutex);
    m_pool.clear();
    m_pending.clear();
}

void ThumbnailLoader::clearQueue()
{
    QMutexLocker lk(&m_mutex);
    m_pool.clear();
    m_pending.clear();
}

void ThumbnailLoader::notifyBatchReady(const QStringList& filePaths)
{
    QMetaObject::invokeMethod(this,
        [this, filePaths]() {
            emit thumbnailReady(filePaths);
        }, Qt::QueuedConnection);
}

void ThumbnailLoader::notifyBatchFailed(const QStringList& filePaths)
{
    QMetaObject::invokeMethod(this,
        [this, filePaths]() {
            emit thumbnailFailed(filePaths);
        }, Qt::QueuedConnection);
}

void ThumbnailLoader::removePending(const QString& key)
{
    QMutexLocker lk(&m_mutex);
    m_pending.remove(key);
}

void ThumbnailLoader::onThumbnailCompleted(const QStringList& /*filePaths*/)
{
    // Pending entries are already removed by workers via removePending().
    // This slot exists for any future post-delivery bookkeeping.
}
