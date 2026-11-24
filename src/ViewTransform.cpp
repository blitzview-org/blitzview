#include "ViewTransform.h"

#include "SlideTrace.h"

#include <algorithm>

namespace {
// "a is at or below b" with the tolerance the fit comparisons need
bool fuzzyAtOrBelow(double a, double b)
{
    return a <= b || qFuzzyCompare(a, b);
}
}

double ViewTransform::fitScale() const
{
    if (m_content.isEmpty() || m_viewport.isEmpty())
        return 1.0;
    return std::min(m_viewport.width() / m_content.width(),
                    m_viewport.height() / m_content.height());
}

double ViewTransform::maxScale() const
{
    // 10x the original — but never below the fit scale (tiny content in a
    // huge viewport), otherwise the range would be empty
    return std::max(10.0, fitScale());
}

QTransform ViewTransform::transform() const
{
    QTransform t;
    t.translate(m_origin.x(), m_origin.y());
    t.scale(m_scale, m_scale);
    return t;
}

QRectF ViewTransform::visibleContentRect() const
{
    return QRectF((QPointF(0, 0) - m_origin) / m_scale, m_viewport / m_scale);
}

void ViewTransform::setViewportSize(const QSizeF& size)
{
    m_viewport = size;
    if (!hasContent())
        return;
    if (m_fitted || fuzzyAtOrBelow(m_scale, fitScale()))
        fit();              // fitted stays fitted; zoomed below the new fit snaps to it
    else
        clampOrigin();      // zoomed: keep the scale, re-apply the edge rule
}

void ViewTransform::setContentSize(const QSizeF& size)
{
    m_content = size;
    if (!hasContent()) {
        m_fitted = true;
        m_scale  = 1.0;
        m_origin = QPointF();
        return;
    }
    if (m_fitted || fuzzyAtOrBelow(m_scale, fitScale()))
        fit();
    else
        clampOrigin();
}

void ViewTransform::fit()
{
    m_fitted = true;
    if (!hasContent())
        return;
    m_scale = fitScale();
    clampOrigin();   // at most viewport-sized on both axes: centres
}

ViewTransform::ZoomStep ViewTransform::zoomAt(const QPointF& cursor, double factor)
{
    ZoomStep step;
    step.oldScale = m_scale;
    if (!hasContent() || factor <= 0.0)
        return step;

    const double fitS   = fitScale();
    const double target = std::clamp(m_scale * factor, fitS, maxScale());
    if (qFuzzyCompare(target, m_scale))
        return step;

    step.changed = true;
    if (fuzzyAtOrBelow(target, fitS)) {
        // Zooming out onto the minimum: a clamped anchor would still drift
        // off-centre — fit IS the defined state at the minimum. Fuzzy: a
        // zoom-in/zoom-out pair lands on fit × 1.25 / 1.25, not exactly fit
        step.snappedToFit = true;
        fit();
        return step;
    }

    // Anchor: the content point under the cursor stays under the cursor
    step.anchor       = (cursor - m_origin) / m_scale;
    step.wantedOrigin = cursor - step.anchor * target;
    m_scale  = target;
    m_origin = step.wantedOrigin;
    m_fitted = false;
    clampOrigin();
    return step;
}

bool ViewTransform::panBy(const QPointF& delta)
{
    if (!hasContent() || m_fitted)
        return false;
    const QPointF before = m_origin;
    m_origin += delta;
    clampOrigin();
    return m_origin != before;
}

void ViewTransform::clampOrigin()
{
    if (!hasContent())
        return;
    const double w  = m_content.width() * m_scale;
    const double h  = m_content.height() * m_scale;
    const double vw = m_viewport.width();
    const double vh = m_viewport.height();
    // Larger than the viewport: no empty space on that axis (origin between
    // viewport-size - content-size and 0). Smaller: centred.
    if (w > vw)
        m_origin.setX(std::clamp(m_origin.x(), vw - w, 0.0));
    else
        m_origin.setX((vw - w) / 2.0);
    if (h > vh)
        m_origin.setY(std::clamp(m_origin.y(), vh - h, 0.0));
    else
        m_origin.setY((vh - h) / 2.0);
}

void ViewTransform::traceZoomStep(const char* tag, const QPointF& cursor,
                                  const ZoomStep& step, const ViewTransform& after)
{
    if (!step.changed)
        return;
    if (step.snappedToFit) {
        TRACE_SLIDE("%s zoom step cursor=(%.1f,%.1f) scale %.4f->%.4f (fit)",
                    tag, cursor.x(), cursor.y(), step.oldScale, after.scale());
        return;
    }
    TRACE_SLIDE("%s zoom step cursor=(%.1f,%.1f) img=(%.2f,%.2f) "
                "scale %.4f->%.4f origin (%.1f,%.1f)->(%.1f,%.1f)",
                tag, cursor.x(), cursor.y(), step.anchor.x(), step.anchor.y(),
                step.oldScale, after.scale(),
                step.wantedOrigin.x(), step.wantedOrigin.y(),
                after.origin().x(), after.origin().y());
}
