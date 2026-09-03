#include "MediaModel.h"
#include "ThumbnailLoader.h"
#include "ThumbnailCache.h"
#include "ThumbnailDiskCache.h"
#include "ExifToolService.h"
#include "MetadataCache.h"
#include "SlideTrace.h"

#include <QBuffer>
#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QGuiApplication>
#include <QSize>
#include <QIcon>
#include <QImageReader>
#include <QLocale>
#include <QMimeData>
#include <QSet>
#include <QUrl>
#include <QImage>
#include <QImageReader>
#include <QRunnable>
#include <QThread>
#include <algorithm>
#include <limits>
#include <QtConcurrentRun>
#include <QElapsedTimer>

static const QSet<QString> VIDEO_EXT = {
    "mp4","mkv","avi","mov","webm"
};

// Recognised image extensions are whatever the Qt image plugins present at
// runtime can actually decode -- QImageReader is asked instead of a list being
// maintained here. Installing more plugins (kimageformats and friends) widens
// the coverage without a rebuild; a system without them degrades silently to
// Qt's built-in formats.
//
// supportedImageFormats() returns format identifiers, not the full set of
// spellings a file may carry, so a few common suffix aliases are added.
//
// Initialised on first use rather than at namespace scope: the plugin lookup
// needs a live QCoreApplication for its plugin paths. Function-local statics
// are thread-safe, and the scan runs on a worker thread.
static const QSet<QString>& imageExtensions()
{
    static const QSet<QString> exts = [] {
        QSet<QString> s;
        const QList<QByteArray> formats = QImageReader::supportedImageFormats();
        for (const QByteArray& f : formats)
            s.insert(QString::fromLatin1(f).toLower());
        s += QSet<QString>{ "jpe", "jfi" };
        return s;
    }();
    return exts;
}

static QString formatSize(qint64 bytes)
{
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024*1024) return QString("%1 KB").arg(bytes/1024);
    if (bytes < 1024LL*1024*1024) return QString("%1 MB").arg(bytes/(1024*1024));
    return QString("%1 GB").arg(bytes/(1024LL*1024*1024));
}

static QString formatDuration(qint64 ms)
{
    if (ms <= 0) return QString();
    qint64 s  = ms / 1000;
    qint64 m  = s / 60;
    qint64 h  = m / 60;
    s %= 60; m %= 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m,2,10,QLatin1Char('0')).arg(s,2,10,QLatin1Char('0'));
    return QString("%1:%2").arg(m).arg(s,2,10,QLatin1Char('0'));
}

MediaModel::MediaModel(QObject* parent) : QAbstractTableModel(parent)
{
    // Decode pool: fewer threads than the loader pool — decodes are short
    // (ms range) and must not starve the extraction workers.
    m_decodePool.setMaxThreadCount(qBound(2, QThread::idealThreadCount() / 2, 8));

    m_loader = new ThumbnailLoader(this);
    m_loader->setThumbnailSize(m_thumbSize);
    connect(m_loader, &ThumbnailLoader::thumbnailReady,
            this,     &MediaModel::onThumbnailReady);
    connect(m_loader, &ThumbnailLoader::thumbnailFailed,
            this,     &MediaModel::onThumbnailFailed);
    connect(&ExifToolService::instance(), &ExifToolService::metadataReady,
            this,     &MediaModel::onMetadataReady);

    m_scrollDebounce = new QTimer(this);
    m_scrollDebounce->setSingleShot(true);
    m_scrollDebounce->setInterval(60);
    connect(m_scrollDebounce, &QTimer::timeout,
            this,             &MediaModel::onScrollDebounced);

    m_backgroundPrefetch = new QTimer(this);
    m_backgroundPrefetch->setSingleShot(false);
    m_backgroundPrefetch->setInterval(100);
    connect(m_backgroundPrefetch, &QTimer::timeout,
            this,             &MediaModel::onBackgroundPrefetch);

    // Ready notifications arrive one per item from the "instant preview"
    // promotion inside the loader workers — hundreds per second on large
    // libraries. The per-notification bookkeeping stays inline (cheap), but
    // the expensive tail (decode requests, visible-range re-evaluation,
    // cache bounding) is coalesced into one pass per tick.
    m_readyCoalesce = new QTimer(this);
    m_readyCoalesce->setSingleShot(true);
    m_readyCoalesce->setInterval(40);
    connect(m_readyCoalesce, &QTimer::timeout,
            this,            &MediaModel::flushThumbnailReady);

    // Directory watches are registered in time-boxed slices — see
    // rebuildDirectoryWatches for why they cannot go in one call.
    m_watchAddTimer = new QTimer(this);
    m_watchAddTimer->setSingleShot(false);
    m_watchAddTimer->setInterval(0);
    connect(m_watchAddTimer, &QTimer::timeout,
            this,            &MediaModel::addPendingWatches);

    // Tag filters depend on asynchronously arriving metadata — re-evaluate
    // shortly after new batches land instead of resetting per batch
    m_refilterDebounce = new QTimer(this);
    m_refilterDebounce->setSingleShot(true);
    m_refilterDebounce->setInterval(300);
    connect(m_refilterDebounce, &QTimer::timeout, this, [this]() {
        if (!m_filterTags.isEmpty())
            applyFilter();
    });

    // Sync mode: directoryChanged fires on add/remove/rename within a
    // watched dir; fileChanged fires for the (small, viewport-bounded) set
    // of currently visible files. Both are coalesced into one debounced
    // rescan — see onFsPathChanged / m_syncDebounce.
    m_fsWatcher = new QFileSystemWatcher(this);
    connect(m_fsWatcher, &QFileSystemWatcher::directoryChanged,
            this,        &MediaModel::onFsPathChanged);
    connect(m_fsWatcher, &QFileSystemWatcher::fileChanged,
            this,        &MediaModel::onFsPathChanged);

    m_syncDebounce = new QTimer(this);
    m_syncDebounce->setSingleShot(true);
    m_syncDebounce->setInterval(1000);
    connect(m_syncDebounce, &QTimer::timeout, this, [this]() {
        if (!m_syncEnabled)
            return;
        if (m_lastRecursiveRoots.isEmpty() && m_lastIndividualRoots.isEmpty())
            return;
        TRACE_SLIDE("sync reload");
        loadDirectories(m_lastRecursiveRoots, m_lastIndividualRoots);
    });
}

void MediaModel::cancelBackgroundWork()
{
    const qint64 t0 = slideTraceMs();
    m_scrollDebounce->stop();
    m_backgroundPrefetch->stop();
    m_readyCoalesce->stop();
    m_watchAddTimer->stop();
    m_pendingWatchAdds.clear();
    m_refilterDebounce->stop();
    m_syncDebounce->stop();
    m_readyPending.clear();

    m_loader->cancelAll();
    m_decodePool.clear();

    // The scan/prime futures are deliberately NOT cancelled: QtConcurrent::run
    // is not interruptible, so a cancel would not shorten anything — it would
    // only make the finished() handlers see an empty result store. They run
    // out on their own; their handlers no-op once the window is gone.

    TRACE_SLIDE("shutdown cancelBackgroundWork dur=%lldms",
                (long long)(slideTraceMs() - t0));
}

MediaModel::~MediaModel()
{
    const qint64 t0 = slideTraceMs();
    m_decodePool.clear();
    m_decodePool.waitForDone();
    TRACE_SLIDE("shutdown decodePool dur=%lldms", (long long)(slideTraceMs() - t0));
}

int MediaModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

int MediaModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : Col_COUNT;
}

QVariant MediaModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size())
        return {};

    const MediaItem& item = m_items.at(index.row());
    const int col = index.column();

    if (role == Qt::DecorationRole && col == Col_Thumbnail) {
        auto it = m_decodedCache.constFind(item.filePath);
        if (it != m_decodedCache.constEnd() && !it->pixmap.isNull())
            return it->pixmap;   // any size — the delegate scales stale keys
        // No decoded pixmap yet: request one asynchronously (deduped); the
        // delivery emits dataChanged and the repaint picks it up.
        if (m_loadedPaths.contains(item.filePath))
            const_cast<MediaModel*>(this)->requestDecode({item.filePath});
        return QVariant();
    }

    if (role == Qt::DisplayRole) {
        switch (col) {
        case Col_Thumbnail:    return QVariant();
        case Col_FileName:     return item.fileName;
        case Col_FileSize:     return formatSize(item.fileSize);
        case Col_ModifiedDate: return item.modifiedDate.toString(Qt::ISODate);
        case Col_CreatedDate:  return item.createdDate.toString(Qt::ISODate);
        case Col_Resolution:
            if (!item.resolution.isEmpty())
                return QString("%1 \xc3\x97 %2").arg(item.resolution.width()).arg(item.resolution.height());
            return QVariant();
        case Col_Duration:     return formatDuration(item.duration);
        case Col_FileType:     return item.fileType;
        case Col_Taken:
            return item.takenDate.isValid()
                ? item.takenDate.toString(Qt::ISODate) : QVariant();
        }
    }

    if (role == Qt::UserRole) {
        return item.filePath;
    }

    if (role == Qt::UserRole + 1) {
        return index.row();
    }

    if (role == Qt::UserRole + 3) {
        return item.isVideo;
    }

    if (role == Qt::UserRole + 4) {
        // Pre-scaled thumbnail
        auto it = m_decodedCache.constFind(item.filePath);
        if (it != m_decodedCache.constEnd() && !it->pixmap.isNull())
            return it->pixmap;   // any size — the delegate scales stale keys
        // No decoded pixmap yet: async request, see DecorationRole above
        if (m_loadedPaths.contains(item.filePath))
            const_cast<MediaModel*>(this)->requestDecode({item.filePath});
        return QVariant();
    }

    if (role == Qt::UserRole + 5) {
        auto it = m_decodedCache.constFind(item.filePath);
        if (it != m_decodedCache.constEnd())
            return it->sizeKey;
        return 0;
    }

    if (role == Qt::UserRole + 6) {
        // Tags (mirrored from MetadataCache, may lag async metadata loads)
        return item.tags;
    }

    if (role == Qt::SizeHintRole && col == Col_Thumbnail) {
        return QSize(m_thumbSize.width() + 4, m_thumbSize.height() + 4);
    }

    return {};
}

QVariant MediaModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Col_Thumbnail:    return tr("Preview");
    case Col_FileName:     return tr("Filename");
    case Col_FileSize:     return tr("Size");
    case Col_ModifiedDate: return tr("Modified");
    case Col_CreatedDate:  return tr("Created");
    case Col_Resolution:   return tr("Resolution");
    case Col_Duration:     return tr("Duration");
    case Col_FileType:     return tr("Type");
    case Col_Taken:        return tr("Taken");
    }
    return {};
}

Qt::ItemFlags MediaModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid())
        f |= Qt::ItemIsDragEnabled;
    return f;
}

QStringList MediaModel::mimeTypes() const
{
    return {QStringLiteral("text/uri-list")};
}

QMimeData* MediaModel::mimeData(const QModelIndexList& indexes) const
{
    QList<QUrl> urls;
    QSet<int> seen;
    for (const auto& idx : indexes) {
        if (!idx.isValid() || seen.contains(idx.row()))
            continue;
        seen.insert(idx.row());
        urls.append(QUrl::fromLocalFile(m_items.at(idx.row()).filePath));
    }
    if (urls.isEmpty())
        return nullptr;
    auto* mime = new QMimeData;
    mime->setUrls(urls);
    return mime;
}

Qt::DropActions MediaModel::supportedDragActions() const
{
    return Qt::CopyAction;
}

bool MediaModel::lessThan(const MediaItem& a, const MediaItem& b,
                           int column, Qt::SortOrder order)
{
    bool asc = (order == Qt::AscendingOrder);
    switch (column) {
    case MediaModel::Col_FileName:     return asc ? a.fileName     < b.fileName     : a.fileName     > b.fileName;
    case MediaModel::Col_FileSize:     return asc ? a.fileSize     < b.fileSize     : a.fileSize     > b.fileSize;
    case MediaModel::Col_ModifiedDate: return asc ? a.modifiedDate < b.modifiedDate : a.modifiedDate > b.modifiedDate;
    case MediaModel::Col_CreatedDate:  return asc ? a.createdDate  < b.createdDate  : a.createdDate  > b.createdDate;
    case MediaModel::Col_FileType:     return asc ? a.fileType     < b.fileType     : a.fileType     > b.fileType;
    case MediaModel::Col_Taken: {
        // Missing capture time sorts by its best stand-in, the mtime
        const QDateTime ta = a.takenDate.isValid() ? a.takenDate : a.modifiedDate;
        const QDateTime tb = b.takenDate.isValid() ? b.takenDate : b.modifiedDate;
        return asc ? ta < tb : ta > tb;
    }
    default: return false;
    }
}

void MediaModel::sortItemsInPlace()
{
    if (m_sortColumn < 0 || m_items.isEmpty())
        return;
    const int column = m_sortColumn;
    const Qt::SortOrder order = m_sortOrder;
    std::stable_sort(m_items.begin(), m_items.end(),
        [column, order](const MediaItem& a, const MediaItem& b) {
            return lessThan(a, b, column, order);
        });
}

void MediaModel::resetPriorityLoadingState()
{
    m_visiblePriorityActive = false;
    m_visiblePriorityFirst = -1;
    m_visiblePriorityLast = -1;
    m_visiblePrioritySizeKey = 0;
    m_loader->cancelPending();
}

void MediaModel::sort(int column, Qt::SortOrder order)
{
    m_sortColumn = column;
    m_sortOrder  = order;

    if (m_items.isEmpty()) return;

    resetPriorityLoadingState();

    beginResetModel();
    sortItemsInPlace();
    rebuildRowIndex();
    endResetModel();
}

void MediaModel::loadDirectory(const QString& path, bool recursive)
{
    loadDirectories(QStringList{path}, recursive);
}

void MediaModel::loadDirectories(const QStringList& paths, bool recursive)
{
    ++m_scanGeneration;

    // Loading DIFFERENT roots clears the filter; a rescan of the same roots
    // (after rename/metadata edit/delete) keeps it
    const QStringList loadRoots = QStringList{QStringLiteral("flat:%1").arg(recursive)} + paths;
    if (loadRoots != m_lastLoadRoots && isFiltered()) {
        m_filterPaths.clear();
        m_filterTags.clear();
        emit filterChanged();
    }
    m_lastLoadRoots = loadRoots;
    m_lastRecursiveRoots = recursive ? paths : QStringList{};
    m_lastIndividualRoots = recursive ? QStringList{} : paths;

    beginResetModel();
    m_items.clear();
    m_masterItems.clear();
    m_rowByPath.clear();
    m_loadedPaths.clear();
    m_failedPaths.clear();
    m_decodedCache.clear();
    m_displaySizes.clear();
    m_decodePending.clear();
    m_readyPending.clear();
    m_unloadedCount = 0;
    m_visiblePriorityActive = false;
    m_visiblePriorityFirst = -1;
    m_visiblePriorityLast = -1;
    m_visiblePrioritySizeKey = 0;
    m_freshnessChecked.clear();
    if (!m_watchedVisibleFiles.isEmpty()) {
        m_fsWatcher->removePaths(QStringList(m_watchedVisibleFiles.cbegin(), m_watchedVisibleFiles.cend()));
        m_watchedVisibleFiles.clear();
    }
    m_loader->cancelAll();
    m_backgroundPrefetch->stop();
    endResetModel();

    emit loadingFinished(0);
    emit scanningStarted();

    const quint64 generation = m_scanGeneration;

    if (!m_scanWatcher) {
        m_scanWatcher = new QFutureWatcher<MediaScanResult>(this);
        connect(m_scanWatcher, &QFutureWatcher<MediaScanResult>::finished,
                this, &MediaModel::onScanFinished);
    }

    if (m_scanWatcher->isRunning())
        m_scanWatcher->cancel();

    QFuture<MediaScanResult> future = QtConcurrent::run(
        [paths, recursive]() -> MediaScanResult {
            const qint64 t0 = slideTraceMs();
            MediaScanResult result;
            QSet<QString> seen;
            for (const QString& path : paths) {
                if (path.isEmpty())
                    continue;
                result.visitedDirs.insert(QFileInfo(path).absoluteFilePath());

                QDirIterator::IteratorFlags flags = recursive
                    ? QDirIterator::Subdirectories | QDirIterator::FollowSymlinks
                    : QDirIterator::NoIteratorFlags;
                QDirIterator it(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, flags);

                while (it.hasNext()) {
                    it.next();
                    const QFileInfo fi = it.fileInfo(); // reuses iterator stat data
                    if (fi.isDir()) {
                        if (recursive)
                            result.visitedDirs.insert(fi.absoluteFilePath());
                        continue;
                    }
                    QString ext = fi.suffix().toLower();

                    bool isVideo = VIDEO_EXT.contains(ext);
                    bool isImage = !isVideo && imageExtensions().contains(ext);
                    if (!isImage && !isVideo)
                        continue;

                    QString absPath = fi.absoluteFilePath();
                    if (seen.contains(absPath))
                        continue;
                    seen.insert(absPath);

                    MediaItem item;
                    item.filePath     = absPath;
                    item.fileName     = fi.fileName();
                    item.fileSize     = fi.size();
                    item.modifiedDate = fi.lastModified();
                    item.createdDate  = fi.birthTime();
                    item.fileType     = ext;
                    item.isVideo      = isVideo;
                    result.items.append(item);
                }
            }
            TRACE_SLIDE("scan walk files=%d dirs=%d dur=%lldms",
                        int(result.items.size()), int(result.visitedDirs.size()),
                        (long long)(slideTraceMs() - t0));
            return result;
        });

    m_scanWatcher->setFuture(future);
}

void MediaModel::loadDirectories(const QStringList& recursive, const QStringList& individual)
{
    ++m_scanGeneration;

    // Loading DIFFERENT roots clears the filter; a rescan of the same roots
    // (after rename/metadata edit/delete) keeps it
    const QStringList loadRoots = QStringList{QStringLiteral("r:")} + recursive
                                + QStringList{QStringLiteral("i:")} + individual;
    if (loadRoots != m_lastLoadRoots && isFiltered()) {
        m_filterPaths.clear();
        m_filterTags.clear();
        emit filterChanged();
    }
    m_lastLoadRoots = loadRoots;
    m_lastRecursiveRoots = recursive;
    m_lastIndividualRoots = individual;

    beginResetModel();
    m_items.clear();
    m_masterItems.clear();
    m_rowByPath.clear();
    m_loadedPaths.clear();
    m_failedPaths.clear();
    m_decodedCache.clear();
    m_displaySizes.clear();
    m_decodePending.clear();
    m_readyPending.clear();
    m_unloadedCount = 0;
    m_visiblePriorityActive = false;
    m_visiblePriorityFirst = -1;
    m_visiblePriorityLast = -1;
    m_visiblePrioritySizeKey = 0;
    m_freshnessChecked.clear();
    if (!m_watchedVisibleFiles.isEmpty()) {
        m_fsWatcher->removePaths(QStringList(m_watchedVisibleFiles.cbegin(), m_watchedVisibleFiles.cend()));
        m_watchedVisibleFiles.clear();
    }
    m_loader->cancelAll();
    m_backgroundPrefetch->stop();
    endResetModel();

    emit loadingFinished(0);
    emit scanningStarted();

    if (!m_scanWatcher) {
        m_scanWatcher = new QFutureWatcher<MediaScanResult>(this);
        connect(m_scanWatcher, &QFutureWatcher<MediaScanResult>::finished,
                this, &MediaModel::onScanFinished);
    }
    if (m_scanWatcher->isRunning())
        m_scanWatcher->cancel();

    QFuture<MediaScanResult> future = QtConcurrent::run(
        [recursive, individual]() -> MediaScanResult {
            const qint64 t0 = slideTraceMs();
            MediaScanResult result;
            QSet<QString> seen;

            auto scan = [&](const QString& path, bool isRecursive) {
                if (path.isEmpty()) return;
                result.visitedDirs.insert(QFileInfo(path).absoluteFilePath());

                QDirIterator::IteratorFlags flags = isRecursive
                    ? QDirIterator::Subdirectories | QDirIterator::FollowSymlinks
                    : QDirIterator::NoIteratorFlags;
                QDirIterator it(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, flags);
                while (it.hasNext()) {
                    it.next();
                    const QFileInfo fi = it.fileInfo(); // reuses iterator stat data
                    if (fi.isDir()) {
                        if (isRecursive)
                            result.visitedDirs.insert(fi.absoluteFilePath());
                        continue;
                    }
                    QString ext = fi.suffix().toLower();
                    const bool isVideo = VIDEO_EXT.contains(ext);
                    if (!isVideo && !imageExtensions().contains(ext))
                        continue;
                    QString absPath = fi.absoluteFilePath();
                    if (seen.contains(absPath)) continue;
                    seen.insert(absPath);
                    MediaItem item;
                    item.filePath     = absPath;
                    item.fileName     = fi.fileName();
                    item.fileSize     = fi.size();
                    item.modifiedDate = fi.lastModified();
                    item.createdDate  = fi.birthTime();
                    item.fileType     = ext;
                    item.isVideo      = isVideo;
                    result.items.append(item);
                }
            };

            for (const QString& path : recursive)  scan(path, true);
            for (const QString& path : individual)  scan(path, false);
            TRACE_SLIDE("scan walk files=%d dirs=%d dur=%lldms",
                        int(result.items.size()), int(result.visitedDirs.size()),
                        (long long)(slideTraceMs() - t0));
            return result;
        });

    m_scanWatcher->setFuture(future);
}

void MediaModel::onScanFinished()
{
    if (!m_scanWatcher)
        return;

    // finished() also fires for a CANCELLED future (window closing, a new
    // load starting while this scan still runs). QtConcurrent::run cannot
    // actually be interrupted, so the cancel only marks the future — its
    // result store stays EMPTY and result() would dereference null.
    if (m_scanWatcher->isCanceled() || m_scanWatcher->future().resultCount() == 0)
        return;

    MediaScanResult result = m_scanWatcher->result();
    const qint64 tApply = slideTraceMs();

    resetPriorityLoadingState();

    beginResetModel();
    m_masterItems = std::move(result.items);

    // Check ThumbnailCache over the FULL master list (reuse!)
    auto& cache = ThumbnailCache::instance();
    m_loadedPaths.clear();
    m_decodedCache.clear();
    for (const MediaItem& item : std::as_const(m_masterItems)) {
        if (cache.has(item.filePath)) {
            m_loadedPaths.insert(item.filePath);
            cache.touch(item.filePath); // mark as current generation
        }
    }

    // A surviving filter (same-roots rescan) applies to the fresh scan
    m_items.clear();
    m_unloadedCount = 0;
    for (const MediaItem& item : std::as_const(m_masterItems)) {
        if (!matchesFilter(item))
            continue;
        m_items.append(item);
        if (!m_loadedPaths.contains(item.filePath))
            ++m_unloadedCount;
    }
    sortItemsInPlace();
    rebuildRowIndex();
    endResetModel();
    TRACE_SLIDE("scan apply n=%d dur=%lldms", int(m_items.size()),
                (long long)(slideTraceMs() - tApply));

    emit scanningFinished();
    emit loadingFinished(m_items.size());

    // Evict stale entries from ThumbnailCache
    cache.evict();

    // Sync mode: watch every directory the scan just walked (add/remove/
    // rename detection). A no-op (empty visitedDirs) when sync is off —
    // rebuildDirectoryWatches still clears any watches left from before.
    const qint64 tWatch = slideTraceMs();
    rebuildDirectoryWatches(result.visitedDirs);
    updateVisibleFileWatches();
    TRACE_SLIDE("scan watches n=%d dur=%lldms", int(m_watchedDirs.size()),
                (long long)(slideTraceMs() - tWatch));

    if (!m_items.isEmpty()) {
        m_backgroundPrefetch->start();
        primeMetadataAsync();
    }
}

void MediaModel::filterToPaths(const QStringList& paths)
{
    // The tag filter stays: criteria combine as an intersection. The
    // selection this comes from is already a subset of the displayed
    // (possibly tag-filtered) list, so narrowing is the natural semantics.
    m_filterPaths = QSet<QString>(paths.cbegin(), paths.cend());
    applyFilter();
}

void MediaModel::filterToTags(const QStringList& tags)
{
    m_filterTags = tags;   // empty just removes the tag criterion
    applyFilter();
}

void MediaModel::clearFilter()
{
    if (!isFiltered())
        return;
    m_filterPaths.clear();
    m_filterTags.clear();
    applyFilter();
}

bool MediaModel::matchesFilter(const MediaItem& item) const
{
    if (!m_filterPaths.isEmpty() && !m_filterPaths.contains(item.filePath))
        return false;

    if (!m_filterTags.isEmpty()) {
        // Metadata arrives asynchronously — unread items count as
        // non-matching and appear via the refilter debounce once read
        const auto meta = MetadataCache::instance().peek(item.filePath);
        if (!meta)
            return false;
        bool any = false;
        for (const QString& t : m_filterTags)
            if (meta->tags.contains(t)) { any = true; break; }
        if (!any)
            return false;
    }
    return true;
}

void MediaModel::applyFilter()
{
    resetPriorityLoadingState();

    beginResetModel();
    m_items.clear();
    for (const MediaItem& item : std::as_const(m_masterItems)) {
        if (!matchesFilter(item))
            continue;
        MediaItem copy = item;
        // The master copy may predate async metadata/renames — takenDate and
        // tags are authoritative in MetadataCache
        if (auto meta = MetadataCache::instance().peek(copy.filePath)) {
            if (meta->taken.isValid())
                copy.takenDate = meta->taken;
            copy.tags = meta->tags;
        }
        m_items.append(copy);
    }
    sortItemsInPlace();
    rebuildRowIndex();

    m_unloadedCount = 0;
    for (const MediaItem& item : std::as_const(m_items))
        if (!m_loadedPaths.contains(item.filePath))
            ++m_unloadedCount;
    endResetModel();

    if (m_unloadedCount > 0 && !m_backgroundPrefetch->isActive())
        m_backgroundPrefetch->start();

    emit filterChanged();
    emit loadingFinished(m_items.size());
}

QStringList MediaModel::tagsInDirectory() const
{
    QSet<QString> tags;
    for (const MediaItem& item : std::as_const(m_masterItems)) {
        // Offer tags of the path-filtered scope (NOT tag-filtered — the
        // menu must keep showing unchecked siblings and checked entries)
        if (!m_filterPaths.isEmpty() && !m_filterPaths.contains(item.filePath))
            continue;
        if (auto meta = MetadataCache::instance().peek(item.filePath))
            for (const QString& t : meta->tags)
                tags.insert(t);
    }
    for (const QString& t : m_filterTags)
        tags.insert(t);
    QStringList out(tags.cbegin(), tags.cend());
    out.sort(Qt::CaseInsensitive);
    return out;
}

void MediaModel::primeMetadataAsync()
{
    if (!m_metaPrimeWatcher) {
        m_metaPrimeWatcher = new QFutureWatcher<QStringList>(this);
        connect(m_metaPrimeWatcher, &QFutureWatcher<QStringList>::finished,
                this, [this]() {
            // Cancelled prime (window closing / new load): no result to read
            // — same empty-result-store trap as in onScanFinished.
            if (m_metaPrimeWatcher->isCanceled()
                || m_metaPrimeWatcher->future().resultCount() == 0)
                return;

            // A new directory was loaded while priming: redo for the new list
            if (m_metaPrimeGeneration != m_scanGeneration) {
                primeMetadataAsync();
                return;
            }

            // Apply everything the disk cache had; queue the misses
            for (int i = 0; i < m_items.size(); ++i) {
                if (auto meta = MetadataCache::instance().peek(m_items.at(i).filePath)) {
                    m_items[i].takenDate = meta->taken;
                    m_items[i].tags = meta->tags;
                }
            }
            if (!m_items.isEmpty()) {
                emit dataChanged(index(0, Col_Taken),
                                 index(m_items.size() - 1, Col_Taken));
                if (m_sortColumn == Col_Taken)
                    sort(m_sortColumn, m_sortOrder);
            }
            ExifToolService::instance().request(m_metaPrimeWatcher->result());
        });
    }
    if (m_metaPrimeWatcher->isRunning())
        return;  // finished handler re-triggers if the generation moved on

    QStringList paths;
    paths.reserve(m_items.size());
    for (const MediaItem& item : std::as_const(m_items))
        paths.append(item.filePath);

    m_metaPrimeGeneration = m_scanGeneration;
    m_metaPrimeWatcher->setFuture(QtConcurrent::run(
        [paths]() -> QStringList {
            QStringList misses;
            for (const QString& fp : paths) {
                if (!MetadataCache::instance().get(fp)) // pulls disk → memory
                    misses.append(fp);
            }
            return misses;
        }));
}

void MediaModel::onMetadataReady(const QStringList& filePaths)
{
    // An active tag filter may match differently now
    if (!m_filterTags.isEmpty())
        m_refilterDebounce->start();

    QList<int> changedRows;
    for (const QString& fp : filePaths) {
        const int row = m_rowByPath.value(fp, -1);
        if (row < 0)
            continue;
        if (auto meta = MetadataCache::instance().peek(fp)) {
            m_items[row].takenDate = meta->taken;
            m_items[row].tags = meta->tags;
            changedRows.append(row);
        }
    }
    if (changedRows.isEmpty())
        return;

    const auto [minIt, maxIt] = std::minmax_element(changedRows.begin(), changedRows.end());
    emit dataChanged(index(*minIt, Col_Taken), index(*maxIt, Col_Taken));

    if (m_sortColumn == Col_Taken) {
        const qint64 t0 = slideTraceMs();
        sort(m_sortColumn, m_sortOrder);
        TRACE_SLIDE("metadata re-sort n=%d dur=%lldms", int(filePaths.size()),
                    (long long)(slideTraceMs() - t0));
    }
}

void MediaModel::rebuildRowIndex()
{
    m_rowByPath.clear();
    for (int i = 0; i < m_items.size(); ++i)
        m_rowByPath.insert(m_items.at(i).filePath, i);
}

namespace {
struct AsyncDecoded {
    QString path;
    QImage  image;
    QSize   scaledSize;
    int     sizeKey = 0;
    bool    sharp = false;
};

// Worker-side twin of decodeFromCache for items with KNOWN oriented
// dimensions (every extraction path stores them; without them the display
// size depends on UI-side fallback state — those rare items stay on the
// synchronous path to preserve the pixel-shift invariant). Touches only
// the thread-safe ThumbnailCache.
bool decodeThumbOffThread(const QString& path, int targetKey, qreal dpr, AsyncDecoded& out)
{
    auto& cache = ThumbnailCache::instance();
    const QSize oriented = cache.orientedSize(path);
    if (!oriented.isValid() || oriented.isEmpty())
        return false;

    const QSize targetBox(targetKey, targetKey);
    const QSize scaledSize = oriented.scaled(targetBox, Qt::KeepAspectRatio);
    const QSize devSize = scaledSize * dpr;

    // Same source selection as decodeFromCache: smallest source that still
    // covers the display size.
    bool sharp = false;
    QByteArray jpeg = cache.getSmall(path);
    if (!jpeg.isEmpty()) {
        QBuffer probe(&jpeg);
        probe.open(QIODevice::ReadOnly);
        QImageReader probeReader(&probe, "jpeg");
        const QSize dims = probeReader.size();
        if (dims.width() >= devSize.width() && dims.height() >= devSize.height())
            sharp = true;
        else
            jpeg.clear();
    }
    if (jpeg.isEmpty()) {
        jpeg = cache.getLarge(path);
        if (!jpeg.isEmpty()) {
            sharp = cache.largeSize(path) >= qRound(targetKey * dpr);
        } else {
            jpeg = cache.getSmall(path);
            sharp = false;
        }
    }
    if (jpeg.isEmpty())
        return false;

    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "jpeg");
    reader.setScaledSize(scaledSize * dpr);
    QImage img = reader.read();
    if (img.isNull())
        return false;
    img.setDevicePixelRatio(dpr);

    out = AsyncDecoded{path, std::move(img), scaledSize, targetKey, sharp};
    return true;
}
} // namespace

// See the m_decodePool member comment: decoding must not run on the UI
// thread. Requests are deduped by (path, sizeKey), sliced across the pool,
// and delivered back as queued batches that fill m_decodedCache and emit
// dataChanged. Until delivery the delegate scales the previous pixmap (or
// paints the placeholder for never-decoded items).
void MediaModel::requestDecode(const QStringList& paths)
{
    const int targetKey = m_thumbSize.width();
    const QString keySuffix = QLatin1Char('|') + QString::number(targetKey);

    QStringList batch;
    batch.reserve(paths.size());
    for (const QString& fp : paths) {
        const QString pendingKey = fp + keySuffix;
        if (m_decodePending.contains(pendingKey))
            continue;
        m_decodePending.insert(pendingKey);
        batch.append(fp);
    }
    if (batch.isEmpty())
        return;

    const qreal dpr = qGuiApp->devicePixelRatio();

    const int slices = qBound(1, int(batch.size() / 16) + 1,
                              m_decodePool.maxThreadCount());
    const int per = (int(batch.size()) + slices - 1) / slices;
    for (int off = 0; off < batch.size(); off += per) {
        const QStringList slice = batch.mid(off, per);
        auto* task = QRunnable::create([this, slice, targetKey, dpr]() {
            QList<AsyncDecoded> results;
            QStringList syncFallback;
            results.reserve(slice.size());
            for (const QString& fp : slice) {
                AsyncDecoded r;
                if (decodeThumbOffThread(fp, targetKey, dpr, r))
                    results.append(std::move(r));
                else
                    syncFallback.append(fp);
            }

            QMetaObject::invokeMethod(this,
                [this, results, syncFallback, targetKey]() {
                    const QString suffix = QLatin1Char('|') + QString::number(targetKey);
                    const bool current = (targetKey == m_thumbSize.width());
                    QList<int> rows;

                    for (const AsyncDecoded& r : results) {
                        m_decodePending.remove(r.path + suffix);
                        if (!current)
                            continue;   // size changed — re-requested by the new pass
                        const int row = m_rowByPath.value(r.path, -1);
                        if (row < 0)
                            continue;
                        m_displaySizes[r.path] = r.scaledSize;
                        m_decodedCache[r.path] =
                            DecodedThumb{QPixmap::fromImage(r.image), r.sizeKey, r.sharp};
                        rows.append(row);
                    }
                    for (const QString& fp : syncFallback) {
                        m_decodePending.remove(fp + suffix);
                        if (!current)
                            continue;
                        const int row = m_rowByPath.value(fp, -1);
                        if (row < 0)
                            continue;
                        decodeFromCache(fp, row);
                        rows.append(row);
                    }
                    if (rows.isEmpty())
                        return;

                    evictDecodedCache();

                    std::sort(rows.begin(), rows.end());
                    int runStart = rows.first();
                    int runEnd = runStart;
                    for (int i = 1; i < rows.size(); ++i) {
                        const int row = rows.at(i);
                        if (row == runEnd + 1) { runEnd = row; continue; }
                        emit dataChanged(index(runStart, Col_Thumbnail),
                                         index(runEnd, Col_Thumbnail),
                                         {Qt::DecorationRole, Qt::UserRole + 4, Qt::UserRole + 5});
                        runStart = runEnd = row;
                    }
                    emit dataChanged(index(runStart, Col_Thumbnail),
                                     index(runEnd, Col_Thumbnail),
                                     {Qt::DecorationRole, Qt::UserRole + 4, Qt::UserRole + 5});
                }, Qt::QueuedConnection);
        });
        m_decodePool.start(task);
    }
}

void MediaModel::decodeFromCache(const QString& filePath, int /*row*/) const
{
    auto& cache = ThumbnailCache::instance();

    const int targetKey = m_thumbSize.width();
    const QSize targetBox(targetKey, targetKey);
    const qreal dpr = qGuiApp->devicePixelRatio();

    // Pixel-shift prevention:
    // 1. Compute display size from the ORIENTED ACTUAL IMAGE DIMENSIONS stored
    //    in ThumbnailCache.  Both EXIF and full-decode workers store the same
    //    oriented dimensions, so this is always consistent.
    // 2. Fall back to m_displaySizes (persistent across decoded-cache eviction).
    // 3. Last resort: compute from the raw JPEG dimensions (header read only).
    QSize scaledSize;

    const QSize oriented = cache.orientedSize(filePath);
    if (oriented.isValid() && !oriented.isEmpty()) {
        scaledSize = oriented.scaled(targetBox, Qt::KeepAspectRatio);
    } else {
        auto dsIt = m_displaySizes.constFind(filePath);
        if (dsIt != m_displaySizes.constEnd() && dsIt->isValid()) {
            scaledSize = *dsIt;
        } else {
            QByteArray best = cache.getBest(filePath);
            if (best.isEmpty()) return;
            QBuffer probe(&best);
            probe.open(QIODevice::ReadOnly);
            QImageReader probeReader(&probe, "jpeg");
            const QSize raw = probeReader.size();
            if (!raw.isValid() || raw.isEmpty()) return;
            scaledSize = raw.scaled(targetBox, Qt::KeepAspectRatio);
        }
    }
    m_displaySizes[filePath] = scaledSize;

    // Pick the SMALLEST cached source that still covers the display size:
    // at small cell sizes, decoding the 45 px pixmap from the small tier is
    // an order of magnitude cheaper than from a 1300 px large thumb — and
    // quality is identical once the source covers the target. The decode
    // runs synchronously on the UI thread, so this choice dominates how
    // fast a cached directory fills the grid.
    const QSize devSize = scaledSize * dpr;
    bool sharp = false;
    QByteArray jpeg = cache.getSmall(filePath);
    if (!jpeg.isEmpty()) {
        QBuffer probe(&jpeg);
        probe.open(QIODevice::ReadOnly);
        QImageReader probeReader(&probe, "jpeg");
        const QSize dims = probeReader.size();
        if (dims.width() >= devSize.width() && dims.height() >= devSize.height())
            sharp = true;
        else
            jpeg.clear();   // small does not cover — try the large tier
    }
    if (jpeg.isEmpty()) {
        jpeg = cache.getLarge(filePath);
        if (!jpeg.isEmpty()) {
            // The cache stores extraction sizes in device pixels (see
            // invalidateThumbnails) — compare against the device-sized target
            sharp = cache.largeSize(filePath) >= qRound(targetKey * dpr);
        } else {
            jpeg = cache.getSmall(filePath);   // blurry preview fallback
            sharp = false;
        }
    }
    if (jpeg.isEmpty()) return;

    // Decode directly at display size: the JPEG handler uses libjpeg DCT
    // scaling, which is far cheaper than full decode + SmoothTransformation.
    // Decoding happens at DEVICE resolution and the pixmap carries the
    // screen scale — logical geometry stays scaledSize, HiDPI screens get
    // real pixels instead of an upscale at paint time.
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "jpeg");
    reader.setScaledSize(scaledSize * dpr);
    QImage img = reader.read();
    if (img.isNull()) return;
    img.setDevicePixelRatio(dpr);

    m_decodedCache[filePath] = DecodedThumb{QPixmap::fromImage(std::move(img)), targetKey, sharp};
}

void MediaModel::evictDecodedCache()
{
    if (m_decodedCache.size() <= kMaxDecodedEntries)
        return;

    const int keepFirst = qMax(0, m_visibleFirst - kDecodedMargin);
    const int keepLast = qMin(m_items.size() - 1, m_visibleLast + kDecodedMargin);

    QSet<QString> keepPaths;
    keepPaths.reserve(keepLast - keepFirst + 1);
    for (int i = keepFirst; i <= keepLast; ++i)
        keepPaths.insert(m_items.at(i).filePath);

    auto it = m_decodedCache.begin();
    while (it != m_decodedCache.end()) {
        if (!keepPaths.contains(it.key()))
            it = m_decodedCache.erase(it);
        else
            ++it;
    }
}

// Keeps the in-memory ThumbnailCache within its entry limit during a session
// (evict() alone only runs on directory switches). After LRU eviction,
// m_loadedPaths must be reconciled: evicted items are no longer decodable from
// the cache and have to count as unloaded again so the visible-priority path
// re-requests them when they scroll into view.
void MediaModel::boundThumbnailCache()
{
    auto& cache = ThumbnailCache::instance();
    if (cache.entryCount() <= cache.maxEntries())
        return;

    cache.evict();

    for (auto it = m_loadedPaths.begin(); it != m_loadedPaths.end(); ) {
        if (!cache.has(*it)) {
            it = m_loadedPaths.erase(it);
            ++m_unloadedCount;
        } else {
            ++it;
        }
    }
}

void MediaModel::onFsPathChanged(const QString& /*path*/)
{
    if (!m_syncEnabled)
        return;
    // Coalesce bursts (e.g. copying many files in one go) into one rescan.
    m_syncDebounce->start();
}

void MediaModel::setSyncModeEnabled(bool enabled)
{
    if (m_syncEnabled == enabled)
        return;
    m_syncEnabled = enabled;

    if (!enabled) {
        m_syncDebounce->stop();
        m_watchAddTimer->stop();
        m_pendingWatchAdds.clear();
        if (!m_watchedDirs.isEmpty()) {
            m_fsWatcher->removePaths(QStringList(m_watchedDirs.cbegin(), m_watchedDirs.cend()));
            m_watchedDirs.clear();
        }
        if (!m_watchedVisibleFiles.isEmpty()) {
            m_fsWatcher->removePaths(QStringList(m_watchedVisibleFiles.cbegin(), m_watchedVisibleFiles.cend()));
            m_watchedVisibleFiles.clear();
        }
        return;
    }

    // Re-enabled: catch up on anything that changed while sync was off.
    if (!m_lastRecursiveRoots.isEmpty() || !m_lastIndividualRoots.isEmpty()) {
        TRACE_SLIDE("sync reload (re-enabled)");
        loadDirectories(m_lastRecursiveRoots, m_lastIndividualRoots);
    }
}

// Re-derives the watched-directory set from the directories the most recent
// scan actually walked (see MediaScanResult::visitedDirs) — self-healing on
// every rescan, including newly appeared subdirectories.
void MediaModel::rebuildDirectoryWatches(const QSet<QString>& visitedDirs)
{
    m_pendingWatchAdds.clear();

    if (!m_syncEnabled || visitedDirs.isEmpty()) {
        if (!m_watchedDirs.isEmpty()) {
            m_fsWatcher->removePaths(QStringList(m_watchedDirs.cbegin(), m_watchedDirs.cend()));
            m_watchedDirs.clear();
        }
        m_watchAddTimer->stop();
        return;
    }

    // DIFF, never rebuild: QFileSystemWatcher::addPaths/removePaths are
    // QUADRATIC in the number of already watched paths (measured: 1.8 s each
    // for 18 000 directories). A rescan of the same roots — which is what
    // every sync reload is — sees an identical directory set, so the diff is
    // empty and costs nothing. Without it, each reload paid remove + add.
    QStringList toRemove;
    for (const QString& dir : std::as_const(m_watchedDirs))
        if (!visitedDirs.contains(dir))
            toRemove.append(dir);
    if (!toRemove.isEmpty()) {
        m_fsWatcher->removePaths(toRemove);
        for (const QString& dir : std::as_const(toRemove))
            m_watchedDirs.remove(dir);
    }

    for (const QString& dir : visitedDirs)
        if (!m_watchedDirs.contains(dir))
            m_pendingWatchAdds.append(dir);

    if (m_pendingWatchAdds.isEmpty()) {
        m_watchAddTimer->stop();
        return;
    }

    // Register in time-boxed chunks off the event loop: the first load of a
    // deeply nested library adds tens of thousands of directories, and doing
    // that in one call froze the GUI for seconds right after the grid
    // appeared (trace: "scan watches n=18181 dur=4318ms").
    m_watchAddTimer->start();
}

// One time-boxed slice of the pending watch registrations (see
// rebuildDirectoryWatches). Sync mode is simply not yet complete for the
// directories still queued — they are added within the next few 100 ms.
void MediaModel::addPendingWatches()
{
    if (m_pendingWatchAdds.isEmpty() || !m_syncEnabled) {
        m_pendingWatchAdds.clear();
        m_watchAddTimer->stop();
        return;
    }

    constexpr qint64 budgetMs = 15;
    constexpr int chunk = 128;
    QElapsedTimer budget;
    budget.start();
    int added = 0;

    while (!m_pendingWatchAdds.isEmpty() && budget.elapsed() < budgetMs) {
        const int n = qMin(chunk, int(m_pendingWatchAdds.size()));
        const QStringList slice = m_pendingWatchAdds.mid(0, n);
        m_pendingWatchAdds.remove(0, n);
        const QStringList failed = m_fsWatcher->addPaths(slice);
        const QSet<QString> failedSet(failed.cbegin(), failed.cend());
        for (const QString& dir : slice)
            if (!failedSet.contains(dir))
                m_watchedDirs.insert(dir);
        if (!failed.isEmpty())
            qWarning("MediaModel: sync mode could not watch %d directories "
                     "(OS watch-descriptor limit?)", int(failed.size()));
        added += n;
    }

    if (m_pendingWatchAdds.isEmpty()) {
        m_watchAddTimer->stop();
        TRACE_SLIDE("scan watches complete n=%d", int(m_watchedDirs.size()));
    }
}

// Bounds content-change (fileChanged) watching to the current viewport —
// scales with screen size, not library size. Directory-level watching (see
// rebuildDirectoryWatches) already covers add/remove/rename everywhere.
void MediaModel::updateVisibleFileWatches()
{
    if (!m_syncEnabled)
        return;

    QSet<QString> newVisible;
    if (m_visibleFirst <= m_visibleLast) {
        const int first = qMax(0, m_visibleFirst);
        const int last = qMin(m_visibleLast, m_items.size() - 1);
        for (int i = first; i <= last; ++i)
            newVisible.insert(m_items.at(i).filePath);
    }
    if (newVisible == m_watchedVisibleFiles)
        return;

    const QSet<QString> toRemove = m_watchedVisibleFiles - newVisible;
    const QSet<QString> toAdd = newVisible - m_watchedVisibleFiles;
    if (!toRemove.isEmpty())
        m_fsWatcher->removePaths(QStringList(toRemove.cbegin(), toRemove.cend()));
    if (!toAdd.isEmpty())
        m_fsWatcher->addPaths(QStringList(toAdd.cbegin(), toAdd.cend())); // best-effort
    m_watchedVisibleFiles = newVisible;
}

// Verifies items entering the visible range against their on-disk mtime/size
// (MediaItem's values are only as fresh as the last scan). Catches content
// changes that happened OFF-SCREEN — no directoryChanged fires for a plain
// same-name overwrite, and watching every file individually doesn't scale —
// so this lazy check-on-reveal is the only mechanism for that case, and it
// runs regardless of the sync-mode toggle (a correctness guarantee, not an
// auto-rescan feature).
void MediaModel::checkVisibleFreshness(int first, int last)
{
    if (m_items.isEmpty())
        return;
    first = qMax(0, first);
    last = qMin(last, m_items.size() - 1);

    for (int i = first; i <= last; ++i) {
        MediaItem& item = m_items[i];
        if (m_freshnessChecked.contains(item.filePath))
            continue;
        m_freshnessChecked.insert(item.filePath);

        QFileInfo fi(item.filePath);
        if (!fi.exists())
            continue; // handled by the directory watcher / next rescan
        fi.refresh();
        if (fi.size() == item.fileSize && fi.lastModified() == item.modifiedDate)
            continue;

        TRACE_SLIDE("freshness stale %s", qPrintable(item.filePath));
        item.fileSize = fi.size();
        item.modifiedDate = fi.lastModified();
        for (MediaItem& m : m_masterItems) {
            if (m.filePath == item.filePath) {
                m.fileSize = fi.size();
                m.modifiedDate = fi.lastModified();
                break;
            }
        }
        invalidateContentCache(item.filePath);
    }
}

// Drops every cache tier for a path whose CONTENT changed under an unchanged
// name (unlike renameItem, the old thumb/metadata is no longer valid) and
// immediately re-requests a fresh thumbnail.
void MediaModel::invalidateContentCache(const QString& filePath)
{
    ThumbnailCache::instance().remove(filePath);
    m_decodedCache.remove(filePath);
    m_displaySizes.remove(filePath);
    MetadataCache::instance().remove({filePath});
    m_decodePending.remove(filePath + QLatin1Char('|') + QString::number(m_thumbSize.width()));
    m_failedPaths.remove(filePath);

    if (m_loadedPaths.remove(filePath))
        ++m_unloadedCount;

    requestDecode({filePath});
    if (!m_backgroundPrefetch->isActive())
        m_backgroundPrefetch->start();

    const int row = m_rowByPath.value(filePath, -1);
    if (row >= 0)
        emit dataChanged(index(row, Col_Thumbnail), index(row, Col_Thumbnail),
                          {Qt::DecorationRole, Qt::UserRole + 4, Qt::UserRole + 5});
}

bool MediaModel::renameItem(int row, const QString& newFileName)
{
    if (row < 0 || row >= m_items.size() || newFileName.isEmpty()
        || newFileName.contains(QLatin1Char('/')))
        return false;

    MediaItem& item = m_items[row];
    const QString oldPath = item.filePath;
    const QString newPath = QFileInfo(oldPath).absolutePath()
                          + QLatin1Char('/') + newFileName;
    if (newPath == oldPath)
        return true;
    if (QFileInfo::exists(newPath))
        return false;

    // Disk-cache entries are keyed by (path, mtime, size) — capture them
    // BEFORE the rename (entryBase needs the old file to stat)
    auto& disk = ThumbnailDiskCache::instance();
    const ThumbnailDiskCache::Entry thumbs = disk.read(oldPath);
    const auto meta = MetadataCache::instance().get(oldPath);

    if (!QFile::rename(oldPath, newPath))
        return false;

    // Re-home the caches under the new path (content unchanged: no
    // re-extraction, no flicker). The old disk entries become orphans and
    // age out via the LRU trim.
    if (!thumbs.smallThumb.isEmpty())
        disk.writeSmall(newPath, thumbs.smallThumb, thumbs.oriented);
    if (!thumbs.largeThumb.isEmpty())
        disk.writeLarge(newPath, thumbs.largeThumb, thumbs.largeSizeKey, thumbs.oriented);
    if (meta)
        MetadataCache::instance().put(newPath, *meta);
    MetadataCache::instance().remove({oldPath});
    ThumbnailCache::instance().renameEntry(oldPath, newPath);

    if (m_decodedCache.contains(oldPath))
        m_decodedCache.insert(newPath, m_decodedCache.take(oldPath));
    if (m_displaySizes.contains(oldPath))
        m_displaySizes.insert(newPath, m_displaySizes.take(oldPath));
    if (m_loadedPaths.remove(oldPath))
        m_loadedPaths.insert(newPath);
    if (m_failedPaths.remove(oldPath))
        m_failedPaths.insert(newPath);

    m_rowByPath.remove(oldPath);
    m_rowByPath.insert(newPath, row);
    item.filePath = newPath;
    item.fileName = newFileName;

    // Keep the master list in sync — clearing a filter rebuilds from it
    for (MediaItem& m : m_masterItems) {
        if (m.filePath == oldPath) {
            m.filePath = newPath;
            m.fileName = newFileName;
            break;
        }
    }
    // An active path filter must follow the rename, or the renamed file
    // would silently drop out of the filtered view
    if (m_filterPaths.remove(oldPath))
        m_filterPaths.insert(newPath);

    emit dataChanged(index(row, 0), index(row, Col_COUNT - 1));
    return true;
}

void MediaModel::invalidateThumbnails(const QSize& newSize)
{
    // The target size is LOGICAL (device-independent): size keys and cell
    // geometry stay in logical pixels. Only the extraction below runs at
    // device resolution (× screen scale) so HiDPI screens get real pixels.
    // No hard upper bound on the preview size (the grid's zoom has none —
    // one column in a wide window is a huge preview); 4096 is a sanity
    // clamp against pathological viewport values only.
    const int targetEdge = qBound(32, qMax(newSize.width(), newSize.height()), 4096);
    const QSize target(targetEdge, targetEdge);
    if (target == m_thumbSize)
        return;
    TRACE_SLIDE("invalidateThumbnails %d -> %d (visible %d..%d)",
                m_thumbSize.width(), target.width(), m_visibleFirst,
                m_visibleLast);

    // ±px refits within the same quantized extraction bucket (scrollbar
    // gutter appearing, panel width) change the DISPLAY size but produce
    // IDENTICAL extraction work — cancelling and re-requesting there only
    // disrupted the in-flight queue (visible cells then finished a wave
    // later). Only a bucket change invalidates the loader state.
    const bool sameExtraction =
        ThumbnailLoader::quantizedSizeKey(m_thumbSize.width())
        == ThumbnailLoader::quantizedSizeKey(target.width());

    m_thumbSize = target;
    if (!sameExtraction) {
        m_visiblePriorityActive = false;
        m_visiblePriorityFirst = -1;
        m_visiblePriorityLast = -1;
        m_visiblePrioritySizeKey = 0;
        m_loader->cancelPending();
    }
    m_loader->setThumbnailSize(target * qGuiApp->devicePixelRatio());

    // KEEP m_decodedCache: the stale-size pixmaps are the paint fallback
    // (the delegate scales entries whose sizeKey mismatches) while the
    // async re-decode replaces them. Display sizes are size-dependent and
    // recomputed per delivery; pending decodes are for the old size.
    m_displaySizes.clear();
    m_decodePending.clear();

    if (!m_items.isEmpty()) {
        emit dataChanged(index(0, Col_Thumbnail),
                         index(m_items.size() - 1, Col_Thumbnail),
                         {Qt::DecorationRole, Qt::UserRole + 4, Qt::UserRole + 5});

        if (m_visibleFirst <= m_visibleLast) {
            setVisibleRows(m_visibleFirst, m_visibleLast);
        }

        if (!m_backgroundPrefetch->isActive()) {
            m_backgroundPrefetch->start();
        }
    }
}

void MediaModel::setVisibleRows(int first, int last)
{
    if (m_items.isEmpty()) {
        m_visibleFirst = 0;
        m_visibleLast = -1;
        m_visiblePriorityActive = false;
        m_visiblePriorityFirst = -1;
        m_visiblePriorityLast = -1;
        m_visiblePrioritySizeKey = 0;
        return;
    }

    first = qMax(0, first);
    last = qMin(last, m_items.size() - 1);
    if (first > last)
        return;

    if (first < m_visibleFirst)
        m_scrollDirection = -1;
    else if (first > m_visibleFirst)
        m_scrollDirection = +1;
    else
        m_scrollDirection = 0;

    m_visibleFirst = first;
    m_visibleLast  = last;

    // Queue visible items lacking a current-size pixmap for the ASYNC
    // decode (stale-size entries keep painting scaled until the fresh one
    // lands — the decode itself must never run on the UI thread here)
    const int targetKey = m_thumbSize.width();
    bool hasMissingVisible = false;
    QStringList decodeBatch;
    for (int i = first; i <= last; ++i) {
        const QString& fp = m_items.at(i).filePath;
        if (m_loadedPaths.contains(fp)) {
            auto it = m_decodedCache.constFind(fp);
            if (it == m_decodedCache.constEnd() || it->sizeKey != targetKey)
                decodeBatch.append(fp);
        } else if (!m_failedPaths.contains(fp)) {
            hasMissingVisible = true;
        }
    }
    requestDecode(decodeBatch);

    // The pre-decode loop is the only decoded-cache producer that can run
    // without a subsequent thumbnailReady (fully loaded directory), so the
    // cache bound must be enforced here as well.
    evictDecodedCache();

    // Also check if visible items need a larger thumbnail
    if (!hasMissingVisible) {
        auto& cache = ThumbnailCache::instance();
        for (int i = first; i <= last; ++i) {
            const QString& fp = m_items.at(i).filePath;
            if (m_failedPaths.contains(fp))
                continue;
            if (!cache.hasLarge(fp) || cache.largeSize(fp) < targetKey) {
                hasMissingVisible = true;
                break;
            }
        }
    }

    if (hasMissingVisible) {
        const int sizeKey = m_thumbSize.width();
        // Same range AND same quantized extraction bucket = the queued work
        // is already exactly right — re-issuing would only clear and
        // re-order the in-flight queue (display-size deltas are handled by
        // the async re-decode above).
        const bool samePriorityRange = m_visiblePriorityActive &&
                                       m_visiblePriorityFirst == first &&
                                       m_visiblePriorityLast == last &&
                                       ThumbnailLoader::quantizedSizeKey(m_visiblePrioritySizeKey)
                                           == ThumbnailLoader::quantizedSizeKey(sizeKey);
        if (!samePriorityRange) {
            TRACE_SLIDE("visible reload %d..%d key=%d", first, last, sizeKey);
            m_visiblePriorityActive = true;
            m_visiblePriorityFirst = first;
            m_visiblePriorityLast = last;
            m_visiblePrioritySizeKey = sizeKey;
            m_loader->clearQueue();
            requestVisibleThumbnails(first, last, true);
        }
        return;
    }

    m_visiblePriorityActive = false;
    m_visiblePriorityFirst = -1;
    m_visiblePriorityLast = -1;
    m_visiblePrioritySizeKey = 0;

    m_scrollDebounce->start();
}

void MediaModel::onScrollDebounced()
{
    if (m_items.isEmpty() || m_visibleFirst > m_visibleLast)
        return;

    const int first = qMax(0, m_visibleFirst);
    const int last = qMin(m_visibleLast, m_items.size() - 1);
    if (first > last)
        return;

    updateVisibleFileWatches();
    checkVisibleFreshness(first, last);

    const int targetKey = m_thumbSize.width();
    auto& cache = ThumbnailCache::instance();
    bool hasMissingVisible = false;
    for (int i = first; i <= last; ++i) {
        const QString& fp = m_items.at(i).filePath;
        if (m_failedPaths.contains(fp))
            continue;
        if (!m_loadedPaths.contains(fp) ||
            !cache.hasLarge(fp) || cache.largeSize(fp) < targetKey) {
            hasMissingVisible = true;
            break;
        }
    }

    if (hasMissingVisible) {
        const bool samePriorityRange = m_visiblePriorityActive &&
                                       m_visiblePriorityFirst == first &&
                                       m_visiblePriorityLast == last &&
                                       ThumbnailLoader::quantizedSizeKey(m_visiblePrioritySizeKey)
                                           == ThumbnailLoader::quantizedSizeKey(targetKey);
        if (!samePriorityRange) {
            TRACE_SLIDE("debounce reload %d..%d key=%d", first, last, targetKey);
            m_visiblePriorityActive = true;
            m_visiblePriorityFirst = first;
            m_visiblePriorityLast = last;
            m_visiblePrioritySizeKey = targetKey;
            m_loader->clearQueue();
            requestVisibleThumbnails(first, last, true);
        }
        return;
    }

    m_visiblePriorityActive = false;
    m_visiblePriorityFirst = -1;
    m_visiblePriorityLast = -1;
    m_visiblePrioritySizeKey = 0;
    requestVisibleThumbnails(first, last, false);
}

void MediaModel::requestVisibleThumbnails(int first, int last, bool visibleOnly)
{
    if (first < 0) first = 0;
    if (last >= m_items.size()) last = m_items.size() - 1;
    if (first > last) return;

    const int targetKey = m_thumbSize.width();
    auto& cache = ThumbnailCache::instance();

    auto needsTarget = [&](int i) {
        const QString& fp = m_items.at(i).filePath;
        if (m_failedPaths.contains(fp)) return false;
        return !cache.hasLarge(fp) || cache.largeSize(fp) < targetKey;
    };
    auto needsPrefetch = [&](int i) {
        const QString& fp = m_items.at(i).filePath;
        return !m_failedPaths.contains(fp) && !m_loadedPaths.contains(fp);
    };

    int priority = std::numeric_limits<int>::max();
    bool queueFull = false;

    auto enqueueChunk = [&](int cfirst, int clast, int sizeKey, bool forceQueue,
                            auto&& predicate) -> bool {
        QStringList paths;
        QList<bool> isVideos;
        for (int i = cfirst; i <= clast; ++i) {
            if (!predicate(i)) continue;
            paths.append(m_items.at(i).filePath);
            isVideos.append(m_items.at(i).isVideo);
        }
        if (paths.isEmpty()) return true;
        return m_loader->requestChunk(paths, isVideos, priority--, QSize(sizeKey, sizeKey), forceQueue);
    };

    struct Chunk { int first; int last; };
    QList<Chunk> chunks;
    chunks.reserve(64);
    chunks.append({first, last});

    if (!visibleOnly) {
        // Prefetch in BLOCKS, not single items: one chunk = one pool task =
        // one ready notification — per-item chunks made the UI churn
        // through thousands of one-item notifications on large directories.
        constexpr int prefetchBlock = 8;
        const int maxDistance = qMax(m_items.size(), 500);
        for (int dist = 1; dist <= maxDistance; dist += prefetchBlock) {
            const int leftHi = first - dist;                          // nearest
            const int leftLo = qMax(0, first - (dist + prefetchBlock - 1));
            const int rightLo = last + dist;                          // nearest
            const int rightHi = qMin(int(m_items.size()) - 1,
                                     last + dist + prefetchBlock - 1);
            const bool hasLeft = (leftHi >= 0);
            const bool hasRight = (rightLo < m_items.size());
            if (!hasLeft && !hasRight) break;

            const Chunk leftChunk{leftLo, qMax(leftLo, leftHi)};
            const Chunk rightChunk{rightLo, rightHi};
            if (m_scrollDirection < 0) {
                if (hasLeft)  chunks.append(leftChunk);
                if (hasRight) chunks.append(rightChunk);
            } else if (m_scrollDirection > 0) {
                if (hasRight) chunks.append(rightChunk);
                if (hasLeft)  chunks.append(leftChunk);
            } else {
                if (hasLeft)  chunks.append(leftChunk);
                if (hasRight) chunks.append(rightChunk);
            }
        }
    }

    int basePriority = std::numeric_limits<int>::max();

    for (int chunkIdx = 0; chunkIdx < chunks.size(); ++chunkIdx) {
        if (queueFull) break;
        const Chunk& c = chunks.at(chunkIdx);
        const bool isVisibleChunk = (c.first == first && c.last == last);
        const bool forceQueue = visibleOnly && isVisibleChunk;

        if (isVisibleChunk) {
            // EXIF prefetch for visible JPEGs without any thumbnail
            {
                QStringList exifPaths;
                QList<bool> exifIsVideos;
                for (int i = c.first; i <= c.last; ++i) {
                    const MediaItem& item = m_items.at(i);
                    if (m_loadedPaths.contains(item.filePath)) continue;
                    if (m_failedPaths.contains(item.filePath)) continue;
                    if (!item.isVideo && (item.fileType == QLatin1String("jpg") || item.fileType == QLatin1String("jpeg"))) {
                        exifPaths.append(item.filePath);
                        exifIsVideos.append(false);
                    }
                }
                if (!exifPaths.isEmpty())
                    m_loader->requestExifPrefetch(exifPaths, exifIsVideos, priority--);
            }
            // Items with NO cheap preview stage first (videos, non-JPEG
            // formats): their cells stay WHITE until the full extraction
            // lands, while JPEGs show the EXIF small almost immediately.
            // Extracting the white cells before the JPEG sharpening makes
            // the grid visually complete much earlier on cold starts
            // (previously the videos queued BEHIND all JPEG fulls and
            // trickled in white for hundreds of ms).
            auto isJpeg = [&](int i) {
                const MediaItem& item = m_items.at(i);
                return !item.isVideo
                       && (item.fileType == QLatin1String("jpg")
                           || item.fileType == QLatin1String("jpeg"));
            };
            const bool okNoPreview = enqueueChunk(c.first, c.last, targetKey, forceQueue,
                [&](int i) { return needsTarget(i) && !isJpeg(i); });
            const bool okJpeg = enqueueChunk(c.first, c.last, targetKey, forceQueue,
                [&](int i) { return needsTarget(i) && isJpeg(i); });
            if (!okNoPreview || !okJpeg) { queueFull = true; break; }
        } else {
            // EXIF prefetch for non-visible items
            {
                QStringList exifPaths;
                QList<bool> exifIsVideos;
                for (int i = c.first; i <= c.last; ++i) {
                    if (!needsPrefetch(i)) continue;
                    const MediaItem& item = m_items.at(i);
                    if (!item.isVideo && (item.fileType == QLatin1String("jpg") || item.fileType == QLatin1String("jpeg"))) {
                        exifPaths.append(item.filePath);
                        exifIsVideos.append(false);
                    }
                }
                if (!exifPaths.isEmpty())
                    m_loader->requestExifPrefetch(exifPaths, exifIsVideos, basePriority - chunkIdx);
            }
            // Full decode for non-JPEG items that have no thumbnail
            auto needsNonJpegPrefetch = [&](int i) {
                const MediaItem& item = m_items.at(i);
                if (m_loadedPaths.contains(item.filePath)) return false;
                if (m_failedPaths.contains(item.filePath)) return false;
                return item.isVideo || (item.fileType != QLatin1String("jpg") && item.fileType != QLatin1String("jpeg"));
            };
            const bool ok = enqueueChunk(c.first, c.last, 128, false, needsNonJpegPrefetch);
            if (!ok) { queueFull = true; break; }
        }
    }
}

void MediaModel::onThumbnailReady(const QStringList& filePaths)
{
    if (filePaths.isEmpty())
        return;

    // Cheap, exact bookkeeping runs immediately — request predicates and the
    // unloaded counter must never lag behind the cache. Everything expensive
    // is deferred to flushThumbnailReady().
    for (const QString& fp : filePaths) {
        const int row = m_rowByPath.value(fp, -1);
        if (row < 0 || row >= m_items.size())
            continue;

        const bool wasLoaded = m_loadedPaths.contains(fp);
        m_loadedPaths.insert(fp);
        if (!wasLoaded)
            --m_unloadedCount;

        m_readyPending.insert(fp);
    }

    if (!m_readyPending.isEmpty() && !m_readyCoalesce->isActive())
        m_readyCoalesce->start();
}

void MediaModel::flushThumbnailReady()
{
    if (m_readyPending.isEmpty())
        return;

    const QSet<QString> pending = std::move(m_readyPending);
    m_readyPending.clear();

    const int currentKey = m_thumbSize.width();

    // Only items inside the decoded-cache keep window are worth decoding:
    // anything outside is dropped by evictDecodedCache() right away, so
    // decoding it would burn a pool task, a UI-thread QPixmap conversion and
    // a dataChanged repaint for a pixmap nobody ever paints. The delegate
    // requests a decode lazily (data(), UserRole+4) once such an item
    // actually scrolls into view.
    const int keepFirst = qMax(0, m_visibleFirst - kDecodedMargin);
    const int keepLast  = qMin(int(m_items.size()) - 1, m_visibleLast + kDecodedMargin);

    QStringList decodeBatch;
    int firstRow = std::numeric_limits<int>::max();
    int lastRow = -1;

    for (const QString& fp : pending) {
        const int row = m_rowByPath.value(fp, -1);
        if (row < keepFirst || row > keepLast)
            continue;

        // EXIF and full-decode notifications overlap for the same file —
        // skip the re-decode when the existing pixmap is already sharp at
        // the current size: a second decode cannot improve on a source
        // that covered the display size. The decode itself runs ASYNC; the
        // delivery emits dataChanged, so no repaint is triggered here.
        auto decoded = m_decodedCache.constFind(fp);
        if (decoded != m_decodedCache.constEnd()
            && decoded->sizeKey == currentKey && decoded->sharp)
            continue;

        decodeBatch.append(fp);
        firstRow = qMin(firstRow, row);
        lastRow = qMax(lastRow, row);
    }

    if (!decodeBatch.isEmpty()) {
        TRACE_SLIDE("thumbs ready n=%d rows=%d..%d (visible %d..%d key=%d)",
                    int(decodeBatch.size()), firstRow, lastRow,
                    m_visibleFirst, m_visibleLast, currentKey);
        requestDecode(decodeBatch);
    }

    // Re-evaluate visible range
    if (m_visibleFirst <= m_visibleLast) {
        const int vf = qMax(0, m_visibleFirst);
        const int vl = qMin(m_visibleLast, m_items.size() - 1);
        setVisibleRows(vf, vl);
    }

    if (m_unloadedCount > 0 && !m_backgroundPrefetch->isActive()) {
        m_backgroundPrefetch->start();
    }

    boundThumbnailCache();
    evictDecodedCache();
}

void MediaModel::onThumbnailFailed(const QStringList& filePaths)
{
    for (const QString& fp : filePaths) {
        if (!m_rowByPath.contains(fp) || m_failedPaths.contains(fp))
            continue;
        // Also marked when a small (EXIF) thumb exists: the item then keeps its
        // blurry thumb, but the full decode is never retried this session.
        m_failedPaths.insert(fp);
        if (!m_loadedPaths.contains(fp))
            --m_unloadedCount;
    }
}

void MediaModel::onBackgroundPrefetch()
{
    continueBackgroundPrefetch();
}

void MediaModel::continueBackgroundPrefetch()
{
    if (m_items.isEmpty()) {
        m_backgroundPrefetch->stop();
        return;
    }

    const int targetKey = m_thumbSize.width();
    const int n = m_items.size();

    const int center = (m_visibleFirst >= 0 && m_visibleLast >= m_visibleFirst)
        ? (m_visibleFirst + m_visibleLast) / 2
        : n / 2;

    constexpr int batchLimit = 128;

    QStringList exifPaths;
    QList<bool> exifIsVideos;
    QStringList fullPaths;
    QList<bool> fullIsVideos;
    exifPaths.reserve(batchLimit);
    fullPaths.reserve(batchLimit);

    int collected = 0;

    // Prefetch only a window around the visible center instead of the whole
    // list. Without this bound, huge selections would ping-pong forever with
    // the LRU eviction in boundThumbnailCache(): prefetch loads everything,
    // eviction removes it, prefetch reloads it. Half the cache capacity is
    // reserved for the window, the rest keeps recently visited history.
    const int prefetchRadius = ThumbnailCache::instance().maxEntries() / 4;
    const int maxDist = qMin(qMax(center, n - 1 - center), prefetchRadius);

    for (int dist = 0; dist <= maxDist && collected < batchLimit; ++dist) {
        for (int side = 0; side < 2 && collected < batchLimit; ++side) {
            const int idx = (side == 0) ? center - dist : center + dist;
            if (idx < 0 || idx >= n) continue;
            if (side == 1 && dist == 0) continue;

            const MediaItem& item = m_items.at(idx);
            if (!m_loadedPaths.contains(item.filePath)
                && !m_failedPaths.contains(item.filePath)) {
                if (!item.isVideo && (item.fileType == QLatin1String("jpg") || item.fileType == QLatin1String("jpeg"))) {
                    exifPaths.append(item.filePath);
                    exifIsVideos.append(false);
                } else {
                    fullPaths.append(item.filePath);
                    fullIsVideos.append(item.isVideo);
                }
                ++collected;
            }
        }
    }

    // Window fully loaded (or only failed items left): stop the timer. It is
    // restarted from onThumbnailReady when scrolling brings unloaded items
    // into view and shifts the window.
    if (collected == 0) {
        m_backgroundPrefetch->stop();
        return;
    }

    if (!exifPaths.isEmpty()) {
        m_loader->requestExifPrefetch(exifPaths, exifIsVideos, -1000);
    }
    if (!fullPaths.isEmpty()) {
        m_loader->requestChunk(fullPaths, fullIsVideos, -1001, QSize(targetKey, targetKey), false);
    }
}
