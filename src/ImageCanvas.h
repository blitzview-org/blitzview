#pragma once

#include <QPixmap>
#include <QPointF>
#include <QString>
#include <QWidget>
#include "ViewTransform.h"

// Image page of the MediaViewer: paints one pixmap through a scale/offset
// transform instead of re-scaling it per zoom step. The pixmap is kept at its
// NATIVE pixel size (devicePixelRatio 1) and drawn through the painter's
// logical transform, so the scale is "image pixels -> logical pixels": 1.0 is
// one image pixel per logical pixel on every DPR, and the device-pixel
// resampling happens once, in the paint.
//
// Pure view: it owns the zoom/pan STATE (a ViewTransform, which also holds
// the clamp rules), but no input handling — wheel and mouse events
// deliberately propagate to the MediaViewer, which also receives the ignored
// Ctrl+presses from the edge zones (siblings of the stack, so their events
// can never reach this widget directly).
class ImageCanvas : public QWidget, public ZoomPanPage
{
    Q_OBJECT
public:
    explicit ImageCanvas(QWidget* parent = nullptr);

    // New image: always opens FITTED (zoom never persists across images)
    void setImage(const QImage& image);
    // No image, show this message instead (load failure)
    void setErrorText(const QString& text);

    bool hasImage() const { return !m_pixmap.isNull(); }
    bool isFitted() const { return m_view.isFitted(); }
    double scale() const  { return m_view.scale(); }
    QPointF origin() const { return m_view.origin(); }

    // ZoomPanPage
    QWidget* pageWidget() override { return this; }
    const ViewTransform& view() const override { return m_view; }
    void fitToWindow() override;
    bool zoomAt(const QPointF& cursor, double factor) override;
    void panBy(const QPointF& delta) override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QPixmap       m_pixmap;
    QString       m_errorText;
    ViewTransform m_view;
};
