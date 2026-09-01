#include "DirTree.h"

#include "Path.h"

#include <QFileSystemWatcher>
#include <QDirIterator>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

DirTree::DirTree(QObject* parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &DirTree::onDirectoryChanged);
}

DirTree::~DirTree()
{
    clear();
}

DirNode* DirTree::getOrCreate(const QString& rawPath, DirNode* parentNode)
{
    // nodeByPath is keyed by path, so the key must be canonical — otherwise
    // two spellings of the same directory become two nodes and the selection
    // invariants silently break.
    const QString path = Path::normalize(rawPath);

    auto it = nodeByPath.find(path);
    if (it != nodeByPath.end()) {
        DirNode* existing = it.value();
        if (parentNode && !existing->parent)
            existing->parent = parentNode;
        return existing;
    }

    auto* node = new DirNode;
    node->path = path;
    node->name = QFileInfo(path).fileName();
    if (node->name.isEmpty())
        node->name = path;

    if (parentNode) {
        node->parent = parentNode;
    } else {
        const QString parentPath = Path::parentOf(path);
        if (!parentPath.isEmpty())
            node->parent = nodeByPath.value(parentPath, nullptr);
    }

    node->hasChildren = checkHasChildren(path);
    nodeByPath.insert(path, node);
    m_watcher->addPath(path);
    return node;
}

DirNode* DirTree::get(const QString& path) const
{
    return nodeByPath.value(path, nullptr);
}

DirNode* DirTree::ensurePath(const QString& rawPath)
{
    const QString path = Path::normalize(rawPath);
    if (nodeByPath.contains(path))
        return nodeByPath[path];

    QStringList toCreate;
    QString cur = path;
    while (!cur.isEmpty() && !nodeByPath.contains(cur)) {
        toCreate.prepend(cur);
        cur = Path::parentOf(cur);  // empty at the filesystem root
    }

    for (const QString& p : toCreate)
        getOrCreate(p);

    return nodeByPath.value(path);
}

void DirTree::loadChildren(DirNode* node)
{
    if (!node || node->childrenLoaded)
        return;

    QSet<QString> existingPaths;
    for (DirNode* child : node->children)
        existingPaths.insert(child->path);

    QDirIterator it(node->path, QDir::AllDirs | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        QString childPath = it.next();
        if (existingPaths.contains(childPath))
            continue;

        DirNode* child = getOrCreate(childPath, node);

        if (node->selection == SelectionState::Recursive) {
            child->selection = SelectionState::Recursive;
            propagateRecursiveDown(child); // propagate to already-loaded grandchildren
        }

        auto pos = std::lower_bound(node->children.begin(), node->children.end(),
                                    child, [](DirNode* a, DirNode* b) {
            return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
        });
        node->children.insert(pos, child);
    }

    node->childrenLoaded = true;
    node->hasChildren = !node->children.isEmpty();
}

bool DirTree::checkHasChildren(const QString& path)
{
    QDirIterator it(path, QDir::AllDirs | QDir::NoDotAndDotDot);
    return it.hasNext();
}

// ---------------------------------------------------------------------------
// Central selection API
// ---------------------------------------------------------------------------

void DirTree::setSelection(DirNode* node, SelectionState state)
{
    if (!node) return;

    node->selection = state;

    switch (state) {
    case SelectionState::Recursive:
        propagateRecursiveDown(node);
        checkPromoteToRecursive(node->parent);
        break;
    case SelectionState::Individual:
        // Children keep their own state.
        // If all loaded children are already Recursive, promote immediately.
        checkPromoteToRecursive(node);
        break;
    case SelectionState::None:
        degradeAncestors(node);
        break;
    }

    updateAncestorFlags(node);
}

void DirTree::clearSubtreeSelection(DirNode* node)
{
    if (!node) return;
    node->selection = SelectionState::None;
    node->hasSelectedDescendant = false;
    clearDescendantSelection(node);
    degradeAncestors(node);
    updateAncestorFlags(node);
}

void DirTree::clearAllSelections()
{
    for (auto* node : nodeByPath) {
        node->selection = SelectionState::None;
        node->hasSelectedDescendant = false;
    }
}

void DirTree::propagateRecursiveDown(DirNode* node)
{
    for (DirNode* child : node->children) {
        child->selection = SelectionState::Recursive;
        child->hasSelectedDescendant = false;
        propagateRecursiveDown(child);
    }
    node->hasSelectedDescendant = false;
}

void DirTree::clearDescendantSelection(DirNode* node)
{
    for (DirNode* child : node->children) {
        child->selection = SelectionState::None;
        child->hasSelectedDescendant = false;
        clearDescendantSelection(child);
    }
    node->hasSelectedDescendant = false;
}

void DirTree::checkPromoteToRecursive(DirNode* node)
{
    if (!node || node->selection != SelectionState::Individual)
        return;
    if (!node->childrenLoaded || node->children.isEmpty())
        return;
    for (DirNode* child : node->children)
        if (child->selection != SelectionState::Recursive)
            return;
    node->selection = SelectionState::Recursive;
    node->hasSelectedDescendant = false;
    checkPromoteToRecursive(node->parent);
}

void DirTree::degradeAncestors(DirNode* node)
{
    for (DirNode* p = node->parent; p; p = p->parent) {
        if (p->selection == SelectionState::Recursive)
            p->selection = SelectionState::Individual;
        else
            break;
    }
}

void DirTree::updateAncestorFlags(DirNode* node)
{
    for (DirNode* p = node; p; p = p->parent) {
        bool any = false;
        for (DirNode* child : p->children) {
            if (child->selection != SelectionState::None || child->hasSelectedDescendant) {
                any = true;
                break;
            }
        }
        if (!any) {
            for (auto* n : nodeByPath) {
                if (n != p && Path::isUnder(n->path, p->path) &&
                    n->selection != SelectionState::None) {
                    any = true;
                    break;
                }
            }
        }
        p->hasSelectedDescendant = any;
    }
}

void DirTree::collectSelectionRoots(QStringList& recursive, QStringList& individual) const
{
    recursive.clear();
    individual.clear();
    for (auto* node : nodeByPath) {
        if (node->selection == SelectionState::Recursive) {
            if (!node->parent || node->parent->selection != SelectionState::Recursive)
                recursive.append(node->path);
        } else if (node->selection == SelectionState::Individual) {
            individual.append(node->path);
        }
    }
}

// ---------------------------------------------------------------------------
// Eviction
// ---------------------------------------------------------------------------

void DirTree::evictOutside(const QString& keepPrefix)
{
    QList<DirNode*> toDelete;
    for (auto it = nodeByPath.begin(); it != nodeByPath.end(); ) {
        const QString& p = it.key();
        if (!Path::isSelfOrUnder(p, keepPrefix)) {
            m_watcher->removePath(p);
            toDelete.append(it.value());
            it = nodeByPath.erase(it);
        } else {
            ++it;
        }
    }
    for (DirNode* n : toDelete)
        delete n;

    DirNode* kept = nodeByPath.value(keepPrefix);
    if (kept) {
        kept->parent = nullptr;
        updateAncestorFlags(kept);
    }
}

void DirTree::evictPath(const QString& path)
{
    QList<DirNode*> toDelete;
    DirNode* topParent = nullptr;

    for (auto it = nodeByPath.begin(); it != nodeByPath.end(); ) {
        const QString& p = it.key();
        if (Path::isSelfOrUnder(p, path)) {
            DirNode* node = it.value();
            if (node->parent && !toDelete.contains(node->parent))
                topParent = node->parent;
            m_watcher->removePath(p);
            toDelete.append(node);
            it = nodeByPath.erase(it);
        } else {
            ++it;
        }
    }

    if (topParent) {
        topParent->children.erase(
            std::remove_if(topParent->children.begin(), topParent->children.end(),
                           [&](DirNode* c) { return toDelete.contains(c); }),
            topParent->children.end());
        topParent->hasChildren = !topParent->children.isEmpty();
    }

    for (DirNode* n : toDelete)
        delete n;

    if (topParent)
        updateAncestorFlags(topParent);
}

void DirTree::evictNodeInternal(DirNode* node)
{
    for (DirNode* child : node->children)
        evictNodeInternal(child);
    node->children.clear();
    nodeByPath.remove(node->path);
    m_watcher->removePath(node->path);
    delete node;
}

void DirTree::clear()
{
    const QStringList watched = m_watcher->directories();
    if (!watched.isEmpty())
        m_watcher->removePaths(watched);
    for (DirNode* n : nodeByPath)
        delete n;
    nodeByPath.clear();
}

// ---------------------------------------------------------------------------
// Filesystem monitoring
// ---------------------------------------------------------------------------

void DirTree::onDirectoryChanged(const QString& changedPath)
{
    DirNode* node = get(changedPath);
    if (!node)
        return;

    if (!node->childrenLoaded) {
        bool newHas = checkHasChildren(changedPath);
        if (newHas != node->hasChildren) {
            node->hasChildren = newHas;
            emit nodeChanged(changedPath);
        }
        return;
    }

    QSet<QString> onDisk;
    {
        QDirIterator it(changedPath, QDir::AllDirs | QDir::NoDotAndDotDot);
        while (it.hasNext())
            onDisk.insert(it.next());
    }

    bool changed = false;
    bool selectionChanged = false;

    QList<DirNode*> removed;
    for (DirNode* child : node->children) {
        if (!onDisk.contains(child->path))
            removed.append(child);
    }
    for (DirNode* child : removed) {
        if (child->selection != SelectionState::None || child->hasSelectedDescendant)
            selectionChanged = true;
        node->children.removeOne(child);
        evictNodeInternal(child);
        changed = true;
    }

    for (const QString& childPath : onDisk) {
        if (!nodeByPath.contains(childPath)) {
            DirNode* child = getOrCreate(childPath, node);
            if (node->selection == SelectionState::Recursive)
                child->selection = SelectionState::Recursive;
            auto pos = std::lower_bound(node->children.begin(), node->children.end(),
                                        child, [](DirNode* a, DirNode* b) {
                return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
            });
            node->children.insert(pos, child);
            changed = true;
        }
    }

    node->hasChildren = !node->children.isEmpty();

    if (selectionChanged)
        updateAncestorFlags(node);

    if (changed)
        emit nodeChanged(changedPath);
}
