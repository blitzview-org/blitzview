#include "ImageCanvas.h"

#include "SlideTrace.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <algorithm>
#include <cmath>

ImageCanvas::ImageCanvas(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::NoFocus);
}

void ImageCanvas::setImage(const QImage& image)
{
    m_errorText.clear();
    m_pixmap = QPixmap::fromImage(image);
    // Native pixels, never a DPR-scaled pixmap: the paint transform does the
    // logical->device mapping (see header)
    m_pixmap.setDevicePixelRatio(1.0);
    m_fitted = true;
    fitToWindow();
}

void ImageCanvas::setErrorText(const QString& text)
{
    m_pixmap = QPixmap();
    m_errorText = text;
    m_fitted = true;
    m_scale = 1.0;
    m_origin = QPointF();
    update();
}

double ImageCanvas::fitScale() const
{
    if (m_pixmap.isNull() || width() <= 0 || height() <= 0)
        return 1.0;
    return std::min(double(width()) / m_pixmap.width(),
                    double(height()) / m_pixmap.height());
}

double ImageCanvas::maxScale() const
{
    // 10x the original — but never below the fit scale (tiny image in a
    // huge window), otherwise the range would be empty
    return std::max(10.0, fitScale());
}

void ImageCanvas::fitToWindow()
{
    m_fitted = true;
    if (m_pixmap.isNull())
        return;
    m_scale = fitScale();
    clampOrigin();   // smaller than or equal to the viewport on both axes: centres
    update();
}

bool ImageCanvas::zoomAt(const QPointF& cursor, double factor)
{
    if (m_pixmap.isNull() || factor <= 0.0)
        return false;

    const double fit    = fitScale();
    const double target = std::clamp(m_scale * factor, fit, maxScale());
    if (qFuzzyCompare(target, m_scale))
        return false;

    if (target <= fit || qFuzzyCompare(target, fit)) {
        // Zooming out onto the minimum: a clamped anchor would still drift
        // off-centre — fit IS the defined state at the minimum. Fuzzy: a
        // zoom-in/zoom-out pair lands on fit × 1.25 / 1.25, not exactly fit
        const double oldScale = m_scale;
        fitToWindow();
        TRACE_SLIDE("viewer zoom step cursor=(%.1f,%.1f) scale %.4f->%.4f (fit)",
                    cursor.x(), cursor.y(), oldScale, m_scale);
        return true;
    }

    // Anchor: the image point under the cursor stays under the cursor
    const QPointF imgPt = (cursor - m_origin) / m_scale;
    const QPointF wanted = cursor - imgPt * target;
    const double oldScale = m_scale;
    m_scale  = target;
    m_origin = wanted;
    m_fitted = false;
    clampOrigin();
    TRACE_SLIDE("viewer zoom step cursor=(%.1f,%.1f) img=(%.2f,%.2f) "
                "scale %.4f->%.4f origin (%.1f,%.1f)->(%.1f,%.1f)",
                cursor.x(), cursor.y(), imgPt.x(), imgPt.y(), oldScale, m_scale,
                wanted.x(), wanted.y(), m_origin.x(), m_origin.y());
    update();
    return true;
}

void ImageCanvas::panBy(const QPointF& delta)
{
    if (m_pixmap.isNull() || m_fitted)
        return;
    const QPointF before = m_origin;
    m_origin += delta;
    clampOrigin();
    if (m_origin != before)
        update();
}

void ImageCanvas::clampOrigin()
{
    if (m_pixmap.isNull())
        return;
    const double w = m_pixmap.width() * m_scale;
    const double h = m_pixmap.height() * m_scale;
    // Larger than the viewport: no empty space on that axis (origin between
    // viewport-size - image-size and 0). Smaller: centred.
    if (w > width())
        m_origin.setX(std::clamp(m_origin.x(), width() - w, 0.0));
    else
        m_origin.setX((width() - w) / 2.0);
    if (h > height())
        m_origin.setY(std::clamp(m_origin.y(), height() - h, 0.0));
    else
        m_origin.setY((height() - h) / 2.0);
}

void ImageCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    if (m_pixmap.isNull()) {
        if (!m_errorText.isEmpty()) {
            p.setPen(QColor(0xdd, 0xdd, 0xdd));
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_errorText);
        }
        return;
    }

    // Only the visible part of the source is drawn (large images at high
    // zoom): intersect the widget with the image in IMAGE coordinates,
    // expanded to whole pixels so the resampler sees the real neighbours.
    const QRectF imgRectF(0, 0, m_pixmap.width(), m_pixmap.height());
    const QRectF visibleImg = QRectF((QPointF(0, 0) - m_origin) / m_scale,
                                     QSizeF(width(), height()) / m_scale);
    const QRect src = visibleImg.intersected(imgRectF).toAlignedRect()
                          .intersected(m_pixmap.rect());
    if (src.isEmpty())
        return;

    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.translate(m_origin);
    p.scale(m_scale, m_scale);
    p.drawPixmap(QRectF(src), m_pixmap, QRectF(src));
}

void ImageCanvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_pixmap.isNull())
        return;
    const double fit = fitScale();
    if (m_fitted || m_scale <= fit || qFuzzyCompare(m_scale, fit))
        fitToWindow();          // fitted stays fitted; zoomed below the new fit snaps to it
    else
        clampOrigin();          // zoomed: keep the scale, re-apply the edge rule
}
