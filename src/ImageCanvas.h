#pragma once

#include <QPixmap>
#include <QPointF>
#include <QString>
#include <QWidget>

// Image page of the MediaViewer: paints one pixmap through a scale/offset
// transform instead of re-scaling it per zoom step. The pixmap is kept at its
// NATIVE pixel size (devicePixelRatio 1) and drawn through the painter's
// logical transform, so m_scale is "image pixels -> logical pixels": 1.0 is
// one image pixel per logical pixel on every DPR, and the device-pixel
// resampling happens once, in the paint.
//
// Pure view: it owns the zoom/pan STATE and the clamp rules, but no input
// handling — wheel and mouse events deliberately propagate to the
// MediaViewer, which also receives the ignored Ctrl+presses from the edge
// zones (siblings of the stack, so their events can never reach this widget
// directly).
//
// Layout contract per axis: an image larger than the viewport is clamped so
// no empty space shows on that axis; a smaller one is centred on that axis.
// Zoom range: [fit scale, max(10, fit scale)].
class ImageCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit ImageCanvas(QWidget* parent = nullptr);

    // New image: always opens FITTED (zoom never persists across images)
    void setImage(const QImage& image);
    // No image, show this message instead (load failure)
    void setErrorText(const QString& text);

    bool hasImage() const { return !m_pixmap.isNull(); }
    bool isFitted() const { return m_fitted; }
    double scale() const  { return m_scale; }
    QPointF origin() const { return m_origin; }
    double fitScale() const;
    double maxScale() const;

    // Fit mode: scale = min(w/iw, h/ih) (small images are upscaled),
    // centred; resizes refit while fitted. F key / every new image.
    void fitToWindow();
    // One zoom step by `factor` keeping the image point under `cursor`
    // (widget coords) fixed. Clamps to the zoom range; reaching the fit
    // scale drops back into fit mode. Returns false if nothing changed.
    bool zoomAt(const QPointF& cursor, double factor);
    // Pan by a widget-space delta, clamped (no-op while fitted)
    void panBy(const QPointF& delta);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // Applies the per-axis clamp/centre rule to m_origin
    void clampOrigin();

    QPixmap m_pixmap;
    QString m_errorText;
    double  m_scale  = 1.0;      // image px -> logical px
    QPointF m_origin;            // image top-left in widget logical px
    bool    m_fitted = true;
};
