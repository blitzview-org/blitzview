#include "ThumbnailDiskCache.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include "SlideTrace.h"
#include "AppShutdown.h"
#include <QtConcurrentRun>

#include <algorithm>

namespace {

constexpr quint32 kMagic   = 0x42565443; // "BVTC"
// v2: video thumbnails respect display-matrix rotation — v1 entries for
// portrait videos are stored unrotated and must be treated as misses
constexpr quint16 kVersion = 2;

// Metadata entries (.bvm) — independent format and version
// v3: video taken is the capture-site wall clock (Keys:AndroidTimeZone /
// Keys:CreationDate) — older entries hold viewer-local instants
constexpr quint32 kMetaMagic   = 0x4256544D; // "BVTM"
constexpr quint16 kMetaVersion = 3;

// Trigger a trim after this many bytes have been written since the last one.
constexpr qint64 kTrimCheckInterval = 64 * 1024 * 1024;

// Refresh the entry's mtime so the LRU trim sees it as recently used.
void touchFile(const QString& path)
{
    QFile f(path);
    f.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
}

// Header-only read: the stored sizeKey of a valid entry, or -1.
int entrySizeKey(const QString& filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return -1;

    QDataStream ds(&f);
    quint32 magic = 0;
    quint16 version = 0;
    qint32 w = 0, h = 0, sk = 0;
    ds >> magic >> version >> w >> h >> sk;
    if (ds.status() != QDataStream::Ok || magic != kMagic || version != kVersion)
        return -1;
    return sk;
}

bool readEntryFile(const QString& filePath, QByteArray& jpeg, int& sizeKey, QSize& oriented)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    QDataStream ds(&f);
    quint32 magic = 0;
    quint16 version = 0;
    qint32 w = 0, h = 0, sk = 0;
    ds >> magic >> version >> w >> h >> sk >> jpeg;
    if (ds.status() != QDataStream::Ok || magic != kMagic || version != kVersion
        || jpeg.isEmpty()) {
        jpeg.clear();
        return false;
    }
    sizeKey = sk;
    if (w > 0 && h > 0)
        oriented = QSize(w, h);
    return true;
}

} // namespace

ThumbnailDiskCache& ThumbnailDiskCache::instance()
{
    static ThumbnailDiskCache cache;
    return cache;
}

ThumbnailDiskCache::ThumbnailDiskCache()
{
    m_dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
          + QLatin1String("/thumbnails");
}

void ThumbnailDiskCache::setEnabled(bool enabled)
{
    m_enabled.store(enabled, std::memory_order_relaxed);
}

bool ThumbnailDiskCache::isEnabled() const
{
    return m_enabled.load(std::memory_order_relaxed);
}

void ThumbnailDiskCache::setMaxBytes(qint64 bytes)
{
    m_maxBytes.store(bytes, std::memory_order_relaxed);
}

qint64 ThumbnailDiskCache::maxBytes() const
{
    return m_maxBytes.load(std::memory_order_relaxed);
}

QString ThumbnailDiskCache::entryBase(const QString& mediaPath) const
{
    if (!isEnabled())
        return {};

    const QFileInfo fi(mediaPath);
    if (!fi.exists())
        return {};

    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(mediaPath.toUtf8());
    hash.addData(QByteArrayView("\n"));
    hash.addData(QByteArray::number(fi.lastModified().toMSecsSinceEpoch()));
    hash.addData(QByteArrayView("\n"));
    hash.addData(QByteArray::number(fi.size()));
    const QString hex = QString::fromLatin1(hash.result().toHex());

    return m_dir + QLatin1Char('/') + hex.left(2) + QLatin1Char('/') + hex;
}

ThumbnailDiskCache::Entry ThumbnailDiskCache::read(const QString& mediaPath)
{
    Entry e;
    const QString base = entryBase(mediaPath);
    if (base.isEmpty())
        return e;

    int dummyKey = 0;
    const QString smallPath = base + QLatin1String(".bvs");
    const QString largePath = base + QLatin1String(".bvl");

    if (readEntryFile(smallPath, e.smallThumb, dummyKey, e.oriented))
        touchFile(smallPath);
    if (readEntryFile(largePath, e.largeThumb, e.largeSizeKey, e.oriented))
        touchFile(largePath);

    return e;
}

void ThumbnailDiskCache::writeFile(const QString& filePath, const QByteArray& jpeg,
                                   int sizeKey, const QSize& oriented)
{
    QDir().mkpath(QFileInfo(filePath).absolutePath());

    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly))
        return;

    QDataStream ds(&f);
    ds << kMagic << kVersion
       << qint32(oriented.isValid() ? oriented.width() : 0)
       << qint32(oriented.isValid() ? oriented.height() : 0)
       << qint32(sizeKey)
       << jpeg;
    if (!f.commit())
        return;

    const qint64 written = m_bytesSinceTrim.fetch_add(jpeg.size(), std::memory_order_relaxed)
                         + jpeg.size();
    if (written >= kTrimCheckInterval) {
        m_bytesSinceTrim.store(0, std::memory_order_relaxed);
        trimIfNeeded();
    }
}

void ThumbnailDiskCache::writeSmall(const QString& mediaPath, const QByteArray& jpeg,
                                    const QSize& oriented)
{
    if (jpeg.isEmpty())
        return;
    const QString base = entryBase(mediaPath);
    if (!base.isEmpty())
        writeFile(base + QLatin1String(".bvs"), jpeg, 0, oriented);
}

void ThumbnailDiskCache::writeLarge(const QString& mediaPath, const QByteArray& jpeg,
                                    int sizeKey, const QSize& oriented)
{
    if (jpeg.isEmpty())
        return;
    const QString base = entryBase(mediaPath);
    if (base.isEmpty())
        return;

    // Never downgrade (mirrors ThumbnailCache::putLarge): a stale worker
    // from before a size change may deliver AFTER the re-extraction at the
    // new size has already been persisted — overwriting would make the
    // entry miss at the new size on every subsequent start.
    const QString filePath = base + QLatin1String(".bvl");
    if (entrySizeKey(filePath) >= sizeKey) {
        touchFile(filePath); // still a use — keep it out of the LRU trim
        return;
    }
    writeFile(filePath, jpeg, sizeKey, oriented);
}

std::optional<MediaMetadata> ThumbnailDiskCache::readMeta(const QString& mediaPath)
{
    const QString base = entryBase(mediaPath);
    if (base.isEmpty())
        return std::nullopt;

    const QString metaPath = base + QLatin1String(".bvm");
    QFile f(metaPath);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;

    QDataStream ds(&f);
    quint32 magic = 0;
    quint16 version = 0;
    MediaMetadata meta;
    ds >> magic >> version;
    if (magic != kMetaMagic || version != kMetaVersion)
        return std::nullopt;
    ds >> meta;
    if (ds.status() != QDataStream::Ok)
        return std::nullopt;

    touchFile(metaPath); // LRU
    return meta;
}

void ThumbnailDiskCache::writeMeta(const QString& mediaPath, const MediaMetadata& meta)
{
    const QString base = entryBase(mediaPath);
    if (base.isEmpty())
        return;

    QDir().mkpath(QFileInfo(base).absolutePath());
    QSaveFile f(base + QLatin1String(".bvm"));
    if (!f.open(QIODevice::WriteOnly))
        return;

    QDataStream ds(&f);
    ds << kMetaMagic << kMetaVersion << meta;
    f.commit();
}

qint64 ThumbnailDiskCache::computeCurrentBytes() const
{
    qint64 total = 0;
    QDirIterator it(m_dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

void ThumbnailDiskCache::clear()
{
    QDir dir(m_dir);
    if (dir.exists())
        dir.removeRecursively();
}

void ThumbnailDiskCache::trimIfNeeded()
{
    if (!isEnabled() || maxBytes() <= 0)
        return;
    if (m_trimRunning.exchange(true))
        return;

    auto future = QtConcurrent::run([this]() {
        trimNow();
        m_trimRunning.store(false, std::memory_order_relaxed);
    });
    Q_UNUSED(future);
}

void ThumbnailDiskCache::trimNow()
{
    const qint64 max = maxBytes();
    if (max <= 0)
        return;
    const qint64 tTrim = slideTraceMs();

    struct FileRec {
        QString  path;
        qint64   size;
        QDateTime lastUsed;
    };
    QList<FileRec> files;
    qint64 total = 0;

    QDirIterator it(m_dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        // Walking a large cache directory takes seconds; the process cannot
        // exit while this runs (global pool), so abandon it on quit.
        if (appShuttingDown()) {
            TRACE_SLIDE("disk trim aborted (shutdown) after %d files",
                        int(files.size()));
            return;
        }
        const QFileInfo fi = it.fileInfo();
        files.append({fi.absoluteFilePath(), fi.size(), fi.lastModified()});
        total += fi.size();
    }

    if (total <= max) {
        TRACE_SLIDE("disk trim scan files=%d dur=%lldms (no trim)",
                    int(files.size()), (long long)(slideTraceMs() - tTrim));
        return;
    }

    // LRU: remove least-recently-used entries until at 90% of the limit
    // (hysteresis, same rationale as ThumbnailCache::evict).
    std::sort(files.begin(), files.end(), [](const FileRec& a, const FileRec& b) {
        return a.lastUsed < b.lastUsed;
    });

    const qint64 target = max * 9 / 10;
    int removed = 0;
    for (const FileRec& f : files) {
        if (total <= target || appShuttingDown())
            break;
        if (QFile::remove(f.path)) {
            total -= f.size;
            ++removed;
        }
    }
    TRACE_SLIDE("disk trim files=%d removed=%d dur=%lldms", int(files.size()),
                removed, (long long)(slideTraceMs() - tTrim));
}
