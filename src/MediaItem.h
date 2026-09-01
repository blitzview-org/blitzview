#pragma once

#include <QString>
#include <QDateTime>
#include <QSize>

struct MediaItem {
    QString   filePath;
    QString   fileName;
    qint64    fileSize     = 0;
    QDateTime modifiedDate;
    QDateTime createdDate;
    QString   fileType;
    bool      isVideo      = false;

    QSize     resolution;
    qint64    duration     = 0;

    // Capture time from metadata (exiftool), filled asynchronously via
    // MetadataCache; invalid until read
    QDateTime takenDate;

    // Tags from metadata (exiftool), same async lifecycle as takenDate;
    // empty until read. Mirrored here so GridDelegate can paint tag badges
    // without taking the MetadataCache mutex per cell per frame.
    QStringList tags;
};
