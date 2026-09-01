#include "TagSymbols.h"

#include "AppSettings.h"

#include <QCoreApplication>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QtMath>
#include <QPalette>
#include <QGuiApplication>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QTransform>
#include <utility>

namespace {

// Canonical emoji (stored form) with a text-glyph fallback each for
// systems without an emoji font. Fallback glyphs have DejaVu-class
// coverage (verified via fc-list :charset) and are drawn with the given
// pen color; color emoji bring their own colors.
struct SymbolDef {
    const char16_t* emoji;      // canonical, persisted
    const char16_t* fallback;   // DejaVu-covered text glyph
    QColor          color;      // pen color for the fallback glyph
    const char*     name;       // shown next to the symbol in pickers
};
const SymbolDef kSymbolDefs[] = {
    { u"❤️", u"❤", QColor(235,  60,  90), QT_TRANSLATE_NOOP("TagSymbols", "Heart")        },
    { u"⭐", u"★", QColor(255, 200,  50), QT_TRANSLATE_NOOP("TagSymbols", "Star")         },
    { u"🔥", u"☀", QColor(255, 140,  40), QT_TRANSLATE_NOOP("TagSymbols", "Fire")         },
    { u"👍", u"✔", QColor( 90, 200,  90), QT_TRANSLATE_NOOP("TagSymbols", "Thumbs Up")    },
    { u"❗", u"!", QColor(255, 120,  50), QT_TRANSLATE_NOOP("TagSymbols", "Exclamation")  },
    { u"🚫", u"✖", QColor(255,  90,  60), QT_TRANSLATE_NOOP("TagSymbols", "Prohibited")   },
    { u"📌", u"⚑", QColor(190, 120, 255), QT_TRANSLATE_NOOP("TagSymbols", "Pin")          },
    { u"⚡️", u"⚡", QColor(255, 235, 100), QT_TRANSLATE_NOOP("TagSymbols", "Lightning")    },
};

const SymbolDef* defFor(const QString& symbol)
{
    for (const SymbolDef& d : kSymbolDefs)
        if (symbol == QString(d.emoji))
            return &d;
    return nullptr;
}

// Canonicalize a stored symbol: text-glyph fallbacks (the pre-emoji
// palette persisted exactly these) map to their emoji; unknown → as-is
QString canonicalSymbol(const QString& symbol)
{
    for (const SymbolDef& d : kSymbolDefs) {
        if (symbol == QString(d.emoji) || symbol == QString(d.fallback))
            return QString(d.emoji);
    }
    // A short-lived version stored "none" for "show no indicator"; that is
    // now the meaning of an EMPTY value. Left in place, the string would be
    // drawn as the literal word "none" on the thumbnail.
    if (symbol == QLatin1String("none"))
        return QString();
    return symbol;
}

// caseFolded tag → symbol (lookup), caseFolded tag → display casing (persist)
QHash<QString, QString> s_symbols;
QHash<QString, QString> s_displayName;
bool s_loaded = false;

void ensureLoaded()
{
    if (s_loaded)
        return;
    const QStringList pairs = AppSettings::tagSymbolPairs();
    // Walk two at a time; a malformed trailing odd entry is skipped.
    // canonicalSymbol migrates values stored by the pre-emoji palette
    // (text glyphs like ❤ ★) to their emoji form.
    for (int i = 0; i + 1 < pairs.size(); i += 2) {
        const QString symbol = canonicalSymbol(pairs.at(i + 1));
        if (symbol.isEmpty())
            continue;      // migrated away, or never meant anything
        const QString folded = pairs.at(i).toCaseFolded();
        s_symbols.insert(folded, symbol);
        s_displayName.insert(folded, pairs.at(i));
    }
    s_loaded = true;
}

void persist()
{
    QStringList pairs;
    pairs.reserve(s_symbols.size() * 2);
    for (auto it = s_symbols.cbegin(); it != s_symbols.cend(); ++it) {
        pairs.append(s_displayName.value(it.key(), it.key()));
        pairs.append(it.value());
    }
    AppSettings::setTagSymbolPairs(pairs);
}

} // namespace

const QStringList& TagSymbols::palette()
{
    static const QStringList kPalette = [] {
        QStringList out;
        for (const SymbolDef& d : kSymbolDefs)
            out.append(QString(d.emoji));
        return out;
    }();
    return kPalette;
}

bool TagSymbols::emojiCapable()
{
    static const bool capable = [] {
        if (qEnvironmentVariableIsSet("BLITZVIEW_NO_EMOJI"))
            return false;
        // Known color emoji fonts plus a catch-all on the family name;
        // Windows always has Segoe UI Emoji, macOS Apple Color Emoji.
        static const char* kKnown[] = { "JoyPixels", "OpenMoji" };
        const QStringList families = QFontDatabase::families();
        for (const QString& f : families) {
            if (f.contains(QLatin1String("emoji"), Qt::CaseInsensitive))
                return true;
            for (const char* k : kKnown)
                if (f == QLatin1String(k))
                    return true;
        }
        return false;
    }();
    return capable;
}

QString TagSymbols::displaySymbol(const QString& symbol)
{
    if (emojiCapable())
        return symbol;
    if (const SymbolDef* d = defFor(symbol))
        return QString(d->fallback);
    return symbol;
}

QColor TagSymbols::colorFor(const QString& symbol)
{
    if (const SymbolDef* d = defFor(canonicalSymbol(symbol)))
        return d->color;
    return QColor(255, 255, 255);
}

QColor TagSymbols::badgeBackground()
{
    // Mid gray. The binding constraint is the COLOR EMOJI: ⭐ and ⚡ are
    // yellow and cannot be recolored (they ignore the pen), so the pill has
    // to be dark enough for them to separate from it — that is why Aves uses
    // black. Light enough, though, not to punch a hole into the thumbnail.
    return QColor(105, 105, 105, 200);
}

QColor TagSymbols::badgeNeutralInk()
{
    // DERIVED from the pill, never hardcoded: the pill color is the knob
    // that gets tuned, and the ink has to follow it across the light/dark
    // flip. Hardcoding it is how the neutral glyph ended up white on white.
    return badgeBackground().lightness() > 127 ? QColor( 55,  55,  55, 235)
                                               : QColor(245, 245, 245, 230);
}

QString TagSymbols::nameFor(const QString& symbol)
{
    if (const SymbolDef* d = defFor(canonicalSymbol(symbol)))
        return QCoreApplication::translate("TagSymbols", d->name);
    return {};
}

const QString& TagSymbols::neutralSymbol()
{
    static const QString kNeutral = QStringLiteral("tag");
    return kNeutral;
}

bool TagSymbols::isNeutral(const QString& symbol)
{
    return symbol == neutralSymbol();
}

bool TagSymbols::isDrawable(const QString& symbol)
{
    return isNeutral(symbol) || defFor(canonicalSymbol(symbol)) != nullptr;
}

int TagSymbols::partitionBySymbol(QStringList& tags)
{
    QStringList withSymbol, without;
    for (const QString& t : std::as_const(tags)) {
        if (isDrawable(symbolFor(t)))
            withSymbol.append(t);
        else
            without.append(t);
    }
    const int split = withSymbol.size();
    tags = withSymbol + without;
    return split;
}

QString TagSymbols::symbolFor(const QString& tag)
{
    ensureLoaded();
    return s_symbols.value(tag.toCaseFolded());
}

void TagSymbols::setSymbols(const QHash<QString, QString>& map)
{
    ensureLoaded();
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        const QString folded = it.key().toCaseFolded();
        const QString symbol = canonicalSymbol(it.value());
        if (symbol.isEmpty()) {
            s_symbols.remove(folded);
            s_displayName.remove(folded);
        } else {
            s_symbols.insert(folded, symbol);
            s_displayName.insert(folded, it.key());
        }
    }
    persist();
}

namespace {

// Tag outline pointing left (tip at left middle, rounded body corners),
// built in local coordinates around (0,0); the caller rotates it -45° so
// the tip points up-left — the Material Symbols "sell" orientation.
QPainterPath tagOutlinePath(qreal w, qreal h)
{
    const qreal notch = w * 0.38;
    const qreal round = h * 0.18;
    QPainterPath path;
    path.moveTo(-w / 2, 0);                       // tip
    path.lineTo(-w / 2 + notch, -h / 2);
    path.lineTo(w / 2 - round, -h / 2);
    path.quadTo(w / 2, -h / 2, w / 2, -h / 2 + round);
    path.lineTo(w / 2, h / 2 - round);
    path.quadTo(w / 2, h / 2, w / 2 - round, h / 2);
    path.lineTo(-w / 2 + notch, h / 2);
    path.closeSubpath();
    return path;
}

} // namespace

void TagSymbols::drawNeutralTagGlyph(QPainter* p, const QRectF& box,
                                     bool doubled, const QColor& color)
{
    // Thin outline + hole dot, like Aves' tag overlay icon. Doubled: a
    // dimmer second outline shifted up-right reads as a stack of tags.
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    const qreal s = qMin(box.width(), box.height());
    const qreal w = s * (doubled ? 0.72 : 0.82);
    const qreal h = w * 0.62;
    const qreal pen = qMax<qreal>(1.2, s * 0.09);

    QTransform base;
    base.translate(box.center().x(), box.center().y());
    if (doubled)
        base.translate(-s * 0.07, s * 0.07);
    base.rotate(-45);

    const QPainterPath tag = tagOutlinePath(w, h);
    if (doubled) {
        QTransform back = base;
        back.translate(s * 0.20, -s * 0.20);
        QColor dim = color;
        dim.setAlpha(color.alpha() / 2);
        p->setPen(QPen(dim, pen));
        p->setBrush(Qt::NoBrush);
        p->drawPath(back.map(tag));
    }
    p->setPen(QPen(color, pen));
    p->setBrush(Qt::NoBrush);
    p->drawPath(base.map(tag));
    // hole dot near the tip
    p->setPen(Qt::NoPen);
    p->setBrush(color);
    const QPointF hole = base.map(QPointF(-w / 2 + w * 0.30, 0));
    const qreal hr = qMax<qreal>(0.8, s * 0.07);
    p->drawEllipse(hole, hr, hr);
    p->restore();
}

namespace {

// Baseline offset that puts a glyph's OPTICAL center at the origin passed to
// drawText. Three routes were tried, in this order:
//
//   Qt::AlignCenter          centers the font's LINE box (ascent+descent),
//                            which symbol glyphs rarely fill symmetrically
//   tightBoundingRect()      correct for the text fallback glyphs, but WRONG
//                            for color emoji — those are bitmap glyphs and
//                            the reported box does not match what actually
//                            gets rasterized (⭐ came out low-left, ❤️ high)
//   alpha centroid           the glyph's centre of MASS. Tried and rejected:
//                            it overcorrects where the mass is lopsided —
//                            🔥 (bright wide base) drifted 1.5 px upward in a
//                            22 px badge, which reads as off-center again
//
// So: rasterize once into a scratch image, take the bounding box of what was
// really drawn, and centre THAT. Cached per (font, text); GUI thread only,
// like the rest of this file, so the cache is unguarded.
QPointF inkOffset(const QFont& font, const QString& text)
{
    static QHash<QString, QPointF> cache;
    const QString key = font.toString() + QLatin1Char('\x1f') + text;
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd())
        return *hit;

    const QFontMetricsF fm(font);
    const int pad = 4;   // room for glyphs that overshoot their advance
    QImage probe(qMax(1, qCeil(fm.horizontalAdvance(text)) + 2 * pad),
                 qMax(1, qCeil(fm.height()) + 2 * pad),
                 QImage::Format_ARGB32_Premultiplied);
    probe.fill(Qt::transparent);

    const QPointF origin(pad, pad + fm.ascent());
    {
        QPainter probePainter(&probe);
        probePainter.setFont(font);
        probePainter.setPen(Qt::black);   // opaque; color emoji ignore it
        probePainter.drawText(origin, text);
    }

    int minX = probe.width(), minY = probe.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < probe.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(probe.constScanLine(y));
        for (int x = 0; x < probe.width(); ++x) {
            if (qAlpha(row[x]) > 8) {
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
                minY = qMin(minY, y);
                maxY = qMax(maxY, y);
            }
        }
    }

    QPointF offset;   // nothing rasterized → draw uncorrected
    if (maxX >= 0) {
        offset = origin - QPointF(minX + (maxX - minX + 1) / 2.0,
                                  minY + (maxY - minY + 1) / 2.0);
    }
    cache.insert(key, offset);
    return offset;
}

} // namespace

QPointF TagSymbols::glyphPlacement(const QFont& font, const QString& text,
                                   const QPointF& centre,
                                   QPointF* achievedCentre)
{
    const QPointF offset = inkOffset(font, text);
    const QPointF ideal = centre + offset;
    const QPointF origin(qRound(ideal.x()), qRound(ideal.y()));
    if (achievedCentre)
        *achievedCentre = origin - offset;
    return origin;
}

void TagSymbols::drawGlyphCentered(QPainter* p, const QRectF& box,
                                   const QString& text)
{
    p->drawText(glyphPlacement(p->font(), text, box.center()), text);
}

QColor TagSymbols::readableOn(const QColor& color, const QColor& background)
{
    int h, s, l, a;
    color.getHsl(&h, &s, &l, &a);
    l = background.lightness() > 127 ? qMin(l, 110) : qMax(l, 165);
    QColor out;
    out.setHsl(h, s, l, a);
    return out;
}

QIcon TagSymbols::symbolIcon(const QString& symbol, int px)
{
    const qreal dpr = qGuiApp->devicePixelRatio();
    QPixmap pm(qRound(px * dpr), qRound(px * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    const QPalette pal = QGuiApplication::palette();
    const QColor background = pal.color(QPalette::Base);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (isNeutral(symbol)) {
        QColor c = pal.color(QPalette::Text);
        c.setAlpha(200);
        drawNeutralTagGlyph(&p, QRectF(px * 0.08, px * 0.08,
                                       px * 0.84, px * 0.84),
                            false, c);
    } else if (isDrawable(symbol)) {
        QFont f = p.font();
        f.setPixelSize(px - 2);        // no pill to pad against
        f.setBold(!emojiCapable());    // thin fallback glyphs ("!") need it
        p.setFont(f);
        // Color emoji ignore the pen; this only steers the fallback glyphs
        p.setPen(readableOn(colorFor(symbol), background));
        drawGlyphCentered(&p, QRectF(0, 0, px, px), displaySymbol(symbol));
    }
    // Not drawable = "no indicator": nothing is drawn at all. The pixmap is
    // still created at full size, so combo/list rows keep their icon column
    // and the labels stay aligned with the ones that do carry a glyph.
    p.end();
    return QIcon(pm);
}

QIcon TagSymbols::iconFor(const QString& tag, int px)
{
    return symbolIcon(symbolFor(tag), px);
}
