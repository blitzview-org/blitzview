#pragma once

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QListWidget;
class QListWidgetItem;
class QSpinBox;

// Batch metadata editing for the selected media (right-click in the grid):
// shift the capture time, copy the capture time into the file mtime, and
// edit tags. The dialog only collects the OPERATIONS — MainWindow translates
// them into exiftool write jobs.
//
// Tags are shown as a tri-state list, the standard answer to "the selection
// disagrees": checked = on every selected file, partially checked = on some
// (and left alone), unchecked = removed from all. Only rows the user
// actually changed produce a write.
class EditMetadataDialog : public QDialog
{
    Q_OBJECT
public:
    // One tag found on the selection, with how many files carry it.
    struct TagState {
        QString tag;     // display casing (first spelling encountered)
        int     count = 0;
    };

    struct Edits {
        qint64      shiftSeconds    = 0;      // signed capture-time offset
        bool        setMtimeToTaken = false;
        bool        clearTags       = false;  // "remove all" shortcut
        QStringList addTags;                  // ensure present on every file
        QStringList removeTags;               // remove from every file

        bool isEmpty() const
        {
            return shiftSeconds == 0 && !setMtimeToTaken && !clearTags
                && addTags.isEmpty() && removeTags.isEmpty();
        }
    };

    EditMetadataDialog(int fileCount, const QList<TagState>& presentTags,
                       const QStringList& knownTags,
                       QWidget* parent = nullptr);

    Edits edits() const;

protected:
    // Clicking anywhere on a tag row cycles it, not just the check box.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void addTagFromInput();

private:
    // Roles on the tag rows: the tag in its display casing, the state the row
    // started in (only a CHANGED row is written), and how many of the
    // selected files carry it.
    static constexpr int kTagRole   = Qt::UserRole;
    static constexpr int kInitRole  = Qt::UserRole + 1;
    static constexpr int kCountRole = Qt::UserRole + 2;

    // Per-row tooltip spelling out what THIS row will do on OK. Recomputed on
    // every toggle, so it always describes the pending action rather than a
    // generic legend.
    void updateTagTooltip(QListWidgetItem* item) const;

    // Empties the add-tag combo, icon included (see implementation)
    void clearTagInput();

    // Edge length for the tag symbol icons shown in the list and in the
    // add-tag combo (see TagSymbols::iconFor)
    int tagIconSize() const;

    // Next state for a row, mirroring Qt's own rule: tristate rows cycle
    // through all three, the rest flip between checked and unchecked.
    static void advanceTagState(QListWidgetItem* item);

    int m_fileCount = 0;
    // Row Qt last asked a tooltip for. QToolTip::isVisible() is useless
    // here: by the time the press reaches this filter the tooltip is already
    // torn down. Tracking the QEvent::ToolTip request instead tells us
    // whether one was on screen, so it can be put back with the UPDATED text.
    QListWidgetItem* m_tooltipItem = nullptr;
    QCheckBox* m_shiftEnabled    = nullptr;
    QComboBox* m_shiftSign       = nullptr;
    QSpinBox*  m_shiftDays       = nullptr;
    QSpinBox*  m_shiftHours      = nullptr;
    QSpinBox*  m_shiftMinutes    = nullptr;
    QSpinBox*  m_shiftSeconds    = nullptr;
    QCheckBox* m_setMtime        = nullptr;
    QCheckBox* m_clearTags       = nullptr;
    QComboBox* m_tagInput        = nullptr;
    QListWidget* m_tagList       = nullptr;
};
