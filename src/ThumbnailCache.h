#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QSize>
#include <QString>

class ThumbnailCache
{
public:
    static ThumbnailCache& instance();

    // Thread-safe access. Every touching access stamps the entry with a fresh
    // generation (access clock), so lastUsedGeneration orders entries by
    // recency and evict() can trim least-recently-used entries.
    bool has(const QString& filePath) const;
    bool hasSmall(const QString& filePath) const;
    bool hasLarge(const QString& filePath) const;
    int  largeSize(const QString& filePath) const;

    QByteArray getSmall(const QString& filePath);
    QByteArray getLarge(const QString& filePath);
    QByteArray getBest(const QString& filePath);  // large preferred, then small

    void putSmall(const QString& filePath, const QByteArray& jpeg);
    void putLarge(const QString& filePath, const QByteArray& jpeg, int sizeKey);

    // Store/retrieve oriented (display) image dimensions.
    // Used to compute consistent thumbnail display sizes across EXIF and full decode.
    void setOrientedSize(const QString& filePath, int w, int h);
    QSize orientedSize(const QString& filePath) const;

    void touch(const QString& filePath);
    // File rename: move the entry to the new key (thumbs stay valid,
    // the content did not change)
    void renameEntry(const QString& oldPath, const QString& newPath);
    // Drops the entry outright: the file's CONTENT changed (unlike rename,
    // where the old thumb stays valid under a new key).
    void remove(const QString& filePath);
    void setMaxEntries(int max);
    int  maxEntries() const;
    void evict();
    int  entryCount() const;

private:
    ThumbnailCache() = default;
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    struct Entry {
        QByteArray smallThumb;   // EXIF/low-res JPEG
        QByteArray largeThumb;   // full-decode JPEG
        int        largeThumbSize = 0;
        quint64    lastUsedGeneration = 0;
        int        orientedWidth = 0;   // display-oriented image width (0 = unknown)
        int        orientedHeight = 0;  // display-oriented image height (0 = unknown)
    };

    mutable QMutex          m_mutex;
    QHash<QString, Entry>   m_entries;
    quint64                 m_generation  = 1;
    int                     m_maxEntries  = 15000;
};
