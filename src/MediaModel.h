#pragma once

#include <QAbstractTableModel>
#include <QList>
#include <QSize>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QPixmap>
#include <QFutureWatcher>
#include <QThreadPool>
#include <QFileSystemWatcher>
#include "MediaItem.h"

class ThumbnailLoader;

// Result of an async directory scan: the matched media items, plus every
// directory the scan descended into (including empty ones) — the latter is
// what sync mode watches for filesystem changes (see rebuildDirectoryWatches).
struct MediaScanResult {
    QList<MediaItem> items;
    QSet<QString>    visitedDirs;
};

class MediaModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column {
        Col_Thumbnail    = 0,
        Col_FileName     = 1,
        Col_FileSize     = 2,
        Col_ModifiedDate = 3,
        Col_CreatedDate  = 4,
        Col_Resolution   = 5,
        Col_Duration     = 6,
        Col_FileType     = 7,
        Col_Taken        = 8,  // appended last: stored col-hidden settings stay valid
        Col_COUNT
    };

    explicit MediaModel(QObject* parent = nullptr);
    ~MediaModel() override;

    // QAbstractTableModel interface
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDragActions() const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    // Loading
    void loadDirectory(const QString& path, bool recursive);
    void loadDirectories(const QStringList& paths, bool recursive);
    void loadDirectories(const QStringList& recursive, const QStringList& individual);

    // Thumbnail priority hint
    void setVisibleRows(int first, int last);

    // Discard decoded pixmaps and reload at new size
    void invalidateThumbnails(const QSize& newSize);

    // Renames the file on disk and migrates all cache entries (RAM + disk
    // thumbs, metadata) — no rescan and no thumbnail regeneration needed.
    // newFileName is a bare name (no path). Returns false on failure.
    bool renameItem(int row, const QString& newFileName);

    // Filtering: the model displays a subset of the scanned MASTER list.
    // Path filter and tag filter are independent criteria that COMBINE as an
    // intersection (an item must pass both). Rescans of the SAME root set
    // keep the filter; loading different roots clears it.
    void filterToPaths(const QStringList& paths);  // keeps the tag filter
    void filterToTags(const QStringList& tags);    // ANY-match; keeps the path filter
    void clearFilter();
    bool isFiltered() const { return !m_filterPaths.isEmpty() || !m_filterTags.isEmpty(); }
    QStringList activeTagFilter() const { return m_filterTags; }
    QStringList activeFilterPaths() const
    { return QStringList(m_filterPaths.cbegin(), m_filterPaths.cend()); }
    int masterCount() const { return m_masterItems.size(); }
    // Tags offered for filtering: union over the items passing the PATH
    // filter, plus the currently active tags (so they stay uncheckable)
    QStringList tagsInDirectory() const;

    const MediaItem& item(int row) const { return m_items.at(row); }
    const QList<MediaItem>& items() const { return m_items; }
    int unloadedCount() const { return m_unloadedCount; }

    static QSize thumbnailSize() { return {128, 128}; }

    // Sync mode: watches the loaded directories for filesystem changes
    // (add/remove/rename) and automatically rescans. Independent of this,
    // MediaModel always verifies file freshness (mtime/size) for items as
    // they enter the visible range.
    void setSyncModeEnabled(bool enabled);
    bool syncModeEnabled() const { return m_syncEnabled; }

    // Stops every background producer (loader queue, decode pool, timers,
    // pending scan/prime futures). Called when the window closes so a huge
    // library does not keep the process busy after the UI is gone.
    void cancelBackgroundWork();

signals:
    void loadingFinished(int count);
    void scanningStarted();
    void scanningFinished();
    void filterChanged();

private slots:
    void onThumbnailReady(const QStringList& filePaths);
    void flushThumbnailReady();
    void onThumbnailFailed(const QStringList& filePaths);
    void onMetadataReady(const QStringList& filePaths);
    void onScrollDebounced();
    void onBackgroundPrefetch();
    void onFsPathChanged(const QString& path);

private:
    static bool lessThan(const MediaItem& a, const MediaItem& b,
                          int column, Qt::SortOrder order);
    void sortItemsInPlace();          // sorts m_items per m_sortColumn/-Order, no-op if none active
    void resetPriorityLoadingState(); // cancels in-flight priority loads, clears visible-priority state
    void rebuildRowIndex();
    void decodeFromCache(const QString& filePath, int row) const;
    void requestDecode(const QStringList& paths);
    void evictDecodedCache();
    void boundThumbnailCache();

    // Sync mode: filesystem watcher on directory add/remove/rename +
    // freshness check for visible items.
    void rebuildDirectoryWatches(const QSet<QString>& visitedDirs);
    void addPendingWatches();   // one time-boxed slice of m_pendingWatchAdds
    void updateVisibleFileWatches();
    void checkVisibleFreshness(int first, int last);
    void invalidateContentCache(const QString& filePath);
    bool                 m_syncEnabled = true;
    QFileSystemWatcher*  m_fsWatcher = nullptr;
    QTimer*              m_syncDebounce = nullptr;   // debounces watcher events
    QStringList          m_lastRecursiveRoots, m_lastIndividualRoots; // for self-reload
    QSet<QString>        m_watchedDirs;          // directories of the last scan
    QStringList          m_pendingWatchAdds;     // directories still to be registered
    QTimer*              m_watchAddTimer = nullptr;
    QSet<QString>        m_watchedVisibleFiles;   // currently visible files (content watch)
    QSet<QString>        m_freshnessChecked;      // paths already checked while visible --
                                                   // not re-checked on every scroll

    QList<MediaItem>         m_items;        // DISPLAYED (filtered) list
    QList<MediaItem>         m_masterItems;  // full scan result
    QHash<QString, int>      m_rowByPath;    // displayed rows

    QSet<QString> m_filterPaths;   // empty = no path criterion
    QStringList   m_filterTags;    // empty = no tag criterion
    QStringList   m_lastLoadRoots; // detects same-roots rescans (keep filter)
    QTimer*       m_refilterDebounce = nullptr;  // tag filter + async metadata
    void applyFilter();  // rebuild m_items from m_masterItems (model reset)
    bool matchesFilter(const MediaItem& item) const;
    ThumbnailLoader*         m_loader = nullptr;
    QSize                    m_thumbSize{128, 128};
    QTimer*                  m_scrollDebounce = nullptr;
    QTimer*                  m_backgroundPrefetch = nullptr;
    // Coalesces the loader's per-item ready notifications: the flood of
    // single-path signals (instant-preview promotion inside the workers) used
    // to run one full decode/visible-range/eviction pass EACH — hundreds per
    // second, which is what made large libraries feel sluggish.
    QTimer*                  m_readyCoalesce = nullptr;
    QSet<QString>            m_readyPending;
    int                      m_visibleFirst = 0;
    int                      m_visibleLast = 0;
    int                      m_scrollDirection = 0;
    int                      m_unloadedCount = 0;
    bool                     m_visiblePriorityActive = false;
    int                      m_visiblePriorityFirst = -1;
    int                      m_visiblePriorityLast = -1;
    int                      m_visiblePrioritySizeKey = 0;

    // Tracks which items have ThumbnailCache entries (no mutex needed, UI thread only)
    QSet<QString>            m_loadedPaths;

    // Items whose full decode failed permanently (corrupt/unreadable).
    // Never re-requested until the next directory load.
    QSet<QString>            m_failedPaths;

    // Decoded pixmap cache for fast painting (UI thread only)
    struct DecodedThumb {
        QPixmap pixmap;       // scaled to current display size
        int     sizeKey = 0;  // display size it was scaled for
        bool    sharp = false; // decode source covered the display size —
                               // a re-decode cannot improve this pixmap
    };
    mutable QHash<QString, DecodedThumb> m_decodedCache;
    static constexpr int kMaxDecodedEntries = 600;
    // Items kept/decoded around the visible range (see evictDecodedCache)
    static constexpr int kDecodedMargin = 300;

    // Async decode pipeline: JPEG→pixmap decoding runs on this pool, NOT on
    // the UI thread — one 1900 px preview costs 200+ ms and a zoomed-out
    // screenful is 300 decodes in one gulp; both froze the event loop.
    // Pending keys are "path|sizeKey"; cleared on invalidate/reload (stale
    // deliveries are dropped by their request key).
    QThreadPool              m_decodePool;
    QSet<QString>            m_decodePending;

    // Persistent display-size map: remembers the pixel dimensions for each
    // (filePath, sizeKey) pair so that replacing an EXIF thumb with a full-decode
    // cannot shift the thumbnail, even if the decoded-cache entry was evicted.
    mutable QHash<QString, QSize> m_displaySizes;

    void requestVisibleThumbnails(int first, int last, bool visibleOnly = false);
    void continueBackgroundPrefetch();
    void onScanFinished();

    // Remembered sort so it can be re-applied after async scan
    int               m_sortColumn = -1;
    Qt::SortOrder     m_sortOrder  = Qt::AscendingOrder;

    // Async directory scanning
    QFutureWatcher<MediaScanResult>* m_scanWatcher = nullptr;
    quint64 m_scanGeneration = 0;

    // Metadata priming: warms MetadataCache from disk for all items off the
    // UI thread, returns the paths that are still missing (→ ExifToolService)
    void primeMetadataAsync();
    QFutureWatcher<QStringList>* m_metaPrimeWatcher = nullptr;
    quint64 m_metaPrimeGeneration = 0;
};
