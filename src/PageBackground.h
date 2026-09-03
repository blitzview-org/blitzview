#pragma once
#include <QImageReader>
#include <QString>
#include <Qt>

// A PDF renders onto TRANSPARENT space, not onto white paper: Qt's pdf plugin
// leaves alpha at 0 everywhere the page is blank, and only the glyphs are
// opaque. That is faithful to the format -- a PDF has no background object --
// but it is not what a page looks like, and it makes the page nearly invisible
// twice over: drawn on the viewer's dark background it is black text on black,
// and the thumbnail cache stores JPEG, which has no alpha channel at all, so
// the transparent area is flattened to black on the way in.
//
// Every other renderer treats the paper colour as a rendering parameter rather
// than something painted afterwards: Poppler defaults Document::paperColor to
// white, and KDE's PostScript/PDF thumbnailer runs Ghostscript with
// -sDEVICE=png16m, a device with no alpha channel at all (pngalpha, the
// transparent variant, exists and is deliberately not used). Qt's equivalent
// is QImageReader::setBackgroundColor, which the pdf plugin supports -- the
// handler then fills before rendering, so glyph antialiasing is computed
// against the paper instead of being composited onto it afterwards.
//
// Deliberately NOT applied to every format with an alpha channel. PNG, SVG and
// XCF transparency is content the user wants to see against the application's
// own background; forcing white paper under those would be wrong. Only formats
// whose page is white by definition qualify -- the same split Dolphin makes by
// having a separate thumbnailer for PDF/PS in the first place.
//
// Only pdf is listed. The eps/ps plugin does not support the option and does
// not need it: it already returns an opaque white page.
inline void applyPaperBackground(QImageReader& reader, const QString& suffixLower)
{
    if (suffixLower == QLatin1String("pdf"))
        reader.setBackgroundColor(Qt::white);
}
