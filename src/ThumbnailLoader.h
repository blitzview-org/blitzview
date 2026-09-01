#pragma once

#include <QObject>
#include <QRunnable>
#include <QThreadPool>
#include <QSize>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <atomic>

class ThumbnailLoader : public QObject
{
    Q_OBJECT
public:
    explicit ThumbnailLoader(QObject* parent = nullptr);
    ~ThumbnailLoader() override;

    void setThumbnailSize(const QSize& size);

    // The quantized extraction key a request of this display key maps to
    // (32er buckets, see quantizeExtractionEdge). Two display keys in the
    // same bucket produce IDENTICAL extraction work — callers use this to
    // skip cancel/reload cycles for ±px refits (scrollbar gutter etc.).
    static int quantizedSizeKey(int px);
    QSize thumbnailSize() const { return m_thumbSize; }

    bool requestChunk(const QStringList& filePaths,
                      const QList<bool>& isVideos,
                      int priority,
                      const QSize& size = QSize(),
                      bool forceQueue = false);
    bool requestExifPrefetch(const QStringList& filePaths,
                             const QList<bool>& isVideos,
                             int priority);
    void cancelAll();
    void cancelPending();
    void clearQueue();

    // Called from worker threads
    void notifyBatchReady(const QStringList& filePaths);
    void notifyBatchFailed(const QStringList& filePaths);
    void removePending(const QString& key);
    bool isEpochCurrent(quint64 epoch) const;

signals:
    void thumbnailReady(const QStringList& filePaths);
    // Full decode failed permanently (corrupt/unreadable file). EXIF prefetch
    // misses are not failures — a full decode may still succeed.
    void thumbnailFailed(const QStringList& filePaths);

private:
    void onThumbnailCompleted(const QStringList& filePaths);

    QThreadPool m_pool;
    QSize       m_thumbSize{128, 128};
    QMutex      m_mutex;
    int         m_workerCount = 4;
    int         m_maxPendingJobs = 64;
    std::atomic<quint64> m_cancelEpoch{1};
    QSet<QString> m_pending;
};
