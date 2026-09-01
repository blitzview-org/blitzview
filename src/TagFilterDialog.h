#pragma once

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QListWidget;

// Small checklist dialog for filtering by tag (replaces a QMenu submenu,
// which is awkward to operate with many/long tag names). ANY-match: an item
// passes if it has at least one of the checked tags.
class TagFilterDialog : public QDialog
{
    Q_OBJECT
public:
    // tags: tags to offer, already scoped to the current filter (e.g.
    // narrowed by an active selection filter); reordered for display —
    // those with a symbol first, then a rule, then the rest.
    // activeTags: pre-checked.
    TagFilterDialog(const QStringList& tags,
                    const QStringList& activeTags,
                    QWidget* parent = nullptr);

    QStringList checkedTags() const;

private:
    // Checked rows; *checkable receives the number of rows that CAN be
    // checked (the group rule cannot)
    int  countChecked(int* checkable = nullptr) const;

    // Brings the master checkbox in line with the list: none / some / all
    void syncSelectAll();

    QListWidget* m_list      = nullptr;
    QCheckBox*   m_selectAll = nullptr;
};
