#pragma once

#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QRect>
#include <QVector>

// The hover FOCUS FRAME, shared by the grid cells and the list rows.
//
// Two-tone dashed (Window/Highlight alternating), the same technique as the
// DirectoryPanel context-menu marker — no single color contrasts with both
// the neutral Base and the Highlight selection background in every theme,
// alternating segments do. Square corners and no antialiasing: crisp
// pixels, matching the square selection panel. Selected items need no frame
// of their own; their State_Selected background already marks them.
//
// `rect` is the full cell/row rect — the 2px pen is centered on the inset
// line so it covers the outer two pixels.
inline void paintFocusFrame(QPainter* painter, const QRect& rect,
                            const QPalette& palette)
{
    const QRect frameRect = rect.adjusted(1, 1, -1, -1);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setBrush(Qt::NoBrush);
    // Dash pattern is in pen-width units: 3 × 2px = 6px segments
    const QVector<qreal> dash{3, 3};
    QPen p1(palette.color(QPalette::Window), 2);
    p1.setDashPattern(dash);
    p1.setDashOffset(0);
    painter->setPen(p1);
    painter->drawRect(frameRect);
    QPen p2(palette.color(QPalette::Highlight), 2);
    p2.setDashPattern(dash);
    p2.setDashOffset(3);
    painter->setPen(p2);
    painter->drawRect(frameRect);
    painter->restore();
}
