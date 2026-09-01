#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QHash>

class QFileSystemWatcher;

enum class SelectionState { None, Individual, Recursive };

struct DirNode {
    QString         path;
    QString         name;
    DirNode*        parent                = nullptr;
    bool            hasChildren           = false;
    bool            childrenLoaded        = false;
    QList<DirNode*> children;               // sorted by name, case-insensitive
    SelectionState  selection             = SelectionState::None;
    bool            hasSelectedDescendant = false;
};

class DirTree : public QObject
{
    Q_OBJECT
public:
    explicit DirTree(QObject* parent = nullptr);
    ~DirTree() override;

    DirNode* getOrCreate(const QString& path, DirNode* parentNode = nullptr);
    DirNode* get(const QString& path) const;

    // Creates all missing ancestors top-down, returns the node for path.
    DirNode* ensurePath(const QString& path);

    // Reads children from filesystem. Recursive nodes propagate selection to new children.
    void loadChildren(DirNode* node);

    void evictOutside(const QString& keepPrefix);
    void evictPath(const QString& path);
    void clear();

    // Central selection API — all selection changes go through these methods.
    // They handle downward propagation and upward flag/state updates.
    void setSelection(DirNode* node, SelectionState state);
    void clearSubtreeSelection(DirNode* node);
    void clearDescendantSelection(DirNode* node);
    void clearAllSelections();

    void collectSelectionRoots(QStringList& recursive, QStringList& individual) const;

    // Recomputes hasSelectedDescendant from node upward. Call after structural changes
    // (e.g. goUp adds a new parent node) that happen outside the selection API.
    void updateAncestorFlags(DirNode* node);

    QHash<QString, DirNode*> nodeByPath;

signals:
    void nodeChanged(const QString& path);

private slots:
    void onDirectoryChanged(const QString& changedPath);

private:
    static bool checkHasChildren(const QString& path);
    void evictNodeInternal(DirNode* node);
    void propagateRecursiveDown(DirNode* node);
    void degradeAncestors(DirNode* node);
    void checkPromoteToRecursive(DirNode* node);

    QFileSystemWatcher* m_watcher = nullptr;
};
