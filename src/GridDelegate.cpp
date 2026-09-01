#include "GridDelegate.h"

#include "TagSymbols.h"
#include "FocusFrame.h"

#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QStyleOptionViewItem>
#include <QApplication>

GridDelegate::GridDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void GridDelegate::setIconSize(int size)
{
    m_iconSize = size;
}

void GridDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                         const QModelIndex& index) const
{
    painter->save();

    QStyle* style = QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, painter);

    QRect rect = option.rect;
    const bool selected = option.state & QStyle::State_Selected;

    // Vertical cell layout (must mirror sizeHint): 6px pad, icon area,
    // 4px gap, one text line, 6px pad. Symmetric pads: a portrait image
    // (full icon height) keeps the same distance to the frame at the top
    // as the label at the bottom.
    QRect thumbRect(rect.x() + 4, rect.y() + 6, m_iconSize, m_iconSize);
    thumbRect.moveLeft(rect.x() + (rect.width() - m_iconSize) / 2);

    // Letterbox areas around non-square images: keep the selection highlight
    // painted by PE_PanelItemViewItem instead of overwriting it with Base —
    // the whole cell must appear in the selection color.
    if (!selected)
        painter->fillRect(thumbRect, option.palette.brush(QPalette::Base));

    const QPixmap scaledPixmap = index.data(Qt::UserRole + 4).value<QPixmap>();
    const int scaledSizeKey = index.data(Qt::UserRole + 5).toInt();
    const QStringList tags = index.data(Qt::UserRole + 6).toStringList();

    if (!scaledPixmap.isNull() && scaledSizeKey == m_iconSize) {
        // Pixmaps carry the screen scale — geometry uses LOGICAL pixels
        QRect dest(QPoint(0, 0),
                   scaledPixmap.deviceIndependentSize().toSize());
        dest.moveCenter(thumbRect.center());
        painter->drawPixmap(dest, scaledPixmap);

        if (index.data(Qt::UserRole + 3).toBool()) {
            drawVideoOverlay(painter, dest);
        }
        if (!tags.isEmpty())
            drawTagBadges(painter, dest, tags, option.font);
    } else {
        QPixmap px = index.data(Qt::DecorationRole).value<QPixmap>();
        if (!px.isNull()) {
            // Use fast scaling in the paint fallback path -- the pre-scaled
            // version with SmoothTransformation will replace this shortly.
            // Scale in DEVICE pixels and re-tag the screen scale so the
            // interim thumb is not blurred on HiDPI screens.
            const qreal dpr = painter->device()->devicePixelRatio();
            QPixmap scaled = px.scaled(thumbRect.size() * dpr,
                                       Qt::KeepAspectRatio,
                                       Qt::FastTransformation);
            scaled.setDevicePixelRatio(dpr);
            QRect dest(QPoint(0, 0), scaled.deviceIndependentSize().toSize());
            dest.moveCenter(thumbRect.center());
            painter->drawPixmap(dest, scaled);

            if (index.data(Qt::UserRole + 3).toBool()) {
                drawVideoOverlay(painter, dest);
            }
            if (!tags.isEmpty())
                drawTagBadges(painter, dest, tags, option.font);
        }
    }

    QString name = index.data(Qt::DisplayRole).toString();
    if (name.isEmpty())
        name = index.siblingAtColumn(1).data(Qt::DisplayRole).toString();

    QFontMetrics fm(option.font);
    QRect labelRect(rect.x() + 2, rect.y() + 6 + m_iconSize + 4,
                    rect.width() - 4, fm.height());
    name = fm.elidedText(name, Qt::ElideRight, labelRect.width());
    painter->setPen(option.palette.color(
        selected ? QPalette::HighlightedText : QPalette::Text));
    painter->setFont(option.font);
    painter->drawText(labelRect, Qt::AlignHCenter | Qt::AlignTop, name);

    // Hover focus frame — shared with the list view, see FocusFrame.h
    if (option.state & QStyle::State_MouseOver)
        paintFocusFrame(painter, rect, option.palette);

    painter->restore();
}

void GridDelegate::drawVideoOverlay(QPainter* painter, const QRect& dest) const
{
    const int r = qMin(dest.width(), dest.height()) * 3 / 10;
    const QPoint c = dest.center();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setBrush(QColor(0, 0, 0, 120));
    painter->setPen(Qt::NoPen);
    painter->drawEllipse(c, r, r);
    const int tri = r * 6 / 10;
    const int offX = tri / 4;
    QPainterPath path;
    path.moveTo(c.x() - tri/2 + offX, c.y() - tri);
    path.lineTo(c.x() + tri   + offX, c.y());
    path.lineTo(c.x() - tri/2 + offX, c.y() + tri);
    path.closeSubpath();
    painter->setBrush(QColor(255, 255, 255, 200));
    painter->drawPath(path);
    painter->setRenderHint(QPainter::Antialiasing, false);
}

void GridDelegate::drawTagBadges(QPainter* painter, const QRect& dest,
                                 const QStringList& tags,
                                 const QFont& uiFont) const
{
    // Special symbols of the file's tags, deduped, tag order preserved.
    // A tag with NO symbol configured is invisible — that is the default,
    // so an untouched tag produces no badge. Tags set to the neutral glyph
    // are counted separately: they yield one neutral badge, doubled when
    // several of them are present. Mixed case: symbols only.
    QStringList symbols;
    int neutralTags = 0;
    for (const QString& t : tags) {
        const QString s = TagSymbols::symbolFor(t);
        if (!TagSymbols::isDrawable(s))
            continue;      // unset, or a stale value — never draw it as text
        if (TagSymbols::isNeutral(s)) {
            ++neutralTags;
        } else if (!symbols.contains(s)) {
            symbols.append(s);
        }
    }
    // Nothing configured on any of the file's tags → no badge row at all
    if (symbols.isEmpty() && neutralTags == 0)
        return;

    // Aves-style: one small round pill per icon instead of a shared bar —
    // reads much lighter. Logical px throughout (HiDPI convention).
    //
    // The size is CONSTANT across zoom, like the filename label — a marker
    // whose job is "this file is tagged" gains nothing from growing with the
    // thumbnail. It is tied to the UI FONT rather than to a fixed pixel
    // count, for the same reason the label is: change the system font size
    // and badge and caption keep their proportion.
    //
    // It used to be 13% of the thumbnail, clamped to 11..18 — which in
    // practice meant it sat pinned at one clamp or the other and only moved
    // between roughly 8 and 11 columns.
    const int box   = qBound(11, qRound(QFontMetricsF(uiFont).height()), 24);
    const int pad   = 2;              // pill padding around the glyph
    const int d     = box + 2 * pad;  // pill diameter
    const int gap   = 1;              // between pills (Aves: 1px margins)
    const int inset = 3;              // distance from dest edges
    constexpr int kMaxSymbols = 3;    // overflow → extra pill shows "…"

    const bool neutral = symbols.isEmpty();

    // A tagged file must stay recognizable at EVERY zoom level — like its
    // filename, which gets elided but never dropped. So a badge row that
    // does not fit is shortened, never skipped: drop pills until the row
    // fits, keeping at least one. (This used to bail out entirely below
    // 2*d image height, which made small thumbnails lose their marker.)
    const int fits = qMax(1, (dest.width() - 2 * inset + gap) / (d + gap));

    int shown = 0;
    int count = 1;
    bool overflow = false;
    if (!neutral) {
        shown = qMin(int(symbols.size()), kMaxSymbols);
        overflow = symbols.size() > shown;
        count = shown + (overflow ? 1 : 0);
        if (count > fits) {
            count = fits;
            // The "…" pill only earns its slot while a real symbol remains
            overflow = count > 1 && symbols.size() > count - 1;
            shown = overflow ? count - 1 : count;
        }
    }

    // Bottom-left of dest: clear of the centered video-play circle; dest
    // lies inside thumbRect, so also clear of the 2px hover ring
    int x = dest.left() + inset;
    const int y = dest.bottom() - inset - d + 1;

    painter->setRenderHint(QPainter::Antialiasing, true);
    QFont f = painter->font();
    // Sized against the PILL DIAMETER, not the glyph box: a circle of
    // diameter d only holds a square of d/√2 ≈ 0.71·d, so `box - 1` (up to
    // 0.77·d at large badges) made the glyph poke out of the disc — unevenly
    // top/bottom, which reads as "not centered" even though it is.
    f.setPixelSize(qMax(8, qRound(d * 0.66)));
    f.setBold(!TagSymbols::emojiCapable());   // thin fallback glyphs
    painter->setFont(f);

    // Light neutral pill (Aves uses a dark one) — the palette's fallback
    // glyphs are designed as dark ink, so everything drawn on it must be
    // dark too: neutral ink for the glyphs without a color of their own,
    // and the palette colors clamped against the pill.
    const QColor pill = TagSymbols::badgeBackground();
    const QColor neutralInk = TagSymbols::badgeNeutralInk();

    for (int i = 0; i < count; ++i, x += d + gap) {
        QString glyph;
        QColor ink = neutralInk;
        if (!neutral) {
            if (i < shown) {
                glyph = TagSymbols::displaySymbol(symbols.at(i));
                ink = TagSymbols::readableOn(
                    TagSymbols::colorFor(symbols.at(i)), pill);
            } else {
                glyph = QStringLiteral("…");   // overflow marker
            }
        }

        // The PILL follows the glyph, not the other way around. Ink width and
        // pill diameter usually differ in parity (13 in 22 leaves 9 to split,
        // i.e. margins 4 and 5 — 4.5 does not exist), so one of them has to
        // sit on a half pixel. The glyph must not: color emoji are bitmaps
        // and blur when interpolated. The antialiased disc shifts for free.
        QPointF centre(x + d / 2.0, y + d / 2.0);
        QPointF glyphOrigin;
        if (!glyph.isEmpty())
            glyphOrigin = TagSymbols::glyphPlacement(painter->font(), glyph,
                                                     centre, &centre);

        painter->setPen(Qt::NoPen);
        painter->setBrush(pill);
        painter->drawEllipse(centre, d / 2.0, d / 2.0);

        painter->setPen(ink);
        if (glyph.isEmpty()) {
            TagSymbols::drawNeutralTagGlyph(
                painter, QRectF(centre.x() - box / 2.0, centre.y() - box / 2.0,
                                box, box),
                neutralTags > 1, ink);
        } else {
            painter->drawText(glyphOrigin, glyph);
        }
    }
    painter->setRenderHint(QPainter::Antialiasing, false);
}

QSize GridDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const
{
    // Must mirror the vertical layout in paint(): 6 + icon + 4 + text + 6
    return QSize(m_iconSize + 8,
                 m_iconSize + QFontMetrics(option.font).height() + 16);
}
