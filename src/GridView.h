#pragma once

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QList>
#include <QPointF>
#include <QSet>
#include <QSize>
#include <QUrl>

class MediaModel;
class GridDelegate;
class QSlider;
class QVariantAnimation;
class QWheelEvent;
class QPaintEvent;
class QResizeEvent;
class QMouseEvent;
class QContextMenuEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QTimer;

class GridView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit GridView(QWidget* parent = nullptr);

    // Minimum preview edge in LOGICAL pixels — bounds the zoom-out only
    // (max column count for the current width). There is NO upper size
    // bound: one column in a wide window means one huge preview.
    static constexpr int kMinIconSize = 32;
    // The grid widget cannot be squeezed narrower than this (window or
    // splitter): resizes keep the column count, so an arbitrarily narrow
    // grid would just scale every preview into illegibility.
    static constexpr int kMinGridWidth = 280;

    void setSourceModel(MediaModel* model);
    void setIconSizeSlider(QSlider* slider);

    // Directory that external drops are copied/moved into.
    // Empty = grid shows more than one root, drops are rejected.
    void setDropTargetDir(const QString& dir);

    // While the mouse is inside a Details dialog or a viewer window, the
    // grid draws the focus frame on that window's file (if it is visible;
    // no auto-scroll). A real mouse hover takes precedence. Empty = none.
    void setExternalFocusPath(const QString& path);

    // Freezes the focus frame on the given file and scrolls it into view —
    // orientation aid when returning from the fullscreen viewer. Same
    // release rules as the Ctrl+wheel zoom freeze (deliberate mouse move,
    // leave, modal dialog, model reset).
    void freezeFocusOnPath(const QString& path);

    // Fullscreen transition: call BEFORE the window geometry changes. The
    // painted content glides from its pre-transition SCREEN position to
    // wherever it currently belongs (paint-only translation by the
    // shrinking origin delta); raster reflows started meanwhile run with
    // the REMAINING glide time, so everything arrives together.
    void beginViewportGlide(int durationMs);

    // Snapshot of the current (resting) raster for the fullscreen
    // transition overlay: every visible item with its cell rect in
    // VIEWPORT coordinates (the caller maps to global). Pure arithmetic —
    // matches the static paintEvent branch.
    struct VisibleCell {
        int   row = -1;
        QRect rect;
        bool  selected = false;
    };
    QList<VisibleCell> captureCellRects() const;

    // Raster arithmetic frozen at capture time, usable AFTER the layout is
    // gone (the fullscreen switch replaces it): hypothetical GLOBAL cell
    // rect for ANY row, visible or not. The transition overlay flies rows
    // present on one side only from/to the position they would occupy in
    // this raster (fading in/out on the way).
    struct LayoutSnapshot {
        QSize  cell;
        int    cols = 1;
        int    firstVirtual = 0;   // virtual index of the top-left cell
        int    gridOffset = 0;
        QPoint globalOrigin;       // viewport top-left in global coords
        bool   valid = false;
        QRect  globalRectForRow(int row) const;
    };
    LayoutSnapshot captureLayoutSnapshot() const;

    // While set, paintEvent draws only the background — used to render the
    // window backdrop for the fullscreen transition overlay (the images fly
    // ON the overlay, so the window behind them must show an empty grid).
    // Deliberately no update(): the flag only matters inside an explicit
    // render()/grab() and must not repaint the real window.
    void setSuppressItemPaint(bool on) { m_suppressItemPaint = on; }

    // External hover suspension (ORed into hoverSuspended) — used while
    // the fullscreen transition overlay runs: the real window is mapped
    // but invisible (opacity 0) and the overlay is mouse-transparent, so
    // mouse events would still reach the grid
    void setHoverSuspended(bool on);

    // While on, raster changes from resizes SNAP instead of animating —
    // used while the fullscreen transition overlay lives: the reflow
    // would run invisibly (entering; window at opacity 0) or on the
    // empty backdrop grid (leaving) and could still be mid-flight at
    // handover, breaking the overlay's seamless last frame
    void setReflowSuppressed(bool on);

    // Side-panel slide: ONE coherent flow transition from the current
    // raster to the raster predicted for the final viewport width
    // (current + finalWidthDelta), driven externally by
    // setPanelSlideProgress on the same clock as the sliding panel edge.
    // The painted fold width then equals the moving edge exactly, and new
    // columns emerge continuously through the flow wrap instead of
    // popping when the live raster crosses a column threshold. Per-frame
    // resize reflows are suppressed while active; endPanelSlide snaps to
    // the live raster.
    void beginPanelSlide(int finalWidthDelta);
    void setPanelSlideProgress(qreal t);
    void endPanelSlide();

    // Selection as SOURCE MODEL rows, ascending. setSelectedRows mirrors the
    // list view's selection into the grid;
    // it is a no-op — and emits nothing — when the selection already matches.
    QList<int> selectedRows() const;
    void setSelectedRows(const QList<int>& rows);

    // By-path view state (selection + top-left item) — exactly what a model
    // reset captures and restores. Exposed for window cloning (Ctrl+N):
    // the new window injects the source's snapshot BEFORE its first scan,
    // and the reset that delivers the scan result applies it.
    struct ViewStateSnapshot {
        QStringList selectedPaths;
        QString     topLeftPath;
    };
    ViewStateSnapshot viewStateSnapshot() const;
    void setPendingViewState(const ViewStateSnapshot& state);

public slots:
    void resetGridOffset();
    void selectAll();           // Ctrl+A
    void copySelection();       // selection → clipboard (copy)
    void cutSelection();        // selection → clipboard (cut)
    void pasteFromClipboard();  // clipboard files → drop target dir
    void requestFilterToSelection();  // emits filterToSelectionRequested

public:
    // Paste possible: clipboard has files and exactly one root dir is shown
    bool canPaste() const;
    // EFFECTIVE preview size (width-filling for the current column
    // count); thumbnail invalidation must use this size
    int iconSize() const { return m_iconSize; }

signals:
    void itemDoubleClicked(int sourceRow);
    // The item carrying the focus frame: the mouse-hovered one, or (without
    // hover) the external-focus one (Details/viewer window). -1 = none.
    // Feeds DetailsPanel + status-bar path.
    void focusItemChanged(int sourceRow);
    // True while the frame drawn in the grid comes from the EXTERNAL focus
    // (no mouse hover, external row visible) — the driving Details/viewer
    // window shows its own border while this holds.
    void externalFocusDisplayChanged(bool displayed);
    void selectionChanged(const QList<int>& sourceRows);  // sorted, empty = none
    void filesDropped();   // an external drop was performed into the target dir
    void detailsRequested(int sourceRow);  // context menu, item under cursor
    void editMetadataRequested(const QList<int>& sourceRows);  // context menu
    void renameRequested(const QList<int>& sourceRows);        // context menu
    void deleteRequested(const QList<int>& sourceRows, bool permanent);
    void filterToSelectionRequested(const QList<int>& sourceRows);
    void clearFilterRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    // Catches the Ctrl release that ends a live zoom gesture (the key event
    // may go to any widget) — see the zoom-gesture block below
    bool eventFilter(QObject* obj, QEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private slots:
    void onColumnCountChanged(int cols);
    void updateVisibleRange();
    void onScanningStarted();
    void onScanningFinished();
    void onScanningOverlayTimer();
    // Safety net: the Ctrl-release KeyRelease can be lost (focus change, WM
    // grab) — poll the modifier state while a zoom gesture is active
    void onZoomIdleCheck();

private:
    // --- Reflow animation (paint-only) ---
    // A raster is fully described by these four values.
    struct LayoutParams {
        QSize cell;
        int   cols = 1;
        int   firstRow = 0;
        int   gridOffset = 0;
    };
    LayoutParams currentLayoutParams() const;
    // Continuous generalization of a raster for the FLOW animation: the
    // reading-order strip, measured RELATIVE TO THE TOP-LEFT CELL (item i
    // starts at i*cellW + basePx; the top-left cell is at 0), folded every
    // foldW pixels. At the animation endpoints these equal the raster
    // values exactly; in between every quantity may be fractional — items
    // then travel ALONG their row and wrap at the fold instead of flying
    // diagonally to their target. Anchoring the fold phase at the TOP-LEFT
    // cell (not the list start) is what pins the zoom anchor exactly: the
    // item that is top-left in both rasters has X == 0 for ALL t. (A first
    // version measured from item 0 and lerped firstRow separately — the
    // two interpolation paths only agree at firstRow 0, so the whole grid
    // drifted vertically whenever the view was scrolled.)
    struct FlowParams {
        qreal cellW = 1, cellH = 1;
        qreal foldW = 1;      // wrap width in px (= cols * cellW at rest)
        // Strip position of item 0 relative to the top-left cell, in px
        // (= (gridOffset - firstRow*cols) * cellW at rest; usually negative)
        qreal basePx = 0;
        qreal iconSize = 1;
    };
    static FlowParams flowParams(const LayoutParams& p, int iconSize);
    static FlowParams lerpFlow(const FlowParams& a, const FlowParams& b, qreal t);
    // Uniform scale of a flow around the strip origin (top-left cell): all
    // five scalars × s. A flow scaled by s and translated by (1−s)·A is the
    // grid zoomed around viewport point A — the basis of the live zoom
    // gesture (see beginZoomGesture).
    static FlowParams scaleFlow(const FlowParams& f, qreal s);

    // --- Live Ctrl+wheel zoom gesture ---
    // Two variants, chosen at gesture start; steps stay quantized to whole
    // column counts in both.
    // PINNED (an item carries the focus frame): the raster stays LIVE —
    // every wheel step reflows to the stepped column count with the FRAMED
    // item's pin point held exactly at the cursor for EVERY animation
    // frame (updatePinnedZoomStep: the painted flow is anchored at the
    // pinned item, see pinRelativeFlow). Ctrl release starts NO reflow:
    // the pin point travels to its exact raster position, which moves the
    // grid as a RIGID block onto the raster.
    // SCALE (no frame): the grid does not reflow while Ctrl is held — the
    // frozen raster is painted as a uniform scale around the cursor
    // (m_zoomScaledFlow + m_zoomOrigin), so the point under the mouse
    // stays put. On Ctrl release the gesture commits: ONE reflow re-folds
    // the strip to the viewport width with the mouse item anchored as
    // close to the cursor as the row/column raster allows (leading dummy
    // rows absorb the vertical underflow near the list start, unless the
    // view was already at the start — then item 0 stays top-left and the
    // item drifts off the mouse).
    // pinFallbackNearest: pick the item nearest to the cursor as the pin
    // when no focus frame exists — the PAN gesture can grab the plane
    // anywhere (Ctrl+wheel without a frame keeps the scale variant).
    void beginZoomGesture(bool pinFallbackNearest = false);
    void updateZoomGesture(int columnSteps);   // columnSteps>0 = zoom in
    void updatePinnedZoomStep();               // pinned variant: one live step
    // Currently painted pin state: the lerped pin flow plus the
    // translation that puts the pin point at the (lerped) pin position.
    // The translation folds the pin POINT, never the pinned cell's left
    // edge — see the comment in the implementation.
    FlowParams paintedPinFlow(QPointF* offOut) const;
    void renormPinFlow(FlowParams& f) const;
    // Re-resolve the pin as the item/in-cell point under `pt` on the
    // PAINTED plane — every wheel step (the zoom center follows the
    // mouse) and every pan (re-)grab (the grab point is wherever the
    // button goes down). Jumpless: only the pin reference changes, the
    // new pin point's painted position is `pt` itself. The focus frame
    // follows onto the new pin item.
    void resolvePinAt(const QPointF& pt, bool moveFocus = true);
    // While the PAN (Ctrl+left drag) is engaged the pinned image drags
    // the plane along: both pin points shift by the delta (jumpless
    // mid-animation) and the window-flush basePx follows the horizontal
    // component, so the fold lines stay at the window edges while the
    // strip slides through them.
    void zoomPinFollowMouse(const QPointF& m);
    // Anchor the live raster so the pinned item's pin point lands as
    // close to `pt` as the rows/columns allow (natural fold when the
    // list start would show); commits gridOffset + scrollbar (guarded).
    // Used by every pin step and by the release commit (the mouse may
    // have moved since the last step).
    void anchorPinnedRaster(const QPointF& pt, int itemIdx);
    // Flow of a raster anchored AT THE PINNED ITEM: basePx is measured
    // from the start of the pinned item's row, so the pinned item's strip
    // position is its in-row column offset — lerping two such flows keeps
    // the pinned item stationary (rows before it fold backward, rows
    // after it forward) while the top-left-anchored standard flow would
    // move it. Painted with a translation that puts the pin point at the
    // (lerped) pin position — exact for ALL t, not just the endpoints.
    static FlowParams pinRelativeFlow(const LayoutParams& p, int iconSize,
                                      int pinItem);
    // (Re)starts the shared reflow clock for a pin step/settle; snaps
    // (From=To) when the animation is disabled. keepDeadline: rapid steps
    // retarget with the remaining time, like startReflowAnimation.
    void startZoomPinAnim(bool keepDeadline);
    void commitZoomGesture();                  // Ctrl released: anchored reflow
    // Re-aims a running pin settle at the LIVE raster after a resize
    void retargetPinSettle();
    void endZoomGesture(bool commit);          // commit=false: drop (model reset)
    // Call BEFORE a layout mutation: freezes the currently painted flow
    // state (materializes a running animation, so rapid steps chain
    // without jumps)
    void beginReflowCapture(const LayoutParams& pre, int preIconSize);
    void startReflowAnimation();  // call AFTER the mutation
    void stopReflowAnimation();   // snap to the target state

    // Sizing model: the COLUMN COUNT is the zoom state (slider value,
    // persisted) — window/panel resizes never change it. The cell always
    // fills viewportWidth / columns, so resizing the window simply grows
    // or shrinks the previews (no reflow, no right-edge whitespace beyond
    // the sub-column integer remainder). There is no upper size bound;
    // kMinIconSize bounds only the zoom-out (max columns for the width).
    // Preview size for a viewport width at the current column count
    int iconSizeForWidth(int w) const;
    // Slider maximum: the most columns that keep the preview at least
    // kMinIconSize wide, re-synced on viewport resizes. Never fewer than
    // the current column count — that is the user's intent; a narrow
    // window shows smaller previews instead of dropping columns. The
    // slider VALUE is the MIRRORED position (columns = max + 1 − value,
    // right = larger previews with a left-to-right groove fill), so this
    // also re-maps the value to keep the column count (blocked).
    void updateSliderMaximum();

    // Cell of the current preview size: preview + delegate padding
    QSize baseCellSize() const;
    // Layout cell — equals baseCellSize(); m_iconSize is kept width-
    // filling for m_columns (see the sizing model above), so the fold
    // sits at the window edge up to the integer remainder
    QSize cellSize() const;
    int columns() const;
    // True while a panel slide / fullscreen glide runs — hover evaluation
    // is suspended then (live-raster hit testing vs. painted lerp would
    // make the frame hop; synthetic enters fire every frame)
    bool hoverSuspended() const;
    // The scrollbar counts PIXELS: its value
    // is firstRow × cellH + subRowPx. Rows remain the unit of the RASTER —
    // maxFirstRow and the anchoring logic are row counts, and column
    // alignment lives exclusively in m_gridOffset, so dragging the handle
    // can never shift items into other columns.
    int maxFirstRow() const;
    int effectiveMaxFirstRow() const;
    int firstRow() const;    // scrollbar value / cellH
    int subRowPx() const;    // scrollbar value % cellH (rest between rows)
    int indexAtPoint(const QPoint& p) const;
    void updateScrollBarRange();
    void setFirstRow(int row);

    // --- Smooth scrolling (paint-only) ---
    // The PAINTED position is the scrollbar value + m_scrollLagPx; the glide
    // animates that lag to zero. The logical position is therefore always
    // the target — only the paint is on its way there.
    // Wheel path. animate = false lands immediately (touchpad pixelDelta,
    // which is smooth by itself).
    void  scrollByPixels(qreal dy, bool animate = true);
    // Guarded scrollbar write that KEEPS the painted position and then
    // glides it onto the new value
    void  applyScrollPx(int px, bool animate);
    // Scrollbar path (arrow buttons, track click): the value already moved,
    // the paint glides in from the old one
    void  glideScrollTo(int fromPx, int toPx);
    void  startScrollAnimation();     // glide m_scrollLagPx to zero
    // Ends the glide. Row-aligned positions are a precondition of the zoom
    // gestures (they anchor on the raster), so snapToRow additionally moves
    // the scrollbar to the nearest row.
    void  stopScrollAnimation(bool snapToRow);
    // Virtual index of the top-left CELL (= first row × columns)
    int firstVirtualIndex() const;
    void updateHover(const QPoint& viewportPos);
    void clearHover();
    void emitFocusChange();   // focusItemChanged if the effective row moved
    int  rowForPath(const QString& path) const;   // -1 if not in the model
    void handleSelectionClick(int index, Qt::KeyboardModifiers mods);
    // Pending plain-click action (armed on release, runs after the
    // double-click interval unless a drag/double-click cancels it)
    void flushPendingClick();    // execute now (a new press elsewhere)
    void cancelPendingClick();   // discard (drag, double-click, model change)
    void clearSelection();
    void emitSelection();
    QList<QUrl> selectedUrls() const;
    void captureViewState();   // on modelAboutToBeReset: paths of selection + top-left item
    void restoreViewState();   // on modelReset: re-select and re-scroll by path

    MediaModel*            m_sourceModel  = nullptr;
    GridDelegate*          m_delegate     = nullptr;
    int                    m_iconSize = 128;   // derived: width-filling for m_columns
    int                    m_columns  = 4;     // zoom state (slider value)
    int                    m_sharpThumbSize = 128;
    // Resizes rescale the icon size on every event — sharp thumbnails are
    // re-requested only once the size settles (the paint fallback scales
    // the previous ones in the meantime)
    QTimer*                m_refitInvalidateTimer = nullptr;
    int                    m_wheelRemainder = 0;
    // Ctrl+wheel accumulator: one column per full 120-unit notch — fast
    // spins arrive coalesced in one event, hi-res wheels as many small ones
    int                    m_zoomWheelRemainder = 0;
    // Same accumulator for wheel notches ON the size slider (one notch =
    // one column, applied in eventFilter)
    int                    m_sliderWheelRemainder = 0;

    // --- Live Ctrl+wheel zoom gesture state (see beginZoomGesture) ---
    // While active the LOGICAL raster (scrollbar, m_gridOffset, m_iconSize,
    // slider) is frozen at its gesture-start value; the paint draws
    // m_zoomScaledFlow translated by m_zoomOrigin instead.
    bool                   m_zoomGestureActive = false;
    // FRAMELESS Ctrl+wheel: no gesture runs at all, every notch goes down
    // the slider's path (see wheelEvent). The flag marks the Ctrl-HOLD so
    // the framed/frameless decision is taken ONCE per hold — otherwise a
    // cell growing under the stationary cursor while zooming IN would flip
    // the next notch onto the pinned path mid-zoom. Ended by the same
    // Ctrl-release detection as a gesture (eventFilter + m_zoomIdleTimer),
    // and mutually exclusive with m_zoomGestureActive.
    bool                   m_zoomSliderHold = false;
    LayoutParams           m_zoomBaseLayout;      // frozen raster at start
    int                    m_zoomBaseIconSize = 0;
    FlowParams             m_zoomScaledFlow;      // base flow × m_zoomScale
    QPointF                m_zoomOrigin;          // display = scaledFlow + origin
    qreal                  m_zoomScale = 1.0;     // cols_base / target columns
    int                    m_zoomTargetCols = 1;  // stepped by the wheel
    // Pinned variant (focus frame at gesture start): the raster is LIVE
    // and the painted flow is anchored AT THE PINNED ITEM (see
    // pinRelativeFlow), so its in-cell pin point (m_zoomPinFrac, 0..1) is
    // painted EXACTLY at the pin position in every animation frame —
    // items before it fold backward, items after it forward, the pinned
    // image only scales in place. Steps lerp Flow/PinPoint From→To on
    // the shared reflow clock; the release settle is one more pin
    // animation (the flow is already final, only the pin point travels
    // to its exact raster position — a rigid-block glide), painted until
    // m_zoomPinSettling clears. A step whose raster would show the LIST
    // START folds naturally instead (no leading blank cells/rows — they
    // would stay visible after the release); the pin translation carries
    // the difference and the settle drifts the grid into place.
    bool                   m_zoomPinned = false;
    bool                   m_zoomPinSettling = false;
    // PAN mode: Ctrl+left drag moves the plane (zoomPinFollowMouse per
    // mouse move). Releasing the BUTTON only disengages the pan — the
    // gesture (and the shifted plane) stays until Ctrl is released.
    bool                   m_zoomPanActive = false;
    // Last RAW mouse position of the pan — the drag delta base. Separate
    // from m_zoomPinPointTo: the pin reference gets re-anchored (clamped
    // into the covered strip) when the mouse leaves the grid area, while
    // the delta must keep tracking the real cursor 1:1.
    QPointF                m_zoomPanLastMouse;
    int                    m_zoomPinItem = -1;
    QPointF                m_zoomPinFrac;
    FlowParams             m_zoomPinFlowFrom;
    FlowParams             m_zoomPinFlowTo;
    QPointF                m_zoomPinPointFrom;
    QPointF                m_zoomPinPointTo;
    int                    m_zoomSharpTarget = 0; // debounced sharpen size
    QTimer*                m_zoomSharpTimer = nullptr;
    QTimer*                m_zoomIdleTimer = nullptr;
    // Allowed over-scroll row past the list end (bottom whitespace), set by
    // zoom-anchoring or a window enlarge that would otherwise yank the view
    // down. One-way: released row by row as the user scrolls up (see the
    // valueChanged handler); scrolling down is capped at the current row.
    int                    m_resizeAnchorRow = -1;
    // Leading empty cells before item 0 (virtual = item + offset). Usually
    // column alignment only (< one row), but a Ctrl+wheel zoom committed
    // near the list start may grow it past whole rows: those leading DUMMY
    // rows push the anchored item down to the cursor (see commitZoomGesture).
    // The scrollbar's row math (maxFirstRow, totalVirtual) accounts for them;
    // scrolling up reveals the blank rows above item 0, bounded like the
    // bottom over-scroll whitespace.
    int                    m_gridOffset = 0;
    // Last range reported by updateVisibleRange() — lets the dataChanged
    // handler skip repaints for off-screen thumbnail deliveries.
    int                    m_visibleItemFirst = 0;
    int                    m_visibleItemLast = -1;
    QSlider*               m_sizeSlider = nullptr;
    QPoint                 m_dragStartPos;
    int                    m_dragStartIndex = -1;
    // Plain click on an item changes the selection only after the
    // double-click interval expires: a drag must keep the whole selection,
    // a double-click must open the viewer WITHOUT touching the selection.
    int                    m_pendingClickIndex = -1;
    // Ctrl+left press: becomes a selection TOGGLE on release without
    // movement, or a plane PAN once the drag threshold is crossed (the
    // toggle is discarded then). Applied on release — an immediate toggle
    // would flash on every pan grab.
    bool                   m_ctrlPressPending = false;
    int                    m_ctrlPressIndex = -1;
    QTimer*                m_clickTimer = nullptr;
    int                    m_hoverIndex = -1;
    // Ctrl+wheel zoom freezes the focus frame on the item it marked at
    // gesture start (zoom anchors top-left, items slide under the cursor).
    // updateHover releases the freeze once the mouse has moved further than
    // AppSettings::mouseThresholdPx from the recorded global position.
    bool                   m_hoverFrozen = false;
    QPoint                 m_hoverFreezePos;
    // The context menu popup sends the viewport a Leave event — the hover
    // frame must survive it: it marks the item "Details…" refers to.
    bool                   m_contextMenuOpen = false;
    // File of the Details/viewer window the mouse is in; its row gets the
    // focus frame while the mouse hovers nothing. Row re-resolved on model
    // changes (kept as path — indices shift with rescans).
    QString                m_externalFocusPath;
    int                    m_externalFocusRow = -1;
    int                    m_lastFocusRow = -1;   // last focusItemChanged value
    bool                   m_lastExternalDisplayed = false;
    QSet<int>              m_selected;
    QString                m_dropTargetDir;

    // View state across model resets (rescan after metadata edit, rename,
    // drop, re-sort): selection and scroll anchor survive by file path
    QSet<QString>          m_savedSelectedPaths;
    QString                m_savedTopLeftPath;

    // Reflow animation state: frozen start flow (valid while the animation
    // runs; the target flow is derived from the live raster each frame)
    QVariantAnimation*     m_reflowAnim = nullptr;
    FlowParams             m_reflowFrom;
    bool                   m_reflowCaptured = false;
    // Retargeting a RUNNING reflow (resize drags retarget every frame)
    // keeps the ORIGINAL deadline: completion depends on the configured
    // duration, not on when the gesture ends — fast window growth means
    // faster motion, never a stalled animation.
    QElapsedTimer          m_reflowClock;
    qint64                 m_reflowDeadline = 0;
    bool                   m_reflowChained = false;
    // Fullscreen-transition glide: global viewport origin captured before
    // the geometry change; paint translates by (origin - current)*(1-t)
    QVariantAnimation*     m_vpGlide = nullptr;
    QPoint                 m_vpGlideOrigin;
    // Panel-slide flow transition (see beginPanelSlide): frozen endpoints,
    // progress driven by the panel animation
    bool                   m_hoverExternallySuspended = false;
    bool                   m_reflowExternallySuppressed = false;
    bool                   m_panelSlideActive = false;
    qreal                  m_panelSlideT = 0.0;
    FlowParams             m_slideFrom;
    FlowParams             m_slideTo;
    // True once the CURRENT content has been painted at least once — a
    // transition from a never-shown state must not animate (startup: the
    // scrollbar appears when the first items arrive, which resizes the
    // viewport and refits before anything was on screen). Cleared on
    // model reset / row changes, set by paintEvent when items are drawn.
    bool                   m_itemsPainted = false;
    // Guard: scrollbar changes made by our own zoom/resize step must not
    // trigger the snap-on-user-scroll rule
    bool                   m_inReflowStep = false;

    // Smooth scrolling (see scrollByPixels). A scroll writes the target
    // position and the compensating lag in one step, then animates the lag
    // to zero: the logical position is never mid-move, only the paint is.
    QVariantAnimation*     m_scrollAnim = nullptr;
    qreal                  m_scrollLagPx = 0;   // painted = value + lag
    // Cell height the current scrollbar value is expressed in — a resize or
    // zoom rescales value and range so the position keeps its meaning
    int                    m_scrollCellH = 0;
    // Guard: our own writes must not be mistaken for a scrollbar interaction
    bool                   m_inSmoothScroll = false;
    // Set by QScrollBar::actionTriggered — marks the NEXT value change as
    // a user interaction with the bar (drag, arrows, track), which is what
    // separates a scroll to glide from a computed position to snap to
    bool                   m_scrollBarAction = false;
    int                    m_lastScrollPx = 0;   // where the paint starts from
    // Background-only painting for the transition backdrop (see
    // setSuppressItemPaint)
    bool                   m_suppressItemPaint = false;

    // Scanning overlay
    bool                   m_scanning = false;
    bool                   m_showScanningOverlay = false;
    QTimer*                m_scanningTimer = nullptr;
};
