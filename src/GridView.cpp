#include "GridView.h"
#include "MediaModel.h"
#include "GridDelegate.h"
#include "AppSettings.h"
#include "FileOps.h"
#include "MediaContextMenu.h"
#include "SlideTrace.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QShortcut>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSlider>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

GridView::GridView(QWidget* parent) : QAbstractScrollArea(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
    setMinimumWidth(kMinGridWidth);
    viewport()->setMouseTracking(true);
    viewport()->setAcceptDrops(true);
    setAcceptDrops(true);

    m_delegate = new GridDelegate(this);
    m_delegate->setIconSize(m_iconSize);

    m_scanningTimer = new QTimer(this);
    m_scanningTimer->setSingleShot(true);
    m_scanningTimer->setInterval(200);
    connect(m_scanningTimer, &QTimer::timeout, this, &GridView::onScanningOverlayTimer);

    // Resize refits change the effective icon size on every event during an
    // interactive resize — request sharp thumbnails only once it settles
    m_refitInvalidateTimer = new QTimer(this);
    m_refitInvalidateTimer->setSingleShot(true);
    m_refitInvalidateTimer->setInterval(200);
    connect(m_refitInvalidateTimer, &QTimer::timeout, this, [this]() {
        if (m_sourceModel && m_iconSize != m_sharpThumbSize) {
            m_sourceModel->invalidateThumbnails(QSize(m_iconSize, m_iconSize));
            m_sharpThumbSize = m_iconSize;
        }
    });

    // Sharpen the previews to the current zoom-gesture size while Ctrl is
    // still held — debounced so rapid steps do not thrash the loader
    m_zoomSharpTimer = new QTimer(this);
    m_zoomSharpTimer->setSingleShot(true);
    m_zoomSharpTimer->setInterval(120);
    connect(m_zoomSharpTimer, &QTimer::timeout, this, [this]() {
        if (m_zoomGestureActive && m_sourceModel && m_zoomSharpTarget > 0
            && m_zoomSharpTarget != m_sharpThumbSize) {
            m_sourceModel->invalidateThumbnails(
                QSize(m_zoomSharpTarget, m_zoomSharpTarget));
            m_sharpThumbSize = m_zoomSharpTarget;
            viewport()->update();
        }
    });

    // Safety net for a lost Ctrl release (focus change / WM grab): poll the
    // modifier state while a zoom gesture is active (see onZoomIdleCheck)
    m_zoomIdleTimer = new QTimer(this);
    m_zoomIdleTimer->setInterval(100);
    connect(m_zoomIdleTimer, &QTimer::timeout, this, &GridView::onZoomIdleCheck);

    // Plain-click selection actions wait out the double-click interval —
    // a double-click opens the viewer and must not touch the selection
    m_clickTimer = new QTimer(this);
    m_clickTimer->setSingleShot(true);
    connect(m_clickTimer, &QTimer::timeout, this, [this]() {
        const int idx = m_pendingClickIndex;
        m_pendingClickIndex = -1;
        if (idx >= 0)
            handleSelectionClick(idx, Qt::NoModifier);
    });

    // Modal dialogs (rename, metadata edit, confirmations) refer to the
    // SELECTION — the hover focus frame is misleading while one is open.
    // Hide it on modal activation, re-evaluate once the dialog is gone.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget*) {
        if (QApplication::activeModalWidget()) {
            endZoomGesture(true);   // commit any live zoom before the dialog
            clearHover();
        } else {
            updateHover(viewport()->mapFromGlobal(QCursor::pos()));
        }
    });

    auto* selectAllShortcut = new QShortcut(QKeySequence::SelectAll, this);
    selectAllShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectAllShortcut, &QShortcut::activated, this, &GridView::selectAll);

    // Reflow animation: paint-only lerp between the previous and the new
    // raster on zoom / column-count resize
    m_reflowClock.start();
    m_reflowAnim = new QVariantAnimation(this);
    m_reflowAnim->setStartValue(0.0);
    m_reflowAnim->setEndValue(1.0);
    m_reflowAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_reflowAnim, &QVariantAnimation::valueChanged, this,
            [this]() { viewport()->update(); });
    connect(m_reflowAnim, &QVariantAnimation::finished, this, [this]() {
        if (m_zoomPinSettling) {
            // Pinned-zoom release settle done — painted state == raster.
            // The frame stays frozen unless the mouse moved meanwhile.
            m_zoomPinSettling = false;
            updateHover(viewport()->mapFromGlobal(QCursor::pos()));
        }
        viewport()->update();
    });

    // Fullscreen-transition glide (see beginViewportGlide) — same easing
    // as the reflow so both movements read as one
    m_vpGlide = new QVariantAnimation(this);
    m_vpGlide->setStartValue(0.0);
    m_vpGlide->setEndValue(1.0);
    m_vpGlide->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_vpGlide, &QVariantAnimation::valueChanged, this,
            [this]() { viewport()->update(); });
    connect(m_vpGlide, &QVariantAnimation::finished, this, [this]() {
        viewport()->update();
        // Hover was suspended during the glide — re-evaluate once
        updateHover(viewport()->mapFromGlobal(QCursor::pos()));
    });

    // Smooth scrolling: the row raster jumps, the PAINT glides after it
    // (see scrollByPixels)
    m_scrollAnim = new QVariantAnimation(this);
    m_scrollAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_scrollAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
        m_scrollLagPx = v.toReal();
        viewport()->update();
    });
    connect(m_scrollAnim, &QVariantAnimation::finished, this, [this]() {
        m_scrollLagPx = 0;
        viewport()->update();
        // The glide moved items under the (stationary) cursor
        updateHover(viewport()->mapFromGlobal(QCursor::pos()));
    });

    // Every value change the USER made on the scrollbar itself — handle
    // drag, arrow buttons, track click, wheel over the bar — announces
    // itself here first. QScrollBar emits it for interactions only, never
    // for a programmatic setValue, which is exactly the distinction the
    // glide needs (see the valueChanged handler below).
    connect(verticalScrollBar(), &QScrollBar::actionTriggered, this,
            [this](int) { m_scrollBarAction = true; });

    // Releasing the handle is the end of the scroll — that is when "snap to
    // grid" applies. Snapping DURING the drag is what made the handle hop
    // from row to row under the mouse (user-rejected); the pixel-valued
    // scrollbar lets it follow the mouse and land on a row afterwards.
    connect(verticalScrollBar(), &QScrollBar::sliderReleased, this, [this]() {
        if (!AppSettings::scrollSnapToGrid() || !m_sourceModel)
            return;
        const int cellH = qMax(1, cellSize().height());
        const int v = verticalScrollBar()->value();
        const int aligned =
            qBound(0, int(qRound(qreal(v) / cellH)), effectiveMaxFirstRow())
            * cellH;
        if (aligned != v)
            applyScrollPx(aligned, true);
    });

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        const int fromPx = m_lastScrollPx;
        m_lastScrollPx = value;
        const bool userAction = m_scrollBarAction;
        m_scrollBarAction = false;
        if (!m_inSmoothScroll) {
            if (userAction && !m_inReflowStep) {
                if (verticalScrollBar()->isSliderDown()) {
                    // Dragging the handle is DIRECT manipulation — the
                    // content belongs under the mouse, not trailing it by
                    // an animation. The bar is pixel-valued, so this is
                    // smooth without any glide.
                    m_scrollAnim->stop();
                    m_scrollLagPx = 0;
                } else {
                    // Arrow button, track click, wheel over the bar: a
                    // discrete jump, so the same glide as the grid's wheel
                    glideScrollTo(fromPx, value);
                }
            } else {
                // Everything else (scroll-into-view, zoom anchoring, range
                // clamps, restore) lands immediately: those positions are
                // computed, and a glide left over from the wheel would drag
                // the paint away from them.
                stopScrollAnimation(false);
            }
        }
        // A user scroll during a reflow animation snaps it to its end: the
        // frozen start rects are viewport coordinates of the old scroll
        // position and would paint nonsense once the view moved. Scrollbar
        // writes made by our own zoom/resize step are exempt (guard flag).
        if (!m_inReflowStep) {
            stopReflowAnimation();
            endPanelSlide();   // user scroll mid-slide: frozen rows are stale
        }
        // Bottom whitespace (over-scroll rows past the list end, allowed via
        // m_resizeAnchorRow after zoom or window enlarge) is one-way in the
        // same spirit: scrolling up releases it row by row — the whitespace
        // row just left cannot be scrolled back into.
        if (m_resizeAnchorRow >= 0 && firstRow() < m_resizeAnchorRow) {
            m_resizeAnchorRow = firstRow() > maxFirstRow() ? firstRow() : -1;
            updateScrollBarRange();
        }
        TRACE_SLIDE("scroll px=%d row=%d sub=%d", value, firstRow(), subRowPx());
        viewport()->update();
        updateVisibleRange();
    });
    // Note: the scrollbar counts PIXELS (updateScrollBarRange), but only
    // vertically — the raster below it is row-based, so items never shift
    // columns while scrolling.
}

void GridView::setSourceModel(MediaModel* model)
{
    if (m_sourceModel)
        disconnect(m_sourceModel, nullptr, this, nullptr);

    m_sourceModel = model;
    if (!m_sourceModel) {
        updateScrollBarRange();
        viewport()->update();
        return;
    }

    connect(m_sourceModel, &QAbstractItemModel::modelAboutToBeReset, this,
            [this]() { captureViewState(); });
    connect(m_sourceModel, &QAbstractItemModel::modelReset, this, [this]() {
        endZoomGesture(false);   // frozen base raster is stale now
        stopReflowAnimation();   // frozen indices/rects are stale now
        stopScrollAnimation(false);
        endPanelSlide();
        m_itemsPainted = false;  // new content — nothing shown yet
        m_gridOffset = 0;
        m_resizeAnchorRow = -1;
        cancelPendingClick();   // the pending index is stale now
        clearHover();
        m_externalFocusRow = rowForPath(m_externalFocusPath);
        m_lastFocusRow = -2;   // force re-emit: same row, different file now
        emitFocusChange();
        updateScrollBarRange();
        restoreViewState();
        viewport()->update();
        updateVisibleRange();
    });
    connect(m_sourceModel, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& topLeft, const QModelIndex& bottomRight,
                   const QList<int>&) {
        // Thumbnail deliveries for PREFETCHED items arrive by the hundred per
        // second on large libraries; repainting the whole viewport for rows
        // nobody can see is what starved the event loop. Only changes inside
        // the visible range are worth a repaint — off-screen rows are painted
        // from the model when they scroll in.
        if (m_visibleItemFirst <= m_visibleItemLast
            && (bottomRight.row() < m_visibleItemFirst
                || topLeft.row() > m_visibleItemLast))
            return;
        viewport()->update();
    });
    connect(m_sourceModel, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex&, int, int) {
        endZoomGesture(false);
        stopReflowAnimation();
        stopScrollAnimation(false);
        endPanelSlide();
        m_itemsPainted = false;
        m_gridOffset = 0;
        m_resizeAnchorRow = -1;
        cancelPendingClick();
        clearSelection();
        m_externalFocusRow = rowForPath(m_externalFocusPath);
        m_lastFocusRow = -2;
        emitFocusChange();
        updateScrollBarRange();
        viewport()->update();
        updateVisibleRange();
    });
    connect(m_sourceModel, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex&, int, int) {
        endZoomGesture(false);
        stopReflowAnimation();
        stopScrollAnimation(false);
        endPanelSlide();
        m_itemsPainted = false;
        m_gridOffset = 0;
        m_resizeAnchorRow = -1;
        cancelPendingClick();
        clearSelection();
        clearHover();
        m_externalFocusRow = rowForPath(m_externalFocusPath);
        m_lastFocusRow = -2;
        emitFocusChange();
        updateScrollBarRange();
        viewport()->update();
        updateVisibleRange();
    });

    connect(m_sourceModel, &MediaModel::scanningStarted, this, &GridView::onScanningStarted);
    connect(m_sourceModel, &MediaModel::scanningFinished, this, &GridView::onScanningFinished);

    updateScrollBarRange();
    viewport()->update();
    updateVisibleRange();
}

void GridView::onScanningStarted()
{
    m_scanning = true;
    m_showScanningOverlay = false;
    m_scanningTimer->start();
}

void GridView::onScanningFinished()
{
    m_scanning = false;
    m_showScanningOverlay = false;
    m_scanningTimer->stop();
    viewport()->update();
}

void GridView::onScanningOverlayTimer()
{
    if (m_scanning && m_sourceModel && m_sourceModel->rowCount() == 0) {
        m_showScanningOverlay = true;
        viewport()->update();
    }
}

void GridView::setIconSizeSlider(QSlider* slider)
{
    m_sizeSlider = slider;
    // Wheel notches on the slider step whole columns — intercepted in
    // eventFilter (QSlider's own wheel handling scrolls several values
    // per notch depending on the system wheel-lines setting)
    slider->installEventFilter(this);
    // The slider VALUE is the mirrored position (right = larger
    // previews): columns = maximum + 1 − value. Mirroring in the value
    // domain keeps the groove filling left-to-right — invertedAppearance
    // would fill it from the right.
    connect(slider, &QSlider::valueChanged, this, [this](int v) {
        onColumnCountChanged(m_sizeSlider->maximum() + 1 - v);
    });
    onColumnCountChanged(slider->maximum() + 1 - slider->value());
}

void GridView::onColumnCountChanged(int cols)
{
    if (cols < 1)
        return;

    // The zoom anchors on the RASTER (the top-left item stays top-left), so
    // it needs the raster to be what is on screen: land on the nearest row
    // first (no-op with snapping on, where that is always the case).
    stopScrollAnimation(true);

    // Freeze the on-screen flow BEFORE anything mutates — columns() and
    // cellSize() still reflect the old state here
    const LayoutParams pre = currentLayoutParams();
    const int topLeftItem = pre.firstRow * pre.cols - pre.gridOffset;
    const int oldCols = m_columns;
    m_columns = cols;
    if (cols != oldCols)
        AppSettings::setGridColumns(cols);
    const int eff = iconSizeForWidth(viewport()->width());
    if (eff == m_iconSize && cols == oldCols)
        return;   // nothing changes (startup: no settled viewport yet)
    beginReflowCapture(pre, m_iconSize);
    m_inReflowStep = true;

    m_iconSize = eff;
    m_wheelRemainder = 0;
    m_delegate->setIconSize(eff);

    if (m_sourceModel && eff != m_sharpThumbSize) {
        m_sourceModel->invalidateThumbnails(QSize(eff, eff));
        m_sharpThumbSize = eff;
    }

    // Zoom anchor: the item that was top-left stays top-left (grid-aligned).
    // This is the slider's path — and also the FRAMELESS Ctrl+wheel's, which
    // routes straight here via setValue (see wheelEvent): without a focus
    // frame the zoom has no image to be about, so it must behave exactly
    // like the slider. Ctrl+wheel WITH a frame anchors on that frame
    // instead (beginZoomGesture). An earlier version anchored Ctrl+wheel on
    // the item under the cursor always — that made the content jump around
    // too much.
    const int newCols = columns();
    if (topLeftItem > 0 && newCols > 0)
        m_gridOffset = (newCols - (topLeftItem % newCols)) % newCols;
    else
        m_gridOffset = 0;
    const int newRow = (qMax(0, topLeftItem) + m_gridOffset) / qMax(1, newCols);

    m_resizeAnchorRow = -1;
    updateScrollBarRange();
    if (newRow > maxFirstRow())
        m_resizeAnchorRow = newRow;
    updateScrollBarRange();
    setFirstRow(newRow);

    m_inReflowStep = false;
    startReflowAnimation();

    viewport()->update();
    updateVisibleRange();

    // A zoom-in from a parked state (maximum held above the width fit)
    // may free the maximum again — re-sync it (and the mirrored handle)
    updateSliderMaximum();

    // The raster changed — re-evaluate what is under the cursor now (no-op
    // while the Ctrl+wheel hover freeze is active)
    updateHover(viewport()->mapFromGlobal(QCursor::pos()));
}

void GridView::beginZoomGesture(bool pinFallbackNearest)
{
    // The pin math places cells by the logical raster — a sub-row offset
    // would move the pinned image away from the cursor by that amount
    stopScrollAnimation(true);

    // A new gesture while the previous pinned settle still runs: carry
    // the currently painted pin point over so the restart is jumpless
    // (the frame is frozen, so the pinned item is the same one)
    bool carryPin = false;
    QPointF carryPoint;
    if (m_zoomPinSettling
        && m_reflowAnim->state() == QAbstractAnimation::Running) {
        const qreal t = m_reflowAnim->currentValue().toReal();
        carryPoint = m_zoomPinPointFrom
            + (m_zoomPinPointTo - m_zoomPinPointFrom) * t;
        carryPin = true;
    }
    m_zoomPinSettling = false;

    // Freeze the current resting raster as the gesture base — every scaled
    // frame and the eventual commit are derived from it. Any running
    // reflow/slide is materialized away first (clean start).
    stopReflowAnimation();
    endPanelSlide();
    m_zoomBaseLayout = currentLayoutParams();
    m_zoomBaseIconSize = m_iconSize;
    m_zoomScale = 1.0;
    m_zoomOrigin = QPointF(0, 0);
    m_zoomTargetCols = m_zoomBaseLayout.cols;
    m_zoomScaledFlow = flowParams(m_zoomBaseLayout, m_zoomBaseIconSize);
    m_zoomSharpTarget = 0;

    // PINNED variant when an item carries the focus frame: the user wants
    // THAT item zoomed around the cursor — the raster then stays live and
    // every step reflows around the pinned item. Without a frame the
    // gesture paints the frozen raster as a uniform scale instead.
    int pin = m_hoverIndex >= 0 ? m_hoverIndex : m_externalFocusRow;
    if (pin < 0 && pinFallbackNearest && m_sourceModel
        && m_sourceModel->rowCount() > 0) {
        // The PAN can grab the plane anywhere: without a focus frame the
        // anchor is the item nearest to the cursor (clamp the point into
        // the occupied raster)
        const QPointF mm(viewport()->mapFromGlobal(QCursor::pos()));
        const QSize cs = m_zoomBaseLayout.cell;
        const int cols = qMax(1, m_zoomBaseLayout.cols);
        const int col = qBound(0, int(mm.x()) / qMax(1, cs.width()), cols - 1);
        const int row = qMax(0, int(mm.y()) / qMax(1, cs.height()));
        const int vi = (m_zoomBaseLayout.firstRow + row) * cols + col;
        pin = qBound(0, vi - m_zoomBaseLayout.gridOffset,
                     m_sourceModel->rowCount() - 1);
    }
    m_zoomPinned = pin >= 0 && m_sourceModel && pin < m_sourceModel->rowCount();
    m_zoomPinItem = m_zoomPinned ? pin : -1;
    if (m_zoomPinned) {
        // Frame from a Details/viewer window: adopt it as the (frozen)
        // hover so it survives the gesture like a mouse-hover frame
        m_hoverIndex = pin;
        // Pin fraction: the exact in-cell point under the cursor if the
        // cursor is on the framed item's cell, its CENTER otherwise — the
        // frame is then MOVED to the cursor by the first step's reflow.
        // A settle carry keeps the previous fraction (same item, and its
        // cell has moved off the raster position).
        const QPointF m(viewport()->mapFromGlobal(QCursor::pos()));
        const QSizeF cs(m_zoomBaseLayout.cell);
        const int cols = qMax(1, m_zoomBaseLayout.cols);
        const int vi = pin + m_zoomBaseLayout.gridOffset;
        const QPointF tl((vi % cols) * cs.width(),
                         (vi / cols - m_zoomBaseLayout.firstRow) * cs.height());
        if (!carryPin) {
            m_zoomPinFrac = QRectF(tl, cs).contains(m)
                    && cs.width() > 0 && cs.height() > 0
                ? QPointF((m.x() - tl.x()) / cs.width(),
                          (m.y() - tl.y()) / cs.height())
                : QPointF(0.5, 0.5);
        }
        // Resting pin state: the base raster anchored at the pinned item,
        // pin point at its current on-screen position (mid-settle
        // position on a carry — the first step then travels from there)
        m_zoomPinFlowFrom = m_zoomPinFlowTo =
            pinRelativeFlow(m_zoomBaseLayout, m_zoomBaseIconSize, pin);
        m_zoomPinPointFrom = m_zoomPinPointTo = carryPin
            ? carryPoint
            : tl + QPointF(m_zoomPinFrac.x() * cs.width(),
                           m_zoomPinFrac.y() * cs.height());
        emitFocusChange();
    }

    m_zoomGestureActive = true;
    // The Ctrl release may be delivered to any widget — filter globally
    qApp->installEventFilter(this);
    m_zoomIdleTimer->start();
    viewport()->update();
}

void GridView::updateZoomGesture(int columnSteps)
{
    if (!m_zoomGestureActive || columnSteps == 0)
        return;

    const int colsBase = qMax(1, m_zoomBaseLayout.cols);
    const int padW = baseCellSize().width() - m_delegate->iconSize();
    const int vpW = viewport()->width();

    int target = qMax(1, m_zoomTargetCols - columnSteps);   // >0 = zoom in
    // Zoom-in stops at one column (no upper preview size bound); zoom-out
    // never shrinks the preview below kMinIconSize.
    const int maxCols = qMax(1, vpW / qMax(1, kMinIconSize + padW));
    target = qMin(target, maxCols);
    if (target == m_zoomTargetCols)
        return;
    m_zoomTargetCols = target;

    if (m_zoomPinned) {
        // Pinned variant: the raster steps live, the framed item stays
        // pinned to the cursor (its neighbours re-fold around it)
        updatePinnedZoomStep();
        return;
    }

    const qreal newScale = qreal(colsBase) / target;
    // Compose the new scale onto the current display around the CURSOR: a
    // point shown at D stays at A + k·(D−A) with k = newScale/oldScale, so
    // origin' = k·origin + (1−k)·A. The item under the mouse holds still.
    const qreal k = newScale / m_zoomScale;
    const QPointF a = QPointF(viewport()->mapFromGlobal(QCursor::pos()));
    m_zoomOrigin = k * m_zoomOrigin + (1.0 - k) * a;
    m_zoomScale = newScale;
    m_zoomScaledFlow =
        scaleFlow(flowParams(m_zoomBaseLayout, m_zoomBaseIconSize), newScale);

    // Sharpen to the size the delegate now paints (debounced)
    m_zoomSharpTarget = qMax(1, qRound(newScale * m_zoomBaseIconSize));
    m_zoomSharpTimer->start();

    viewport()->update();
    updateVisibleRange();
}

// One step of the PINNED zoom variant: the ZOOM CENTER is re-resolved
// under the mouse first (it may have wandered onto a different image
// since the last step), then the logical raster jumps to the stepped
// column count immediately (scrollbar, slider, prefetch all track it
// live), placed so the pin point lands as close to the mouse as the
// row/column raster allows. The step is animated as a lerp of two
// PIN-ANCHORED flows (see pinRelativeFlow) painted with the pin point
// held at the mouse — the pinned image only scales in place while its
// neighbours re-fold around it; the sub-cell residual the raster cannot
// absorb stays a paint-only translation until the release settle.
void GridView::updatePinnedZoomStep()
{
    if (!m_sourceModel || m_sourceModel->rowCount() <= 0)
        return;

    // Materialize the painted state and re-resolve the ZOOM CENTER under
    // the mouse — it may have wandered onto a different image since the
    // last step (see resolvePinAt); with the pan engaged this resolves to
    // the pinned item itself, which sits under the mouse by construction.
    const bool wasRunning =
        m_reflowAnim->state() == QAbstractAnimation::Running;
    const QPointF m = QPointF(viewport()->mapFromGlobal(QCursor::pos()));
    resolvePinAt(m);
    const int itemIdx = m_zoomPinItem;

    // Apply the stepped column count — the preview size follows
    // (width-filling, same math as the scale variant's commit)
    m_columns = m_zoomTargetCols;
    const int eff = iconSizeForWidth(viewport()->width());
    m_iconSize = eff;
    m_delegate->setIconSize(eff);

    anchorPinnedRaster(m, itemIdx);
    const QSize csT = cellSize();
    TRACE_SLIDE("zoom step targetCols=%d realCols=%d eff=%d vpW=%d",
                m_zoomTargetCols, columns(), eff,
                viewport()->width());

    // Re-syncs the maximum AND the mirrored handle position (blocked, no
    // retrigger); persist explicitly — the new zoom must survive a restart
    updateSliderMaximum();
    AppSettings::setGridColumns(m_columns);

    // New target: WINDOW-FLUSH folding — the strip is positioned so the
    // pinned item sits at the continuous MOUSE column position, which
    // puts the fold lines at the window edges: the predecessors fill the
    // row to the LEFT of the pinned image up to the window border and
    // wrap into the row above, instead of leaving the raster's sub-cell
    // residual as whitespace. The raster's own column only matters for
    // the release settle, which re-anchors basePx via pinRelativeFlow.
    m_zoomPinFlowTo = pinRelativeFlow(currentLayoutParams(), eff, itemIdx);
    m_zoomPinFlowTo.basePx = (m.x() - m_zoomPinFrac.x() * csT.width())
        - qreal(itemIdx) * csT.width();
    // Mouse in the right-edge whitespace puts Xpoint past the fold —
    // normalize so the step animation cannot cross a fold row
    renormPinFlow(m_zoomPinFlowTo);
    m_zoomPinPointTo = m;

    startZoomPinAnim(wasRunning);

    // Sharpen to the new size while Ctrl is still held (debounced)
    m_zoomSharpTarget = eff;
    m_zoomSharpTimer->start();

    viewport()->update();
    updateVisibleRange();
}

// Anchor the pinned item in the LIVE raster: nearest column via
// m_gridOffset, nearest row via the scrollbar. The COLUMN is kept even
// when the list start shows — the leading dummy cells before item 0 are
// the accepted price for the pinned image keeping its x position on
// release (user choice; an earlier version folded naturally there, which
// made the image jump columns on release). Only whole dummy ROWS are
// avoided: a vertical underflow clamps to row 0 and the image drifts up
// with the settle instead.
void GridView::anchorPinnedRaster(const QPointF& pt, int itemIdx)
{
    const int newCols = qMax(1, columns());
    const QSize csT = cellSize();
    const qreal txT = pt.x() - m_zoomPinFrac.x() * csT.width();
    const qreal tyT = pt.y() - m_zoomPinFrac.y() * csT.height();
    const int col = qBound(0, qRound(txT / qMax(1, csT.width())), newCols - 1);
    const int screenRow = qRound(tyT / qMax(1, csT.height()));
    const int gridOffset = ((col - itemIdx) % newCols + newCols) % newCols;
    const int firstRow =
        qMax(0, (itemIdx + gridOffset) / newCols - screenRow);

    m_inReflowStep = true;
    m_gridOffset = gridOffset;
    m_resizeAnchorRow = -1;
    updateScrollBarRange();
    if (firstRow > maxFirstRow())
        m_resizeAnchorRow = firstRow;     // over-scroll near the list end
    updateScrollBarRange();
    setFirstRow(firstRow);
    m_inReflowStep = false;
}

// Mouse movement during the pinned gesture: the pinned image follows the
// cursor. Shifting BOTH pin points by the delta keeps a running step
// animation jumpless (the painted point moves 1:1 with the mouse), and
// shifting both basePx values by the horizontal component keeps the
// window-flush invariant — the fold lines stay at the window edges while
// the whole strip slides through them (edge items wrap marquee-style).
void GridView::zoomPinFollowMouse(const QPointF& m)
{
    const QPointF d = m - m_zoomPanLastMouse;
    if (qAbs(d.x()) < 0.5 && qAbs(d.y()) < 0.5)
        return;
    m_zoomPanLastMouse = m;
    m_zoomPinPointTo += d;
    m_zoomPinPointFrom += d;
    m_zoomPinFlowTo.basePx += d.x();
    m_zoomPinFlowFrom.basePx += d.x();
    renormPinFlow(m_zoomPinFlowFrom);
    renormPinFlow(m_zoomPinFlowTo);
    // The pin reference is the mouse and can leave the grid area (drag
    // across the side panel, or into the right-edge whitespace past the
    // fold). The painted strip only covers [off.x, off.x + foldW) — a
    // far-out reference breaks window coverage just like the fold-row
    // jump above (white grid, ghost column). Re-anchor the pin on the
    // nearest covered point: resolvePinAt is jumpless by construction,
    // the plane stays put and the drag continues from the clamped
    // reference (the raw delta base m_zoomPanLastMouse is unaffected).
    const qreal right =
        qMin(qreal(viewport()->width()), m_zoomPinFlowTo.foldW) - 1;
    const qreal bottom = viewport()->height() - 1;
    const QPointF clamped(qBound(0.0, m_zoomPinPointTo.x(), right),
                          qBound(0.0, m_zoomPinPointTo.y(), bottom));
    if (clamped != m_zoomPinPointTo)
        resolvePinAt(clamped, /*moveFocus=*/false);
    viewport()->update();
    updateVisibleRange();
}

// Currently painted pin state: the lerped pin flow plus the translation
// that puts the pin point at the (lerped) pin position. The translation
// folds the pin POINT, not the pinned cell's left edge: a pinned cell in
// a left column can OVERHANG the window edge (its edge's strip position
// goes negative) — folding the edge then wraps it a row up and the
// compensation shifts the whole grid by ~foldW out of the viewport
// (everything culled, blank view). The point sits inside the viewport at
// both lerp endpoints and moves linearly, so its fold row is stable; the
// overhanging cell itself is painted correctly by the straddler
// double-draw in paintEvent.
// Marquee renormalization (gauge change): shifting basePx by whole fold
// widths re-expresses the IDENTICAL painted plane — every item moves one
// row, the pin translation compensates exactly. Keeping the pin's strip
// position inside [0, foldW) in EVERY set flow matters twice over:
// (1) a pan drifts the strip position across fold lines — the painted
//     fold row (floor in paintedPinFlow) would jump there and shift the
//     whole plane by (±foldW, ∓cellH), leaving the window uncovered
//     (white grid sweeping through, ghost column at the right edge);
// (2) a step animation lerps Xpoint and foldW linearly — with BOTH
//     endpoints normalized the quotient stays in [0,1) for every t, so
//     the fold row cannot jump mid-animation either (a From flow left
//     k rows off would drag the plane across k fold lines while
//     animating: white flicker sweep on zoom steps with a moved center).
void GridView::renormPinFlow(FlowParams& f) const
{
    const qreal w = qMax(1.0, f.foldW);
    const qreal Xp =
        (qreal(m_zoomPinItem) + m_zoomPinFrac.x()) * f.cellW + f.basePx;
    f.basePx -= std::floor(Xp / w) * w;
}

GridView::FlowParams GridView::paintedPinFlow(QPointF* offOut) const
{
    const qreal t = m_reflowAnim->state() == QAbstractAnimation::Running
        ? m_reflowAnim->currentValue().toReal()
        : 1.0;
    const FlowParams f = lerpFlow(m_zoomPinFlowFrom, m_zoomPinFlowTo, t);
    if (offOut) {
        const QPointF pinPt = m_zoomPinPointFrom
            + (m_zoomPinPointTo - m_zoomPinPointFrom) * t;
        const qreal Xpoint =
            (qreal(m_zoomPinItem) + m_zoomPinFrac.x()) * f.cellW + f.basePx;
        const qreal rowVis = std::floor(Xpoint / qMax(1.0, f.foldW));
        *offOut = pinPt
            - QPointF(Xpoint - rowVis * f.foldW,
                      (rowVis + m_zoomPinFrac.y()) * f.cellH);
    }
    return f;
}

// Re-resolve the pin as the item/in-cell point under `pt` on the PAINTED
// plane: invert the painted flow (translation off, fold rows, strip
// position → item + in-cell fraction). Jumpless by construction — the
// flow definition is item-independent, only the pin reference (item,
// frac, points) changes, and the new pin point's painted position is
// `pt` itself; From==To leaves a still-running step animation painting
// the same state (harmless freeze, the next step re-materializes). Used
// by every wheel step (the zoom center follows the mouse) and by every
// pan (re-)grab (the grab point is wherever the button goes down — NOT
// the point of the original grab; the plane must not jump on a re-grab).
void GridView::resolvePinAt(const QPointF& pt, bool moveFocus)
{
    if (!m_sourceModel || m_sourceModel->rowCount() <= 0)
        return;
    const int count = m_sourceModel->rowCount();
    QPointF off;
    const FlowParams painted = paintedPinFlow(&off);
    const QPointF fp = pt - off;                      // flow coordinates
    const qreal rowF = std::floor(fp.y() / qMax(1.0, painted.cellH));
    const qreal X = rowF * painted.foldW + fp.x();
    const qreal itemF = (X - painted.basePx) / qMax(1.0, painted.cellW);
    const int itemIdx = qBound(0, int(std::floor(itemF)), count - 1);
    m_zoomPinFrac = QPointF(
        qBound(0.0, itemF - itemIdx, 1.0),
        qBound(0.0, fp.y() / qMax(1.0, painted.cellH) - rowF, 1.0));
    m_zoomPinItem = itemIdx;
    m_zoomPinFlowFrom = painted;
    m_zoomPinFlowTo = painted;
    // Same painted plane, canonical parametrization (new pin, new row 0)
    renormPinFlow(m_zoomPinFlowFrom);
    renormPinFlow(m_zoomPinFlowTo);
    m_zoomPinPointFrom = pt;
    m_zoomPinPointTo = pt;
    // The frame follows the pin only when `pt` is where the USER points —
    // a wheel step re-resolves at the cursor, so the framed item is the one
    // under it. The PAN's clamp re-resolves at an artificial point (the
    // cursor left the covered strip), and dragging the frame along there
    // moved it onto a picture the user never pointed at: it changed image
    // seemingly at random, mid-drag, with nothing in the trace to show it.
    if (moveFocus && m_hoverIndex != itemIdx) {
        TRACE_SLIDE("hover %d -> %d (pin re-resolved)", m_hoverIndex, itemIdx);
        m_hoverIndex = itemIdx;
        emitFocusChange();
    }
}

GridView::FlowParams GridView::pinRelativeFlow(const LayoutParams& p,
                                               int iconSize, int pinItem)
{
    const int cols = qMax(1, p.cols);
    FlowParams f;
    f.cellW = p.cell.width();
    f.cellH = p.cell.height();
    f.foldW = qreal(cols) * p.cell.width();
    // Strip position of item 0 measured from the start of the PINNED
    // item's row: the pinned item sits at its in-row column offset
    const int vi = pinItem + p.gridOffset;
    f.basePx = (qreal(vi % cols) - qreal(pinItem)) * p.cell.width();
    f.iconSize = iconSize;
    return f;
}

void GridView::startZoomPinAnim(bool keepDeadline)
{
    int ms = AppSettings::reflowAnimationMs();
    m_reflowAnim->stop();
    if (ms <= 0 || !m_itemsPainted) {
        m_zoomPinFlowFrom = m_zoomPinFlowTo;   // animation disabled → snap
        m_zoomPinPointFrom = m_zoomPinPointTo;
        return;
    }
    // Same deadline semantics as startReflowAnimation: retargeting a
    // running step keeps the original deadline (larger remaining distance
    // simply moves faster); a fresh step / the settle runs the full time.
    if (keepDeadline) {
        const qint64 remaining = m_reflowDeadline - m_reflowClock.elapsed();
        if (remaining > 0)
            ms = int(remaining);
        else
            m_reflowDeadline = m_reflowClock.elapsed() + ms;
    } else {
        m_reflowDeadline = m_reflowClock.elapsed() + ms;
    }
    m_reflowAnim->setDuration(ms);
    m_reflowAnim->start();
}

void GridView::onZoomIdleCheck()
{
    if (!m_zoomGestureActive && !m_zoomSliderHold)
        return;
    if (!(QApplication::queryKeyboardModifiers() & Qt::ControlModifier))
        endZoomGesture(true);
}

bool GridView::eventFilter(QObject* obj, QEvent* event)
{
    if ((m_zoomGestureActive || m_zoomSliderHold)
        && event->type() == QEvent::KeyRelease) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Control
            && !(QApplication::queryKeyboardModifiers() & Qt::ControlModifier))
            endZoomGesture(true);
    }
    // A wheel notch ON the size slider steps exactly ONE column —
    // QSlider's own wheel handling (several value units per notch,
    // depending on the system wheel-lines setting) is swallowed
    if (obj == m_sizeSlider && event->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(event);
        int angleY = we->angleDelta().y();
        if (we->inverted())
            angleY = -angleY;
        m_sliderWheelRemainder += angleY;
        const int steps = m_sliderWheelRemainder / 120;   // >0 = zoom in
        m_sliderWheelRemainder %= 120;
        // The value is the mirrored position: up = larger previews =
        // higher value (fewer columns); setValue clamps at the ends
        if (steps != 0)
            m_sizeSlider->setValue(m_sizeSlider->value() + steps);
        return true;
    }
    return QAbstractScrollArea::eventFilter(obj, event);
}

void GridView::endZoomGesture(bool commit)
{
    // A frameless Ctrl-hold has no gesture state to commit — every notch
    // already went through the slider. Only the hold itself is released
    // (shared entry point, so a model reset / modal dialog clears it too).
    if (m_zoomSliderHold) {
        m_zoomSliderHold = false;
        qApp->removeEventFilter(this);
        m_zoomIdleTimer->stop();
    }
    if (!m_zoomGestureActive)
        return;
    m_zoomGestureActive = false;
    m_zoomPanActive = false;
    qApp->removeEventFilter(this);
    m_zoomIdleTimer->stop();
    m_zoomSharpTimer->stop();
    if (commit) {
        commitZoomGesture();
    } else {
        // Drop (model reset / rows changed): a pinned gesture's raster is
        // already valid — only the painted pin translation is discarded
        m_zoomPinned = false;
        m_zoomPinSettling = false;
        m_zoomPinItem = -1;
        viewport()->update();
    }
}

void GridView::commitZoomGesture()
{
    // PINNED variant: the logical raster is already final — it stepped
    // live during the gesture. NO reflow here: the settle is ONE MORE pin
    // animation whose flow is already the final raster — only the pin
    // point travels from the cursor to its exact raster position, which
    // moves the grid as a RIGID block (relative item positions fixed; a
    // still-running step refold finishes within the settle). The frame
    // stays frozen on the pinned item; the release position becomes the
    // new reference for the unfreeze threshold, so the frame holds until
    // the mouse moves AFTER the release.
    if (m_zoomPinned) {
        m_zoomPinned = false;
        m_hoverFreezePos = QCursor::pos();

        // Materialize the currently painted pin state as the settle start
        const bool wasRunning =
            m_reflowAnim->state() == QAbstractAnimation::Running;
        if (wasRunning) {
            const qreal t = m_reflowAnim->currentValue().toReal();
            m_zoomPinFlowFrom = lerpFlow(m_zoomPinFlowFrom, m_zoomPinFlowTo, t);
            m_zoomPinPointFrom = m_zoomPinPointFrom
                + (m_zoomPinPointTo - m_zoomPinPointFrom) * t;
        } else {
            m_zoomPinFlowFrom = m_zoomPinFlowTo;
            m_zoomPinPointFrom = m_zoomPinPointTo;
        }
        // The mouse may have moved since the last wheel step — re-anchor
        // the raster at the CURRENT pin position first, then settle to it
        if (m_sourceModel && m_sourceModel->rowCount() > 0)
            anchorPinnedRaster(m_zoomPinPointTo,
                               qBound(0, m_zoomPinItem,
                                      m_sourceModel->rowCount() - 1));

        // Exact raster position of the pin point = settle target
        const LayoutParams cur = currentLayoutParams();
        const int cols = qMax(1, cur.cols);
        const int vi = qMax(0, m_zoomPinItem) + cur.gridOffset;
        m_zoomPinFlowTo = pinRelativeFlow(cur, m_iconSize, m_zoomPinItem);
        m_zoomPinPointTo = QPointF(
            (qreal(vi % cols) + m_zoomPinFrac.x()) * cur.cell.width(),
            (qreal(vi / cols - cur.firstRow) + m_zoomPinFrac.y())
                * cur.cell.height());

        if (wasRunning
            || (m_zoomPinPointFrom - m_zoomPinPointTo).manhattanLength()
                   >= 1.0) {
            startZoomPinAnim(false);
            m_zoomPinSettling =
                m_reflowAnim->state() == QAbstractAnimation::Running;
        } else {
            // Nothing moved (no step happened) — no settle to animate
            m_zoomPinFlowFrom = m_zoomPinFlowTo;
            m_zoomPinPointFrom = m_zoomPinPointTo;
        }

        if (m_sourceModel && m_iconSize != m_sharpThumbSize) {
            m_sourceModel->invalidateThumbnails(QSize(m_iconSize, m_iconSize));
            m_sharpThumbSize = m_iconSize;
        }
        updateSliderMaximum();
        viewport()->update();
        updateVisibleRange();
        return;
    }

    if (!m_sourceModel) {
        viewport()->update();
        return;
    }
    const int count = m_sourceModel->rowCount();
    if (count <= 0) {
        viewport()->update();
        return;
    }

    const qreal s = m_zoomScale;
    const QPointF o = m_zoomOrigin;
    const LayoutParams& b = m_zoomBaseLayout;
    const int colsB = qMax(1, b.cols);
    const qreal cwB = b.cell.width();
    const qreal chB = b.cell.height();

    // 1. Item under the cursor in the scaled display, and the fraction of
    //    its cell the cursor points at (kept invariant across the reflow).
    const QPointF m = QPointF(viewport()->mapFromGlobal(QCursor::pos()));
    const QPointF pBase = (m - o) / s;             // back to base coords
    int colB = qBound(0, int(std::floor(pBase.x() / qMax(1.0, cwB))), colsB - 1);
    int screenRowB = qMax(0, int(std::floor(pBase.y() / qMax(1.0, chB))));
    const int viBase = (b.firstRow + screenRowB) * colsB + colB;
    const int itemIdx = qBound(0, viBase - b.gridOffset, count - 1);
    // Recompute the anchor cell's scaled top-left from the (clamped) item,
    // then the cursor fraction within it.
    const int viB2 = itemIdx + b.gridOffset;
    const qreal gtlx = (viB2 % colsB) * cwB * s + o.x();
    const qreal gtly = ((viB2 / colsB) - b.firstRow) * chB * s + o.y();
    const qreal gw = qMax(1.0, cwB * s);
    const qreal gh = qMax(1.0, chB * s);
    const qreal fx = qBound(0.0, (m.x() - gtlx) / gw, 1.0);
    const qreal fy = qBound(0.0, (m.y() - gtly) / gh, 1.0);

    // 2. Apply the target column count (the gesture is over — logical state
    //    unfreezes to the zoomed-to raster); the preview size follows.
    m_columns = m_zoomTargetCols;
    const int eff = iconSizeForWidth(viewport()->width());
    m_iconSize = eff;
    m_delegate->setIconSize(eff);
    if (eff != m_sharpThumbSize) {
        m_sourceModel->invalidateThumbnails(QSize(eff, eff));
        m_sharpThumbSize = eff;
    }

    // 3. Place the anchor item so the cursor point lands as close to the
    //    cursor as the row/column raster allows.
    const int newCols = qMax(1, columns());
    const QSize csT = cellSize();
    const qreal txT = m.x() - fx * csT.width();
    const qreal tyT = m.y() - fy * csT.height();
    const int col = qBound(0, qRound(txT / qMax(1, csT.width())), newCols - 1);
    const int screenRow = qRound(tyT / qMax(1, csT.height()));
    int gridOffset = ((col - itemIdx) % newCols + newCols) % newCols;
    int firstRow = (itemIdx + gridOffset) / newCols - screenRow;
    if (firstRow < 0) {
        // The item is near the list start; keeping it under the cursor
        // needs empty rows above it. If the view was ALREADY at the list
        // start, inserting dummy rows looks odd — pin item 0 top-left and
        // let the anchor drift up (the reflow makes that legible). Else add
        // whole leading DUMMY rows (grid offset past one row) to push the
        // item down to the cursor exactly.
        if (b.firstRow == 0 && b.gridOffset == 0) {
            gridOffset = 0;
            firstRow = 0;
        } else {
            gridOffset += -firstRow * newCols;   // add (-firstRow) dummy rows
            firstRow = 0;
        }
    }

    // 4. Commit the logical raster. Guard the scrollbar writes so the
    //    valueChanged handler does not mistake them for a user scroll.
    m_inReflowStep = true;
    m_gridOffset = gridOffset;
    m_resizeAnchorRow = -1;
    updateScrollBarRange();
    if (firstRow > maxFirstRow())
        m_resizeAnchorRow = firstRow;     // over-scroll near the list end
    updateScrollBarRange();
    setFirstRow(firstRow);
    m_inReflowStep = false;

    // Re-syncs the maximum AND the mirrored handle position (blocked, no
    // retrigger); persist explicitly — the slider signal never fires here
    updateSliderMaximum();
    AppSettings::setGridColumns(m_columns);

    // 5. Animate the re-fold: reflow the flow from the scaled gesture state
    //    to the target raster while a viewport glide slides the 2D zoom
    //    origin back to zero — together they carry the scaled display onto
    //    the settled raster. Endpoints are exact; disabled → snap.
    const int ms = AppSettings::reflowAnimationMs();
    const bool animate = ms > 0 && m_itemsPainted && (s != 1.0 || !o.isNull());
    if (animate) {
        m_vpGlideOrigin = viewport()->mapToGlobal(QPoint(0, 0))
            + QPoint(qRound(o.x()), qRound(o.y()));
        m_vpGlide->stop();
        m_vpGlide->setDuration(ms);
        m_vpGlide->start();
        m_reflowFrom = m_zoomScaledFlow;   // scaled base flow, anchored at origin
        m_reflowChained = false;
        m_reflowCaptured = true;
        startReflowAnimation();            // couples to the glide duration
    } else {
        stopReflowAnimation();
    }

    viewport()->update();
    updateVisibleRange();
    // Frame stays frozen on its item until a deliberate mouse move
    updateHover(viewport()->mapFromGlobal(QCursor::pos()));
}

void GridView::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    // Paint diagnostics (BLITZVIEW_TRACE_SLIDE): counts cells drawn without
    // a decoded pixmap ("white") to tell data gaps from paint starvation.
    QElapsedTimer paintTimer;
    paintTimer.start();
    int paintedCells = 0;
    int whiteCells = 0;
    const char* paintBranch = "static";
    auto tracePaint = [&]() {
        const qint64 ms = paintTimer.elapsed();
        if (whiteCells > 0 || paintedCells == 0 || ms > 20)
            TRACE_SLIDE("paint %s cells=%d white=%d dur=%lldms",
                        paintBranch, paintedCells, whiteCells, ms);
    };
    auto countCell = [&](const QModelIndex& mi) {
        ++paintedCells;
        if (mi.data(Qt::UserRole + 4).value<QPixmap>().isNull())
            ++whiteCells;
    };

    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), palette().base());

    // Transition backdrop render: the images fly on the overlay — the
    // window rendered behind them shows an empty grid
    if (m_suppressItemPaint)
        return;

    if (m_showScanningOverlay) {
        painter.setPen(palette().color(QPalette::Text));
        QFont f = painter.font();
        f.setPointSize(14);
        painter.setFont(f);
        painter.drawText(viewport()->rect(), Qt::AlignCenter, tr("Scanning..."));
        return;
    }

    if (!m_sourceModel)
        return;

    const int count = m_sourceModel->rowCount();
    if (count <= 0)
        return;

    // From here on, items reach the screen — transitions away from this
    // state may animate (see m_itemsPainted / beginReflowCapture)
    m_itemsPainted = true;

    // Content translation applied on top of the flow: either the live zoom
    // gesture's 2D origin (constant — the scaled grid sits at scaledFlow +
    // origin) or the fullscreen-transition glide, which translates ALL
    // content by the shrinking delta between the origin captured before the
    // transition and the current on-screen origin (the top-left image
    // travels from its old screen position to its new one, its arrival ends
    // the transition; sampling the current origin every frame keeps the
    // path correct while chrome/panel hides shift the viewport). The zoom
    // COMMIT reuses the glide for exactly this: it slides the zoom origin
    // back to zero while the reflow re-folds the strip.
    // On top of both: the smooth-scroll offset (the row raster is already
    // at the target, the paint is still catching up — see scrollByPixels).
    // It applies to EVERY branch: a reflow running while the view rests
    // between rows must keep that offset, or the grid would jump by up to
    // one row when the animation starts.
    QPoint glideOff(0, -qRound(subRowPx() + m_scrollLagPx));
    const bool pinnedFlowPaint =
        (m_zoomGestureActive && m_zoomPinned) || m_zoomPinSettling;
    FlowParams pinFlow;
    if (pinnedFlowPaint) {
        // Pinned zoom: the flow is anchored AT THE PINNED ITEM and the
        // translation puts its pin point at the (lerped) pin position —
        // exact for EVERY animation frame, not just the endpoints. Items
        // before the pinned one fold backward (negative rows), items
        // after it forward; the pinned image only scales in place.
        QPointF off;
        pinFlow = paintedPinFlow(&off);
        glideOff += QPoint(qRound(off.x()), qRound(off.y()));
    } else if (m_zoomGestureActive) {
        glideOff += QPoint(qRound(m_zoomOrigin.x()), qRound(m_zoomOrigin.y()));
    } else if (m_vpGlide->state() == QAbstractAnimation::Running) {
        const qreal t = m_vpGlide->currentValue().toReal();
        glideOff += (m_vpGlideOrigin - viewport()->mapToGlobal(QPoint(0, 0)))
                    * (1.0 - t);
    }
    if (!glideOff.isNull())
        painter.translate(glideOff);
    // Culling happens in translated coordinates
    const QRect cullRect = viewport()->rect().translated(-glideOff);

    // Reflow animation (flow style): the reading-order strip is folded at
    // the interpolated fold width — items travel ALONG their row; a cell
    // crossing the fold is drawn twice (outbound part clipped at the fold,
    // inbound twin growing from the left edge of the next row), so the
    // wrap itself is what animates. The delegate gets the interpolated
    // icon size just for this loop (its scaledSizeKey check then misses on
    // purpose and falls back to fast scaling). All layout/input math
    // elsewhere stays target-based throughout.
    // The panel slide paints its own frozen from→to lerp, driven by the
    // panel edge's progress — fold width == moving edge, exactly. The live
    // zoom gesture paints the frozen base flow scaled by the current zoom
    // (no interpolation — the mouse wheel drives it directly).
    const bool zoomScalePaint = m_zoomGestureActive && !m_zoomPinned;
    if (pinnedFlowPaint || zoomScalePaint || m_panelSlideActive
        || m_reflowAnim->state() == QAbstractAnimation::Running) {
        const FlowParams p = pinnedFlowPaint
            ? pinFlow
            : zoomScalePaint
            ? m_zoomScaledFlow
            : m_panelSlideActive
            ? lerpFlow(m_slideFrom, m_slideTo, m_panelSlideT)
            : lerpFlow(m_reflowFrom,
                       flowParams(currentLayoutParams(), m_iconSize),
                       m_reflowAnim->currentValue().toReal());
        if (m_panelSlideActive)
            TRACE_SLIDE("paint slide t=%.3f foldW=%.1f vpW=%d", m_panelSlideT,
                        p.foldW, viewport()->width());
        m_delegate->setIconSize(qMax(1, qRound(p.iconSize)));
        const int foldEdge = int(std::ceil(p.foldW));
        // GROWING rasters: the interpolated fold sits INSIDE the viewport
        // for the whole animation (it lerps old→new width while the
        // viewport is already at the new one). An item wrapping UP a row
        // would be REVEALED at that interior line — left edge quasi-static,
        // growing rightward — instead of visibly entering. Shifting the
        // straddler's upper-row copy right by the gap keeps it right-
        // aligned with the actual window edge: it SLIDES IN from there,
        // which reads as the physical push-in. Shrinking rasters have the
        // fold outside the viewport (windowEdge < foldEdge): shift 0,
        // clipping at the window edge as before.
        // The window-edge push-in is a REFLOW-entry effect (growing raster,
        // fold inside the viewport). The live zoom gesture has no target to
        // enter — its fold wraps honestly (marquee), so no shift there.
        // With constant cells the fold line sits INSIDE the viewport
        // (whitespace right of it) — a straddling cell must slide over
        // the whitespace to/from the actual window edge instead of being
        // cut off at the invisible fold line. This applies to the pinned
        // zoom/pan too (its fold is cols·cellW as well); only the SCALE
        // gesture keeps the honest marquee wrap (its frozen raster is
        // painted uniformly scaled, there is no window-flush fold).
        const int windowEdge = cullRect.right() + 1;
        // The push-in bridges the gap between fold and window edge caused
        // by FOLD-WIDTH INTERPOLATION (growing reflow), and those frames
        // have no horizontal translation. A TRANSLATED plane (pan, and the
        // rigid-block settle after release) opens the same gap for a
        // different reason, and measuring it against the translated window
        // armed the push-in mid-settle the moment the shrinking gap fell
        // below one cell — the right-hand cells then visibly re-entered
        // from the window edge. Translated frames wrap marquee-style.
        int foldShift = (zoomScalePaint || glideOff.x() != 0)
            ? 0 : qMax(0, windowEdge - foldEdge);
        // The push-in is a REFLOW-entry effect and assumes the fold sits
        // just inside the viewport — the gap it bridges is sub-cell. A
        // PANNED plane puts the fold a whole period away, and shifting the
        // straddler by that much tears it out of its slot and leaves a
        // cell-wide hole behind. Beyond one cell the wrap is honest, like
        // the scale gesture's. (This is the condition the trace point has
        // been flagging all along.)
        if (foldShift > p.cellW) {
            TRACE_SLIDE("paint foldShift=%d foldEdge=%d off=(%d,%d) "
                        "(> cellW, push-in disabled)",
                        foldShift, foldEdge, glideOff.x(), glideOff.y());
            foldShift = 0;
        }

        auto drawCell = [&](const QRect& rect, int itemIdx, int clipLeft,
                            int clipRight) {
            if (!rect.intersects(cullRect))
                return;
            QStyleOptionViewItem option;
            option.initFrom(viewport());
            option.rect = rect;
            option.state |= QStyle::State_Enabled;
            option.state |= QStyle::State_Active;
            option.state &= ~QStyle::State_MouseOver;
            // EVERY copy of the focused item gets the frame. A cell that
            // wraps is drawn in two pieces — leaving on the left, entering
            // on the right a row up — and its frame has to be split the
            // same way. Marking only the first copy made the frame vanish
            // outright the moment that piece was culled and reappear at
            // the other one.
            if (itemIdx == (m_hoverIndex >= 0 ? m_hoverIndex
                                              : m_externalFocusRow))
                option.state |= QStyle::State_MouseOver;
            if (m_selected.contains(itemIdx))
                option.state |= QStyle::State_Selected;

            painter.save();
            // The wrap edge: outbound cells vanish there, the inbound twin
            // emerges from the left edge. Each copy is clipped to the fold
            // period it belongs to, so it cannot bleed into a neighbour's.
            painter.setClipRect(QRect(clipLeft, rect.y(),
                                      clipRight - clipLeft, rect.height()));
            const QModelIndex mi = m_sourceModel->index(itemIdx, MediaModel::Col_Thumbnail);
            m_delegate->paint(&painter, option, mi);
            painter.restore();
            countCell(mi);
        };

        // Items whose interpolated row band can touch the viewport (one
        // row of margin on both sides for cells sliding in or out; the
        // glide translation widens the band). Row 0 is the anchor's row —
        // X is measured from the top-left cell.
        // Items whose fold row can touch the viewport, INVERTED from the
        // visible rectangle instead of estimated from the viewport height
        // plus margins for the translation. The fold is invertible, so
        // this is exact for any translation — which is the whole point:
        // the margin estimate fell short as soon as the plane was panned,
        // and the items past its end were then simply never drawn ("the
        // last images disappear"). cullRect already carries the
        // translation, so it is in the same coordinates as (x, y) below.
        // One row of slack either way covers the wrap-around copies.
        const qreal cellH = qMax(1.0, p.cellH);
        const qreal rowMin = std::floor(cullRect.top() / cellH) - 1.0;
        const qreal rowMax = std::floor(cullRect.bottom() / cellH) + 1.0;
        const qreal Xmin = rowMin * p.foldW + cullRect.left() - p.cellW;
        const qreal Xmax = rowMax * p.foldW + cullRect.right() + p.cellW;
        const int iFirst = qMax(0,
            int(std::floor((Xmin - p.basePx) / p.cellW)));
        const int iLast = qMin(count - 1,
            int(std::ceil((Xmax - p.basePx) / p.cellW)));

        for (int i = iFirst; i <= iLast; ++i) {
            const qreal X = i * p.cellW + p.basePx;
            const qreal rowF = std::floor(X / p.foldW);
            const qreal x = X - rowF * p.foldW;          // in [0, foldW)
            const qreal y = rowF * p.cellH;
            const QSize cs(qRound(p.cellW), qRound(p.cellH));
            const QRect rect(QPoint(qRound(x), qRound(y)), cs);
            if (x + p.cellW > p.foldW) {
                // Straddles the fold: the upper-row copy enters from the
                // WINDOW edge, the lower-row twin slides out over the
                // row's left edge. The window-edge shift is GRADED by the
                // straddle depth — full at entry (cell right at the
                // edge), 0 once fully inside — so the position is
                // continuous when the straddling ends or a retarget
                // reshuffles rows (a binary shift jumped there). The
                // entering copy may briefly duplicate content still
                // visible in the lower row — deliberate: it must visibly
                // slide LEFT into its slot, never sit and be revealed.
                const qreal depth = (x + p.cellW - p.foldW) / p.cellW;
                const int shift = qRound(foldShift * depth);
                drawCell(rect.translated(shift, 0), i, 0,
                         foldShift > 0 ? windowEdge : foldEdge);
                drawCell(QRect(QPoint(qRound(x - p.foldW),
                                      qRound(y + p.cellH)), cs),
                         i, 0, foldEdge);
            } else {
                drawCell(rect, i, 0, foldEdge);
            }

            // WRAP-AROUND COPIES. The strip is folded with period foldW, so
            // the painted plane only covers [0, foldW) — one period. That
            // is enough while the fold sits at the window edge, but the PAN
            // slides the whole plane horizontally (translation off.x), and
            // then part of the window falls OUTSIDE the covered period and
            // stayed blank: a few cells, a wide gap, one stray cell (the
            // `paint foldShift` trace fires on exactly this).
            // What belongs in the uncovered part is the ADJACENT fold row —
            // the same items, one period over and one row back/forward.
            // Both copies are culled by rect against the viewport, so this
            // costs nothing while the fold is where it normally is.
            const int foldW = qRound(p.foldW);
            drawCell(QRect(QPoint(qRound(x - p.foldW), qRound(y + p.cellH)), cs),
                     i, -foldW, 0);
            // The strip right of the fold has TWO possible owners, and they
            // must not both paint into it. While the push-in is active (a
            // REFLOW entering the window edge, foldShift > 0) that strip is
            // its slide-in lane — the wrap copy on top of it is the odd
            // re-draw of the right-hand column at the end of a gesture.
            if (foldShift == 0)
                drawCell(QRect(QPoint(qRound(x + p.foldW), qRound(y - p.cellH)), cs),
                         i, foldEdge, foldEdge + foldW);
            // A STRADDLER is clipped at the fold, and its remainder is
            // drawn a row down — right while the fold sits at the window
            // edge. With the plane panned the window reaches PAST the
            // fold, and that clipped-off part is exactly the cell-wide
            // hole that appeared there. Draw it once more into the next
            // period, where the window now shows it.
            // …but ONLY when the window actually reaches past the fold.
            // With the fold at the window edge (the resting case) there is
            // nothing to show there, and drawing into that strip risks
            // painting over the last column on rounding alone.
            if (x + p.cellW > p.foldW && cullRect.right() >= foldEdge
                && foldShift == 0)
                drawCell(rect, i, foldEdge, foldEdge + foldW);
        }
        m_delegate->setIconSize(m_iconSize);
        paintBranch = pinnedFlowPaint ? "pinned"
                    : zoomScalePaint  ? "scale"
                    : m_panelSlideActive ? "slide" : "reflow";
        tracePaint();
        return;
    }

    const QSize cs = cellSize();
    const int cols = columns();
    // A translation (pinned-zoom residual, release glide) can pull rows
    // from beyond the untranslated viewport into view — extend the
    // painted band accordingly: rows ABOVE the first row for a downward
    // shift, extra rows below for an upward one
    const int glideRows =
        (qAbs(glideOff.y()) + cs.height() - 1) / qMax(1, cs.height());
    const int preRows = glideOff.y() > 0 ? glideRows : 0;
    const int visibleRows = qMax(1,
        (viewport()->height() + cs.height() - 1) / cs.height() + glideRows);
    const int first = firstVirtualIndex() - preRows * cols;
    const int totalVirtual = m_gridOffset + count;
    const int remainingVirtual = qMax(0, totalVirtual - first);
    const int remainingRows = (remainingVirtual + cols - 1) / cols;
    const int drawRows = qMax(0, qMin(visibleRows + preRows, remainingRows));

    int virtualIdx = first;

    for (int row = 0; row < drawRows; ++row) {
        const int y = (row - preRows) * cs.height();
        for (int col = 0; col < cols; ++col, ++virtualIdx) {
            const int itemIdx = virtualIdx - m_gridOffset;
            if (itemIdx < 0 || itemIdx >= count)
                continue;

            const QRect rect(col * cs.width(), y, cs.width(), cs.height());
            if (!rect.intersects(cullRect))
                continue;

            QStyleOptionViewItem option;
            option.initFrom(viewport());
            option.rect = rect;
            option.state |= QStyle::State_Enabled;
            option.state |= QStyle::State_Active;
            // initFrom sets State_MouseOver whenever the viewport is under the
            // mouse — that applies per widget, not per cell. Only the focused
            // cell may carry it: the mouse-hovered one, or (without hover)
            // the file of the Details/viewer window the mouse is in.
            option.state &= ~QStyle::State_MouseOver;
            if (itemIdx == (m_hoverIndex >= 0 ? m_hoverIndex
                                              : m_externalFocusRow))
                option.state |= QStyle::State_MouseOver;
            if (m_selected.contains(itemIdx))
                option.state |= QStyle::State_Selected;

            const QModelIndex mi = m_sourceModel->index(itemIdx, MediaModel::Col_Thumbnail);
            m_delegate->paint(&painter, option, mi);
            countCell(mi);
        }
    }
    tracePaint();
}

// A resize landing while the release settle runs invalidates its frozen
// target: the settle keeps gliding onto the raster it captured at commit,
// not the one that now exists. The classic trigger is the COMMIT ITSELF —
// anchorPinnedRaster writes the scroll position, the scrollbar appears,
// and Qt delivers the viewport resize through the event loop right after the
// settle targets were frozen. The settle then finished on the stale raster
// and the switch to the static branch snapped every cell onto the real one
// ("a jerk at the very end, after everything animated into place").
// Retargeting is jumpless: the currently painted state becomes the new
// From, the target is recomputed from the LIVE raster, the deadline keeps.
void GridView::retargetPinSettle()
{
    if (m_zoomPinItem < 0
        || !(m_zoomPinSettling || (m_zoomGestureActive && m_zoomPinned)))
        return;
    const qreal t = m_reflowAnim->state() == QAbstractAnimation::Running
        ? m_reflowAnim->currentValue().toReal()
        : 1.0;
    m_zoomPinFlowFrom = lerpFlow(m_zoomPinFlowFrom, m_zoomPinFlowTo, t);
    m_zoomPinPointFrom = m_zoomPinPointFrom
        + (m_zoomPinPointTo - m_zoomPinPointFrom) * t;

    const LayoutParams cur = currentLayoutParams();
    const int cols = qMax(1, cur.cols);
    const int vi = qMax(0, m_zoomPinItem) + cur.gridOffset;
    m_zoomPinFlowTo = pinRelativeFlow(cur, m_iconSize, m_zoomPinItem);
    m_zoomPinPointTo = QPointF(
        (qreal(vi % cols) + m_zoomPinFrac.x()) * cur.cell.width(),
        (qreal(vi / cols - cur.firstRow) + m_zoomPinFrac.y())
            * cur.cell.height());
    startZoomPinAnim(true);
    if (m_zoomPinSettling)
        m_zoomPinSettling =
            m_reflowAnim->state() == QAbstractAnimation::Running;
}

void GridView::resizeEvent(QResizeEvent* event)
{
    const int keepRow = firstRow();

    QAbstractScrollArea::resizeEvent(event);

    TRACE_SLIDE("grid resize %dx%d -> %dx%d cols=%d slide=%d t=%.3f",
                event->oldSize().width(), event->oldSize().height(),
                event->size().width(), event->size().height(), m_columns,
                int(m_panelSlideActive), m_panelSlideT);

    // The column count NEVER changes on a resize — the cells rescale to
    // keep filling the width (previews simply grow or shrink with the
    // window). Nothing refolds, so nothing animates; the top-left item
    // stays top-left by construction (same rows, same columns). Sharp
    // thumbnails arrive debounced once the size settles (timer); the
    // paint fallback scales the previous ones in the meantime.
    const int eff = iconSizeForWidth(viewport()->width());
    if (eff > 0 && eff != m_iconSize) {
        m_iconSize = eff;
        m_delegate->setIconSize(eff);
        // The debounce exists for INTERACTIVE resizes. Before anything was
        // painted (startup: window show, panel show, scrollbar appearing)
        // there is nothing to thrash — applying the size immediately avoids
        // a first load pass at a stale size that would be discarded and
        // re-requested when the timer fires. (Model resets end an active
        // panel slide, so this cannot fire per-frame during a slide.)
        if (!m_itemsPainted) {
            if (m_sourceModel && m_iconSize != m_sharpThumbSize) {
                m_sourceModel->invalidateThumbnails(QSize(m_iconSize, m_iconSize));
                m_sharpThumbSize = m_iconSize;
            }
        } else {
            m_refitInvalidateTimer->start();
        }
    }

    // (A resize rescales the cells and with them the scrollbar's pixel
    // value — updateScrollBarRange below does that conversion.)

    // If the window grew and the current row now over-scrolls past the
    // list end, keep it as bottom whitespace (released row by row when
    // scrolling up) instead of yanking the view down to the new maximum.
    if (keepRow > maxFirstRow())
        m_resizeAnchorRow = qMax(m_resizeAnchorRow, keepRow);
    updateScrollBarRange();
    retargetPinSettle();
    viewport()->update();
    updateVisibleRange();

    // Slider right end == max zoom-out for the new width. Deferred during
    // the panel slide (per-frame resizes; endPanelSlide re-syncs once).
    if (!m_panelSlideActive)
        updateSliderMaximum();
}

void GridView::wheelEvent(QWheelEvent* event)
{
    if (!m_sourceModel || m_sourceModel->rowCount() == 0) {
        QAbstractScrollArea::wheelEvent(event);
        return;
    }

    int angleY = event->angleDelta().y();
    if (angleY == 0) {
        QAbstractScrollArea::wheelEvent(event);
        return;
    }
    if (event->inverted())
        angleY = -angleY;

    if (event->modifiers() & Qt::ControlModifier) {
        if (m_sizeSlider) {
            // Zoom steps walk the COLUMN COUNT — exactly ONE column per
            // full 120-unit wheel notch (fast spins arrive coalesced in a
            // single event, hi-res wheels/touchpads as many small ones —
            // both must not change the per-notch step).
            m_zoomWheelRemainder += angleY;
            const int steps = m_zoomWheelRemainder / 120;   // >0 = zoom in
            m_zoomWheelRemainder %= 120;
            if (steps != 0) {
                // A NEW Ctrl-HOLD re-resolves the frame from where the
                // mouse ACTUALLY is. A frame left FROZEN by the PREVIOUS
                // zoom is stale orientation, not a pin target: re-pinning
                // it yanks that image back across the grid to the cursor
                // (user-rejected). If nothing sits under the cursor now,
                // the frame simply disappears and the slider path below
                // takes over. Resolved against the LOGICAL raster,
                // deliberately bypassing updateHover's freeze AND its
                // suspension — a running reflow/pin settle is purely
                // visual, its raster is already final, and going through
                // stopReflowAnimation here would break the smooth
                // retargeting of consecutive notches. WITHIN a hold the
                // frozen frame keeps ruling: that freeze is the whole
                // point of holding Ctrl down.
                if (!m_zoomGestureActive && !m_zoomSliderHold) {
                    const int live = QApplication::activeModalWidget()
                        ? -1
                        : indexAtPoint(viewport()->mapFromGlobal(QCursor::pos()));
                    if (live != m_hoverIndex) {
                        m_hoverIndex = live;
                        viewport()->update();
                        emitFocusChange();
                    }
                    m_hoverFrozen = false;   // re-armed right below
                }
                // Freeze the focus frame for orientation (released by a
                // deliberate mouse move via updateHover). Without a frame
                // this pins the ABSENCE of one: a cell reflowing under the
                // stationary cursor must not turn the next notch into a
                // pinned gesture halfway through the zoom.
                if (!m_hoverFrozen) {
                    m_hoverFrozen = true;
                    m_hoverFreezePos = QCursor::pos();
                }
                // NO focus frame: there is no image the zoom could be
                // about, so this is a plain zoom of the whole grid —
                // exactly what the slider does. Take the SLIDER's path
                // (top-left anchored, one reflow per step) instead of the
                // gesture machinery, which would preview a cursor-centred
                // scale and re-fold only on Ctrl release.
                if (!m_zoomGestureActive
                    && (m_zoomSliderHold
                        || (m_hoverIndex < 0 && m_externalFocusRow < 0))) {
                    if (!m_zoomSliderHold) {
                        // Hold the decision for the rest of this Ctrl press
                        // (Ctrl release detected exactly like a gesture's:
                        // the KeyRelease may go to any widget, the idle
                        // timer is the backstop)
                        m_zoomSliderHold = true;
                        qApp->installEventFilter(this);
                        m_zoomIdleTimer->start();
                    }
                    // Mirrored value: up = larger previews = higher value
                    // (fewer columns); setValue clamps at the ends
                    m_sizeSlider->setValue(m_sizeSlider->value() + steps);
                    event->accept();
                    return;
                }
                // With a focus frame the gesture PINS the framed item to
                // the cursor and reflows the live raster on every step
                // (see beginZoomGesture).
                if (!m_zoomGestureActive)
                    beginZoomGesture();
                updateZoomGesture(steps);
            }
        }
        event->accept();
        return;
    }

    // SNAPPED: one notch = one ROW, and sub-notch deltas (hi-res wheels)
    // accumulate in m_wheelRemainder until a full notch is complete —
    // that is what keeps every rest position row-aligned.
    // FREE: the wheel resolution follows the system instead of the raster —
    // one notch scrolls a row divided by the system's wheel-lines setting
    // (3 by default, i.e. three notches per row), sub-notch deltas move
    // proportionally, and a touchpad's pixelDelta is taken 1:1. The rest
    // position may then be anywhere between two rows.
    // An active over-scroll (m_resizeAnchorRow) is NOT cleared here — the
    // valueChanged handler releases it row by row as the user scrolls up,
    // and scrolling further down is simply capped at the current position.
    const qreal cellH = qMax(1, cellSize().height());
    if (!AppSettings::scrollSnapToGrid()) {
        m_wheelRemainder = 0;
        int pixelY = event->pixelDelta().y();
        if (event->inverted())
            pixelY = -pixelY;
        if (pixelY != 0) {
            // Touchpads deliver the finger movement itself — already
            // smooth, so gliding after it would only add lag
            scrollByPixels(-pixelY, false);
        } else {
            const qreal step = cellH / qMax(1, QApplication::wheelScrollLines());
            scrollByPixels(-angleY / 120.0 * step);
        }
    } else {
        m_wheelRemainder += angleY;
        const int steps = m_wheelRemainder / 120;
        m_wheelRemainder %= 120;
        if (steps == 0) {
            event->accept();
            return;
        }
        scrollByPixels(-steps * cellH);
    }
    viewport()->update();
    updateVisibleRange();
    // Scrolling moves a different item under the (stationary) cursor
    updateHover(viewport()->mapFromGlobal(QCursor::pos()));
    event->accept();
}

void GridView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
        return;
    }

    // The viewer opens WITHOUT touching the selection — the armed
    // single-click action of the first click is discarded
    cancelPendingClick();

    // A fast second Ctrl/Shift-click is another selection operation, not a
    // request to open the viewer
    if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
        handleSelectionClick(indexAtPoint(event->position().toPoint()),
                             event->modifiers());
        event->accept();
        return;
    }

    const int row = indexAtPoint(event->position().toPoint());
    if (row >= 0) {
        emit itemDoubleClicked(row);
        event->accept();
        return;
    }

    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void GridView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        // A fast second click on a DIFFERENT spot is a new single click,
        // not a double-click — apply the first click's pending action now
        flushPendingClick();

        m_dragStartPos = event->pos();
        m_dragStartIndex = indexAtPoint(event->pos());

        const bool ctrl  = event->modifiers() & Qt::ControlModifier;
        const bool shift = event->modifiers() & Qt::ShiftModifier;
        if (ctrl && !shift) {
            // Ctrl+press: either a selection TOGGLE (release without
            // movement) or the start of a plane PAN (drag) — decided in
            // mouseMoveEvent/mouseReleaseEvent. Never a file drag.
            m_ctrlPressPending = true;
            m_ctrlPressIndex = m_dragStartIndex;
            m_dragStartIndex = -1;
        } else if (shift || m_dragStartIndex < 0) {
            // Shift clicks cannot start a drag and never open the viewer;
            // a plain click on empty area only clears — both act
            // immediately.
            handleSelectionClick(m_dragStartIndex, event->modifiers());
        } else {
            // A plain press on an item changes NOTHING yet: it may be the
            // start of a drag (of the whole selection — collapsing here
            // would reduce the drag to one file) or of a double-click
            // (viewer; must not touch the selection). The click action is
            // armed on release and runs only after the double-click
            // interval expires without either.
            m_pendingClickIndex = m_dragStartIndex;
        }
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void GridView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_ctrlPressPending) {
            m_ctrlPressPending = false;
            if (m_zoomPanActive) {
                // The press became a PAN: releasing the button only lets
                // go of the plane — it stays where it is, the gesture
                // (and the settle) waits for the Ctrl release
                m_zoomPanActive = false;
            } else if ((event->pos() - m_dragStartPos).manhattanLength()
                           < QApplication::startDragDistance()) {
                // Release without movement: a Ctrl+CLICK — toggle the
                // item (deferred from press time, which is what makes it
                // distinguishable from a pan grab)
                handleSelectionClick(m_ctrlPressIndex, Qt::ControlModifier);
            }
            m_ctrlPressIndex = -1;
        }
        if (m_pendingClickIndex >= 0) {
            // No drag happened — arm the single-click action; a
            // double-click within the interval cancels it
            m_clickTimer->start(QApplication::doubleClickInterval());
        }
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void GridView::flushPendingClick()
{
    if (m_pendingClickIndex < 0)
        return;
    m_clickTimer->stop();
    const int idx = m_pendingClickIndex;
    m_pendingClickIndex = -1;
    handleSelectionClick(idx, Qt::NoModifier);
}

void GridView::cancelPendingClick()
{
    m_clickTimer->stop();
    m_pendingClickIndex = -1;
    m_ctrlPressPending = false;   // stale index after model changes
    m_ctrlPressIndex = -1;
}

void GridView::handleSelectionClick(int index, Qt::KeyboardModifiers mods)
{
    const bool ctrl  = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;

    if (index < 0) {
        // Click on empty area: plain click clears the selection
        if (!ctrl && !shift && !m_selected.isEmpty())
            clearSelection();
        return;
    }

    if (shift) {
        // Additive range to the nearest selected item — mirrors the
        // DirectoryPanel shift+click semantics. Existing selections are kept.
        if (m_selected.isEmpty()) {
            m_selected.insert(index);
        } else {
            int nearest = -1;
            int bestDist = -1;
            for (int s : std::as_const(m_selected)) {
                const int d = qAbs(s - index);
                if (bestDist < 0 || d < bestDist) {
                    bestDist = d;
                    nearest = s;
                }
            }
            for (int i = qMin(nearest, index); i <= qMax(nearest, index); ++i)
                m_selected.insert(i);
        }
    } else if (ctrl) {
        // Toggle the clicked item, keep the rest
        if (m_selected.contains(index))
            m_selected.remove(index);
        else
            m_selected.insert(index);
    } else {
        // Plain click: select exclusively — a click into a multi-selection
        // collapses it to the clicked item. Only re-clicking a single-item
        // selection toggles it off.
        if (m_selected.size() == 1 && m_selected.contains(index))
            m_selected.clear();
        else
            m_selected = {index};
    }

    viewport()->update();
    emitSelection();
}

void GridView::clearSelection()
{
    if (m_selected.isEmpty())
        return;
    m_selected.clear();
    viewport()->update();
    emitSelection();
}

void GridView::emitSelection()
{
    emit selectionChanged(selectedRows());
}

QList<int> GridView::selectedRows() const
{
    QList<int> rows = m_selected.values();
    std::sort(rows.begin(), rows.end());
    return rows;
}

void GridView::setSelectedRows(const QList<int>& rows)
{
    // Mirror of the list view's selection. Returning early when nothing
    // changes is what breaks the sync loop between the two views — both
    // sides re-emit their (identical) selection otherwise.
    QSet<int> next;
    const int count = m_sourceModel ? m_sourceModel->rowCount() : 0;
    for (int r : rows)
        if (r >= 0 && r < count)
            next.insert(r);
    if (next == m_selected)
        return;
    m_selected = next;
    viewport()->update();
    emitSelection();
}

void GridView::setExternalFocusPath(const QString& path)
{
    if (m_externalFocusPath == path)
        return;
    m_externalFocusPath = path;
    m_externalFocusRow = rowForPath(path);
    viewport()->update();
    emitFocusChange();
}

void GridView::freezeFocusOnPath(const QString& path)
{
    const int row = rowForPath(path);
    if (row < 0)
        return;

    // Scroll the item into view with minimal movement — unlike the external
    // focus (no auto-scroll), this is an explicit orientation aid: the user
    // returns from the fullscreen viewer and wants to see where they were.
    const int cols = qMax(1, columns());
    const int gridRow = (row + m_gridOffset) / cols;
    const int first = firstRow();
    const int fullyVisibleRows =
        qMax(1, viewport()->height() / qMax(1, cellSize().height()));
    if (gridRow < first)
        setFirstRow(gridRow);
    else if (gridRow >= first + fullyVisibleRows)
        setFirstRow(gridRow - fullyVisibleRows + 1);

    // Freeze the frame on the item, released like the Ctrl+wheel zoom
    // freeze (deliberate mouse move via updateHover, leave, modal, reset)
    m_hoverIndex = row;
    m_hoverFrozen = true;
    m_hoverFreezePos = QCursor::pos();
    viewport()->update();
    updateVisibleRange();
    emitFocusChange();
}

void GridView::emitFocusChange()
{
    const int row = m_hoverIndex >= 0 ? m_hoverIndex : m_externalFocusRow;
    if (row != m_lastFocusRow) {
        m_lastFocusRow = row;
        emit focusItemChanged(row);
    }
    // Displayed whenever the frame the grid shows IS the external item:
    // either no hover overrides it, or the mouse hovers exactly that item —
    // a Details dialog opens on the very item under the cursor, and its
    // border must light up immediately, not only after the hover moves.
    const bool displayed = m_externalFocusRow >= 0
        && (m_hoverIndex < 0 || m_hoverIndex == m_externalFocusRow);
    if (displayed != m_lastExternalDisplayed) {
        m_lastExternalDisplayed = displayed;
        emit externalFocusDisplayChanged(displayed);
    }
}

int GridView::rowForPath(const QString& path) const
{
    if (!m_sourceModel || path.isEmpty())
        return -1;
    for (int r = 0; r < m_sourceModel->rowCount(); ++r)
        if (m_sourceModel->item(r).filePath == path)
            return r;
    return -1;
}

void GridView::updateHover(const QPoint& viewportPos)
{
    // No hover re-evaluation during the panel slide / fullscreen glide:
    // hit testing runs on the LIVE raster while the painted state is the
    // transition lerp — the frame would hop between items (and the
    // per-frame resizes fire synthetic enter events constantly). The
    // transition end re-evaluates once (endPanelSlide / glide finished).
    if (hoverSuspended()) {
        // Which flag holds it — otherwise a stuck frame is indistinguishable
        // from "the mouse did not move" in a trace.
        TRACE_SLIDE("hover SUSPENDED (ext=%d slide=%d zoom=%d settle=%d glide=%d)",
                    int(m_hoverExternallySuspended), int(m_panelSlideActive),
                    int(m_zoomGestureActive), int(m_zoomPinSettling),
                    int(m_vpGlide->state() == QAbstractAnimation::Running));
        return;
    }
    // Ctrl+wheel zoom freezes the frame on the item it marked at gesture
    // start — the top-left anchor slides other items under the stationary
    // cursor, and a wandering frame would break orientation. Only a
    // significant mouse move (Settings → Input, shared mouse threshold)
    // hands the frame back to the cursor position.
    if (m_hoverFrozen) {
        const int moved = (QCursor::pos() - m_hoverFreezePos).manhattanLength();
        if (moved < AppSettings::mouseThresholdPx()) {
            TRACE_SLIDE("hover FROZEN (moved=%d < threshold=%d)",
                        moved, AppSettings::mouseThresholdPx());
            return;
        }
        m_hoverFrozen = false;
    }

    // Hover is independent of the selection — an item can be hovered and
    // selected at the same time. While a modal dialog is open there is no
    // hover focus (the dialog acts on the selection, not the hovered item).
    const int idx = QApplication::activeModalWidget()
        ? -1 : indexAtPoint(viewportPos);
    if (idx == m_hoverIndex)
        return;
    TRACE_SLIDE("hover %d -> %d (slide=%d)", m_hoverIndex, idx,
                int(m_panelSlideActive));
    m_hoverIndex = idx;
    viewport()->update();
    emitFocusChange();
}

void GridView::clearHover()
{
    m_hoverFrozen = false;   // leave/modal/model reset ends the freeze
    if (m_hoverIndex == -1)
        return;
    m_hoverIndex = -1;
    viewport()->update();
    emitFocusChange();
}

void GridView::leaveEvent(QEvent* event)
{
    // Transitions resize the widget every frame, which makes Qt synthesize
    // Leave/Enter pairs under a STATIONARY cursor — clearing here would
    // also kill the hover freeze and re-open per-frame re-evaluation. A
    // genuine leave is corrected by the end-of-transition re-evaluation.
    if (!m_contextMenuOpen && !hoverSuspended())
        clearHover();
    QAbstractScrollArea::leaveEvent(event);
}

void GridView::enterEvent(QEnterEvent* event)
{
    // Re-evaluate immediately, not only on the first move — the cursor can
    // re-enter without moving (closing a window that covered the grid)
    updateHover(viewport()->mapFromGlobal(QCursor::pos()));
    QAbstractScrollArea::enterEvent(event);
}

void GridView::mouseMoveEvent(QMouseEvent* event)
{
    // Ctrl+left held: crossing the drag threshold turns the press into a
    // PAN of the grid plane (the armed selection toggle is discarded on
    // release then). The pan engages the pinned gesture's mouse-follow.
    if (m_ctrlPressPending && (event->buttons() & Qt::LeftButton)
        && (event->modifiers() & Qt::ControlModifier)
        && (m_zoomPanActive
            || (event->pos() - m_dragStartPos).manhattanLength()
                   >= QApplication::startDragDistance())) {
        if (!m_zoomGestureActive) {
            if (!m_hoverFrozen) {
                m_hoverFrozen = true;
                m_hoverFreezePos = QCursor::pos();
            }
            beginZoomGesture(true);   // grab works anywhere (nearest item)
        }
        if (m_zoomPinned) {
            // Engaging the pan (fresh grab or RE-GRAB with Ctrl still
            // held): the grab point is where THIS press went down — not
            // the previous grab's pin, which would make the plane jump
            // under the new press position
            if (!m_zoomPanActive) {
                resolvePinAt(QPointF(m_dragStartPos));
                m_zoomPanLastMouse = QPointF(m_dragStartPos);
            }
            m_zoomPanActive = true;
            zoomPinFollowMouse(event->position());
        }
        event->accept();
        return;
    }

    // While the pinned zoom gesture is active, plain mouse movement only
    // moves the ZOOM CENTER (re-resolved at the next wheel step) — the
    // plane itself moves only while the pan is engaged. Hover stays
    // suspended/frozen either way.
    if (m_zoomGestureActive && m_zoomPinned) {
        if (m_zoomPanActive)
            zoomPinFollowMouse(event->position());
        event->accept();
        return;
    }

    if (!(event->buttons() & Qt::LeftButton) || m_dragStartIndex < 0) {
        updateHover(event->pos());
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }

    if ((event->pos() - m_dragStartPos).manhattanLength() < QApplication::startDragDistance())
        return;

    if (!m_sourceModel || m_dragStartIndex >= m_sourceModel->rowCount())
        return;

    // It IS a drag — the pending click action must not fire
    cancelPendingClick();

    // Dragging a selected item drags the whole selection
    QList<QUrl> urls;
    if (m_selected.contains(m_dragStartIndex))
        urls = selectedUrls();
    else
        urls.append(QUrl::fromLocalFile(m_sourceModel->item(m_dragStartIndex).filePath));

    auto* mimeData = new QMimeData;
    mimeData->setUrls(urls);

    auto* drag = new QDrag(this);
    drag->setMimeData(mimeData);

    // Use thumbnail from model's data() for drag pixmap
    QPixmap px = m_sourceModel->data(
        m_sourceModel->index(m_dragStartIndex, MediaModel::Col_Thumbnail),
        Qt::DecorationRole).value<QPixmap>();
    if (!px.isNull()) {
        QPixmap pm = px.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(pm.width() / 2, pm.height() / 2));
    }

    drag->exec(Qt::CopyAction);
    m_dragStartIndex = -1;
}

void GridView::setDropTargetDir(const QString& dir)
{
    m_dropTargetDir = dir;
}

GridView::ViewStateSnapshot GridView::viewStateSnapshot() const
{
    ViewStateSnapshot snap;
    if (!m_sourceModel)
        return snap;
    const int count = m_sourceModel->rowCount();
    for (int r : std::as_const(m_selected))
        if (r >= 0 && r < count)
            snap.selectedPaths.append(m_sourceModel->item(r).filePath);

    const int firstItem = firstVirtualIndex() - m_gridOffset;
    if (firstItem >= 0 && firstItem < count)
        snap.topLeftPath = m_sourceModel->item(firstItem).filePath;
    return snap;
}

void GridView::setPendingViewState(const ViewStateSnapshot& state)
{
    m_savedSelectedPaths = QSet<QString>(state.selectedPaths.cbegin(),
                                         state.selectedPaths.cend());
    m_savedTopLeftPath = state.topLeftPath;
}

void GridView::captureViewState()
{
    if (!m_sourceModel)
        return;
    const int count = m_sourceModel->rowCount();
    // Async rescans reset twice: clear (count 0), then scan result. The
    // empty intermediate state must not overwrite what we captured from the
    // real content.
    if (count == 0)
        return;

    m_savedSelectedPaths.clear();
    m_savedTopLeftPath.clear();
    for (int r : std::as_const(m_selected))
        if (r >= 0 && r < count)
            m_savedSelectedPaths.insert(m_sourceModel->item(r).filePath);

    const int firstItem = firstVirtualIndex() - m_gridOffset;
    if (firstItem >= 0 && firstItem < count)
        m_savedTopLeftPath = m_sourceModel->item(firstItem).filePath;
}

void GridView::restoreViewState()
{
    // Empty intermediate reset: keep the saved state for the reset that
    // delivers the scan result
    if (m_sourceModel && m_sourceModel->rowCount() == 0
        && (!m_savedSelectedPaths.isEmpty() || !m_savedTopLeftPath.isEmpty())) {
        m_selected.clear();
        setFirstRow(0);
        emitSelection();
        return;
    }

    m_selected.clear();
    int topLeftRow = -1;

    if (m_sourceModel && (!m_savedSelectedPaths.isEmpty() || !m_savedTopLeftPath.isEmpty())) {
        const int count = m_sourceModel->rowCount();
        for (int i = 0; i < count; ++i) {
            const QString& fp = m_sourceModel->item(i).filePath;
            if (m_savedSelectedPaths.contains(fp))
                m_selected.insert(i);
            if (topLeftRow < 0 && fp == m_savedTopLeftPath)
                topLeftRow = i;
        }
    }
    m_savedSelectedPaths.clear();
    m_savedTopLeftPath.clear();

    // The previous top-left item stays the top-left item (row-aligned);
    // gone or never captured → top. gridOffset is 0 after a reset, so the
    // item's row is simply index / columns.
    const int cols = qMax(1, columns());
    setFirstRow(topLeftRow > 0 ? topLeftRow / cols : 0);
    emitSelection();
}

QList<QUrl> GridView::selectedUrls() const
{
    QList<int> rows = m_selected.values();
    std::sort(rows.begin(), rows.end());

    QList<QUrl> urls;
    urls.reserve(rows.size());
    for (int r : rows) {
        if (m_sourceModel && r < m_sourceModel->rowCount())
            urls.append(QUrl::fromLocalFile(m_sourceModel->item(r).filePath));
    }
    return urls;
}

void GridView::selectAll()
{
    if (!m_sourceModel || m_sourceModel->rowCount() == 0)
        return;
    m_selected.clear();
    for (int i = 0; i < m_sourceModel->rowCount(); ++i)
        m_selected.insert(i);
    viewport()->update();
    emitSelection();
}

void GridView::copySelection()
{
    FileOps::setClipboardFiles(selectedUrls(), FileOps::Op::Copy);
}

void GridView::cutSelection()
{
    FileOps::setClipboardFiles(selectedUrls(), FileOps::Op::Move);
}

void GridView::requestFilterToSelection()
{
    if (m_selected.isEmpty())
        return;
    QList<int> rows = m_selected.values();
    std::sort(rows.begin(), rows.end());
    emit filterToSelectionRequested(rows);
}

bool GridView::canPaste() const
{
    return !m_dropTargetDir.isEmpty()
        && !FileOps::clipboardFiles().urls.isEmpty();
}

void GridView::pasteFromClipboard()
{
    if (m_dropTargetDir.isEmpty())
        return;

    const FileOps::ClipboardFiles cf = FileOps::clipboardFiles();
    if (cf.urls.isEmpty())
        return;

    if (FileOps::perform(cf.urls, m_dropTargetDir, cf.op, this) > 0) {
        // Cut is one-shot: after a successful move the clipboard is cleared,
        // matching file manager behavior
        if (cf.op == FileOps::Op::Move)
            QGuiApplication::clipboard()->clear();
        emit filesDropped();
    }
}

void GridView::contextMenuEvent(QContextMenuEvent* event)
{
    // A still-pending left click was a real click — let it take effect
    // before the menu reads the selection
    flushPendingClick();

    const int idx = indexAtPoint(viewport()->mapFrom(this, event->pos()));

    // Right-clicking an item OUTSIDE the selection makes it the selection
    // first (the Dolphin/Finder/Explorer convention). Without this the menu
    // opens with almost every entry greyed out — on exactly the item the
    // user pointed at. Right-clicking INSIDE the selection leaves it alone,
    // so a carefully built multi-selection survives, and empty area never
    // touches it either (the menu then still offers the selection actions).
    if (idx >= 0 && !m_selected.contains(idx)) {
        m_selected = {idx};
        viewport()->update();
        emitSelection();
    }

    MediaContextMenu::Context ctx;
    ctx.hasItems       = m_sourceModel && m_sourceModel->rowCount() > 0;
    ctx.selectionCount = m_selected.size();
    ctx.pastePossible  = canPaste();
    ctx.filtered       = m_sourceModel && m_sourceModel->isFiltered();
    // idx was captured at event time — opening the menu fires leaveEvent
    // and clears m_hoverIndex.
    ctx.hoverItemValid = idx >= 0;

    m_contextMenuOpen = true;
    const auto chosen = MediaContextMenu::exec(this, event->globalPos(), ctx);
    m_contextMenuOpen = false;
    // The popup swallowed the mouse events meanwhile — re-evaluate at the
    // current cursor position (no Enter event follows; from Qt's point of
    // view the cursor never left the widget)
    updateHover(viewport()->mapFromGlobal(QCursor::pos()));

    using A = MediaContextMenu::Action;
    switch (chosen) {
    case A::None:      break;
    case A::Details:   emit detailsRequested(idx); break;
    case A::SelectAll: selectAll(); break;
    case A::Copy:      copySelection(); break;
    case A::Cut:       cutSelection(); break;
    case A::CopyPaths: {
        // Plain-text paths (one per line) — for terminals, chats, scripts
        QStringList paths;
        for (const QUrl& u : selectedUrls())
            paths.append(u.toLocalFile());
        QGuiApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
        break;
    }
    case A::Paste:       pasteFromClipboard(); break;
    case A::ClearFilter: emit clearFilterRequested(); break;
    default: {
        const QList<int> rows = selectedRows();
        if (chosen == A::EditMetadata)           emit editMetadataRequested(rows);
        else if (chosen == A::Rename)            emit renameRequested(rows);
        else if (chosen == A::FilterBySelection) emit filterToSelectionRequested(rows);
        else                                     emit deleteRequested(rows, chosen == A::DeletePermanent);
        break;
    }
    }
    event->accept();
}

void GridView::dragEnterEvent(QDragEnterEvent* event)
{
    // Accept external file drags only when the grid shows exactly one root
    // directory (the drop target). Own drags must not drop onto ourselves.
    if (m_dropTargetDir.isEmpty() || !event->mimeData()->hasUrls()
        || event->source() == this || event->source() == viewport()) {
        event->ignore();
        return;
    }
    // Always report Copy so the source never deletes anything itself; a
    // "move" is performed exclusively by our own code.
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void GridView::dragMoveEvent(QDragMoveEvent* event)
{
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void GridView::dropEvent(QDropEvent* event)
{
    if (m_dropTargetDir.isEmpty() || !event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }

    event->setDropAction(Qt::CopyAction);
    event->accept();

    const QList<QUrl> urls = event->mimeData()->urls();
    const auto op = FileOps::askDropMode(this, QCursor::pos());
    if (!op)
        return;

    if (FileOps::perform(urls, m_dropTargetDir, *op, this) > 0)
        emit filesDropped();
}

void GridView::updateVisibleRange()
{
    if (!m_sourceModel)
        return;

    const int count = m_sourceModel->rowCount();
    if (count <= 0)
        return;

    if (m_zoomGestureActive && !m_zoomPinned) {
        // Scale variant: the logical raster is frozen; the visible items
        // follow the scaled display (scaledFlow translated by the zoom
        // origin). Mirror the paint's strip-range math so thumbnails load
        // for what is on screen. (The pinned variant keeps the raster LIVE
        // and takes the normal path below.)
        const FlowParams p = m_zoomScaledFlow;
        const qreal h = qMax(1, viewport()->height());
        const qreal rowLo = (-m_zoomOrigin.y()) / p.cellH - 1.0;
        const qreal rowHi = (h - m_zoomOrigin.y()) / p.cellH + 1.0;
        const int firstItem = qMax(0,
            int(std::floor((rowLo * p.foldW - p.basePx) / p.cellW)));
        const int lastItem = qMin(count - 1,
            int(std::ceil((rowHi * p.foldW - p.basePx) / p.cellW)));
        m_visibleItemFirst = firstItem;
        m_visibleItemLast  = qMax(firstItem, lastItem);
        m_sourceModel->setVisibleRows(m_visibleItemFirst, m_visibleItemLast);
        return;
    }

    const QSize cs = cellSize();
    const int cols = columns();
    // + subRowPx(): a position between rows shows one row more
    const int visibleRows = qMax(1,
        (viewport()->height() + subRowPx() + cs.height() - 1) / cs.height());

    const int first = firstVirtualIndex();
    const int firstItem = qMax(0, first - m_gridOffset);
    const int lastVirtual = first + visibleRows * cols - 1;
    const int lastItem = qMin(count - 1, lastVirtual - m_gridOffset);
    m_visibleItemFirst = firstItem;
    m_visibleItemLast  = lastItem;
    m_sourceModel->setVisibleRows(firstItem, lastItem);
}

int GridView::iconSizeForWidth(int w) const
{
    // Width-filling preview for the current column count: cell width =
    // w / columns (integer division — the sub-column remainder, at most
    // columns−1 px, stays right of the raster), preview = cell − padding.
    if (w <= 0)
        return m_iconSize;   // startup: no settled viewport yet
    const int padW = baseCellSize().width() - m_delegate->iconSize();
    return qMax(1, w / qMax(1, m_columns) - padW);
}

QSize GridView::baseCellSize() const
{
    if (!m_delegate)
        return QSize(m_iconSize + 8, m_iconSize + 34);
    return m_delegate->sizeHint(QStyleOptionViewItem(), QModelIndex());
}

QSize GridView::cellSize() const
{
    // The COLUMN COUNT is the zoom state — cells always fill the width
    // (viewportWidth / columns; m_iconSize is kept in sync by resize and
    // zoom paths), so a window resize rescales the previews instead of
    // changing the column count. (Fourth iteration of the sizing model,
    // all user-decided: stretching the PREVIEW within nominal cells read
    // as "the preview changes when I resize"; stretching the CELLS made
    // the padding breathe; constant cells left right-edge whitespace and
    // refolded on every resize. Constant COLUMNS avoid both the churn
    // and the whitespace — resizes scale, only zooming refolds.)
    return baseCellSize();
}

int GridView::columns() const
{
    return qMax(1, m_columns);
}

int GridView::firstVirtualIndex() const
{
    return firstRow() * columns();
}

int GridView::maxFirstRow() const
{
    if (!m_sourceModel)
        return 0;

    const int count = m_sourceModel->rowCount();
    if (count <= 0)
        return 0;

    const int cols = columns();
    const QSize cs = cellSize();
    const int fullyVisibleRows = qMax(1, viewport()->height() / qMax(1, cs.height()));
    const int totalVirtual = m_gridOffset + count;
    const int totalRows = (totalVirtual + cols - 1) / cols;
    return qMax(0, totalRows - fullyVisibleRows);
}

int GridView::effectiveMaxFirstRow() const
{
    const int maxRow = maxFirstRow();
    if (m_resizeAnchorRow < 0 || !m_sourceModel)
        return maxRow;
    return qMax(maxRow, m_resizeAnchorRow);
}

int GridView::indexAtPoint(const QPoint& p) const
{
    if (!m_sourceModel || m_sourceModel->rowCount() <= 0)
        return -1;

    const QSize cs = cellSize();
    if (cs.width() <= 0 || cs.height() <= 0)
        return -1;

    if (!viewport()->rect().contains(p))
        return -1;

    const int count = m_sourceModel->rowCount();
    const int first = firstVirtualIndex();
    const int cols = columns();
    const int totalVirtual = m_gridOffset + count;
    const int remainingVirtual = qMax(0, totalVirtual - first);
    const int remainingRows = (remainingVirtual + cols - 1) / cols;
    // Resting between rows (snapping off) shifts the raster up by
    // subRowPx() and makes one more row reachable. Hit testing follows
    // the LOGICAL raster, i.e. the scroll target — a running glide is
    // purely visual, exactly like a running reflow.
    const int visibleRows = qMax(1,
        (viewport()->height() + subRowPx() + cs.height() - 1) / cs.height());
    const int drawRows = qMax(0, qMin(visibleRows, remainingRows));

    const int col = p.x() / cs.width();
    const int row = (p.y() + subRowPx()) / cs.height();
    if (col < 0 || col >= cols || row < 0)
        return -1;
    if (row >= drawRows)
        return -1;

    const int virtualIdx = first + row * cols + col;
    const int itemIdx = virtualIdx - m_gridOffset;
    return (itemIdx >= 0 && itemIdx < count) ? itemIdx : -1;
}

// The scrollbar counts PIXELS, not rows: only that gives the handle a
// position between two rows, which is what makes a drag follow the mouse
// instead of hopping from row to row. The RASTER is still row-based —
// firstRow()/subRowPx() split the value, and everything downstream works
// in rows exactly as before.
void GridView::updateScrollBarRange()
{
    const int cellH = qMax(1, cellSize().height());
    int old = verticalScrollBar()->value();
    // Resize or zoom rescaled the cells: re-express the position in the new
    // cell height, or the row it stands for would silently change
    if (m_scrollCellH > 0 && m_scrollCellH != cellH)
        old = int(qRound(qreal(old) * cellH / m_scrollCellH));
    m_scrollCellH = cellH;

    const int maxPx = effectiveMaxFirstRow() * cellH;
    verticalScrollBar()->setRange(0, maxPx);

    const int fullyVisibleRows = qMax(1, viewport()->height() / cellH);
    verticalScrollBar()->setPageStep(fullyVisibleRows * cellH);
    verticalScrollBar()->setSingleStep(cellH);   // arrow button = one row
    verticalScrollBar()->setValue(qBound(0, old, maxPx));
}

int GridView::firstRow() const
{
    return verticalScrollBar()->value() / qMax(1, cellSize().height());
}

int GridView::subRowPx() const
{
    return verticalScrollBar()->value() % qMax(1, cellSize().height());
}

void GridView::setFirstRow(int row)
{
    verticalScrollBar()->setValue(qBound(0, row, effectiveMaxFirstRow())
                                 * qMax(1, cellSize().height()));
}

// Wheel scrolling. The scroll amount is a pixel distance (one row per
// notch when snapped, a system-sized fraction of one when free — see
// wheelEvent); snapping quantizes the TARGET, so the position comes to
// rest on a row.
void GridView::scrollByPixels(qreal dy, bool animate)
{
    const qreal cellH = qMax(1, cellSize().height());
    const int maxPx = effectiveMaxFirstRow() * int(cellH);
    qreal target = qBound(0.0, verticalScrollBar()->value() + dy, qreal(maxPx));
    if (AppSettings::scrollSnapToGrid())
        target = qRound(target / cellH) * cellH;
    applyScrollPx(qBound(0, int(qRound(target)), maxPx), animate);
}

// Writing the target position immediately (instead of animating the
// scrollbar) keeps every layout/prefetch path on the FINAL position — the
// animation is purely how it gets there on screen, exactly like the reflow.
void GridView::applyScrollPx(int px, bool animate)
{
    const qreal paintedAbs = verticalScrollBar()->value() + m_scrollLagPx;
    m_inSmoothScroll = true;
    verticalScrollBar()->setValue(px);
    m_inSmoothScroll = false;
    // Unchanged painted position (the value may have been clamped) — the
    // glide below is what actually moves the content
    m_scrollLagPx = paintedAbs - verticalScrollBar()->value();
    if (animate) {
        startScrollAnimation();
    } else {
        m_scrollAnim->stop();
        m_scrollLagPx = 0;
        viewport()->update();
    }
}

// Scrollbar interactions arrive as a finished value change (Qt moved the
// value, the raster followed): keep the painted position where it was and
// glide from there, exactly like the wheel path.
void GridView::glideScrollTo(int fromPx, int toPx)
{
    m_scrollLagPx += qreal(fromPx - toPx);
    startScrollAnimation();
}

void GridView::startScrollAnimation()
{
    m_scrollAnim->stop();
    // A glide is a painted band: every row between the painted and the
    // target position has to be drawn. Dragging the handle across the
    // whole list would fly through hundreds of rows — cap the distance at
    // roughly one screen, which still reads as "it came from over there"
    // but keeps the paint bounded.
    const qreal cap = qMax(qreal(cellSize().height()), qreal(viewport()->height()));
    m_scrollLagPx = qBound(-cap, m_scrollLagPx, cap);
    const int ms = AppSettings::scrollAnimationMs();
    // Nothing on screen yet (startup) must not animate — same rule as the
    // reflow (see beginReflowCapture)
    if (ms <= 0 || !m_itemsPainted || qAbs(m_scrollLagPx) < 0.5) {
        m_scrollLagPx = 0;
        viewport()->update();
        return;
    }
    // Consecutive notches restart from the CURRENT lag with the full
    // duration: the distance grows while the glide is still running, so a
    // fast spin simply scrolls faster.
    m_scrollAnim->setStartValue(m_scrollLagPx);
    m_scrollAnim->setEndValue(0.0);
    m_scrollAnim->setDuration(ms);
    m_scrollAnim->start();
    viewport()->update();
}

void GridView::stopScrollAnimation(bool snapToRow)
{
    m_scrollAnim->stop();
    m_scrollLagPx = 0;
    if (snapToRow && m_sourceModel) {
        // Land on the nearest row. The zoom paths anchor on the raster
        // (top-left item, pinned cell), which only means what it says while
        // the raster is what is painted.
        const int cellH = qMax(1, cellSize().height());
        if (verticalScrollBar()->value() % cellH != 0)
            setFirstRow(int(qRound(qreal(verticalScrollBar()->value()) / cellH)));
    }
    viewport()->update();
}

GridView::LayoutParams GridView::currentLayoutParams() const
{
    LayoutParams p;
    p.cell = cellSize();
    p.cols = qMax(1, columns());
    p.firstRow = firstRow();
    p.gridOffset = m_gridOffset;
    return p;
}

GridView::FlowParams GridView::flowParams(const LayoutParams& p, int iconSize)
{
    FlowParams f;
    f.cellW = p.cell.width();
    f.cellH = p.cell.height();
    f.foldW = qreal(p.cols) * p.cell.width();
    f.basePx = qreal(p.gridOffset - p.firstRow * p.cols) * p.cell.width();
    f.iconSize = iconSize;
    return f;
}

GridView::FlowParams GridView::lerpFlow(const FlowParams& a, const FlowParams& b,
                                        qreal t)
{
    auto lerp = [t](qreal x, qreal y) { return x + (y - x) * t; };
    FlowParams f;
    f.cellW = lerp(a.cellW, b.cellW);
    f.cellH = lerp(a.cellH, b.cellH);
    f.foldW = lerp(a.foldW, b.foldW);
    f.basePx = lerp(a.basePx, b.basePx);
    f.iconSize = lerp(a.iconSize, b.iconSize);
    return f;
}

GridView::FlowParams GridView::scaleFlow(const FlowParams& f, qreal s)
{
    FlowParams r;
    r.cellW = f.cellW * s;
    r.cellH = f.cellH * s;
    r.foldW = f.foldW * s;
    r.basePx = f.basePx * s;
    r.iconSize = f.iconSize * s;
    return r;
}

void GridView::beginReflowCapture(const LayoutParams& pre, int preIconSize)
{
    m_reflowCaptured = false;
    // Side-panel slide: the per-frame resizes belong to the slide's own
    // frozen from→to flow — no chained per-frame reflows on top of it.
    // Fullscreen transition overlay: the switch resize must snap — the
    // window is invisible or its grid empty (see setReflowSuppressed)
    if (m_panelSlideActive || m_reflowExternallySuppressed) {
        stopReflowAnimation();
        return;
    }
    // During a fullscreen glide the raster flow is part of the transition
    // and runs even when the standalone reflow animation is disabled
    const bool gliding = m_vpGlide->state() == QAbstractAnimation::Running;
    if ((AppSettings::reflowAnimationMs() <= 0 && !gliding) || !m_sourceModel
        || m_sourceModel->rowCount() == 0) {
        stopReflowAnimation();   // also covers "setting switched off mid-flight"
        return;
    }
    // A state that was never on screen must not animate away (startup: the
    // scrollbar appears when the first items arrive and refits the still
    // unseen layout). A RUNNING animation is exempt — its interpolated
    // frames are what is on screen right now (rapid-step chaining).
    if (m_reflowAnim->state() != QAbstractAnimation::Running && !m_itemsPainted)
        return;

    // `pre` is the raster before the mutation, i.e. the previous target.
    // With a running animation the currently painted state is the lerp
    // towards it — materializing that as the new start makes rapid steps
    // chain without a jump. Without one, the start IS the pre raster.
    const FlowParams preFlow = flowParams(pre, preIconSize);
    m_reflowChained = m_reflowAnim->state() == QAbstractAnimation::Running;
    if (m_reflowChained)
        m_reflowFrom = lerpFlow(m_reflowFrom, preFlow,
                                m_reflowAnim->currentValue().toReal());
    else
        m_reflowFrom = preFlow;
    m_reflowCaptured = true;
}

void GridView::startReflowAnimation()
{
    if (!m_reflowCaptured)
        return;
    m_reflowCaptured = false;
    m_reflowAnim->stop();
    int ms = AppSettings::reflowAnimationMs();
    // Fullscreen transition: every raster change belongs to the glide and
    // ends together with it — the top-left image arriving at its target
    // position IS the end of the whole transition
    if (m_vpGlide->state() == QAbstractAnimation::Running) {
        ms = qMax(1, m_vpGlide->duration() - int(m_vpGlide->currentTime()));
        m_reflowDeadline = m_reflowClock.elapsed() + ms;
    } else if (m_reflowChained) {
        // Retarget of a RUNNING animation (resize drags retarget every
        // frame): keep the original deadline — completion depends on the
        // configured duration, larger remaining distance simply moves
        // faster. Restarting the full duration each time would hold the
        // animation open for as long as the gesture lasts.
        const qint64 remaining = m_reflowDeadline - m_reflowClock.elapsed();
        if (remaining > 0) {
            ms = int(remaining);
        } else {
            // Deadline already passed — the old animation just had not
            // processed its final tick yet (busy event loop). This is a
            // NEW step, not a mid-gesture retarget: with the expired
            // deadline it would run for ~1 ms and SNAP instead of
            // animating.
            m_reflowDeadline = m_reflowClock.elapsed() + ms;
        }
    } else {
        m_reflowDeadline = m_reflowClock.elapsed() + ms;
    }
    m_reflowChained = false;
    TRACE_SLIDE("startReflowAnimation ms=%d", ms);
    m_reflowAnim->setDuration(ms);
    m_reflowAnim->start();
}

void GridView::stopReflowAnimation()
{
    m_reflowCaptured = false;
    // A snap also ends a pinned-zoom release settle: QAbstractAnimation
    // emits finished() only when the animation REACHES its end — an
    // explicit stop() (user scroll, model reset, suppression) does not,
    // so the flag must fall here or the paint keeps showing the frozen
    // pin flow and the view no longer follows the scrollbar.
    m_zoomPinSettling = false;
    if (m_reflowAnim->state() == QAbstractAnimation::Stopped)
        return;
    m_reflowAnim->stop();
    viewport()->update();
}

void GridView::beginViewportGlide(int durationMs)
{
    if (durationMs <= 0 || !m_itemsPainted)
        return;
    const QPoint cur = viewport()->mapToGlobal(QPoint(0, 0));
    if (m_vpGlide->state() == QAbstractAnimation::Running) {
        // Retarget without a jump: the visually current origin becomes the
        // new start
        const qreal t = m_vpGlide->currentValue().toReal();
        m_vpGlideOrigin = cur + (m_vpGlideOrigin - cur) * (1.0 - t);
    } else {
        m_vpGlideOrigin = cur;
    }
    m_vpGlide->stop();
    m_vpGlide->setDuration(durationMs);
    m_vpGlide->start();
}

void GridView::beginPanelSlide(int finalWidthDelta)
{
    // FROM = whatever is painted right now — materialize an active slide
    // (retarget while running) or a running reflow, so chaining is jumpless
    FlowParams from = flowParams(currentLayoutParams(), m_iconSize);
    if (m_panelSlideActive)
        from = lerpFlow(m_slideFrom, m_slideTo, m_panelSlideT);
    else if (m_reflowAnim->state() == QAbstractAnimation::Running)
        from = lerpFlow(m_reflowFrom, from,
                        m_reflowAnim->currentValue().toReal());
    endPanelSlide();
    stopReflowAnimation();
    if (!m_sourceModel || m_sourceModel->rowCount() == 0 || !m_itemsPainted
        || finalWidthDelta == 0)
        return;
    m_slideFrom = from;

    // TO = the raster predicted for the final width — same sizing model
    // (constant COLUMNS, width-filling cells) the per-frame resizes will
    // converge to by the end of the slide. The column count never
    // changes, so rows/offset stay put and only the cell size scales.
    const int w = qMax(1, viewport()->width() + finalWidthDelta);
    const int padW = baseCellSize().width() - m_delegate->iconSize();
    const int padH = baseCellSize().height() - m_delegate->iconSize();
    const int icon = qMax(1, w / qMax(1, m_columns) - padW);
    LayoutParams to;
    to.cols = qMax(1, m_columns);
    to.cell = QSize(icon + padW, icon + padH);
    to.gridOffset = m_gridOffset;
    to.firstRow = firstRow();
    m_slideTo = flowParams(to, icon);

    // Hit testing stays LIVE-raster based while the painted state is the
    // frozen flow lerp — re-evaluating hover mid-slide (synthetic enters
    // from the per-frame resizes) would make the frame hop between items.
    // Freeze it exactly like the Ctrl+wheel zoom: it stays on its item
    // until a deliberate mouse move releases it.
    m_hoverFrozen = true;
    m_hoverFreezePos = QCursor::pos();

    m_panelSlideT = 0.0;
    m_panelSlideActive = true;
    TRACE_SLIDE("beginPanelSlide delta=%d vpW=%d from(foldW=%.1f cellW=%.1f "
                "base=%.1f) to(foldW=%.1f cellW=%.1f base=%.1f)",
                finalWidthDelta, viewport()->width(), m_slideFrom.foldW,
                m_slideFrom.cellW, m_slideFrom.basePx, m_slideTo.foldW,
                m_slideTo.cellW, m_slideTo.basePx);
    viewport()->update();
}

void GridView::setPanelSlideProgress(qreal t)
{
    if (!m_panelSlideActive)
        return;
    m_panelSlideT = qBound(0.0, t, 1.0);
    viewport()->update();
}

void GridView::endPanelSlide()
{
    if (!m_panelSlideActive)
        return;
    TRACE_SLIDE("endPanelSlide t=%.3f vpW=%d", m_panelSlideT,
                viewport()->width());
    m_panelSlideActive = false;
    viewport()->update();
    updateSliderMaximum();   // deferred during the slide (see resizeEvent)
    // Hover was suspended for the whole transition — re-evaluate once
    // against the final raster (the freeze from beginPanelSlide still
    // holds the frame until a deliberate mouse move)
    updateHover(viewport()->mapFromGlobal(QCursor::pos()));
}

bool GridView::hoverSuspended() const
{
    // During a live zoom gesture hit testing would run on the logical
    // raster while the painted state is translated/scaled — freeze the
    // frame until the gesture commits and settles (pin settle for the
    // pinned variant, glide finished for the scale variant).
    return m_hoverExternallySuspended || m_panelSlideActive
        || m_zoomGestureActive || m_zoomPinSettling
        || m_vpGlide->state() == QAbstractAnimation::Running;
}

void GridView::setReflowSuppressed(bool on)
{
    m_reflowExternallySuppressed = on;
    if (on)
        stopReflowAnimation();   // snap anything already running
}

void GridView::setHoverSuspended(bool on)
{
    if (m_hoverExternallySuspended == on)
        return;
    m_hoverExternallySuspended = on;
    if (on) {
        // Fullscreen transition: the switch rescales every cell (constant
        // columns) and drops/restores panel and toolbar, so a stationary
        // cursor ends up over a DIFFERENT item — the end-of-transition
        // re-evaluation would hop the frame there. Freeze it on its item
        // instead (same rule as the Ctrl+wheel zoom and the panel slide):
        // the frame follows ITS item into the new raster and hands over
        // only on a deliberate mouse move.
        if (m_hoverIndex >= 0 && !m_hoverFrozen) {
            m_hoverFrozen = true;
            m_hoverFreezePos = QCursor::pos();
        }
        return;
    }
    // Re-evaluate once against the current raster (a no-op while the
    // freeze above holds — released by a deliberate mouse move)
    updateHover(viewport()->mapFromGlobal(QCursor::pos()));
}

QList<GridView::VisibleCell> GridView::captureCellRects() const
{
    QList<VisibleCell> cells;
    if (!m_sourceModel || m_sourceModel->rowCount() <= 0)
        return cells;

    const QSize cs = cellSize();
    if (cs.width() <= 0 || cs.height() <= 0)
        return cells;
    const int count = m_sourceModel->rowCount();
    const int cols = columns();
    const int visibleRows = qMax(1,
        (viewport()->height() + subRowPx() + cs.height() - 1) / cs.height());
    const int first = firstVirtualIndex();
    const int totalVirtual = m_gridOffset + count;
    const int remainingVirtual = qMax(0, totalVirtual - first);
    const int remainingRows = (remainingVirtual + cols - 1) / cols;
    const int drawRows = qMax(0, qMin(visibleRows, remainingRows));

    int virtualIdx = first;
    for (int row = 0; row < drawRows; ++row) {
        // Same shift the paint applies when the view rests between rows
        const int y = row * cs.height() - subRowPx();
        for (int col = 0; col < cols; ++col, ++virtualIdx) {
            const int itemIdx = virtualIdx - m_gridOffset;
            if (itemIdx < 0 || itemIdx >= count)
                continue;
            const QRect rect(col * cs.width(), y, cs.width(), cs.height());
            if (!rect.intersects(viewport()->rect()))
                continue;
            cells.append({itemIdx, rect, m_selected.contains(itemIdx)});
        }
    }
    return cells;
}

QRect GridView::LayoutSnapshot::globalRectForRow(int row) const
{
    if (!valid || row < 0)
        return QRect();
    const int virtualIdx = row + gridOffset;
    const int col = virtualIdx % cols;
    // firstVirtual is a whole-row multiple of cols (see firstVirtualIndex)
    const int gridRow = virtualIdx / cols - firstVirtual / cols;
    return QRect(globalOrigin + QPoint(col * cell.width(),
                                       gridRow * cell.height()),
                 cell);
}

GridView::LayoutSnapshot GridView::captureLayoutSnapshot() const
{
    LayoutSnapshot s;
    const QSize cs = cellSize();
    if (!m_sourceModel || cs.width() <= 0 || cs.height() <= 0)
        return s;
    s.cell = cs;
    s.cols = columns();
    s.firstVirtual = firstVirtualIndex();
    s.gridOffset = m_gridOffset;
    s.globalOrigin = viewport()->mapToGlobal(QPoint(0, -subRowPx()));
    s.valid = true;
    return s;
}

void GridView::updateSliderMaximum()
{
    if (!m_sizeSlider || !m_delegate)
        return;
    const int w = viewport()->width();
    if (w <= 0)
        return;
    // Most columns that keep the preview at least kMinIconSize wide …
    const int padW = baseCellSize().width() - m_delegate->iconSize();
    const int fitMax = qMax(1, w / (kMinIconSize + padW));
    // … but NEVER fewer than the current column count: dropping it below
    // would clamp the zoom like a user action, permanently — a narrow
    // window shows smaller previews instead, and the untouched column
    // count restores them when the window grows again.
    const int maxCols = qMax(fitMax, m_columns);
    // The value is the MIRRORED position (columns = max + 1 − value), so
    // every maximum change must re-map the value to keep the column
    // count — blocked: nothing changes logically.
    const int v = maxCols + 1 - m_columns;
    if (m_sizeSlider->maximum() == maxCols && m_sizeSlider->value() == v)
        return;
    QSignalBlocker block(m_sizeSlider);
    m_sizeSlider->setMaximum(maxCols);
    m_sizeSlider->setValue(v);
}

void GridView::resetGridOffset()
{
    m_gridOffset = 0;
    m_resizeAnchorRow = -1;
    updateScrollBarRange();
    setFirstRow(0);
    viewport()->update();
    updateVisibleRange();
}
