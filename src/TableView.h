#pragma once

#include <QTableView>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>
#include <Qt>

#include "PaddedListProxy.h"

class MediaModel;

// List view over the SAME MediaModel the grid shows. Filtering and sorting
// therefore live in the source model for both views; selection is mirrored
// between them by MainWindow. The proxy in
// between never sorts or filters — it only adds the blank pad rows the zoom
// anchor uses as dummy space.
class TableView : public QTableView
{
    Q_OBJECT
public:
    explicit TableView(QWidget* parent = nullptr);

    // Row zoom: the thumbnail edge in px; the row is that plus padding.
    static constexpr int kMinThumbSize = 24;
    static constexpr int kMaxThumbSize = 160;
    static constexpr int kRowPadding   = 4;
    static constexpr int kZoomStep     = 8;   // px per Ctrl+wheel notch

    void setSourceModel(MediaModel* model);
    void setThumbnailSize(int size);
    void saveColumnVisibility();
    void restoreColumnVisibility();

    // Selection as SOURCE MODEL rows, ascending. setSelectedRows mirrors the
    // grid's selection; it emits nothing when the selection already matches
    // (that is what breaks the sync loop between the two views).
    QList<int> selectedRows() const;
    void setSelectedRows(const QList<int>& rows);

    // Header sort indicator — driven by MainWindow so it always shows the
    // sort actually applied to the source model (also for sorts started
    // from the toolbar combos).
    void showSortIndicator(int column, Qt::SortOrder order);

    // Directory that external drops (and paste) go into. Empty = more than
    // one root is shown, both are rejected — same rule as the grid.
    void setDropTargetDir(const QString& dir) { m_dropTargetDir = dir; }
    bool canPaste() const;

    // Scrolls the first selected row into view — used when the list becomes
    // the active view, so the shared selection is actually visible.
    void scrollToSelection();

    // Rows are the unit of this view, so nothing that merely makes an ITEM
    // visible (selection, current index, keyboard navigation, drops) may
    // scroll sideways — that made the list drift out to the left, cutting
    // off the first column. Horizontal scrolling stays available
    // explicitly: scrollbar, shift+wheel, Ctrl+drag pan.
    void scrollTo(const QModelIndex& index, ScrollHint hint = EnsureVisible) override;

signals:
    void itemDoubleClicked(int sourceRow);
    void selectionChangedRows(const QList<int>& sourceRows);  // sorted
    // Row carrying the hover focus frame, -1 = none. Independent of the
    // selection; feeds DetailsPanel + status-bar path, same as the grid's.
    void focusItemChanged(int sourceRow);
    void sortRequested(int column, Qt::SortOrder order);
    // Ctrl+wheel zoom: the new row size. Handled SYNCHRONOUSLY by
    // MainWindow (slider + setThumbnailSize) — wheelEvent reads the applied
    // row height back right after emitting to re-anchor the scroll position.
    void thumbnailSizeChangeRequested(int size);
    // Context menu — same set as GridView, handled by the same MainWindow slots
    void detailsRequested(int sourceRow);
    void editMetadataRequested(const QList<int>& sourceRows);
    void renameRequested(const QList<int>& sourceRows);
    void deleteRequested(const QList<int>& sourceRows, bool permanent);
    void filterToSelectionRequested(const QList<int>& sourceRows);
    void clearFilterRequested();
    void copyRequested();
    void cutRequested();
    void pasteRequested();
    void filesDropped();   // an external drop was performed into the target dir

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onDoubleClicked(const QModelIndex& index);
    void onScrolled(int value);
    void toggleColumn(int col, bool visible);

private:
    void showColumnMenu(const QPoint& globalPos);
    void setHoverRow(int row);           // -1 = none; emits focusItemChanged
    QRect hoverRowRect() const;          // viewport rect of the hovered row
    // DUMMY SPACE that lets the zoom anchor hold a row the natural scroll
    // range cannot reach (see wheelEvent): the heights of the proxy's blank
    // leading/trailing rows. Qt derives scroll range, scrollbar visibility
    // and painting from them like from any other row. One-way — scrolling
    // back releases them.
    void setDummySpace(int topPx, int bottomPx);
    void applyDummySpace();   // (re-)applies the heights after a row resize
    void releaseDummySpace(int scrollValue);   // consume it while scrolling
    int  bottomPadFor(int scrollTarget, int topPad, int rowH) const;
    // Proxy row under the mouse, -1 for none or a pad row
    int  rowUnderCursor() const;

    MediaModel*            m_sourceModel = nullptr;
    PaddedListProxy*       m_proxy       = nullptr;
    QString                m_dropTargetDir;
    int                    m_hoverRow = -1;   // PROXY row under the cursor
    // Ctrl+wheel accumulator: one step per full 120-unit notch — fast
    // spins arrive coalesced in one event, hi-res wheels as many small ones
    int                    m_zoomWheelRemainder = 0;
    // Dummy space kept for the zoom anchor, in px (see setDummySpace)
    int                    m_dummyTopPx    = 0;
    int                    m_dummyBottomPx = 0;
    // Section last used as the trailing pad — it becomes a real row when
    // the model grows and has to get the default height back
    int                    m_lastBottomPadSection = -1;
    // True while WE are moving the scroll value — the release rule must not
    // read our own anchoring as the user scrolling back
    bool                   m_inScrollUpdate = false;
    // Ctrl+left press: a selection TOGGLE on release without movement, or a
    // PAN of the view once the drag threshold is crossed (the toggle is
    // discarded then) — the grid's contract, here scrolling both axes.
    bool                   m_ctrlPressPending = false;
    bool                   m_panActive        = false;
    QPoint                 m_panStartGlobal;
    int                    m_panStartH = 0;
    int                    m_panStartV = 0;
    // Guard: applying a mirrored selection must not echo back out
    bool                   m_applyingSelection = false;
};
