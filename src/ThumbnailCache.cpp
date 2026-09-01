#include "ThumbnailCache.h"
#include <QMutexLocker>
#include <algorithm>

ThumbnailCache& ThumbnailCache::instance()
{
    static ThumbnailCache cache;
    return cache;
}

bool ThumbnailCache::has(const QString& filePath) const
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.constFind(filePath);
    return it != m_entries.constEnd()
        && (!it->smallThumb.isEmpty() || !it->largeThumb.isEmpty());
}

bool ThumbnailCache::hasSmall(const QString& filePath) const
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.constFind(filePath);
    return it != m_entries.constEnd() && !it->smallThumb.isEmpty();
}

bool ThumbnailCache::hasLarge(const QString& filePath) const
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.constFind(filePath);
    return it != m_entries.constEnd() && !it->largeThumb.isEmpty();
}

int ThumbnailCache::largeSize(const QString& filePath) const
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.constFind(filePath);
    if (it == m_entries.constEnd()) return 0;
    return it->largeThumbSize;
}

QByteArray ThumbnailCache::getSmall(const QString& filePath)
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.find(filePath);
    if (it == m_entries.end()) return {};
    it->lastUsedGeneration = ++m_generation;
    return it->smallThumb;
}

QByteArray ThumbnailCache::getLarge(const QString& filePath)
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.find(filePath);
    if (it == m_entries.end()) return {};
    it->lastUsedGeneration = ++m_generation;
    return it->largeThumb;
}

QByteArray ThumbnailCache::getBest(const QString& filePath)
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.find(filePath);
    if (it == m_entries.end()) return {};
    it->lastUsedGeneration = ++m_generation;
    if (!it->largeThumb.isEmpty()) return it->largeThumb;
    return it->smallThumb;
}

void ThumbnailCache::putSmall(const QString& filePath, const QByteArray& jpeg)
{
    if (jpeg.isEmpty()) return;
    QMutexLocker lk(&m_mutex);
    Entry& e = m_entries[filePath];
    e.smallThumb = jpeg;
    e.lastUsedGeneration = ++m_generation;
}

void ThumbnailCache::putLarge(const QString& filePath, const QByteArray& jpeg, int sizeKey)
{
    if (jpeg.isEmpty()) return;
    QMutexLocker lk(&m_mutex);
    Entry& e = m_entries[filePath];
    if (sizeKey >= e.largeThumbSize) {
        e.largeThumb = jpeg;
        e.largeThumbSize = sizeKey;
    }
    e.lastUsedGeneration = ++m_generation;
}

void ThumbnailCache::setOrientedSize(const QString& filePath, int w, int h)
{
    QMutexLocker lk(&m_mutex);
    Entry& e = m_entries[filePath];
    if (e.orientedWidth == 0 && e.orientedHeight == 0) {
        e.orientedWidth = w;
        e.orientedHeight = h;
    }
    e.lastUsedGeneration = ++m_generation;
}

QSize ThumbnailCache::orientedSize(const QString& filePath) const
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.constFind(filePath);
    if (it == m_entries.constEnd() || it->orientedWidth <= 0 || it->orientedHeight <= 0)
        return {};
    return QSize(it->orientedWidth, it->orientedHeight);
}

void ThumbnailCache::touch(const QString& filePath)
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.find(filePath);
    if (it != m_entries.end())
        it->lastUsedGeneration = ++m_generation;
}

void ThumbnailCache::renameEntry(const QString& oldPath, const QString& newPath)
{
    QMutexLocker lk(&m_mutex);
    auto it = m_entries.find(oldPath);
    if (it == m_entries.end())
        return;
    Entry e = std::move(it.value());
    m_entries.erase(it);
    e.lastUsedGeneration = ++m_generation;
    m_entries.insert(newPath, std::move(e));
}

void ThumbnailCache::remove(const QString& filePath)
{
    QMutexLocker lk(&m_mutex);
    m_entries.remove(filePath);
}

void ThumbnailCache::setMaxEntries(int max)
{
    QMutexLocker lk(&m_mutex);
    m_maxEntries = max;
}

int ThumbnailCache::maxEntries() const
{
    QMutexLocker lk(&m_mutex);
    return m_maxEntries;
}

void ThumbnailCache::evict()
{
    QMutexLocker lk(&m_mutex);
    if (m_entries.size() <= m_maxEntries)
        return;

    // LRU trim down to 90% of the limit. The 10% slack is hysteresis so a
    // caller polling entryCount() > maxEntries() doesn't re-trigger eviction
    // on every subsequent insert.
    const int target = m_maxEntries * 9 / 10;
    const int removeCount = m_entries.size() - target;

    QList<QPair<quint64, QString>> byAge;
    byAge.reserve(m_entries.size());
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        byAge.append({it->lastUsedGeneration, it.key()});

    std::nth_element(byAge.begin(), byAge.begin() + removeCount, byAge.end());
    for (int i = 0; i < removeCount; ++i)
        m_entries.remove(byAge.at(i).second);
}

int ThumbnailCache::entryCount() const
{
    QMutexLocker lk(&m_mutex);
    return m_entries.size();
}
