#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QTransform>

class QWidget;

// Zoom/pan viewport maths shared by the viewer pages (ImageCanvas,
// VideoSurface): a piece of CONTENT of `contentSize` (in content pixels —
// image pixels, or the ORIENTED video frame size) shown in a viewport of
// `viewportSize` logical pixels through `translate(origin) · scale(scale)`.
// Pure value type, no Qt object: the owning widget feeds it sizes and input
// and paints through transform().
//
// Rules (user contract, see the viewer-interaction doc):
// - fit: scale = min(vw/cw, vh/ch), centred — small content is upscaled;
//   while fitted, viewport/content size changes refit automatically.
// - zoom range [fit, max(10, fit)]; reaching the minimum snaps into fit
//   mode (an anchored clamp would leave the content off-centre). Fuzzy,
//   because fit × 1.25 / 1.25 is not bit-identical to fit.
// - anchor: the content point under the cursor stays under the cursor,
//   then the edge rule is applied.
// - edge / centre rule per axis: content larger than the viewport on an
//   axis → origin clamped to [view − content, 0] (no empty space); smaller
//   → centred on that axis.
class ViewTransform
{
public:
    // Result of one zoom step, mainly for the trace line of the caller
    struct ZoomStep {
        bool    changed      = false;
        bool    snappedToFit = false;
        double  oldScale     = 1.0;
        QPointF anchor;         // content point under the cursor (anchored step)
        QPointF wantedOrigin;   // origin before the edge rule (anchored step)
    };

    bool    hasContent() const   { return !m_content.isEmpty(); }
    QSizeF  contentSize() const  { return m_content; }
    QSizeF  viewportSize() const { return m_viewport; }
    double  scale() const        { return m_scale; }
    QPointF origin() const       { return m_origin; }
    bool    isFitted() const     { return m_fitted; }
    double  fitScale() const;
    double  maxScale() const;

    // Painter transform: content coordinates -> viewport logical pixels
    QTransform transform() const;
    // Viewport rectangle in content coordinates (what is visible)
    QRectF visibleContentRect() const;

    // Viewport resized: fitted stays fitted; zoomed below the new fit snaps
    // to fit; otherwise the scale is kept and the edge rule re-applied
    void setViewportSize(const QSizeF& size);
    // Content changed size (new image, video frame size known/changed):
    // same policy as setViewportSize. An empty size resets to identity.
    void setContentSize(const QSizeF& size);

    // Enter fit mode (F key, every new medium)
    void fit();
    // One zoom step by `factor` with the content point under `cursor`
    // (viewport coords) fixed; clamps to the zoom range
    ZoomStep zoomAt(const QPointF& cursor, double factor);
    // Pan by a viewport-space delta, clamped. No-op while fitted.
    // Returns whether the origin changed.
    bool panBy(const QPointF& delta);
    // Re-applies the per-axis edge/centre rule to the origin
    void clampOrigin();

    // Trace helper (BLITZVIEW_TRACE_SLIDE): logs "<tag> zoom step ..." in
    // the documented format; `tag` distinguishes the pages
    static void traceZoomStep(const char* tag, const QPointF& cursor,
                              const ZoomStep& step, const ViewTransform& after);

private:
    QSizeF  m_content;
    QSizeF  m_viewport;
    double  m_scale  = 1.0;    // content px -> logical px
    QPointF m_origin;          // content top-left in viewport logical px
    bool    m_fitted = true;
};

// Common face of the viewer pages that own a ViewTransform (ImageCanvas,
// VideoSurface), so the MediaViewer's wheel/mouse handlers drive the image
// and the video page through one pointer. Not a QObject on purpose: the
// pages stay plain QWidget subclasses.
class ZoomPanPage
{
public:
    virtual ~ZoomPanPage() = default;
    virtual QWidget* pageWidget() = 0;
    virtual const ViewTransform& view() const = 0;
    // Fit mode (F key / every new medium)
    virtual void fitToWindow() = 0;
    // One zoom step by `factor` keeping the content point under `cursor`
    // (page widget coords) fixed. Returns false if nothing changed.
    virtual bool zoomAt(const QPointF& cursor, double factor) = 0;
    // Pan by a widget-space delta, clamped (no-op while fitted)
    virtual void panBy(const QPointF& delta) = 0;
};
