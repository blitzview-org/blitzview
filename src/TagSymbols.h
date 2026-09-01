#pragma once

#include <QColor>
#include <QHash>
#include <QIcon>
#include <QPainterPath>
#include <QString>
#include <QStringList>

// Global tag → special-symbol map, persisted via
// AppSettings::tagSymbolPairs. Symbols are per-tag configuration, NOT
// per-file metadata — MediaMetadata/.bvm are unaffected.
// The palette is a fixed set of color emoji (canonical form, this is what
// gets stored). Qt has no bundled emoji — rendering relies on a system
// emoji font (Linux: noto-fonts-emoji; Windows: Segoe UI Emoji, always
// present). Without one, every palette entry degrades to a DejaVu-covered
// TEXT glyph drawn in an explicit per-symbol color (see displaySymbol);
// BLITZVIEW_NO_EMOJI=1 forces that fallback for testing.
// Lookup is case-folded so casing variants of a tag ("Beach"/"beach", which
// can coexist in files) share one symbol; the stored display casing is
// whatever was configured last.
// GUI-thread only (delegate paint + dialogs) — no locking.
class TagSymbols
{
public:
    TagSymbols() = delete;

    // Fixed curated symbol choices offered by TagSettingsPage
    // (canonical emoji strings)
    static const QStringList& palette();

    // True when a color emoji font is available (cached; overridable with
    // BLITZVIEW_NO_EMOJI=1)
    static bool emojiCapable();

    // What to actually draw for a canonical symbol: the emoji itself, or
    // its text-glyph fallback when no emoji font exists. Unknown symbols
    // pass through unchanged.
    static QString displaySymbol(const QString& symbol);

    // Pen color for a palette symbol's FALLBACK glyph (white for unknown
    // symbols; color emoji ignore the pen, setting it is harmless)
    static QColor colorFor(const QString& symbol);

    // Translatable display name of a palette symbol ("Heart", "Star", …);
    // empty for symbols outside the palette
    static QString nameFor(const QString& symbol);

    // Clamps a symbol color against the surface it is drawn on: keeps the
    // HUE (that is what the color encodes) and moves only the lightness —
    // down on light surfaces, up on dark ones. Needed wherever the backdrop
    // is not dark, i.e. the light grid pill and the backdrop-less widget
    // icons; bright gold would otherwise wash out and white vanish.
    // Color emoji ignore the pen and are unaffected.
    static QColor readableOn(const QColor& color, const QColor& background);

    // Backdrop of the thumbnail badges. Light and neutral: the palette's
    // fallback glyphs read as dark ink, and a light pill separates them
    // from a photo as well as a dark one did.
    static QColor badgeBackground();
    // Ink for the badge elements that have no palette color of their own
    // (neutral tag glyph, the "…" overflow marker).
    static QColor badgeNeutralInk();

    // Sentinel selecting the neutral tag glyph (drawNeutralTagGlyph) as a
    // tag's symbol. It is an explicit CHOICE, not the default — the default
    // (empty symbol) shows no indicator at all. Plain ASCII, so it can never
    // collide with a palette emoji or with a pre-emoji text glyph, and stays
    // readable in the settings file.
    static const QString& neutralSymbol();
    static bool isNeutral(const QString& symbol);

    // True when the symbol is something this app can actually DRAW: a
    // palette emoji or the neutral sentinel. Anything else — empty, or a
    // stale value written by an older version — means "no indicator" and
    // must NEVER reach drawText, or it shows up as literal text on the
    // thumbnail (this happened with a "none" sentinel).
    static bool isDrawable(const QString& symbol);

    // Reorders `tags` so that those carrying a drawable symbol come first,
    // each group keeping its original order, and returns the SIZE of that
    // first group (i.e. the index where the symbol-less tags start).
    // Shared by every list that shows tags with their symbols.
    static int partitionBySymbol(QStringList& tags);

    // Symbol configured for the tag; empty = no indicator (the default),
    // neutralSymbol() = neutral tag glyph
    static QString symbolFor(const QString& tag);

    // Persist the given tag → symbol assignments (empty symbol = remove).
    // Mappings for tags NOT in the map are kept: knownTags never shrinks,
    // and a vanished tag regaining its symbol on reappearance is desirable.
    static void setSymbols(const QHash<QString, QString>& map);

    // Neutral tag glyph, Aves-style (Material Symbols "sell" look): thin
    // OUTLINE of a 45°-rotated luggage tag with a hole dot; doubled =
    // second outline offset behind for "several tags". Shared by
    // GridDelegate (grid badges) and symbolIcon (dialog icons).
    static void drawNeutralTagGlyph(QPainter* p, const QRectF& box,
                                    bool doubled, const QColor& color);

    // Baseline origin at which `text` must be drawn for its optical centre to
    // land as close as possible to `centre`, with the origin SNAPPED to whole
    // pixels — color emoji are bitmaps and blur when placed on a fraction.
    // Snapping leaves a residual of up to half a pixel, which is unavoidable
    // anyway whenever ink width and badge diameter differ in parity (13 in
    // 22 → margins 4 and 5, never 4.5). `achievedCentre` reports where the
    // ink really ends up, so a caller that also draws the BADGE can move the
    // badge instead of blurring the glyph.
    static QPointF glyphPlacement(const QFont& font, const QString& text,
                                  const QPointF& centre,
                                  QPointF* achievedCentre = nullptr);

    // Convenience for callers with nothing to align to (dialog icons):
    // glyphPlacement + drawText.
    static void drawGlyphCentered(QPainter* p, const QRectF& box,
                                  const QString& text);

    // Icon for list/combo widgets: the bare symbol glyph (neutralSymbol() =
    // neutral tag shape, empty symbol = fully blank icon — "no indicator" is
    // shown as nothing, but still as a correctly SIZED pixmap so combo rows
    // stay aligned), NO dark backdrop — the pill belongs on thumbnails,
    // where it separates the glyph from the photo; on a widget surface it
    // just adds noise. Colors are clamped against the palette instead.
    static QIcon symbolIcon(const QString& symbol, int px);

    // Convenience: symbolIcon of the tag's configured symbol
    static QIcon iconFor(const QString& tag, int px);
};
