#pragma once

#include <QPoint>

class QWidget;

// The item context menu shared by GridView and TableView. Both views must
// offer EXACTLY the same entries with the same enable rules — building the
// menu twice would let them drift apart. This builds and execs it and
// returns the chosen action; PERFORMING the action stays with the view
// (grid and table reach their selection/clipboard differently).
namespace MediaContextMenu {

enum class Action {
    None,
    Details,
    SelectAll,
    Copy,
    Cut,
    CopyPaths,
    Paste,
    EditMetadata,
    Rename,
    FilterBySelection,
    ClearFilter,
    Trash,
    DeletePermanent
};

struct Context {
    bool hasItems       = false;   // model is non-empty
    int  selectionCount = 0;
    bool pastePossible  = false;
    bool filtered       = false;   // model has an active filter
    bool hoverItemValid = false;   // an item sits under the cursor → "Details…"
};

// Returns Action::None when the menu is dismissed (or not worth showing).
Action exec(QWidget* parent, const QPoint& globalPos, const Context& ctx);

}
