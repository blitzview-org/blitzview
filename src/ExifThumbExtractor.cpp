#include "ExifThumbExtractor.h"

#include <QElapsedTimer>
#include <QFile>
#include <cstring>
#include "MediaReadGate.h"
#include <QtEndian>

// Lightweight JPEG EXIF thumbnail extractor.
// Parses: SOI → APP1 marker → TIFF header → IFD0 → IFD1 → thumbnail offset/length.
// Only reads the EXIF segment (typically first 64KB), never the full image.

static constexpr int kMaxExifRead = 65536;

static inline quint16 readU16(const uchar* p, bool bigEndian)
{
    return bigEndian ? qFromBigEndian<quint16>(p) : qFromLittleEndian<quint16>(p);
}

static inline quint32 readU32(const uchar* p, bool bigEndian)
{
    return bigEndian ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
}

// Full-image dimensions from the SOF marker. Walks the marker chain from
// SOI: inside the already-read header for free, beyond it with a 4-byte
// seek+read per segment. Modern phone JPEGs place SOF past the 64 KB header
// (Samsung: 60–75 KB, after a 40–50 KB APP1 plus ICC/XMP); the previous
// fallback for that case was a QImageReader on the whole file — one 3 MB
// read per thumbnail on the USB disk, for two numbers, and it made the
// cold visible wave crawl at ~300 ms per cell. Runs inside the read slot.
static void parseSofDimensions(QFile& file, const QByteArray& header, ExifThumbResult& result)
{
    const uchar* d = reinterpret_cast<const uchar*>(header.constData());
    const qint64 size = header.size();
    if (size < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return;

    constexpr qint64 kMaxWalk = 16 * 1024 * 1024;
    auto isSof = [](quint8 m) {
        return (m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) ||
               (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF);
    };
    auto readAt = [&](qint64 pos, uchar* out, int n) -> bool {
        if (pos + n <= size) {
            memcpy(out, d + pos, n);
            return true;
        }
        return file.seek(pos) && file.read(reinterpret_cast<char*>(out), n) == n;
    };

    qint64 pos = 2;
    while (pos + 4 <= kMaxWalk) {
        uchar hdr[4];
        if (!readAt(pos, hdr, 4))
            return;
        if (hdr[0] != 0xFF)
            return;
        const quint8 m = hdr[1];
        if (m == 0xFF) { ++pos; continue; }                       // fill byte
        if (m == 0xDA) return;                                    // SOS: no SOF seen
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { pos += 2; continue; } // no length
        const int len = (hdr[2] << 8) | hdr[3];
        if (isSof(m)) {
            uchar sof[5];   // precision, height(2), width(2)
            if (!readAt(pos + 4, sof, 5))
                return;
            result.imageHeight = (sof[1] << 8) | sof[2];
            result.imageWidth  = (sof[3] << 8) | sof[4];
            return;
        }
        if (len < 2)
            return;
        pos += 2 + len;
    }
}

ExifThumbResult extractExifThumbnail(const QString& filePath, bool urgent)
{
    ExifThumbResult result;

    QByteArray header;
    {
        const MediaReadSlot slot(urgent);   // storage access only, see MediaReadGate.h
        result.waitMs = slot.waitMs();
        QElapsedTimer timer;
        timer.start();
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly))
            return result;
        header = file.read(kMaxExifRead);
        parseSofDimensions(file, header, result);
        result.readMs = timer.elapsed();
    }
    const int size = header.size();
    if (size < 12)
        return result;

    const uchar* d = reinterpret_cast<const uchar*>(header.constData());

    // Check JPEG SOI marker
    if (d[0] != 0xFF || d[1] != 0xD8)
        return result;

    // Find APP1 (EXIF) marker
    int pos = 2;
    while (pos + 4 < size) {
        if (d[pos] != 0xFF)
            break;
        const quint8 marker = d[pos + 1];
        const int segLen = (d[pos + 2] << 8) | d[pos + 3];

        if (marker == 0xE1) // APP1
            break;

        // Skip non-APP1 markers
        if (marker == 0xDA) // SOS - start of scan, no more metadata
            return result;
        pos += 2 + segLen;
    }

    if (pos + 10 >= size || d[pos] != 0xFF || d[pos + 1] != 0xE1)
        return result;

    const int app1Len = (d[pos + 2] << 8) | d[pos + 3];
    const int app1Start = pos + 4; // after marker + length
    const int app1End = qMin(pos + 2 + app1Len, size);

    // Check "Exif\0\0" signature
    if (app1Start + 6 >= app1End)
        return result;
    if (d[app1Start] != 'E' || d[app1Start+1] != 'x' || d[app1Start+2] != 'i' ||
        d[app1Start+3] != 'f' || d[app1Start+4] != 0 || d[app1Start+5] != 0)
        return result;

    // TIFF header starts after "Exif\0\0"
    const int tiffBase = app1Start + 6;
    if (tiffBase + 8 >= app1End)
        return result;

    // Byte order
    bool bigEndian;
    if (d[tiffBase] == 'M' && d[tiffBase+1] == 'M')
        bigEndian = true;
    else if (d[tiffBase] == 'I' && d[tiffBase+1] == 'I')
        bigEndian = false;
    else
        return result;

    // TIFF magic 42
    if (readU16(d + tiffBase + 2, bigEndian) != 42)
        return result;

    // Offset to IFD0
    quint32 ifd0Offset = readU32(d + tiffBase + 4, bigEndian);
    int ifdPos = tiffBase + static_cast<int>(ifd0Offset);
    if (ifdPos + 2 >= app1End)
        return result;

    // Read IFD0 to find orientation tag (0x0112)
    quint16 ifd0Count = readU16(d + ifdPos, bigEndian);
    int ifd0EntryPos = ifdPos + 2;
    for (int i = 0; i < ifd0Count && ifd0EntryPos + 12 <= app1End; ++i, ifd0EntryPos += 12) {
        quint16 tag = readU16(d + ifd0EntryPos, bigEndian);
        if (tag == 0x0112) { // Orientation
            quint16 type = readU16(d + ifd0EntryPos + 2, bigEndian);
            if (type == 3) // SHORT
                result.orientation = readU16(d + ifd0EntryPos + 8, bigEndian);
            else
                result.orientation = static_cast<int>(readU32(d + ifd0EntryPos + 8, bigEndian));
            break;
        }
    }

    ifdPos += 2 + ifd0Count * 12; // skip past IFD0 entries
    if (ifdPos + 4 >= app1End)
        return result;

    // Next IFD offset (IFD1)
    quint32 ifd1Offset = readU32(d + ifdPos, bigEndian);
    if (ifd1Offset == 0)
        return result; // no IFD1

    int ifd1Pos = tiffBase + static_cast<int>(ifd1Offset);
    if (ifd1Pos + 2 >= app1End)
        return result;

    quint16 ifd1Count = readU16(d + ifd1Pos, bigEndian);
    ifd1Pos += 2;

    quint32 thumbOffset = 0;
    quint32 thumbLength = 0;
    quint16 compression = 0;

    for (int i = 0; i < ifd1Count && ifd1Pos + 12 <= app1End; ++i, ifd1Pos += 12) {
        quint16 tag = readU16(d + ifd1Pos, bigEndian);
        quint32 value = readU32(d + ifd1Pos + 8, bigEndian);

        switch (tag) {
        case 0x0103: compression = static_cast<quint16>(value); break; // Compression
        case 0x0201: thumbOffset = value; break; // JPEGInterchangeFormat
        case 0x0202: thumbLength = value; break; // JPEGInterchangeFormatLength
        }
    }

    // Only JPEG thumbnails (compression == 6)
    if (compression != 6 || thumbOffset == 0 || thumbLength == 0)
        return result;

    const int absOffset = tiffBase + static_cast<int>(thumbOffset);
    const int absEnd = absOffset + static_cast<int>(thumbLength);
    if (absOffset < 0 || absEnd > app1End || absEnd > size)
        return result;

    // Verify it starts with JPEG SOI
    if (d[absOffset] != 0xFF || d[absOffset+1] != 0xD8)
        return result;

    result.data = header.mid(absOffset, static_cast<int>(thumbLength));
    // Full-image dimensions were parsed by parseSofDimensions() while the
    // file was open (SOF usually sits beyond the 64 KB header).

    return result;
}
