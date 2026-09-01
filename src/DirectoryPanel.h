#pragma once

#include <QMap>
#include <QUrl>
#include <QWidget>
#include "DirTree.h"

class QTreeView;
class QToolBar;
class QMenu;
class QModelIndex;
class FileSystemModel;

class DirectoryPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DirectoryPanel(QWidget* parent = nullptr);

    void setCurrentDirectory(const QString& path);
    void setSelectedDirectories(const QStringList& recursive,
                                const QStringList& individual = {});
    void restoreTreeAnchor(const QString& anchor);
    QString currentDirectory() const { return m_lastActivePath; }
    QStringList selectedDirectories() const;
    QString treeAnchor() const { return m_treeAnchor; }
    void resetTreeAnchor();
    void goHome();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void selectedDirectoriesChanged(const QStringList& recursive,
                                    const QStringList& individual);
    void treeAnchorChanged(const QString& anchor);
    void hideRequested();
    // File drop onto a directory in the tree; copy/move is decided upstream
    void filesDroppedOnDir(const QList<QUrl>& urls, const QString& targetDir);
    // Context menu: open this folder in a new BlitzView main window
    void openInNewWindowRequested(const QString& path);

private slots:
    void onClicked(const QModelIndex& idx);
    void onDoubleClicked(const QModelIndex& idx);
    void goUp();
    void goDown();
    void onClose();
    void showAnchorContextMenu(const QPoint& pos);

private:
    void initializeTreeAnchor();
    void updateTreeDisplay();
    void updateButtonStates();
    void showTreeContextMenu(const QPoint& pos);
    void navigateToPath(const QString& path);
    void populateDeviceMenu();
    QString commonNextChild() const;
    QString findRootForPath(const QString& path);
    QString mountDevice(const QString& devicePath);
    void unmountDevice(const QString& devicePath);
    void clearIfUnderPath(const QString& path);
    bool selectRangeTo(const QModelIndex& targetIdx);
    void setTreeAnchor(const QString& anchor);
    void navigateToRoot(const QString& root);
    void onDirectoryLoaded(const QString& path);
    void applyExpandAndSelect();
    void clearAllSelections();
    void emitSelection();

    QTreeView*          m_tree        = nullptr;
    FileSystemModel*    m_fsModel     = nullptr;
    QToolBar*           m_toolBar     = nullptr;
    QWidget*            m_anchorLabel = nullptr;
    QMenu*              m_deviceMenu = nullptr;
    QAction*            m_upAction   = nullptr;
    QAction*            m_downAction = nullptr;
    DirTree*            m_dirTree    = nullptr;
    QString             m_lastActivePath;
    QString             m_treeAnchor;
    bool                m_ignoreClick = false;
    QStringList         m_pendingExpandPaths;
    QString             m_pendingSelectPath;
    QMap<QString,QString> m_rootMemory;
};
