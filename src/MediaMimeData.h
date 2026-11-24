#pragma once

#include <QByteArray>
#include <QImage>
#include <QMimeData>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <optional>

// QMimeData for the file clipboard that can ADDITIONALLY offer one still
// image as pixel data, so image editors (GIMP, Krita, LibreOffice, …) paste
// the picture while file managers still see a plain file copy.
//
// The image is decoded lazily: copying only records the path. The file is
// read and decoded (full resolution, EXIF orientation applied) the first
// time a consumer asks for "application/x-qt-image" or "image/png", and the
// result is cached inside this object so later requests do not decode again.
// On X11/Wayland that request arrives while the clipboard owner (this app)
// serves the selection, i.e. on the UI thread — a one-off stall of the
// decode time, accepted for now.
//
// Qt's platform clipboards (QInternalMimeData) expand "application/x-qt-image"
// into every writable image format (image/png, image/bmp, image/jpeg, …) and
// encode the QImage on request; "image/png" is listed explicitly as well so
// in-process consumers and the TARGETS list see it directly.
//
// If the file disappears or cannot be decoded, the image formats stay
// advertised but deliver empty data — never a crash.
class MediaMimeData : public QMimeData
{
    Q_OBJECT
public:
    // imagePath empty → behaves exactly like a plain QMimeData.
    explicit MediaMimeData(const QString& imagePath = QString());

    QString imagePath() const { return m_imagePath; }
    bool offersImage() const { return !m_imagePath.isEmpty(); }

    QStringList formats() const override;
    bool hasFormat(const QString& mimeType) const override;

    static QString imageFormat() { return QStringLiteral("application/x-qt-image"); }
    static QString pngFormat()   { return QStringLiteral("image/png"); }

protected:
    QVariant retrieveData(const QString& mimeType, QMetaType type) const override;

private:
    const QImage& image() const;        // decodes on first call, cached
    const QByteArray& png() const;      // encodes on first call, cached

    QString m_imagePath;
    mutable std::optional<QImage> m_image;
    mutable std::optional<QByteArray> m_png;
};
