#include "MediaMimeData.h"

#include <QBuffer>
#include <QImageReader>

#include "MemoryImageReader.h"

MediaMimeData::MediaMimeData(const QString& imagePath)
    : m_imagePath(imagePath)
{
}

QStringList MediaMimeData::formats() const
{
    QStringList list = QMimeData::formats();
    if (offersImage()) {
        if (!list.contains(imageFormat()))
            list.append(imageFormat());
        if (!list.contains(pngFormat()))
            list.append(pngFormat());
    }
    return list;
}

bool MediaMimeData::hasFormat(const QString& mimeType) const
{
    if (offersImage() && (mimeType == imageFormat() || mimeType == pngFormat()))
        return true;
    return QMimeData::hasFormat(mimeType);
}

QVariant MediaMimeData::retrieveData(const QString& mimeType, QMetaType type) const
{
    if (offersImage()) {
        if (mimeType == imageFormat())
            return QVariant::fromValue(image());
        if (mimeType == pngFormat())
            return png();
    }
    return QMimeData::retrieveData(mimeType, type);
}

const QImage& MediaMimeData::image() const
{
    if (!m_image) {
        // Gate::None: this runs on the UI thread while serving the clipboard —
        // it must never queue behind the thumbnail workers. setAutoTransform
        // applies the EXIF orientation, the same way the viewer loads images.
        // A missing or unreadable file simply yields a null image.
        MemoryImageReader mem(m_imagePath, MemoryImageReader::Gate::None);
        mem.reader->setAutoTransform(true);
        m_image = mem.reader->read();
    }
    return *m_image;
}

const QByteArray& MediaMimeData::png() const
{
    if (!m_png) {
        QByteArray bytes;
        const QImage& img = image();
        if (!img.isNull()) {
            QBuffer buf(&bytes);
            buf.open(QIODevice::WriteOnly);
            // Quality 80 = zlib level 1. Encoding dominates the request time
            // (20 MP photo: decode ~0.1 s, PNG default level ~1.5–7 s, level 1
            // ~0.5–2 s), and Qt's xcb clipboard reader gives up after 5 s. The
            // bytes never hit a disk, so compression ratio hardly matters.
            img.save(&buf, "PNG", 80);
        }
        m_png = bytes;
    }
    return *m_png;
}
