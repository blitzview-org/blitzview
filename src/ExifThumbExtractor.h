#pragma once

#include <QByteArray>
#include <QString>

struct ExifThumbResult {
    QByteArray data;       // embedded JPEG thumbnail bytes
    int orientation = 1;   // EXIF orientation tag (1-8), 1 = normal
    int imageWidth  = 0;   // full image width from SOF marker (0 = unknown)
    int imageHeight = 0;   // full image height from SOF marker (0 = unknown)
};

// Extracts the embedded EXIF thumbnail from a JPEG file without decoding
// the main image. Reads only the first ~64KB (EXIF segment).
// Also reads the orientation tag so the thumbnail can be correctly rotated.
ExifThumbResult extractExifThumbnail(const QString& filePath);
