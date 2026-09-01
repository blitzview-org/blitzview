#include "TableView.h"
#include "MediaModel.h"
#include "MediaContextMenu.h"
#include "FileOps.h"
#include "FocusFrame.h"
#include "SlideTrace.h"

#include <QApplication>

#include <QCursor>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>

#include <QHeaderView>
#include <QContextMenuEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QItemSelectionModel>
#include <QMenu>
#include <QScrollBar>
#include "AppSettings.h"
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <algorithm>

// Delegate that paints a play overlay on video thumbnails
class TableThumbDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);

        if (!index.data(Qt::UserRole + 3).toBool())
            return;
        QPixmap px = index.data(Qt::DecorationRole).value<QPixmap>();
        if (px.isNull())
            return;

        painter->save();
        // Logical (device-independent) size — pixmaps carry the screen scale
        const QSize di = px.deviceIndependentSize().toSize();
        const int thumbH = qMin(di.height(), option.rect.height() - 4);
        const int thumbW = qMin(di.width(), option.rect.width() - 4);
        const int side = qMin(thumbW, thumbH);
        const int r = side * 3 / 10;
        const QPoint c = option.rect.center();

        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setBrush(QColor(0, 0, 0, 120));
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(c, r, r);
        const int tri = r * 6 / 10;
        const int offX = tri / 4;
        QPainterPath path;
        path.moveTo(c.x() - tri/2 + offX, c.y() - tri);
        path.lineTo(c.x() + tri   + offX, c.y());
        path.lineTo(c.x() - tri/2 + offX, c.y() + tri);
        path.closeSubpath();
        painter->setBrush(QColor(255, 255, 255, 200));
        painter->drawPath(path);
        painter->restore();
    }
};

TableView::TableView(QWidget* parent) : QTableView(parent)
{
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setDragEnabled(true);
    // Accepts external file drops like the grid does; the item view's own
    // drop handling never runs — dropEvent below is not chained.
    setDragDropMode(QAbstractItemView::DragDrop);
    setDropIndicatorShown(false);
    setDefaultDropAction(Qt::CopyAction);
    // Qt's own sorting stays OFF: the order is a property of the SOURCE
    // model (both views show it), so a header click is routed out as
    // sortRequested and comes back as MediaModel::sort.
    setSortingEnabled(false);
    setAlternatingRowColors(true);
    verticalHeader()->setDefaultSectionSize(40);
    verticalHeader()->hide();
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setSectionsMovable(true);
    // Left-aligned titles, like Dolphin — the cell contents are left-aligned
    // too (MediaModel sets no TextAlignmentRole), so centered headers sat
    // off to the side of the data they label.
    horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Needed for the hover focus frame — mouseMoveEvent only fires with a
    // button held otherwise
    viewport()->setMouseTracking(true);
    // Ctrl+wheel zoom anchors on a FRACTIONAL row position and Ctrl+drag
    // pans 1:1; per-item scrolling can express neither (see wheelEvent)
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_inScrollUpdate)
            releaseDummySpace(v);
        // Rows moved under a stationary cursor — the frame marks what the
        // mouse is over, so it has to follow
        if (viewport()->underMouse())
            setHoverRow(rowUnderCursor());
    });
    horizontalHeader()->setSectionsClickable(true);
    horizontalHeader()->setSortIndicatorShown(true);
    // Own menu policy for the header: without it the header's context menu
    // event propagates into contextMenuEvent — in WIDGET coordinates, while
    // viewport events arrive in VIEWPORT ones, and the two are impossible
    // to tell apart there (it silently hit the row above).
    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        showColumnMenu(horizontalHeader()->mapToGlobal(pos));
    });
    setShowGrid(false);

    connect(this, &QTableView::doubleClicked, this, &TableView::onDoubleClicked);
    connect(horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int col) {
        // Re-clicking the sorted column flips the order, like any file manager
        auto* h = horizontalHeader();
        const bool same = h->sortIndicatorSection() == col;
        const Qt::SortOrder order =
            (same && h->sortIndicatorOrder() == Qt::AscendingOrder)
                ? Qt::DescendingOrder : Qt::AscendingOrder;
        emit sortRequested(col, order);
    });
}

void TableView::setSourceModel(MediaModel* model)
{
    m_sourceModel = model;
    m_proxy = new PaddedListProxy(this);
    m_proxy->setSourceModel(model);
    setModel(m_proxy);
    // The pad rows carry the zoom anchor's dummy space and must be free to
    // collapse completely when it is zero
    verticalHeader()->setMinimumSectionSize(0);
    // A reset drops the section sizes — and any dummy space with them
    connect(m_proxy, &QAbstractItemModel::modelReset, this, [this]() {
        m_dummyTopPx = 0;
        m_dummyBottomPx = 0;
        applyDummySpace();
    });
    setItemDelegateForColumn(MediaModel::Col_Thumbnail, new TableThumbDelegate(this));

    setThumbnailSize(40);

    restoreColumnVisibility();

    connect(verticalScrollBar(), &QScrollBar::valueChanged,
            this, &TableView::onScrolled);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, [this]() {
        onScrolled(verticalScrollBar()->value());
    });
    connect(selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) {
        if (m_applyingSelection)
            return;
        emit selectionChangedRows(selectedRows());
    });
}

QList<int> TableView::selectedRows() const
{
    QList<int> rows;
    if (!m_proxy || !selectionModel())
        return rows;
    const auto indexes = selectionModel()->selectedRows();
    rows.reserve(indexes.size());
    for (const QModelIndex& idx : indexes)
        rows.append(m_proxy->mapToSource(idx).row());
    std::sort(rows.begin(), rows.end());
    return rows;
}

void TableView::setSelectedRows(const QList<int>& rows)
{
    if (!m_proxy || !m_sourceModel || !selectionModel())
        return;
    if (selectedRows() == rows)
        return;   // breaks the mirror loop between grid and list

    QItemSelection sel;
    const int count = m_sourceModel->rowCount();
    const int lastCol = m_proxy->columnCount() - 1;
    for (int r : rows) {
        if (r < 0 || r >= count)
            continue;
        const QModelIndex left  = m_proxy->mapFromSource(m_sourceModel->index(r, 0));
        const QModelIndex right = m_proxy->mapFromSource(m_sourceModel->index(r, lastCol));
        if (left.isValid() && right.isValid())
            sel.select(left, right);
    }

    m_applyingSelection = true;
    selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
    m_applyingSelection = false;
}

void TableView::scrollTo(const QModelIndex& index, ScrollHint hint)
{
    const int h = horizontalScrollBar()->value();
    QTableView::scrollTo(index, hint);
    horizontalScrollBar()->setValue(h);
}

void TableView::scrollToSelection()
{
    if (!m_proxy || !m_sourceModel || !selectionModel())
        return;
    const QList<int> rows = selectedRows();
    if (rows.isEmpty())
        return;
    const QModelIndex idx = m_proxy->mapFromSource(m_sourceModel->index(rows.first(), 0));
    if (idx.isValid())
        scrollTo(idx, QAbstractItemView::PositionAtCenter);
}

bool TableView::canPaste() const
{
    return !m_dropTargetDir.isEmpty()
        && !FileOps::clipboardFiles().urls.isEmpty();
}

// External file drops — same contract as GridView: only with exactly one
// root shown, always reported as Copy so the source never deletes anything
// itself (a "move" is performed exclusively by our own code), and own drags
// are rejected.
void TableView::dragEnterEvent(QDragEnterEvent* event)
{
    if (m_dropTargetDir.isEmpty() || !event->mimeData()->hasUrls()
        || event->source() == this || event->source() == viewport()) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void TableView::dragMoveEvent(QDragMoveEvent* event)
{
    if (m_dropTargetDir.isEmpty() || !event->mimeData()->hasUrls()
        || event->source() == this || event->source() == viewport()) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void TableView::dropEvent(QDropEvent* event)
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

void TableView::showSortIndicator(int column, Qt::SortOrder order)
{
    // Columns MediaModel cannot sort by (preview, resolution, duration) keep
    // no indicator — MainWindow simply ignores clicks on them.
    horizontalHeader()->setSortIndicator(column, order);
}

void TableView::setThumbnailSize(int size)
{
    const int clamped = qBound(kMinThumbSize, size, kMaxThumbSize);
    verticalHeader()->setDefaultSectionSize(clamped + kRowPadding);
    setColumnWidth(MediaModel::Col_Thumbnail, clamped + 8);
    // Pixel scrolling (needed by the Ctrl+wheel anchor) has no notion of
    // rows — keep a plain wheel notch worth three of them, as it is with
    // Qt's per-item scrolling.
    verticalScrollBar()->setSingleStep(clamped + kRowPadding);
    applyDummySpace();   // the default size reset the pad rows too
}

void TableView::onDoubleClicked(const QModelIndex& index)
{
    if (!m_proxy || !m_sourceModel) return;
    QModelIndex src = m_proxy->mapToSource(index);
    emit itemDoubleClicked(src.row());
}

void TableView::onScrolled(int /*value*/)
{
    if (!m_sourceModel) return;
    // Hidden in the stack: indexAt on the never-laid-out viewport returns
    // invalid indices, and the fallback below would report the WHOLE list
    // as visible — triggering full-size thumbnail loads for every file
    // while the grid is the active view. showEvent re-syncs on switch.
    if (!isVisible()) return;
    QModelIndex first = indexAt(viewport()->rect().topLeft());
    QModelIndex last  = indexAt(viewport()->rect().bottomRight());
    if (!first.isValid()) first = m_proxy->index(0, 0);
    if (!last.isValid())  last  = m_proxy->index(m_proxy->rowCount() - 1, 0);
    // The pad rows map to no source row — clamp into the real range
    const int lastSource = m_sourceModel->rowCount() - 1;
    if (lastSource < 0)
        return;
    const int srcFirst = qBound(0, first.row() - PaddedListProxy::kPadRows, lastSource);
    const int srcLast  = qBound(srcFirst, last.row() - PaddedListProxy::kPadRows, lastSource);
    m_sourceModel->setVisibleRows(srcFirst, srcLast);
}

void TableView::showEvent(QShowEvent* event)
{
    QTableView::showEvent(event);
    // Becoming the active view: report the actually visible rows (onScrolled
    // is a no-op while hidden).
    onScrolled(0);
}

void TableView::hideEvent(QHideEvent* event)
{
    QTableView::hideEvent(event);
    // Switching away does not send a leaveEvent — the frame (and the
    // details/status-bar path it drives) must not survive as a stale hover.
    setHoverRow(-1);
}

// --- Hover focus frame (see FocusFrame.h) ---

void TableView::setHoverRow(int row)
{
    if (row == m_hoverRow)
        return;
    m_hoverRow = row;
    viewport()->update();
    const int sourceRow = (row >= 0 && m_proxy)
        ? m_proxy->mapToSource(m_proxy->index(row, 0)).row() : -1;
    emit focusItemChanged(sourceRow);
}

QRect TableView::hoverRowRect() const
{
    if (m_hoverRow < 0 || !m_proxy)
        return {};
    QRect r;
    for (int c = 0; c < m_proxy->columnCount(); ++c) {
        if (isColumnHidden(c))
            continue;
        const QRect cell = visualRect(m_proxy->index(m_hoverRow, c));
        r = r.isNull() ? cell : r.united(cell);
    }
    return r;
}

void TableView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_ctrlPressPending) {
        const QPoint d = event->globalPosition().toPoint() - m_panStartGlobal;
        if (!m_panActive
            && d.manhattanLength() >= QApplication::startDragDistance()) {
            m_panActive = true;
            viewport()->setCursor(Qt::ClosedHandCursor);
        }
        if (m_panActive) {
            // The content follows the mouse 1:1 — grab and drag the list
            horizontalScrollBar()->setValue(m_panStartH - d.x());
            verticalScrollBar()->setValue(m_panStartV - d.y());
        }
        event->accept();
        return;
    }
    QTableView::mouseMoveEvent(event);
    setHoverRow(rowUnderCursor());
}

void TableView::leaveEvent(QEvent* event)
{
    QTableView::leaveEvent(event);
    setHoverRow(-1);
}

// Ctrl+wheel zoom, anchored on the row under the cursor — the grid's
// contract, adapted to a uniform-height list: the CONTINUOUS row position
// under the mouse (fractional part included) is what stays put, so the
// hovered row does not drift even when the step does not divide evenly.
// Needs ScrollPerPixel (set in the constructor); with the default
// ScrollPerItem the scrollbar counts whole rows and cannot express the
// sub-row offset the anchor needs.
void TableView::wheelEvent(QWheelEvent* event)
{
    int angleY = event->angleDelta().y();
    if (event->inverted())
        angleY = -angleY;

    if (!(event->modifiers() & Qt::ControlModifier) || angleY == 0) {
        QTableView::wheelEvent(event);   // pad rows scroll like any other
        return;
    }

    m_zoomWheelRemainder += angleY;
    const int steps = m_zoomWheelRemainder / 120;   // >0 = zoom in
    m_zoomWheelRemainder %= 120;
    event->accept();
    if (steps == 0)
        return;

    const int oldRowH = verticalHeader()->defaultSectionSize();
    const int oldSize = qBound(kMinThumbSize, oldRowH - kRowPadding, kMaxThumbSize);
    const int newSize = qBound(kMinThumbSize, oldSize + steps * kZoomStep, kMaxThumbSize);
    if (newSize == oldSize)
        return;

    // Global → viewport: a wheel event's position() is ambiguous here
    // (scroll areas forward viewport events), the global one never is.
    const QPoint mousePos = viewport()->mapFromGlobal(event->globalPosition().toPoint());
    const int mouseY = mousePos.y();

    // No row under the cursor (empty area below the last one): there is
    // nothing to hold, so just resize. Anchoring on the empty position
    // would ask for a scroll offset the content cannot fill and leave the
    // view blank.
    const QModelIndex atCursor = indexAt(mousePos);
    if (!atCursor.isValid() || m_proxy->isPadRow(atCursor.row())) {
        emit thumbnailSizeChangeRequested(newSize);
        executeDelayedItemsLayout();
        TRACE_SLIDE("list zoom rowH %d -> %d (no row under cursor, not anchored)",
                    oldRowH, verticalHeader()->defaultSectionSize());
        return;
    }

    // Continuous SOURCE row position under the cursor, in the OLD metric.
    // The content starts with the top pad row, so that height comes off
    // first; everything below it is a plain row grid.
    const qreal anchor =
        (verticalScrollBar()->value() + mouseY - m_dummyTopPx) / qreal(qMax(1, oldRowH));

    emit thumbnailSizeChangeRequested(newSize);   // applied synchronously

    const int newRowH = verticalHeader()->defaultSectionSize();
    // For the anchor to hold: scroll - dummyTop == anchor*newRowH - mouseY.
    // scroll cannot go below 0, so a negative difference becomes top pad.
    const int delta    = qRound(anchor * newRowH) - mouseY;
    int want           = qMax(0, delta);
    const int dummyTop = qMax(0, -delta);

    const int dummyBottom = bottomPadFor(want, dummyTop, newRowH);

    m_inScrollUpdate = true;
    setDummySpace(dummyTop, dummyBottom);
    // Qt VERTAGS the layout after a section resize, so the scroll range
    // still describes the OLD heights here — setting the anchored value
    // against it would silently clamp (that is what made the anchor drift).
    // The layout alone is not enough; the range is recomputed separately.
    executeDelayedItemsLayout();
    updateGeometries();
    verticalScrollBar()->setValue(want);
    m_inScrollUpdate = false;

    // A constant `anchor` across consecutive steps is the whole contract;
    // want != got means the value was clamped, i.e. the row drifted.
    TRACE_SLIDE("list zoom rowH %d -> %d anchor=%.3f mouseY=%d want=%d "
                "got=%d max=%d dummy(top=%d bot=%d)",
                oldRowH, newRowH, anchor, mouseY, want,
                verticalScrollBar()->value(), verticalScrollBar()->maximum(),
                m_dummyTopPx, m_dummyBottomPx);

    setHoverRow(rowUnderCursor());   // rows moved under a stationary cursor
}

// Blank space needed BELOW the last row so that the scroll range reaches
// `scrollTarget` — and, just as important, far enough to consume `topPad`:
// without that, a top pad on a list shorter than the viewport has no
// scrollbar to be scrolled away with and sits there forever.
//
// The scroll maximum is qMax(0, content - viewport), so a target of 0 is
// always reachable and needs no padding at all. Missing that was the bug:
// it padded the bottom until the content exactly filled the viewport,
// which pins the maximum at 0.
int TableView::bottomPadFor(int scrollTarget, int topPad, int rowH) const
{
    const int needMax = qMax(scrollTarget, topPad);
    if (needMax <= 0)
        return 0;
    const int rows = m_sourceModel ? m_sourceModel->rowCount() : 0;
    return qMax(0, needMax + viewport()->height() - topPad - rows * rowH);
}

void TableView::setDummySpace(int topPx, int bottomPx)
{
    m_dummyTopPx    = qMax(0, topPx);
    m_dummyBottomPx = qMax(0, bottomPx);
    applyDummySpace();
}

void TableView::applyDummySpace()
{
    if (!m_proxy || m_proxy->rowCount() == 0)
        return;
    const int last = m_proxy->trailingPadRow();
    // The TRAILING pad moves as the model grows, and an explicit section
    // height sticks to the SECTION: the row that was the pad while the
    // model was still empty becomes a REAL row once it fills, and would
    // keep the pad's height (0 — it silently vanished). Hand it back to
    // the default height before padding the new one. Same reason
    // setRowHidden must not be used here at all.
    if (m_lastBottomPadSection >= 0 && m_lastBottomPadSection != last
        && m_lastBottomPadSection < m_proxy->rowCount())
        setRowHeight(m_lastBottomPadSection, verticalHeader()->defaultSectionSize());

    setRowHeight(0, m_dummyTopPx);
    setRowHeight(last, m_dummyBottomPx);
    m_lastBottomPadSection = last;
}

// Scrolling eats the dummy space GRADUALLY — it must not simply be
// scrolled past: the top pad can be larger than the whole scrollable
// range, in which case "past it" is unreachable and the blank area would
// never go away.
//
// Shrinking the top pad by the amount just scrolled and taking the same
// amount off the scroll value is visually a no-op (the content sits at
// padHeight - scroll either way), so the space is consumed without any
// jump. One-way, like the grid's leading rows: what is eaten is gone.
void TableView::releaseDummySpace(int v)
{
    if (m_dummyTopPx > 0 && v > 0) {
        const int eaten = qMin(m_dummyTopPx, v);
        m_inScrollUpdate = true;
        setDummySpace(m_dummyTopPx - eaten, m_dummyBottomPx);
        executeDelayedItemsLayout();
        updateGeometries();
        verticalScrollBar()->setValue(v - eaten);
        m_inScrollUpdate = false;
        TRACE_SLIDE("list dummy release v=%d eaten=%d top=%d max=%d",
                    v, eaten, m_dummyTopPx, verticalScrollBar()->maximum());
        return;   // the corrected value re-enters here
    }
    // The bottom pad only has to keep the CURRENT position — and whatever
    // top pad is left — reachable; scrolling back up shrinks it.
    if (m_dummyBottomPx > 0) {
        const int needed = bottomPadFor(v, m_dummyTopPx,
                                        verticalHeader()->defaultSectionSize());
        if (needed < m_dummyBottomPx) {
            m_inScrollUpdate = true;
            setDummySpace(m_dummyTopPx, needed);
            executeDelayedItemsLayout();
            updateGeometries();
            m_inScrollUpdate = false;
        }
    }
}

int TableView::rowUnderCursor() const
{
    const QModelIndex idx = indexAt(viewport()->mapFromGlobal(QCursor::pos()));
    if (!idx.isValid() || m_proxy->isPadRow(idx.row()))
        return -1;
    return idx.row();
}

// --- Ctrl+left drag: pan both axes ---

void TableView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton
        && (event->modifiers() & Qt::ControlModifier)) {
        // Neither select nor start a file drag yet — the release decides
        // between a selection toggle and a pan (an immediate toggle would
        // flash on every pan grab).
        m_ctrlPressPending = true;
        m_panActive = false;
        m_panStartGlobal = event->globalPosition().toPoint();
        m_panStartH = horizontalScrollBar()->value();
        m_panStartV = verticalScrollBar()->value();
        event->accept();
        return;
    }
    QTableView::mousePressEvent(event);
}

void TableView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_ctrlPressPending) {
        const bool panned = m_panActive;
        m_ctrlPressPending = false;
        m_panActive = false;
        viewport()->unsetCursor();
        event->accept();
        if (!panned) {
            // No movement: the Ctrl+click toggles that row, keeping the rest
            const QModelIndex idx = indexAt(event->pos());
            if (idx.isValid()) {
                selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
                selectionModel()->select(idx, QItemSelectionModel::Toggle
                                            | QItemSelectionModel::Rows);
            }
        }
        return;
    }
    QTableView::mouseReleaseEvent(event);
}

void TableView::paintEvent(QPaintEvent* event)
{
    QTableView::paintEvent(event);
    const QRect r = hoverRowRect();
    if (r.isNull())
        return;
    QPainter painter(viewport());
    painter.setClipRect(viewport()->rect());
    paintFocusFrame(&painter, r, palette());
}

// The header has its own menu via CustomContextMenuPolicy (see
// setSourceModel), so this only ever fires for the VIEWPORT — and a
// scroll area forwards viewport context menu events with VIEWPORT
// coordinates, which indexAt expects unchanged.
void TableView::contextMenuEvent(QContextMenuEvent* event)
{
    if (!m_proxy || !m_sourceModel)
        return;

    const QModelIndex idx = indexAt(event->pos());
    const int row = idx.isValid() ? m_proxy->mapToSource(idx).row() : -1;

    // Right-clicking an item OUTSIDE the selection makes it the selection
    // first (same rule as the grid, see GridView::contextMenuEvent).
    if (row >= 0 && !selectedRows().contains(row)) {
        setSelectedRows({row});
        emit selectionChangedRows(selectedRows());   // mirror into the grid
    }

    const QList<int> rows = selectedRows();

    MediaContextMenu::Context ctx;
    ctx.hasItems       = m_sourceModel->rowCount() > 0;
    ctx.selectionCount = rows.size();
    ctx.pastePossible  = canPaste();
    ctx.filtered       = m_sourceModel->isFiltered();
    ctx.hoverItemValid = row >= 0;

    using A = MediaContextMenu::Action;
    const A chosen = MediaContextMenu::exec(this, event->globalPos(), ctx);
    switch (chosen) {
    case A::None:      break;
    case A::Details:   emit detailsRequested(row); break;
    case A::SelectAll: selectAll(); break;
    case A::Copy:      emit copyRequested(); break;
    case A::Cut:       emit cutRequested(); break;
    case A::CopyPaths: {
        QStringList paths;
        for (int r : rows)
            paths.append(m_sourceModel->item(r).filePath);
        QGuiApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
        break;
    }
    case A::Paste:             emit pasteRequested(); break;
    case A::ClearFilter:       emit clearFilterRequested(); break;
    case A::EditMetadata:      emit editMetadataRequested(rows); break;
    case A::Rename:            emit renameRequested(rows); break;
    case A::FilterBySelection: emit filterToSelectionRequested(rows); break;
    case A::Trash:             emit deleteRequested(rows, false); break;
    case A::DeletePermanent:   emit deleteRequested(rows, true); break;
    }
    event->accept();
}

void TableView::showColumnMenu(const QPoint& globalPos)
{
    QMenu menu(this);
    menu.setTitle(tr("Columns"));

    QStringList names = {
        tr("Preview"), tr("Filename"), tr("Size"),
        tr("Modified"), tr("Created"), tr("Resolution"),
        tr("Duration"), tr("Type"), tr("Taken")
    };

    for (int col = 0; col < MediaModel::Col_COUNT; ++col) {
        QAction* act = menu.addAction(names.value(col));
        act->setCheckable(true);
        act->setChecked(!isColumnHidden(col));
        act->setData(col);
        connect(act, &QAction::toggled, this, [this, col](bool visible) {
            toggleColumn(col, visible);
        });
    }
    menu.exec(globalPos);
}

void TableView::toggleColumn(int col, bool visible)
{
    setColumnHidden(col, !visible);
    saveColumnVisibility();
}

void TableView::saveColumnVisibility()
{
    AppSettings::setAllColumnsHidden(MediaModel::Col_COUNT,
                                     [this](int col) { return isColumnHidden(col); });
}

void TableView::restoreColumnVisibility()
{
    for (int col = 0; col < MediaModel::Col_COUNT; ++col)
        setColumnHidden(col, AppSettings::columnHidden(col, false));
}
