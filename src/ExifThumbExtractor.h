#pragma once

#include <QByteArray>
#include <QString>

struct ExifThumbResult {
    QByteArray data;       // embedded JPEG thumbnail bytes
    int orientation = 1;   // EXIF orientation tag (1-8), 1 = normal
    int imageWidth  = 0;   // full image width from SOF marker (0 = unknown)
    int imageHeight = 0;   // full image height from SOF marker (0 = unknown)
    qint64 waitMs = 0;     // diagnostics: queued for a media read slot
    qint64 readMs = 0;     // diagnostics: the header read itself
};

// Extracts the embedded EXIF thumbnail from a JPEG file without decoding
// the main image. Reads only the first ~64KB (EXIF segment).
// Also reads the orientation tag so the thumbnail can be correctly rotated.
// urgent: the read takes an urgent MediaReadGate slot (visible wave).
ExifThumbResult extractExifThumbnail(const QString& filePath, bool urgent = false);
