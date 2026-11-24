#include "ImageCanvas.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

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
    m_view.setViewportSize(size());
    m_view.setContentSize(m_pixmap.size());
    m_view.fit();   // every image opens fitted
    update();
}

void ImageCanvas::setErrorText(const QString& text)
{
    m_pixmap = QPixmap();
    m_errorText = text;
    m_view.setContentSize(QSizeF());
    update();
}

void ImageCanvas::fitToWindow()
{
    m_view.fit();
    update();
}

bool ImageCanvas::zoomAt(const QPointF& cursor, double factor)
{
    const ViewTransform::ZoomStep step = m_view.zoomAt(cursor, factor);
    if (!step.changed)
        return false;
    ViewTransform::traceZoomStep("viewer", cursor, step, m_view);
    update();
    return true;
}

void ImageCanvas::panBy(const QPointF& delta)
{
    if (m_view.panBy(delta))
        update();
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
    const QRect src = m_view.visibleContentRect().intersected(QRectF(m_pixmap.rect()))
                          .toAlignedRect().intersected(m_pixmap.rect());
    if (src.isEmpty())
        return;

    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setTransform(m_view.transform(), true);
    p.drawPixmap(QRectF(src), m_pixmap, QRectF(src));
}

void ImageCanvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_view.setViewportSize(size());
}
