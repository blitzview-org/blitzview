#include "AppSettings.h"

#include <QSettings>
#include <algorithm>

// ---------------------------------------------------------------------------
// Last opened directory
// ---------------------------------------------------------------------------

QString AppSettings::lastDir()
{
    QSettings s;
    return s.value("lastDir").toString();
}

void AppSettings::setLastDir(const QString& path)
{
    QSettings s;
    s.setValue("lastDir", path);
}

// ---------------------------------------------------------------------------
// Window geometry
// ---------------------------------------------------------------------------

QByteArray AppSettings::windowGeometry()
{
    QSettings s;
    return s.value("windowGeometry").toByteArray();
}

void AppSettings::setWindowGeometry(const QByteArray& geo)
{
    QSettings s;
    s.setValue("windowGeometry", geo);
}

// ---------------------------------------------------------------------------
// Splitter state
// ---------------------------------------------------------------------------

QByteArray AppSettings::splitterState()
{
    QSettings s;
    return s.value("splitterState").toByteArray();
}

void AppSettings::setSplitterState(const QByteArray& state)
{
    QSettings s;
    s.setValue("splitterState", state);
}

// ---------------------------------------------------------------------------
// Directory panel visibility
// ---------------------------------------------------------------------------

bool AppSettings::dirPanelVisible(bool defaultVisible)
{
    QSettings s;
    return s.value("dirPanelVisible", defaultVisible).toBool();
}

void AppSettings::setDirPanelVisible(bool visible)
{
    QSettings s;
    s.setValue("dirPanelVisible", visible);
}

// ---------------------------------------------------------------------------
// Details panel
// ---------------------------------------------------------------------------

bool AppSettings::detailsPanelVisible(bool defaultVisible)
{
    QSettings s;
    return s.value("detailsPanelVisible", defaultVisible).toBool();
}

void AppSettings::setDetailsPanelVisible(bool visible)
{
    QSettings s;
    s.setValue("detailsPanelVisible", visible);
}

QByteArray AppSettings::sideSplitterState()
{
    QSettings s;
    return s.value("sideSplitterState").toByteArray();
}

void AppSettings::setSideSplitterState(const QByteArray& state)
{
    QSettings s;
    s.setValue("sideSplitterState", state);
}

// ---------------------------------------------------------------------------
// Tree anchor
// ---------------------------------------------------------------------------

QString AppSettings::treeAnchor()
{
    QSettings s;
    return s.value("treeAnchor").toString();
}

void AppSettings::setTreeAnchor(const QString& anchor)
{
    QSettings s;
    s.setValue("treeAnchor", anchor);
}

// ---------------------------------------------------------------------------
// Selected directories
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Selected directories — new lazy model
// ---------------------------------------------------------------------------

QStringList AppSettings::recursiveDirs()
{
    QSettings s;
    if (s.contains("recursiveDirs"))
        return s.value("recursiveDirs").toStringList();

    // One-time migration from the old flat "selectedDirs" list.
    // The old list contained every individual subdir; reduce to minimal roots.
    QStringList old = s.value("selectedDirs").toStringList();
    if (old.isEmpty())
        return {};

    QStringList sorted = old;
    std::sort(sorted.begin(), sorted.end());
    QStringList roots;
    for (const QString& path : sorted) {
        if (!roots.isEmpty() && path.startsWith(roots.last() + '/'))
            continue;
        roots.append(path);
    }
    return roots;
}

void AppSettings::setRecursiveDirs(const QStringList& dirs)
{
    QSettings s;
    s.setValue("recursiveDirs", dirs);
}

QStringList AppSettings::individualDirs()
{
    QSettings s;
    return s.value("individualDirs").toStringList();
}

void AppSettings::setIndividualDirs(const QStringList& dirs)
{
    QSettings s;
    s.setValue("individualDirs", dirs);
}

// ---------------------------------------------------------------------------
// Legacy selected directories (kept for one-time migration only)
// ---------------------------------------------------------------------------

QStringList AppSettings::selectedDirs()
{
    QSettings s;
    return s.value("selectedDirs").toStringList();
}

void AppSettings::setSelectedDirs(const QStringList& dirs)
{
    QSettings s;
    s.setValue("selectedDirs", dirs);
}

// ---------------------------------------------------------------------------
// Last used rename pattern
// ---------------------------------------------------------------------------

QString AppSettings::lastRenamePattern()
{
    QSettings s;
    return s.value("rename/lastPattern").toString();
}

void AppSettings::setLastRenamePattern(const QString& pattern)
{
    QSettings s;
    s.setValue("rename/lastPattern", pattern);
}

QStringList AppSettings::renamePatterns()
{
    QSettings s;
    return s.value("rename/patterns").toStringList();
}

void AppSettings::setRenamePatterns(const QStringList& patterns)
{
    QSettings s;
    s.setValue("rename/patterns", patterns);
}

// ---------------------------------------------------------------------------
// Known metadata tags
// ---------------------------------------------------------------------------

QStringList AppSettings::knownTags()
{
    QSettings s;
    return s.value("metadata/knownTags").toStringList();
}

void AppSettings::setKnownTags(const QStringList& tags)
{
    QSettings s;
    s.setValue("metadata/knownTags", tags);
}

QStringList AppSettings::tagSymbolPairs()
{
    QSettings s;
    return s.value("metadata/tagSymbols").toStringList();
}

void AppSettings::setTagSymbolPairs(const QStringList& pairs)
{
    QSettings s;
    s.setValue("metadata/tagSymbols", pairs);
}

// ---------------------------------------------------------------------------
// Media viewer
// ---------------------------------------------------------------------------

bool AppSettings::multipleViewers()
{
    QSettings s;
    return s.value("viewer/multipleWindows", false).toBool();
}

void AppSettings::setMultipleViewers(bool enabled)
{
    QSettings s;
    s.setValue("viewer/multipleWindows", enabled);
}

QByteArray AppSettings::viewerGeometry()
{
    QSettings s;
    return s.value("viewer/geometry").toByteArray();
}

void AppSettings::setViewerGeometry(const QByteArray& geo)
{
    QSettings s;
    s.setValue("viewer/geometry", geo);
}

bool AppSettings::permanentDeleteEnabled()
{
    QSettings s;
    return s.value("files/permanentDelete", false).toBool();
}

void AppSettings::setPermanentDeleteEnabled(bool enabled)
{
    QSettings s;
    s.setValue("files/permanentDelete", enabled);
}

bool AppSettings::syncModeEnabled()
{
    QSettings s;
    return s.value("sync/enabled", true).toBool();
}

void AppSettings::setSyncModeEnabled(bool enabled)
{
    QSettings s;
    s.setValue("sync/enabled", enabled);
}

int AppSettings::gridColumns(int defaultColumns)
{
    QSettings s;
    return qMax(1, s.value("view/gridColumns", defaultColumns).toInt());
}

void AppSettings::setGridColumns(int columns)
{
    QSettings s;
    s.setValue("view/gridColumns", columns);
}

int AppSettings::tableIconSize(int defaultSize)
{
    QSettings s;
    return s.value("view/tableIconSize", defaultSize).toInt();
}

void AppSettings::setTableIconSize(int size)
{
    QSettings s;
    s.setValue("view/tableIconSize", size);
}

bool AppSettings::listViewActive(bool defaultActive)
{
    QSettings s;
    return s.value("view/listViewActive", defaultActive).toBool();
}

void AppSettings::setListViewActive(bool active)
{
    QSettings s;
    s.setValue("view/listViewActive", active);
}

int AppSettings::doubleClickIntervalMs()
{
    QSettings s;
    return s.value("input/doubleClickMs", 0).toInt();
}

void AppSettings::setDoubleClickIntervalMs(int ms)
{
    QSettings s;
    s.setValue("input/doubleClickMs", ms);
}

int AppSettings::focusBorderWidth()
{
    QSettings s;
    return s.value("ui/focusBorderWidth", 1).toInt();
}

void AppSettings::setFocusBorderWidth(int px)
{
    QSettings s;
    s.setValue("ui/focusBorderWidth", px);
}

int AppSettings::reflowAnimationMs()
{
    QSettings s;
    return s.value("ui/reflowAnimMs", 150).toInt();
}

void AppSettings::setReflowAnimationMs(int ms)
{
    QSettings s;
    s.setValue("ui/reflowAnimMs", ms);
}

int AppSettings::scrollAnimationMs()
{
    QSettings s;
    return s.value("ui/scrollAnimMs", 120).toInt();
}

void AppSettings::setScrollAnimationMs(int ms)
{
    QSettings s;
    s.setValue("ui/scrollAnimMs", ms);
}

bool AppSettings::scrollSnapToGrid()
{
    QSettings s;
    return s.value("ui/scrollSnapToGrid", true).toBool();
}

void AppSettings::setScrollSnapToGrid(bool on)
{
    QSettings s;
    s.setValue("ui/scrollSnapToGrid", on);
}

int AppSettings::fullscreenAnimationMs()
{
    QSettings s;
    return s.value("ui/fullscreenAnimMs", 200).toInt();
}

void AppSettings::setFullscreenAnimationMs(int ms)
{
    QSettings s;
    s.setValue("ui/fullscreenAnimMs", ms);
}

int AppSettings::mouseThresholdPx()
{
    QSettings s;
    return s.value("input/mouseThresholdPx", 8).toInt();
}

void AppSettings::setMouseThresholdPx(int px)
{
    QSettings s;
    s.setValue("input/mouseThresholdPx", px);
}

// ---------------------------------------------------------------------------
// Settings dialog state
// ---------------------------------------------------------------------------

QString AppSettings::settingsLastPage()
{
    QSettings s;
    return s.value("settings/lastPage").toString();
}

void AppSettings::setSettingsLastPage(const QString& pageId)
{
    QSettings s;
    s.setValue("settings/lastPage", pageId);
}

QByteArray AppSettings::settingsGeometry()
{
    QSettings s;
    return s.value("settings/geometry").toByteArray();
}

void AppSettings::setSettingsGeometry(const QByteArray& geo)
{
    QSettings s;
    s.setValue("settings/geometry", geo);
}

static int g_systemDoubleClickInterval = 400;

void AppSettings::rememberSystemDoubleClickInterval(int ms)
{
    g_systemDoubleClickInterval = ms;
}

int AppSettings::systemDoubleClickInterval()
{
    return g_systemDoubleClickInterval;
}

// ---------------------------------------------------------------------------
// Thumbnail disk cache
// ---------------------------------------------------------------------------

bool AppSettings::diskCacheEnabled()
{
    QSettings s;
    return s.value("diskCache/enabled", true).toBool();
}

void AppSettings::setDiskCacheEnabled(bool enabled)
{
    QSettings s;
    s.setValue("diskCache/enabled", enabled);
}

qint64 AppSettings::diskCacheMaxBytes()
{
    QSettings s;
    return s.value("diskCache/maxBytes", qint64(1024) * 1024 * 1024).toLongLong();
}

void AppSettings::setDiskCacheMaxBytes(qint64 bytes)
{
    QSettings s;
    s.setValue("diskCache/maxBytes", bytes);
}

// ---------------------------------------------------------------------------
// Column visibility in the table view
// ---------------------------------------------------------------------------

bool AppSettings::columnHidden(int col, bool defaultHidden)
{
    QSettings s;
    return s.value(QString("table/col%1_hidden").arg(col), defaultHidden).toBool();
}

void AppSettings::setColumnHidden(int col, bool hidden)
{
    QSettings s;
    s.setValue(QString("table/col%1_hidden").arg(col), hidden);
}

void AppSettings::setAllColumnsHidden(int colCount,
                                      std::function<bool(int)> isHidden)
{
    QSettings s;
    for (int col = 0; col < colCount; ++col)
        s.setValue(QString("table/col%1_hidden").arg(col), isHidden(col));
}
