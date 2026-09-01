#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPalette>
#include <QPointer>
#include <QSet>
#include <QString>

#include "GridView.h"   // GridView::LayoutSnapshot member

class DirectoryPanel;
class DetailsPanel;
class ElidedLabel;
class FullscreenTransitionOverlay;
class MediaModel;
class TableView;
class MediaViewer;
class QStackedWidget;
class QSlider;
class QLabel;
class QSplitter;
class QAction;
class QComboBox;
class QMenu;
class QToolButton;
class QToolBar;
class QDialog;
class QShortcut;
class QTimer;
class QVariantAnimation;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    // cloneSource (Ctrl+N, see openCloneWindow) makes the new window adopt
    // that window's directories, view mode, sort, splitter layout and
    // geometry instead of the persisted session state.
    explicit MainWindow(const QStringList& initialDirs = {}, QWidget* parent = nullptr,
                        const MainWindow* cloneSource = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // Watches Details dialogs and viewer windows: the grid frames the file
    // of the window the mouse is in, or (if the mouse is in none) of the
    // window that has focus (GridView::setExternalFocusPath)
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onSelectedDirectoriesChanged(const QStringList& recursive, const QStringList& individual);
    void toggleSidePanel();
    void showSidePanel();
    // Ctrl+N: independent top-level window that starts as a copy of this
    // one (same directories, view, sort, layout; offset by a title bar).
    void openCloneWindow();
    void switchToGrid();
    void switchToTable();
    void onItemDoubleClicked(int sourceRow);
    void onDetailsRequested(int row);
    void onEditMetadataRequested(const QList<int>& rows);
    void onRenameRequested(const QList<int>& rows);
    void onDeleteRequested(const QList<int>& rows, bool permanent);
    void updateStatusBar(int total);
    void onThumbnailLoaded();
    void onSortChanged();
    // Header click in the list view: adopts the column/order into the
    // toolbar combos and sorts the SOURCE model, so grid and list always
    // show the same order.
    void onTableSortRequested(int column, Qt::SortOrder order);
    void onFilterToSelection(const QList<int>& rows);
    // Hover focus frame of the ACTIVE view (grid or list) → details panel
    // and status-bar path. Independent of the selection.
    void onFocusItemChanged(int row);
    void onTableThumbSizeChanged(int value);
    void setFullscreenMode(bool on);
    // Reload button (toolbar): rescans the current directories, then jumps
    // to the first image once the rescan lands — unlike a sync-triggered
    // reload, which keeps the current scroll position.
    void performManualReload();

private:
    void buildToolBar();
    void buildMenuBar();
    void buildStatusBar();
    void openFolderDialog();
    // Opens the settings dialog. A non-empty pageId jumps straight to that
    // page with the sidebar collapsed (Settings menu shortcuts); an empty one
    // is the general entrance and shows the full page list.
    void openSettings(const QString& pageId = QString());
    void updateWindowTitle();
    void watchViewerFocus(MediaViewer* viewer);
    // External focus frame (see eventFilter). Aux windows (Details dialogs,
    // viewers) belong to ALL MainWindows of the application — the state is
    // static and applied to every open window's grid.
    static QString auxWindowPath(QWidget* w);
    static void setAuxBorderActive(QWidget* w, bool on);
    static void updateExternalFocus();
    static void applyDriverBorder();
    // Re-applies the current sort WITHOUT scrolling to top — used after
    // operations (metadata edit, rename, drop); the grid restores its view
    // state by path. onSortChanged (explicit user sort) still scrolls top.
    void reapplySort();
    void updateReloadButtonTooltip();

    // Fullscreen presentation mode: chrome hidden (mouse at the top edge
    // reveals it), side panel hidden, app-wide dark palette. All UI work is
    // driven from changeEvent(WindowStateChange) so WM-initiated fullscreen
    // takes the same path as F11/Escape. Chrome/side-panel slides and the
    // palette fade run over AppSettings::fullscreenAnimationMs (0 = off).
    void applyFullscreenUi(bool on);
    void revealFullscreenChrome();
    void hideFullscreenChrome();
    void updateFsHotZoneGeometry();
    // Desktop-overlay transition: cells fly
    // from source to target screen positions while the background fades
    // transparent<->black; the real window switches at windowOpacity 0.
    void startFullscreenOverlay(bool enteringFullscreen);
    void finishFullscreenOverlay();
    // After a mid-flight reversal returned the overlay to its source
    // state: the opposite window switch just ran — wait for the settled
    // layout, then hand over (mirrors finishFullscreenOverlay's settle)
    void finishOverlayReversal();
    // Restore the real window under the overlay's final frame and tear
    // the overlay down after a presentation delay (shared by the normal
    // finish and the reversal finish)
    void restoreWindowAfterOverlay();
    // The actual window-state switch (immediate path; the overlay path
    // calls it deferred, once its first frame is on screen)
    void applyFullscreenWindowState(bool on);
    // Enter-only overlay backdrop: this window rendered with an EMPTY
    // grid (the images are the overlay's flying cells), composed into a
    // screen grab of the decorated frame when possible. *globalRect
    // receives the backdrop's screen rect (frame or client area).
    QPixmap grabWindowBackdrop(QRect* globalRect);
    // Routes every side-panel visibility change; slides the panel while
    // the grid tracks the moving edge (normal mode AND fullscreen)
    void setSidePanelVisibleAnimated(bool visible);
    // Fades THIS window's palette between system and dark (window-level
    // override on top of the already-switched app palette)
    void startPaletteFade(const QPalette& from, const QPalette& to);
    static QPalette makeDarkPalette(const QPalette& system);
    static QPalette lerpPalette(const QPalette& from, const QPalette& to,
                                qreal t);
    static void acquireDarkPalette();
    static void releaseDarkPalette();

    DirectoryPanel* m_dirPanel   = nullptr;
    DetailsPanel*   m_detailsPanel = nullptr;
    MediaModel*     m_model      = nullptr;
    GridView*       m_gridView   = nullptr;
    TableView*      m_tableView  = nullptr;
    MediaViewer*    m_mediaViewer = nullptr;
    QStackedWidget* m_stack      = nullptr;
    QSplitter*      m_hSplitter  = nullptr;  // horizontal: side panel | content
    QSplitter*      m_sidePanel  = nullptr;  // vertical: directory panel / details panel
    QSlider*        m_sizeSlider = nullptr;
    QSlider*        m_tableSizeSlider = nullptr;
    QComboBox*      m_sortColumnCombo = nullptr;
    QComboBox*      m_sortOrderCombo = nullptr;
    QLabel*         m_statusLabel = nullptr;
    ElidedLabel*    m_focusPathLabel = nullptr;

    QAction*        m_actGrid    = nullptr;
    QAction*        m_actTable   = nullptr;
    // Toolbar slots of the two zoom sliders — one visible at a time (see
    // buildToolBar: the WIDGET's visibility does not stick in a QToolBar)
    QAction*        m_actSizeSlider     = nullptr;
    QAction*        m_actListSizeSlider = nullptr;
    QAction*        m_actSidePanel = nullptr;
    QAction*        m_actDetails = nullptr;
    QAction*        m_actCopy    = nullptr;
    QAction*        m_actCut     = nullptr;
    QAction*        m_actPaste   = nullptr;
    QAction*        m_actFullscreen = nullptr;
    QToolButton*    m_filterButton = nullptr;
    QToolButton*    m_reloadButton = nullptr;
    QAction*        m_actSyncMode = nullptr;
    QToolBar*       m_toolBar    = nullptr;

    // Throttles the "n loaded" status text: thumbnail deliveries fire
    // dataChanged far faster than a status line needs to change, and every
    // setText relayouts the status bar on the UI thread.
    QTimer*         m_statusThrottle = nullptr;

    // Fullscreen presentation mode state
    QWidget*    m_fsHotZone     = nullptr;  // top-edge reveal strip
    QTimer*     m_fsChromeTimer = nullptr;  // polls to re-hide revealed chrome
    QShortcut*  m_escShortcut   = nullptr;  // enabled only while fullscreen
    bool        m_fsUiApplied   = false;
    bool        m_fsSidePanelWasVisible = false;
    QByteArray  m_fsWindowedGeometry;      // geometry before entering fullscreen
    int         m_fsWindowedStackW = -1;   // stack width before entering —
                                           // expected settle width on exit
    // Fullscreen transition overlay (flying cells over the desktop);
    // while it lives, further fullscreen toggles are ignored
    QPointer<FullscreenTransitionOverlay> m_fsOverlay;
    int m_fsOverlaySettleRetries = 0;   // waiting for the switched layout
    int m_fsOverlayLastWidth = -1;
    // Window switch armed, waiting for the overlay's first presented
    // frame (or the fallback timer) — guards the once-only execution
    bool m_fsOverlaySwitchPending = false;
    // Mid-flight reversal state: original direction, "startTo not yet
    // called" (reversal must wait for the flight), reversal requested
    // during that wait, and "end sequence running" (toggles ignored)
    bool m_fsOverlayEnter = false;
    bool m_fsOverlayAwaitingTarget = false;
    bool m_fsOverlayReversePending = false;
    bool m_fsOverlayFinishing = false;
    // Stack width at overlay start — the EXACT settle target for a
    // reversal (it returns to the layout the transition started from)
    int m_fsOverlaySourceStackW = -1;
    // Source-side state for finishFullscreenOverlay: which rows already
    // fly (captured at overlay start), and the pre-switch raster for the
    // hypothetical origin of rows visible only in the target
    QSet<int> m_fsOverlaySourceRows;
    GridView::LayoutSnapshot m_fsOverlaySourceLayout;

    // Fullscreen animations (durations from fullscreenAnimationMs)
    QVariantAnimation* m_fsChromeAnim  = nullptr;  // menu+toolbar height slide
    QVariantAnimation* m_sidePanelAnim = nullptr;  // side-panel width slide
    QVariantAnimation* m_paletteAnim   = nullptr;  // system↔dark fade
    QPalette m_paletteFadeFrom;             // endpoints of the running fade
    QPalette m_paletteFadeTo;
    int m_fsMenuNaturalH = 0;               // captured at slide start
    int m_fsToolNaturalH = 0;
    int m_sidePanelLastWidth = 220;         // slide-in target width

    int             m_loadedCount = 0;
    QStringList     m_selectedRecursive;
    QStringList     m_selectedIndividual;

    // Whether THIS window's grid currently displays the external focus
    // (no local hover, row visible) — the driving aux window's border is
    // on while ANY open grid does (see applyDriverBorder)
    bool m_externalDisplayed = false;
    // Grid and list mirror one selection. Suspended across a model reset:
    // the list's selection model clears itself there, and that empty state
    // must not overwrite what the grid restores by path afterwards.
    bool m_selectionSyncBlocked = false;
    void syncSelectionToTable();

    // App-global state: aux windows belong to all MainWindows.
    static QList<MainWindow*> s_allWindows;
    // Modeless per-file details dialogs (context menu "Details…"), keyed by
    // file path — at most one dialog per file; a second request raises it.
    static QHash<QString, QPointer<QDialog>> s_detailsDialogs;
    // Aux windows driving the grids' external focus frame: the one under
    // the mouse wins over the focused one; the current driver shows a
    // solid border while a grid displays the external focus
    static QPointer<QWidget> s_frameMouseWin;
    static QPointer<QWidget> s_frameActiveWin;
    static QPointer<QWidget> s_frameDrivingWin;

    // Dark presentation palette is app-wide and refcounted — with multiple
    // MainWindows the system palette returns only when the LAST fullscreen
    // window leaves the mode.
    static int      s_fullscreenWindows;
    static QPalette s_systemPalette;

    // Metadata edit in flight: outstanding exiftool write jobs, then (for
    // videos with "mtime ← taken") a re-read + C++ setFileTime phase, then
    // cache drop + rescan.
    void finalizeMetadataWrites();
    void onMetadataReadyForMtime(const QStringList& filePaths);
    void finishMetadataEdit();
    int             m_pendingMetaWrites = 0;
    QStringList     m_pendingMetaPaths;
    QStringList     m_pendingMtimeVideos;
};
