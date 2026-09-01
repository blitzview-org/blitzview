#pragma once

#include <functional>
#include <QString>

// Encapsulates all persistent application settings (QSettings).
// All methods are static – no instantiation needed.
class AppSettings
{
public:
    AppSettings() = delete;

    // --- Last opened directory ---
    static QString lastDir();
    static void    setLastDir(const QString& path);

    // --- Window geometry ---
    static QByteArray windowGeometry();
    static void setWindowGeometry(const QByteArray& geo);

    // --- Splitter state (captures directory panel width) ---
    static QByteArray splitterState();
    static void setSplitterState(const QByteArray& state);

    // --- Side panel (directory panel + details panel) visibility ---
    // Key name kept for backward compatibility with existing settings files.
    static bool dirPanelVisible(bool defaultVisible = false);
    static void setDirPanelVisible(bool visible);

    // --- Details panel (inside the side panel) ---
    static bool detailsPanelVisible(bool defaultVisible = false);
    static void setDetailsPanelVisible(bool visible);
    static QByteArray sideSplitterState();
    static void setSideSplitterState(const QByteArray& state);

    // --- Tree anchor ---
    static QString treeAnchor();
    static void setTreeAnchor(const QString& anchor);

    // --- Selected directories: new lazy model ---
    // recursive: folders selected with all descendants (only roots stored)
    // individual: folders selected without descendants
    // First call reads old "selectedDirs" key and migrates automatically.
    static QStringList recursiveDirs();
    static void        setRecursiveDirs(const QStringList& dirs);
    static QStringList individualDirs();
    static void        setIndividualDirs(const QStringList& dirs);

    // Legacy key — kept only for migration, do not use in new code
    static QStringList selectedDirs();
    static void        setSelectedDirs(const QStringList& dirs);

    // --- Last used rename pattern ---
    static QString lastRenamePattern();
    static void setLastRenamePattern(const QString& pattern);

    // --- Rename pattern presets (user-editable list in the rename dialog;
    //     empty = not seeded yet, RenameDialog inserts its default) ---
    static QStringList renamePatterns();
    static void        setRenamePatterns(const QStringList& patterns);

    // --- Known metadata tags (completion choices when adding tags) ---
    static QStringList knownTags();
    static void setKnownTags(const QStringList& tags);

    // --- Tag symbols: flat pair list [tag0, symbol0, tag1, symbol1, …] ---
    // Tag text is user data and may contain '/' or '=' — unsafe as QSettings
    // KEYS, safe as VALUES; same QStringList encoding as metadata/knownTags.
    // Use TagSymbols (cached, case-folded lookup) instead of calling these
    // directly.
    static QStringList tagSymbolPairs();
    static void        setTagSymbolPairs(const QStringList& pairs);

    // --- Media viewer ---
    static bool multipleViewers();             // default: false (single window)
    static void setMultipleViewers(bool enabled);
    // Geometry of the last closed viewer window (new viewers start with it)
    static QByteArray viewerGeometry();
    static void setViewerGeometry(const QByteArray& geo);

    // --- Permanent delete in the grid context menu (default: off) ---
    static bool permanentDeleteEnabled();
    static void setPermanentDeleteEnabled(bool enabled);

    // --- Automatic filesystem sync mode (default: on) ---
    static bool syncModeEnabled();
    static void setSyncModeEnabled(bool enabled);

    // --- Thumbnail disk cache ---
    static bool   diskCacheEnabled();          // default: true
    static void   setDiskCacheEnabled(bool enabled);
    static qint64 diskCacheMaxBytes();         // 0 = unlimited, default: 1 GiB
    static void   setDiskCacheMaxBytes(qint64 bytes);

    // --- Grid/Table zoom ---
    // Grid zoom state is the COLUMN COUNT; the table keeps a thumbnail
    // pixel size.
    static int  gridColumns(int defaultColumns = 4);
    static void setGridColumns(int columns);
    static int  tableIconSize(int defaultSize = 40);
    static void setTableIconSize(int size);

    // Which view was active last — restored at startup. True = the list
    // ("Table" in code, "List" in the UI), false = the grid.
    static bool listViewActive(bool defaultActive = false);
    static void setListViewActive(bool active);

    // --- Focus border of Details/viewer windows (px, default 1, 0 = off) ---
    // Reserved margin that is colored while the window drives the grid's
    // focus frame.
    static int  focusBorderWidth();
    static void setFocusBorderWidth(int px);

    // --- Grid reflow animation (ms, default 150, 0 = off) ---
    // Duration of the paint-only transition between the old and the new
    // grid raster on zoom and column-count resizes.
    static int  reflowAnimationMs();
    static void setReflowAnimationMs(int ms);

    // --- Grid scroll animation (ms, default 120, 0 = off) ---
    // Duration of the paint-only catch-up after a wheel scroll in the grid:
    // the raster jumps to the new position, the PAINT glides there.
    // 0 = the old instant row jump.
    static int  scrollAnimationMs();
    static void setScrollAnimationMs(int ms);

    // --- Grid scroll snapping (default true) ---
    // true: a scroll always comes to rest on a whole row (the grid raster);
    // false: it may rest between rows, and hi-res wheels/touchpads scroll
    // sub-notch amounts.
    static bool scrollSnapToGrid();
    static void setScrollSnapToGrid(bool on);

    // --- UI transition animations (ms, default 200, 0 = off) ---
    // Duration of the side-panel slide (windowed and fullscreen), the
    // fullscreen chrome slide, the system↔dark palette fade and the grid
    // glide when entering/leaving the fullscreen presentation mode
    static int  fullscreenAnimationMs();
    static void setFullscreenAnimationMs(int ms);

    // --- Shared mouse-movement threshold (px, default 8) ---
    // What counts as a "deliberate" mouse position: releases the frozen
    // focus frame after a Ctrl+wheel zoom in the grid, and sizes the
    // bottom-corner park zones in the fullscreen viewer.
    static int  mouseThresholdPx();
    static void setMouseThresholdPx(int px);

    // --- Double-click interval (app-wide, ms; 0 = system default) ---
    static int  doubleClickIntervalMs();
    static void setDoubleClickIntervalMs(int ms);
    // Pristine system value, captured at startup BEFORE any override —
    // needed to restore "system default" without an app restart.
    // Runtime-only, not persisted.
    static void rememberSystemDoubleClickInterval(int ms);
    static int  systemDoubleClickInterval();

    // --- Settings dialog state ---
    // Page shown when the dialog is opened without a target page, plus the
    // dialog geometry. The sidebar mode is NOT persisted — the entry point
    // decides it (jump = collapsed, general entrance = expanded).
    static QString    settingsLastPage();
    static void       setSettingsLastPage(const QString& pageId);
    static QByteArray settingsGeometry();
    static void       setSettingsGeometry(const QByteArray& geo);

    // --- Column visibility in the table view ---
    // col: column index (0 … Col_COUNT-1), defaultHidden: fallback value
    static bool columnHidden(int col, bool defaultHidden = false);
    static void setColumnHidden(int col, bool hidden);
    // Write all columns at once (colCount entries)
    static void setAllColumnsHidden(int colCount,
                                    std::function<bool(int)> isHidden);
};
