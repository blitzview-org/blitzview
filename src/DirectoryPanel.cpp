#include "DirectoryPanel.h"
#include "DirTree.h"
#include "FileSystemModel.h"
#include "ExternalDeviceManager.h"
#include "AppSettings.h"
#include "Path.h"

#include <QTreeView>
#include <QFileSystemModel>
#include <QVBoxLayout>
#include <QToolBar>
#include <QAction>
#include <QItemSelectionModel>
#include <QFileInfo>
#include <QDir>
#include <QSignalBlocker>
#include <QStyle>
#include <QMenu>
#include <QToolButton>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QLabel>
#include <QFrame>
#include <QPainter>
#include <QLinearGradient>


// ---------------------------------------------------------------------------
// AnchorLabel — shows rightmost portion of path with left fade on overflow
// ---------------------------------------------------------------------------

class AnchorLabel : public QWidget
{
public:
    explicit AnchorLabel(QWidget* parent = nullptr) : QWidget(parent) {}

    void setText(const QString& text)
    {
        m_text = text;
        update();
    }

    QSize sizeHint() const override
    {
        QFontMetrics fm(font());
        return QSize(0, fm.height() + 4);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (m_text.isEmpty())
            return;

        const int hpad = 6;
        const int availW = width() - 2 * hpad;
        if (availW <= 0)
            return;

        QFontMetrics fm(font());
        const int textW = fm.horizontalAdvance(m_text);
        const bool overflow = textW > availW;

        QPainter p(this);
        p.setFont(font());
        p.setPen(palette().windowText().color());

        QRect r(hpad, 0, availW, height());
        int flags = Qt::AlignVCenter | (overflow ? Qt::AlignRight : Qt::AlignLeft);
        p.drawText(r, flags, m_text);

        if (overflow) {
            const int fadeW = qMin(32, availW / 3);
            QLinearGradient grad(hpad, 0, hpad + fadeW, 0);
            QColor bg = palette().window().color();
            grad.setColorAt(0.0, bg);
            grad.setColorAt(1.0, Qt::transparent);
            p.fillRect(hpad, 0, fadeW, height(), grad);
        }
    }

private:
    QString m_text;
};


// ---------------------------------------------------------------------------
// DirectoryTreeView — draws full-width row backgrounds
// ---------------------------------------------------------------------------

class DirectoryTreeView : public QTreeView
{
public:
    using QTreeView::QTreeView;

    QFileSystemModel* fsModel          = nullptr;
    DirTree*          dirTree          = nullptr;
    QString           contextMenuPath;

protected:
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override
    {
        if (fsModel && dirTree && index.isValid()) {
            QString path = fsModel->filePath(index);
            DirNode* node = dirTree->get(path);

            if (node) {
                bool expanded = isExpanded(index);
                bool strong = false;
                bool weak   = false;

                switch (node->selection) {
                case SelectionState::Recursive:
                    strong = true;
                    break;
                case SelectionState::Individual:
                    if (expanded)
                        strong = true;
                    else
                        weak = true;
                    break;
                case SelectionState::None:
                    if (!expanded && node->hasSelectedDescendant)
                        weak = true;
                    break;
                }

                if (strong || weak) {
                    QColor bg = strong
                        ? option.palette.color(QPalette::Highlight)
                        : option.palette.color(QPalette::Midlight);
                    painter->fillRect(
                        QRect(0, option.rect.y(), viewport()->width(), option.rect.height()), bg);
                }
            }
        }
        QTreeView::drawRow(painter, option, index);

        if (!contextMenuPath.isEmpty() && fsModel && index.isValid()) {
            if (fsModel->filePath(index) == contextMenuPath) {
                painter->save();
                QRect r(0, option.rect.y(),
                        viewport()->width(), option.rect.height() - 1);
                const QVector<qreal> dash{4, 4};
                QPen p1(option.palette.color(QPalette::Window));
                p1.setDashPattern(dash);
                p1.setDashOffset(0);
                painter->setPen(p1);
                painter->drawRect(r);
                QPen p2(option.palette.color(QPalette::Highlight));
                p2.setDashPattern(dash);
                p2.setDashOffset(4);
                painter->setPen(p2);
                painter->drawRect(r);
                painter->restore();
            }
        }
    }
};


// ---------------------------------------------------------------------------
// PartialSelectionDelegate — adjusts text/icon rendering for selection states
// ---------------------------------------------------------------------------

class PartialSelectionDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QTreeView*        tree    = nullptr;
    QFileSystemModel* fsModel = nullptr;
    DirTree*          dirTree = nullptr;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        if (!tree || !fsModel || !dirTree || !index.isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QString path = fsModel->filePath(index);
        DirNode* node = dirTree->get(path);
        if (!node) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        bool expanded = tree->isExpanded(index);
        bool strong = false;
        bool weak   = false;

        switch (node->selection) {
        case SelectionState::Recursive:
            strong = true;
            break;
        case SelectionState::Individual:
            strong = expanded;
            weak   = !expanded;
            break;
        case SelectionState::None:
            weak = !expanded && node->hasSelectedDescendant;
            break;
        }

        if (!strong && !weak) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt = option;
        if (strong) {
            opt.state |= QStyle::State_Selected;
        } else {
            QColor bg           = opt.palette.color(QPalette::Midlight);
            QColor textNormal   = opt.palette.color(QPalette::Text);
            QColor textHighlight = opt.palette.color(QPalette::HighlightedText);
            qreal cNormal    = qAbs(bg.lightnessF() - textNormal.lightnessF());
            qreal cHighlight = qAbs(bg.lightnessF() - textHighlight.lightnessF());
            opt.palette.setColor(QPalette::Text,
                cNormal >= cHighlight ? textNormal : textHighlight);
        }
        QStyledItemDelegate::paint(painter, opt, index);
    }
};


// ---------------------------------------------------------------------------
// DirectoryPanel
// ---------------------------------------------------------------------------

DirectoryPanel::DirectoryPanel(QWidget* parent) : QWidget(parent)
{
    m_dirTree = new DirTree(this);

    m_fsModel = new FileSystemModel(this);
    m_fsModel->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);

    connect(m_fsModel, &QFileSystemModel::directoryLoaded,
            this, &DirectoryPanel::onDirectoryLoaded);

    auto* treeView = new DirectoryTreeView(this);
    treeView->fsModel = m_fsModel;
    treeView->dirTree = m_dirTree;
    m_tree = treeView;

    auto* delegate = new PartialSelectionDelegate(this);
    delegate->tree    = m_tree;
    delegate->fsModel = m_fsModel;
    delegate->dirTree = m_dirTree;
    m_tree->setItemDelegate(delegate);
    m_tree->setModel(m_fsModel);
    m_tree->setHeaderHidden(true);
    m_tree->setAnimated(false);
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    m_tree->setFocusPolicy(Qt::NoFocus);
    m_tree->sortByColumn(0, Qt::AscendingOrder);

    // Accept file drops onto directories (drop indicator + auto-expand come
    // from QTreeView). The model only reports the drop via filesDroppedOnDir;
    // the actual copy/move is handled upstream (MainWindow).
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setDragDropMode(QAbstractItemView::DropOnly);
    connect(m_fsModel, &FileSystemModel::filesDroppedOnDir,
            this, &DirectoryPanel::filesDroppedOnDir);

    for (int i = 1; i < m_fsModel->columnCount(); ++i)
        m_tree->hideColumn(i);

    m_toolBar = new QToolBar(this);
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    const int sz = style()->pixelMetric(QStyle::PM_SmallIconSize);
    m_toolBar->setIconSize(QSize(sz, sz));
    const int tbm = style()->pixelMetric(QStyle::PM_ToolBarFrameWidth);
    m_toolBar->layout()->setContentsMargins(2, tbm, 2, tbm);

    m_upAction = m_toolBar->addAction(
        style()->standardIcon(QStyle::SP_ArrowUp), tr("Go to parent directory"));
    connect(m_upAction, &QAction::triggered, this, &DirectoryPanel::goUp);

    m_downAction = m_toolBar->addAction(
        style()->standardIcon(QStyle::SP_ArrowDown), tr("Go to child directory"));
    connect(m_downAction, &QAction::triggered, this, &DirectoryPanel::goDown);

    QAction* homeAction = m_toolBar->addAction(
        style()->standardIcon(QStyle::SP_DirHomeIcon), tr("Go to home directory"));
    connect(homeAction, &QAction::triggered, this, &DirectoryPanel::goHome);

    m_deviceMenu = new QMenu(this);
    connect(m_deviceMenu, &QMenu::aboutToShow, this, &DirectoryPanel::populateDeviceMenu);

    QAction* deviceAction = m_toolBar->addAction(
        style()->standardIcon(QStyle::SP_DriveHDIcon), tr("Show device list"));
    if (auto* btn = qobject_cast<QToolButton*>(m_toolBar->widgetForAction(deviceAction))) {
        btn->setMenu(m_deviceMenu);
        btn->setPopupMode(QToolButton::InstantPopup);
    }

    QWidget* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(spacer);

    QAction* closeAction = m_toolBar->addAction(
        style()->standardIcon(QStyle::SP_TitleBarCloseButton), tr("Close panel"));
    connect(closeAction, &QAction::triggered, this, &DirectoryPanel::onClose);

    auto* anchorLabel = new AnchorLabel(this);
    QFont labelFont = anchorLabel->font();
    labelFont.setPointSizeF(labelFont.pointSizeF() * 0.75);
    anchorLabel->setFont(labelFont);
    anchorLabel->setContextMenuPolicy(Qt::CustomContextMenu);
    anchorLabel->setToolTip(tr("Anchor path — root of the visible tree"));
    m_anchorLabel = anchorLabel;
    connect(anchorLabel, &QWidget::customContextMenuRequested,
            this, &DirectoryPanel::showAnchorContextMenu);

    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_toolBar);
    layout->addWidget(separator);
    layout->addWidget(m_anchorLabel);
    layout->addWidget(m_tree, 1);

    m_tree->viewport()->installEventFilter(this);
    m_tree->setContextMenuPolicy(Qt::PreventContextMenu);

    connect(m_tree, &QTreeView::clicked,       this, &DirectoryPanel::onClicked);
    connect(m_tree, &QTreeView::doubleClicked, this, &DirectoryPanel::onDoubleClicked);

    connect(ExternalDeviceManager::instance(), &ExternalDeviceManager::mountChanged,
            this, [this](const QString& mountPath, bool mounted) {
        if (!mounted)
            clearIfUnderPath(mountPath);
    });

    connect(m_tree, &QTreeView::collapsed, this, [this](const QModelIndex&) {
        m_tree->viewport()->update();
    });
    connect(m_tree, &QTreeView::expanded, this, [this](const QModelIndex& idx) {
        if (idx.isValid()) {
            QString path = m_fsModel->filePath(idx);
            if (!path.isEmpty()) {
                DirNode* node = m_dirTree->getOrCreate(path);
                m_dirTree->loadChildren(node);
            }
        }
        m_tree->viewport()->update();
    });

    connect(m_dirTree, &DirTree::nodeChanged, this, [this]() {
        m_tree->viewport()->update();
    });
}

void DirectoryPanel::clearAllSelections()
{
    m_dirTree->clearAllSelections();
}

QStringList DirectoryPanel::selectedDirectories() const
{
    QStringList result;
    for (auto* node : m_dirTree->nodeByPath)
        if (node->selection != SelectionState::None)
            result.append(node->path);
    return result;
}

void DirectoryPanel::emitSelection()
{
    QStringList recursive, individual;
    m_dirTree->collectSelectionRoots(recursive, individual);
    emit selectedDirectoriesChanged(recursive, individual);
}

void DirectoryPanel::setCurrentDirectory(const QString& path)
{
    if (path.isEmpty()) {
        clearAllSelections();
        m_lastActivePath.clear();
        setTreeAnchor(QString());
        m_fsModel->setRootPath(QString());
        m_tree->setRootIndex(QModelIndex());
        m_tree->viewport()->update();
        emit selectedDirectoriesChanged({}, {});
        return;
    }

    QString root = findRootForPath(path);
    if (!root.isEmpty())
        m_rootMemory[root] = path;

    clearAllSelections();
    DirNode* node = m_dirTree->ensurePath(path);
    m_dirTree->setSelection(node, SelectionState::Recursive);
    m_lastActivePath = path;

    if (!m_treeAnchor.isEmpty()) {
        bool underAnchor = Path::isSelfOrUnder(path, m_treeAnchor);
        if (!underAnchor)
            resetTreeAnchor();
    }

    if (m_treeAnchor.isEmpty())
        initializeTreeAnchor();
    else
        updateTreeDisplay();

    m_tree->viewport()->update();
    emitSelection();
    updateButtonStates();
}

void DirectoryPanel::restoreTreeAnchor(const QString& anchor)
{
    if (anchor.isEmpty() || !QDir(anchor).exists())
        return;

    setTreeAnchor(anchor);
    m_fsModel->setRootPath(anchor);
    m_tree->setRootIndex(m_fsModel->index(anchor));
}

void DirectoryPanel::setSelectedDirectories(const QStringList& recursive,
                                             const QStringList& individual)
{
    QStringList validRecursive, validIndividual;
    for (const QString& d : recursive)
        if (QDir(d).exists()) validRecursive.append(d);
    for (const QString& d : individual)
        if (QDir(d).exists()) validIndividual.append(d);

    if (validRecursive.isEmpty() && validIndividual.isEmpty()) {
        setCurrentDirectory(QString());
        return;
    }

    QStringList all = validRecursive + validIndividual;
    QString lastPath = all.last();

    blockSignals(true);
    setCurrentDirectory(lastPath);
    blockSignals(false);

    // initializeTreeAnchor uses only lastPath. If other selected paths lie outside
    // the anchor, walk up until the anchor covers all of them.
    for (const QString& d : all) {
        while (!m_treeAnchor.isEmpty() && !Path::isSelfOrUnder(d, m_treeAnchor)) {
            const QString parent = Path::parentOf(m_treeAnchor);
            if (parent.isEmpty()) break;  // already at the filesystem root
            m_dirTree->ensurePath(parent);
            setTreeAnchor(parent);
            m_fsModel->setRootPath(parent);
            m_tree->setRootIndex(m_fsModel->index(parent));
        }
    }

    clearAllSelections();
    for (const QString& d : validRecursive) {
        DirNode* n = m_dirTree->ensurePath(d);
        m_dirTree->setSelection(n, SelectionState::Recursive);
    }
    for (const QString& d : validIndividual) {
        DirNode* n = m_dirTree->ensurePath(d);
        m_dirTree->setSelection(n, SelectionState::Individual);
    }
    m_lastActivePath = lastPath;

    // Individual nodes: expand self + ancestors (to show mixed child state).
    for (const QString& dir : validIndividual) {
        QString path = dir;
        while (!path.isEmpty() && !Path::equal(path, m_treeAnchor)) {
            const QString parent = Path::parentOf(path);
            if (parent.isEmpty()) break;
            if (!Path::equal(parent, m_treeAnchor) && !m_pendingExpandPaths.contains(parent))
                m_pendingExpandPaths.append(parent);
            path = parent;
        }
        if (!m_pendingExpandPaths.contains(dir))
            m_pendingExpandPaths.append(dir);
    }

    // Recursive roots: expand ancestors only (not self) so they appear as visible
    // collapsed blue rows. For roots whose parent is already an Individual node the
    // Individual loop above already added that parent — the contains-check makes those
    // iterations no-ops.
    for (const QString& dir : validRecursive) {
        QString path = Path::parentOf(dir); // parent of dir, not dir itself
        while (!path.isEmpty() && !Path::equal(path, m_treeAnchor)) {
            if (!m_pendingExpandPaths.contains(path))
                m_pendingExpandPaths.append(path);
            path = Path::parentOf(path);
        }
    }

    applyExpandAndSelect();
    m_tree->viewport()->update();
    emitSelection();
    updateButtonStates();
}

void DirectoryPanel::initializeTreeAnchor()
{
    QString last = currentDirectory();
    if (last.isEmpty())
        return;

    QString anchor = Path::parentOf(last);

    setTreeAnchor(anchor);
    m_fsModel->setRootPath(anchor);
    m_tree->setRootIndex(m_fsModel->index(anchor));

    updateTreeDisplay();
}

void DirectoryPanel::updateTreeDisplay()
{
    QString last = currentDirectory();
    if (last.isEmpty() || m_treeAnchor.isEmpty())
        return;

    m_pendingExpandPaths.clear();
    m_pendingSelectPath = last;

    QString path = last;
    while (!path.isEmpty() && !Path::equal(path, m_treeAnchor)) {
        const QString parent = Path::parentOf(path);
        if (parent.isEmpty()) break;
        if (!Path::equal(parent, m_treeAnchor))
            m_pendingExpandPaths.prepend(parent);
        path = parent;
    }

    applyExpandAndSelect();
    updateButtonStates();
}

QString DirectoryPanel::findRootForPath(const QString& path)
{
    if (path.isEmpty())
        return QString();

    QString homePath = QDir::homePath();
    if (Path::isSelfOrUnder(path, homePath))
        return homePath;

    QStringList mountPaths = ExternalDeviceManager::allMountPaths();
    for (const QString& mountPath : mountPaths) {
        if (Path::isSelfOrUnder(path, mountPath))
            return mountPath;
    }

    // On Windows the drives themselves are navigable roots — there is no
    // single "/" every path hangs off. On Linux this stays as it was: a path
    // outside home and outside any mount has no root.
    if (Path::flavor() == Path::Flavor::Windows) {
        for (const QString& r : Path::roots())
            if (Path::isSelfOrUnder(path, r))
                return r;
    }
    return QString();
}

void DirectoryPanel::onClicked(const QModelIndex& idx)
{
    if (!idx.isValid() || m_ignoreClick)
        return;

    QString path = m_fsModel->filePath(idx);
    if (path.isEmpty())
        return;

    bool ctrlHeld  = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
    bool shiftHeld = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
    DirNode* node = m_dirTree->getOrCreate(path);
    bool expanded = m_tree->isExpanded(idx);

    if (shiftHeld && selectRangeTo(idx)) {
        m_lastActivePath = path;
    } else if (ctrlHeld) {
        if (expanded) {
            // Deselect if selected; otherwise select as Individual (only this dir)
            SelectionState target = (node->selection == SelectionState::None)
                ? SelectionState::Individual : SelectionState::None;
            m_dirTree->setSelection(node, target);
        } else {
            if (node->selection != SelectionState::None)
                m_dirTree->clearSubtreeSelection(node);
            else
                m_dirTree->setSelection(node, SelectionState::Recursive);
        }
    } else {
        QStringList curRecursive, curIndividual;
        m_dirTree->collectSelectionRoots(curRecursive, curIndividual);
        bool wasOnlyRecursive = (curRecursive.size() == 1
                                  && curRecursive.first() == path
                                  && curIndividual.isEmpty());
        clearAllSelections();
        if (!wasOnlyRecursive) {
            m_dirTree->setSelection(node, SelectionState::Recursive);
            m_lastActivePath = path;
        } else {
            m_lastActivePath.clear();
        }
    }

    m_tree->viewport()->update();
    emitSelection();
    if (!m_treeAnchor.isEmpty() && !m_lastActivePath.isEmpty())
        m_rootMemory[m_treeAnchor] = m_lastActivePath;
    updateButtonStates();
}

bool DirectoryPanel::selectRangeTo(const QModelIndex& targetIdx)
{
    QModelIndex target = targetIdx.siblingAtColumn(0);

    // Anchor = the visible selected row closest to the target.
    QStringList recursive, individual;
    m_dirTree->collectSelectionRoots(recursive, individual);

    const int targetY = m_tree->visualRect(target).top();
    QModelIndex anchor;
    int bestDist = -1;
    for (const QString& selPath : recursive + individual) {
        QModelIndex selIdx = m_fsModel->index(selPath);
        if (!selIdx.isValid())
            continue;
        QRect r = m_tree->visualRect(selIdx);
        if (!r.isValid())
            continue; // row not visible (collapsed ancestor or outside anchor)
        int dist = qAbs(r.top() - targetY);
        if (bestDist < 0 || dist < bestDist) {
            bestDist = dist;
            anchor = selIdx;
        }
    }
    if (!anchor.isValid())
        return false; // no visible anchor — caller falls back to normal click
    if (anchor == target)
        return true;  // degenerate range, nothing to do

    QModelIndex from = anchor;
    QModelIndex to   = target;
    if (m_tree->visualRect(from).top() > m_tree->visualRect(to).top())
        std::swap(from, to);

    // Top-down walk over the visible rows of the range. Expanded rows become
    // Individual — their visible children are range rows themselves, so only
    // the part inside the range gets selected; if all children end up
    // Recursive, checkPromoteToRecursive promotes the parent anyway.
    for (QModelIndex cur = from; cur.isValid(); cur = m_tree->indexBelow(cur)) {
        QString p = m_fsModel->filePath(cur);
        if (!p.isEmpty()) {
            DirNode* n = m_dirTree->getOrCreate(p);
            if (n->selection == SelectionState::None) {
                SelectionState st = m_tree->isExpanded(cur)
                    ? SelectionState::Individual : SelectionState::Recursive;
                m_dirTree->setSelection(n, st);
            }
        }
        if (cur == to)
            break;
    }
    return true;
}

void DirectoryPanel::onDoubleClicked(const QModelIndex& idx)
{
    if (!idx.isValid())
        return;

    // "Selection leads" (2026-07, final after trying "always end selected",
    // "pure expander toggle via deferred single clicks" — the deferral made
    // every single click feel sluggish; do not reintroduce it): only the
    // FIRST click of the pair fires clicked (verified), and its plain-click
    // semantics — select exclusively / toggle off — already produced the
    // final selection state. The double click extends it structurally:
    // expansion follows the selection — selected ⇒ expanded, deselected ⇒
    // collapsed. Net: double-click on unselected selects and opens; on
    // selected it deselects and closes. expandsOnDoubleClick stays false
    // (Qt's built-in toggle would fight this).
    const bool modifierHeld = QGuiApplication::keyboardModifiers()
                              & (Qt::ControlModifier | Qt::ShiftModifier);
    if (modifierHeld) {
        // Modifier clicks keep their own selection semantics; the double
        // click just toggles the expander.
        m_tree->setExpanded(idx, !m_tree->isExpanded(idx));
        return;
    }

    QString path = m_fsModel->filePath(idx);
    if (path.isEmpty())
        return;
    DirNode* node = m_dirTree->get(path);
    const bool selected = node && node->selection != SelectionState::None;
    m_tree->setExpanded(idx, selected);
}

void DirectoryPanel::goUp()
{
    if (m_treeAnchor.isEmpty()) return;

    QString parentPath = Path::parentOf(m_treeAnchor);

    QString oldAnchor = m_treeAnchor;

    if (!parentPath.isEmpty()) {
        m_dirTree->ensurePath(parentPath);
        // The new parent node starts with hasSelectedDescendant = false.
        // Walk up from the old anchor to propagate the flag into the new node.
        DirNode* oldAnchorNode = m_dirTree->get(oldAnchor);
        if (oldAnchorNode)
            m_dirTree->updateAncestorFlags(oldAnchorNode);
    }

    QStringList expandedPaths;
    std::function<void(const QModelIndex&)> collectExpanded = [&](const QModelIndex& parent) {
        for (int i = 0; i < m_fsModel->rowCount(parent); ++i) {
            QModelIndex child = m_fsModel->index(i, 0, parent);
            if (m_tree->isExpanded(child)) {
                expandedPaths.append(m_fsModel->filePath(child));
                collectExpanded(child);
            }
        }
    };
    collectExpanded(QModelIndex());

    setTreeAnchor(parentPath);
    m_fsModel->setRootPath(parentPath);
    m_tree->setRootIndex(m_fsModel->index(parentPath));

    for (const QString& path : expandedPaths) {
        QModelIndex eidx = m_fsModel->index(path);
        if (eidx.isValid())
            m_tree->expand(eidx);
    }

    QModelIndex oldAnchorIdx = m_fsModel->index(oldAnchor);
    if (oldAnchorIdx.isValid())
        m_tree->expand(oldAnchorIdx);

    m_tree->viewport()->update();
    updateButtonStates();
}

// The single step down all selections agree on, or QString() if going down is
// not possible. goDown() and updateButtonStates() must answer this question
// identically — keeping it in one place is what stops them from drifting apart.
QString DirectoryPanel::commonNextChild() const
{
    QStringList curRecursive, curIndividual;
    m_dirTree->collectSelectionRoots(curRecursive, curIndividual);
    const QStringList allSelected = curRecursive + curIndividual;
    if (allSelected.isEmpty())
        return QString();

    QString commonChild;
    for (const QString& root : allSelected) {
        // With an empty anchor the view sits above the root(s), so the next
        // step down is the root the selection lives on. On Windows selections
        // can sit on different drives — then there is no common step.
        const QString nextChild = m_treeAnchor.isEmpty()
            ? Path::rootOf(root)
            : Path::nextChildInChain(m_treeAnchor, root);
        if (nextChild.isEmpty())
            continue;
        if (commonChild.isEmpty())
            commonChild = nextChild;
        else if (!Path::equal(commonChild, nextChild))
            return QString();
    }

    // Stop if the selection is already a direct child of the anchor — moving
    // further would make it the root and remove it from view.
    if (allSelected.contains(commonChild))
        return QString();
    return commonChild;
}

void DirectoryPanel::goDown()
{
    const QString commonChild = commonNextChild();
    if (commonChild.isEmpty()) return;

    DirNode* anchorNode = m_dirTree->getOrCreate(commonChild);

    // Transfer Recursive from ancestors about to be evicted
    for (DirNode* p = anchorNode->parent; p; p = p->parent) {
        if (p->selection == SelectionState::Recursive) {
            m_dirTree->setSelection(anchorNode, SelectionState::Recursive);
            break;
        }
    }

    m_dirTree->evictOutside(commonChild);

    QStringList expandedPaths;
    std::function<void(const QModelIndex&)> collectExpanded = [&](const QModelIndex& parent) {
        for (int i = 0; i < m_fsModel->rowCount(parent); ++i) {
            QModelIndex child = m_fsModel->index(i, 0, parent);
            if (m_tree->isExpanded(child)) {
                expandedPaths.append(m_fsModel->filePath(child));
                collectExpanded(child);
            }
        }
    };
    collectExpanded(QModelIndex());

    setTreeAnchor(commonChild);
    m_fsModel->setRootPath(commonChild);
    m_tree->setRootIndex(m_fsModel->index(commonChild));

    for (const QString& path : expandedPaths) {
        QModelIndex eidx = m_fsModel->index(path);
        if (eidx.isValid())
            m_tree->expand(eidx);
    }

    m_tree->viewport()->update();
    updateButtonStates();
}

void DirectoryPanel::onClose()
{
    emit hideRequested();
}

void DirectoryPanel::goHome()
{
    navigateToRoot(QDir::homePath());
}

void DirectoryPanel::populateDeviceMenu()
{
    m_deviceMenu->clear();

    QList<ExternalDevice> devices = ExternalDeviceManager::allDevices();

    if (devices.isEmpty()) {
        QAction* emptyAction = m_deviceMenu->addAction(tr("No external devices found"));
        emptyAction->setEnabled(false);
    } else {
        for (const ExternalDevice& device : devices) {
            QStyle::StandardPixmap iconType = device.mountPath.isEmpty()
                ? QStyle::SP_DriveHDIcon : QStyle::SP_DirIcon;

            QAction* action = m_deviceMenu->addAction(
                style()->standardIcon(iconType), device.displayName());

            QString deviceName = device.deviceName;
            QString mountPath  = device.mountPath;

            if (mountPath.isEmpty()) {
                connect(action, &QAction::triggered, this, [this, deviceName]() {
                    QString mountPoint = mountDevice(deviceName);
                    if (!mountPoint.isEmpty())
                        navigateToRoot(mountPoint);
                });
            } else {
                connect(action, &QAction::triggered, this, [this, mountPath]() {
                    navigateToRoot(mountPath);
                });
            }
        }

        bool hasMounted = false;
        for (const ExternalDevice& device : devices) {
            if (!device.mountPath.isEmpty()) {
                if (!hasMounted) {
                    m_deviceMenu->addSeparator();
                    hasMounted = true;
                }
                QString deviceName = device.deviceName;
                QString mountPath  = device.mountPath;
                QAction* unmountAction = m_deviceMenu->addAction(
                    tr("Unmount %1").arg(device.displayName()));
                connect(unmountAction, &QAction::triggered, this, [this, deviceName, mountPath]() {
                    unmountDevice(deviceName);
                    clearIfUnderPath(mountPath);
                });
            }
        }
    }
}

void DirectoryPanel::resetTreeAnchor()
{
    m_treeAnchor.clear();
    m_fsModel->setRootPath(QString());
    m_tree->setRootIndex(QModelIndex());
    updateButtonStates();
}

void DirectoryPanel::setTreeAnchor(const QString& newAnchor)
{
    if (m_treeAnchor != newAnchor) {
        m_treeAnchor = newAnchor;
        emit treeAnchorChanged(newAnchor);
    }
    if (m_anchorLabel)
        static_cast<AnchorLabel*>(m_anchorLabel)->setText(newAnchor);
}

QString DirectoryPanel::mountDevice(const QString& devicePath)
{
    QString error;
    const QString mountPoint = ExternalDeviceManager::mount(devicePath, &error);
    if (mountPoint.isEmpty())
        QMessageBox::warning(this, tr("Mount Error"),
            tr("Failed to mount %1:\n%2").arg(devicePath, error));
    return mountPoint;
}

void DirectoryPanel::unmountDevice(const QString& devicePath)
{
    QString error;
    if (!ExternalDeviceManager::unmount(devicePath, &error))
        QMessageBox::warning(this, tr("Unmount Error"),
            tr("Failed to unmount %1:\n%2").arg(devicePath, error));
}

void DirectoryPanel::clearIfUnderPath(const QString& path)
{
    if (path.isEmpty())
        return;

    QStringList curRecursive, curIndividual;
    m_dirTree->collectSelectionRoots(curRecursive, curIndividual);
    QStringList allSelected = curRecursive + curIndividual;

    bool selectionAffected = false;
    for (const QString& r : allSelected) {
        if (Path::isSelfOrUnder(r, path)) {
            selectionAffected = true;
            break;
        }
    }

    bool anchorAffected = !m_treeAnchor.isEmpty() && Path::isSelfOrUnder(m_treeAnchor, path);

    if (!selectionAffected && !anchorAffected)
        return;

    m_dirTree->evictPath(path);

    if (anchorAffected)
        m_treeAnchor.clear();

    bool hadSelection = selectionAffected;
    clearAllSelections();
    m_lastActivePath.clear();
    m_tree->viewport()->update();
    if (hadSelection)
        emit selectedDirectoriesChanged({}, {});

    navigateToRoot(QDir::homePath());
}

static QString shortenPath(const QString& path)
{
    return Path::display(path);
}

void DirectoryPanel::showAnchorContextMenu(const QPoint& pos)
{
    QMenu menu(m_anchorLabel);

    QAction* copyAction = menu.addAction(tr("Copy anchor path"));
    copyAction->setEnabled(!m_treeAnchor.isEmpty());
    connect(copyAction, &QAction::triggered, this, [this]() {
        QGuiApplication::clipboard()->setText(m_treeAnchor);
    });

    menu.addSeparator();

    QString clipText = QGuiApplication::clipboard()->text().trimmed();
    if (clipText.startsWith('~'))
        clipText.replace(0, 1, QDir::homePath());
    QFileInfo clipInfo(clipText);
    bool clipValid = clipInfo.exists() && clipInfo.isDir();
    QString clipAbsPath = clipValid ? clipInfo.absoluteFilePath() : QString();
    QString clipDirName = clipValid ? clipInfo.fileName() : QString();

    // "Add clipboard folder to selection"
    {
        QString addLabel = clipValid
            ? tr("Add \"%1\" to selection").arg(shortenPath(clipAbsPath))
            : tr("Add clipboard folder to selection");
        QAction* addAction = menu.addAction(addLabel);
        addAction->setEnabled(clipValid);
        if (clipValid) {
            connect(addAction, &QAction::triggered, this, [this, clipAbsPath]() {
                // Collect currently expanded paths using the view's root index
                // (not QModelIndex() which is the invisible model root).
                QStringList expandedPaths;
                std::function<void(const QModelIndex&)> collectExpanded =
                    [&](const QModelIndex& parent) {
                    for (int i = 0; i < m_fsModel->rowCount(parent); ++i) {
                        QModelIndex child = m_fsModel->index(i, 0, parent);
                        if (m_tree->isExpanded(child)) {
                            expandedPaths.append(m_fsModel->filePath(child));
                            collectExpanded(child);
                        }
                    }
                };
                collectExpanded(m_tree->rootIndex());

                // Walk anchor up until the new path is strictly inside it.
                // Also triggers when clipAbsPath == m_treeAnchor so the folder
                // becomes a visible child row.
                QString anchorBeforeMove = m_treeAnchor;
                while (!m_treeAnchor.isEmpty() &&
                       !Path::isUnder(clipAbsPath, m_treeAnchor)) {
                    const QString parent = Path::parentOf(m_treeAnchor);
                    if (parent.isEmpty()) break;
                    m_dirTree->ensurePath(parent);
                    DirNode* oldAnchorNode = m_dirTree->get(m_treeAnchor);
                    if (oldAnchorNode)
                        m_dirTree->updateAncestorFlags(oldAnchorNode);
                    setTreeAnchor(parent);
                    m_fsModel->setRootPath(parent);
                    m_tree->setRootIndex(m_fsModel->index(parent));
                }

                // Re-expand what was expanded before
                for (const QString& p : expandedPaths) {
                    QModelIndex eidx = m_fsModel->index(p);
                    if (eidx.isValid())
                        m_tree->expand(eidx);
                }

                // Directly expand the old anchor (now a visible child) like goUp does.
                // Don't use pendingExpandPaths here — directoryLoaded may have already
                // fired synchronously during setRootPath above (cached directory).
                if (anchorBeforeMove != m_treeAnchor && !anchorBeforeMove.isEmpty()) {
                    QModelIndex eidx = m_fsModel->index(anchorBeforeMove);
                    if (eidx.isValid()) {
                        m_tree->expand(eidx);
                    } else {
                        // Not yet in model cache — fall back to pending
                        if (!m_pendingExpandPaths.contains(anchorBeforeMove))
                            m_pendingExpandPaths.prepend(anchorBeforeMove);
                    }
                }

                // Add the selection
                DirNode* n = m_dirTree->ensurePath(clipAbsPath);
                m_dirTree->setSelection(n, SelectionState::Recursive);

                // Scroll to the newly selected folder
                m_pendingSelectPath = clipAbsPath;
                applyExpandAndSelect();

                m_tree->viewport()->update();
                emitSelection();
                updateButtonStates();
            });
        }
    }

    // "Paste as anchor" — only if all current selections lie under the clip path
    bool selectionCompatible = false;
    if (clipValid) {
        QStringList curRecursive, curIndividual;
        m_dirTree->collectSelectionRoots(curRecursive, curIndividual);
        QStringList allSelected = curRecursive + curIndividual;
        selectionCompatible = true;
        for (const QString& sel : allSelected) {
            if (!Path::isUnder(sel, clipAbsPath)) {
                selectionCompatible = false;
                break;
            }
        }
    }

    {
        QString pasteLabel = clipValid
            ? tr("Set \"%1\" as anchor").arg(shortenPath(clipAbsPath))
            : tr("Set clipboard folder as anchor");
        QAction* pasteAction = menu.addAction(pasteLabel);
        pasteAction->setEnabled(clipValid && selectionCompatible);

        if (clipValid && selectionCompatible) {
            connect(pasteAction, &QAction::triggered, this, [this, clipAbsPath]() {
                m_dirTree->ensurePath(clipAbsPath);
                setTreeAnchor(clipAbsPath);
                m_fsModel->setRootPath(clipAbsPath);
                m_tree->setRootIndex(m_fsModel->index(clipAbsPath));

                m_pendingExpandPaths.clear();
                m_pendingSelectPath.clear();
                if (!m_lastActivePath.isEmpty()) {
                    m_pendingSelectPath = m_lastActivePath;
                    QString path = m_lastActivePath;
                    while (!path.isEmpty() && !Path::equal(path, clipAbsPath)) {
                        const QString parent = Path::parentOf(path);
                        if (parent.isEmpty()) break;
                        if (!Path::equal(parent, clipAbsPath)
                                && !m_pendingExpandPaths.contains(parent))
                            m_pendingExpandPaths.prepend(parent);
                        path = parent;
                    }
                }
                applyExpandAndSelect();
                m_tree->viewport()->update();
                updateButtonStates();
            });
        }
    }

    menu.exec(m_anchorLabel->mapToGlobal(pos));
}

void DirectoryPanel::showTreeContextMenu(const QPoint& pos)
{
    QModelIndex idx = m_tree->indexAt(pos);
    QString itemPath = idx.isValid() ? m_fsModel->filePath(idx) : QString();

    auto* dtv = static_cast<DirectoryTreeView*>(m_tree);
    if (idx.isValid()) {
        dtv->contextMenuPath = itemPath;
        m_tree->viewport()->update();
    }

    QMenu menu(m_tree);

    if (!itemPath.isEmpty()) {
        DirNode* node = m_dirTree->get(itemPath);
        bool selfSelected = node && node->selection != SelectionState::None;
        bool fullySelected = node && node->selection == SelectionState::Recursive;
        bool hasSelDesc = node && node->hasSelectedDescendant;

        if (!fullySelected) {
            QAction* actSelectRecursive = menu.addAction(tr("Select with subfolders"));
            connect(actSelectRecursive, &QAction::triggered, this, [this, itemPath]() {
                DirNode* n = m_dirTree->getOrCreate(itemPath);
                m_dirTree->setSelection(n, SelectionState::Recursive);
                m_tree->viewport()->update();
                emitSelection();
                updateButtonStates();
            });
        }

        {
            QAction* actSelect = menu.addAction(tr("Select without subfolders"));
            connect(actSelect, &QAction::triggered, this, [this, itemPath]() {
                DirNode* n = m_dirTree->getOrCreate(itemPath);
                m_dirTree->clearDescendantSelection(n);
                m_dirTree->setSelection(n, SelectionState::Individual);
                m_tree->viewport()->update();
                emitSelection();
                updateButtonStates();
            });
        }

        if (selfSelected || hasSelDesc) {
            QAction* actDeselectRecursive = menu.addAction(tr("Deselect with subfolders"));
            connect(actDeselectRecursive, &QAction::triggered, this, [this, itemPath]() {
                DirNode* n = m_dirTree->get(itemPath);
                if (n)
                    m_dirTree->clearSubtreeSelection(n);
                m_tree->viewport()->update();
                emitSelection();
                updateButtonStates();
            });
        }

        if (selfSelected) {
            QAction* actDeselect = menu.addAction(tr("Deselect without subfolders"));
            connect(actDeselect, &QAction::triggered, this, [this, itemPath]() {
                DirNode* n = m_dirTree->get(itemPath);
                if (n)
                    m_dirTree->setSelection(n, SelectionState::None);
                m_tree->viewport()->update();
                emitSelection();
                updateButtonStates();
            });
        }

        menu.addSeparator();

        QAction* actOpenNew = menu.addAction(tr("Open in New Window"));
        connect(actOpenNew, &QAction::triggered, this, [this, itemPath]() {
            emit openInNewWindowRequested(itemPath);
        });

        QAction* actNewDir = menu.addAction(tr("New Subdirectory…"));
        connect(actNewDir, &QAction::triggered, this, [this, itemPath]() {
            bool ok = false;
            const QString name = QInputDialog::getText(this,
                tr("New Subdirectory"),
                tr("Name of the new folder in\n%1:").arg(itemPath),
                QLineEdit::Normal, QString(), &ok).trimmed();
            if (!ok || name.isEmpty())
                return;
            if (!QDir(itemPath).mkdir(name)) {
                QMessageBox::warning(this, tr("New Subdirectory"),
                    tr("Could not create \"%1\".").arg(name));
                return;
            }
            // QFileSystemModel picks the new dir up automatically; just
            // make it visible
            const QModelIndex parentIdx = m_fsModel->index(itemPath);
            if (parentIdx.isValid())
                m_tree->expand(parentIdx);
        });

        // Whole directory trees end up in the trash — confirm, and never
        // offer it for the home directory
        if (!Path::equal(itemPath, QDir::homePath())) {
            QAction* actTrashDir = menu.addAction(tr("Move to Trash…"));
            connect(actTrashDir, &QAction::triggered, this, [this, itemPath]() {
                const auto answer = QMessageBox::warning(this,
                    tr("Move to Trash"),
                    tr("Move this folder and everything in it to the trash?\n\n%1")
                        .arg(itemPath),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
                if (answer != QMessageBox::Yes)
                    return;
                if (!QFile::moveToTrash(itemPath)) {
                    QMessageBox::warning(this, tr("Move to Trash"),
                        tr("Could not move \"%1\" to the trash.").arg(itemPath));
                    return;
                }
                // Drop the subtree from the selection tree and re-announce
                // the selection so the media list updates
                m_dirTree->evictPath(itemPath);
                m_tree->viewport()->update();
                emitSelection();
                updateButtonStates();
            });
        }

        menu.addSeparator();

        QModelIndex menuIdx = m_tree->indexAt(pos);
        bool hasCh = menuIdx.isValid() && m_fsModel->hasChildren(menuIdx);
        if (hasCh) {
            QAction* actExpandChildren = menu.addAction(tr("Expand children"));
            connect(actExpandChildren, &QAction::triggered, this, [this, menuIdx, itemPath]() {
                if (!m_tree->isExpanded(menuIdx))
                    m_tree->expand(menuIdx);
                DirNode* node = m_dirTree->get(itemPath);
                if (node) {
                    for (DirNode* child : node->children) {
                        if (!m_pendingExpandPaths.contains(child->path))
                            m_pendingExpandPaths.append(child->path);
                    }
                    applyExpandAndSelect();
                }
                m_tree->viewport()->update();
            });

            QAction* actCollapseChildren = menu.addAction(tr("Collapse children"));
            connect(actCollapseChildren, &QAction::triggered, this, [this, menuIdx]() {
                std::function<void(const QModelIndex&)> collapseRec =
                    [&](const QModelIndex& idx) {
                    int rows = m_fsModel->rowCount(idx);
                    for (int i = 0; i < rows; ++i) {
                        QModelIndex child = m_fsModel->index(i, 0, idx);
                        if (child.isValid() && m_tree->isExpanded(child))
                            collapseRec(child);
                    }
                    m_tree->collapse(idx);
                };
                int rows = m_fsModel->rowCount(menuIdx);
                for (int i = 0; i < rows; ++i) {
                    QModelIndex child = m_fsModel->index(i, 0, menuIdx);
                    if (child.isValid())
                        collapseRec(child);
                }
                m_tree->viewport()->update();
            });
        }

        if (node && node->hasSelectedDescendant) {
            QAction* actExpandToSel = menu.addAction(tr("Expand to selections"));
            connect(actExpandToSel, &QAction::triggered, this, [this, itemPath]() {
                for (auto* node : m_dirTree->nodeByPath) {
                    if (!Path::isSelfOrUnder(node->path, itemPath))
                        continue;
                    if (node->selection == SelectionState::Individual
                            || node->hasSelectedDescendant) {
                        if (!m_pendingExpandPaths.contains(node->path))
                            m_pendingExpandPaths.append(node->path);
                    }
                }
                applyExpandAndSelect();
                m_tree->viewport()->update();
            });
        }

        // "Set as anchor" — only if all selections lie under this folder
        if (!Path::equal(itemPath, m_treeAnchor)) {
            QStringList curRecursive, curIndividual;
            m_dirTree->collectSelectionRoots(curRecursive, curIndividual);
            QStringList allSelected = curRecursive + curIndividual;
            bool canSetAnchor = true;
            for (const QString& sel : allSelected) {
                // Selection must be strictly under itemPath; if the folder itself
                // is selected, making it the anchor would hide it from view.
                if (!Path::isUnder(sel, itemPath)) {
                    canSetAnchor = false;
                    break;
                }
            }
            if (canSetAnchor) {
                QAction* actSetAnchor = menu.addAction(tr("Set as anchor"));
                connect(actSetAnchor, &QAction::triggered, this, [this, itemPath]() {
                    QStringList expandedPaths;
                    std::function<void(const QModelIndex&)> collectExpanded =
                        [&](const QModelIndex& parent) {
                        for (int i = 0; i < m_fsModel->rowCount(parent); ++i) {
                            QModelIndex child = m_fsModel->index(i, 0, parent);
                            if (m_tree->isExpanded(child)) {
                                expandedPaths.append(m_fsModel->filePath(child));
                                collectExpanded(child);
                            }
                        }
                    };
                    collectExpanded(QModelIndex());

                    setTreeAnchor(itemPath);
                    m_fsModel->setRootPath(itemPath);
                    m_tree->setRootIndex(m_fsModel->index(itemPath));

                    // Re-expand paths still under the new anchor
                    for (const QString& p : expandedPaths) {
                        if (Path::isSelfOrUnder(p, itemPath)) {
                            QModelIndex eidx = m_fsModel->index(p);
                            if (eidx.isValid())
                                m_tree->expand(eidx);
                        }
                    }

                    // Scroll to last active path if it's still under the new anchor
                    m_pendingExpandPaths.clear();
                    m_pendingSelectPath.clear();
                    if (!m_lastActivePath.isEmpty()
                        && Path::isSelfOrUnder(m_lastActivePath, itemPath)) {
                        m_pendingSelectPath = m_lastActivePath;
                    }
                    applyExpandAndSelect();
                    m_tree->viewport()->update();
                    updateButtonStates();
                });
            }
        }

        menu.addSeparator();

        QAction* copyAction = menu.addAction(tr("Copy folder path"));
        connect(copyAction, &QAction::triggered, this, [itemPath]() {
            QGuiApplication::clipboard()->setText(itemPath);
        });
    }

    menu.exec(m_tree->viewport()->mapToGlobal(pos));

    dtv->contextMenuPath.clear();
    m_tree->viewport()->update();
}

bool DirectoryPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_tree->viewport()
        && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::RightButton) {
            showTreeContextMenu(me->pos());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void DirectoryPanel::navigateToPath(const QString& input)
{
    QString path = input.trimmed();
    if (path.isEmpty())
        return;
    if (path.startsWith('~'))
        path.replace(0, 1, QDir::homePath());

    QFileInfo info(path);
    if (!info.exists() || !info.isDir())
        return;

    setCurrentDirectory(info.absoluteFilePath());
}

void DirectoryPanel::navigateToRoot(const QString& root)
{
    if (root.isEmpty())
        return;

    QString last = currentDirectory();
    if (!last.isEmpty()) {
        QString currentRoot = findRootForPath(last);
        if (!currentRoot.isEmpty() && currentRoot != root)
            m_rootMemory[currentRoot] = last;
    }

    QString targetDir;
    {
        const QString candidate = m_rootMemory.value(root);
        if (Path::isSelfOrUnder(candidate, root) && QDir(candidate).exists())
            targetDir = candidate;
    }

    setTreeAnchor(root);
    m_fsModel->setRootPath(root);
    m_tree->setRootIndex(m_fsModel->index(root));

    m_pendingExpandPaths.clear();
    m_pendingSelectPath.clear();

    if (targetDir.isEmpty()) {
        clearAllSelections();
        m_lastActivePath.clear();
        m_tree->viewport()->update();
        emit selectedDirectoriesChanged({}, {});
        m_tree->clearSelection();
        updateButtonStates();
        return;
    }

    QString path = targetDir;
    QStringList chain;
    while (!path.isEmpty() && !Path::equal(path, root)) {
        const QString parent = Path::parentOf(path);
        if (parent.isEmpty()) break;
        if (!Path::equal(parent, root))
            chain.prepend(parent);
        path = parent;
    }
    m_pendingExpandPaths = chain;
    m_pendingSelectPath = targetDir;
    applyExpandAndSelect();

    clearAllSelections();
    DirNode* node = m_dirTree->ensurePath(targetDir);
    m_dirTree->setSelection(node, SelectionState::Recursive);
    m_lastActivePath = targetDir;

    m_tree->viewport()->update();
    emitSelection();
    updateButtonStates();
}

void DirectoryPanel::applyExpandAndSelect()
{
    QMutableListIterator<QString> it(m_pendingExpandPaths);
    while (it.hasNext()) {
        const QString& expandPath = it.next();
        QModelIndex eidx = m_fsModel->index(expandPath);
        if (eidx.isValid()) {
            QModelIndex parentIdx = eidx.parent();
            bool parentLoaded = !parentIdx.isValid() || m_fsModel->rowCount(parentIdx) > 0;
            if (parentLoaded) {
                m_tree->expand(eidx);
                it.remove();
            }
        }
    }

    if (!m_pendingSelectPath.isEmpty()) {
        QModelIndex sidx = m_fsModel->index(m_pendingSelectPath);
        if (sidx.isValid()) {
            QModelIndex parentIdx = sidx.parent();
            bool parentLoaded = !parentIdx.isValid() || m_fsModel->rowCount(parentIdx) > 0;
            if (parentLoaded) {
                m_tree->scrollTo(sidx);
                m_pendingSelectPath.clear();
            }
        }
    }
}

void DirectoryPanel::onDirectoryLoaded(const QString&)
{
    if (!m_pendingExpandPaths.isEmpty() || !m_pendingSelectPath.isEmpty())
        applyExpandAndSelect();
    m_tree->viewport()->update();
}

void DirectoryPanel::updateButtonStates()
{
    if (m_downAction)
        m_downAction->setEnabled(!commonNextChild().isEmpty());

    if (m_upAction)
        m_upAction->setEnabled(!m_treeAnchor.isEmpty());
}
