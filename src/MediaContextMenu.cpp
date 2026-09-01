#include "MediaContextMenu.h"
#include "AppSettings.h"

#include <QMenu>
#include <QAction>
#include <QKeySequence>
#include <QWidget>

namespace MediaContextMenu {

Action exec(QWidget* parent, const QPoint& globalPos, const Context& ctx)
{
    const bool hasSelection = ctx.selectionCount > 0;
    if (!hasSelection && !ctx.pastePossible && !ctx.hasItems)
        return Action::None;

    QMenu menu(parent);
    // Refers to the item under the cursor, independent of the selection.
    QAction* actDetails = menu.addAction(QObject::tr("D&etails…"));
    actDetails->setEnabled(ctx.hoverItemValid);
    menu.addSeparator();
    QAction* actSelectAll = menu.addAction(QObject::tr("Select &All"));
    actSelectAll->setShortcut(QKeySequence::SelectAll);
    actSelectAll->setEnabled(ctx.hasItems);
    menu.addSeparator();
    QAction* actCopy = menu.addAction(QObject::tr("&Copy"));
    actCopy->setShortcut(QKeySequence::Copy);
    actCopy->setEnabled(hasSelection);
    QAction* actCut = menu.addAction(QObject::tr("Cu&t"));
    actCut->setShortcut(QKeySequence::Cut);
    actCut->setEnabled(hasSelection);
    QAction* actCopyPath = menu.addAction(
        ctx.selectionCount > 1 ? QObject::tr("Copy Path&s") : QObject::tr("Copy Pat&h"));
    actCopyPath->setToolTip(QObject::tr("Copy the full file path(s) as text"));
    actCopyPath->setEnabled(hasSelection);
    menu.addSeparator();
    QAction* actPaste = menu.addAction(QObject::tr("&Paste"));
    actPaste->setShortcut(QKeySequence::Paste);
    actPaste->setEnabled(ctx.pastePossible);
    menu.addSeparator();
    QAction* actEditMeta = menu.addAction(QObject::tr("Edit &Metadata…"));
    actEditMeta->setEnabled(hasSelection);
    QAction* actRename = menu.addAction(QObject::tr("&Rename Selected…"));
    actRename->setEnabled(hasSelection);
    menu.addSeparator();
    QAction* actFilterSel = menu.addAction(QObject::tr("Filter &By Selection"));
    actFilterSel->setEnabled(hasSelection);
    QAction* actClearFilter = nullptr;
    if (ctx.filtered)
        actClearFilter = menu.addAction(QObject::tr("Clear Filter"));
    menu.addSeparator();
    QAction* actTrash = menu.addAction(QObject::tr("Move to &Trash"));
    actTrash->setEnabled(hasSelection);
    QAction* actDelete = nullptr;
    if (AppSettings::permanentDeleteEnabled()) {
        actDelete = menu.addAction(QObject::tr("Delete Permanentl&y…"));
        actDelete->setEnabled(hasSelection);
    }

    QAction* chosen = menu.exec(globalPos);
    if (!chosen)                             return Action::None;
    if (chosen == actDetails)                return Action::Details;
    if (chosen == actSelectAll)              return Action::SelectAll;
    if (chosen == actCopy)                   return Action::Copy;
    if (chosen == actCut)                    return Action::Cut;
    if (chosen == actCopyPath)               return Action::CopyPaths;
    if (chosen == actPaste)                  return Action::Paste;
    if (chosen == actEditMeta)               return Action::EditMetadata;
    if (chosen == actRename)                 return Action::Rename;
    if (chosen == actFilterSel)              return Action::FilterBySelection;
    if (actClearFilter && chosen == actClearFilter) return Action::ClearFilter;
    if (chosen == actTrash)                  return Action::Trash;
    if (actDelete && chosen == actDelete)    return Action::DeletePermanent;
    return Action::None;
}

}
