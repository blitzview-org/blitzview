#pragma once

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>

// The video play badge: a thin white ring over a lightly darkened disc,
// with a filled equilateral triangle. ONE drawing for the grid, the list
// view and the viewer's end-of-video state so they stay identical; change
// proportions or colours here only.
//
// Geometry (all relative to `radius`, in logical px):
//   - ring stroke 8 % of the radius, at least 1.5 px
//   - triangle height = radius (half the diameter), width = height·√3/2
//   - the triangle is shifted right by 1/6 of its width so its CENTROID,
//     not its bounding box, sits on the disc centre — a triangle centred
//     by bounding box reads as sitting too far left
//
// Leaves pen/brush as set (callers reconfigure them anyway) and antialiasing
// OFF afterwards, like the delegate code it replaced.
inline void drawPlayBadge(QPainter* painter, const QPointF& centre, qreal radius)
{
    const qreal stroke = qMax(1.5, radius * 0.08);
    painter->setRenderHint(QPainter::Antialiasing, true);

    // Ring: stroke centred on the radius, disc fill inside it
    painter->setPen(QPen(QColor(255, 255, 255, 210), stroke));
    painter->setBrush(QColor(0, 0, 0, 70));
    painter->drawEllipse(centre, radius - stroke / 2, radius - stroke / 2);

    const qreal h = radius;
    const qreal w = h * 0.8660254;   // equilateral: width = height·√3/2
    const qreal x0 = centre.x() - w / 2 + w / 6;
    QPainterPath path;
    path.moveTo(x0,     centre.y() - h / 2);
    path.lineTo(x0 + w, centre.y());
    path.lineTo(x0,     centre.y() + h / 2);
    path.closeSubpath();
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(255, 255, 255, 220));
    painter->drawPath(path);

    painter->setRenderHint(QPainter::Antialiasing, false);
}

// Badge radius for a thumbnail whose shorter side is `side`: 21 % of it.
inline qreal playBadgeRadiusFor(int side)
{
    return side * 0.21;
}
