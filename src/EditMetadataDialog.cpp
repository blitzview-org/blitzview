#include "EditMetadataDialog.h"

#include "TagSymbols.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QToolTip>
#include <QVBoxLayout>

EditMetadataDialog::EditMetadataDialog(int fileCount,
                                       const QList<TagState>& presentTags,
                                       const QStringList& knownTags,
                                       QWidget* parent)
    : QDialog(parent), m_fileCount(fileCount)
{
    setWindowTitle(tr("Edit Metadata — %n file(s)", nullptr, fileCount));

    // --- Capture time ---
    auto* timeGroup = new QGroupBox(tr("Capture Time (Taken)"), this);
    auto* timeLayout = new QVBoxLayout(timeGroup);

    m_shiftEnabled = new QCheckBox(tr("Shift by offset (camera clock / timezone was wrong)"), timeGroup);
    timeLayout->addWidget(m_shiftEnabled);

    auto* shiftRow = new QHBoxLayout;
    m_shiftSign = new QComboBox(timeGroup);
    m_shiftSign->addItem(QStringLiteral("+"));
    m_shiftSign->addItem(QStringLiteral("−"));
    m_shiftDays    = new QSpinBox(timeGroup);
    m_shiftDays->setRange(0, 3650);
    m_shiftDays->setSuffix(tr(" d"));
    m_shiftHours   = new QSpinBox(timeGroup);
    m_shiftHours->setRange(0, 23);
    m_shiftHours->setSuffix(tr(" h"));
    m_shiftMinutes = new QSpinBox(timeGroup);
    m_shiftMinutes->setRange(0, 59);
    m_shiftMinutes->setSuffix(tr(" min"));
    m_shiftSeconds = new QSpinBox(timeGroup);
    m_shiftSeconds->setRange(0, 59);
    m_shiftSeconds->setSuffix(tr(" s"));
    shiftRow->addWidget(m_shiftSign);
    shiftRow->addWidget(m_shiftDays);
    shiftRow->addWidget(m_shiftHours);
    shiftRow->addWidget(m_shiftMinutes);
    shiftRow->addWidget(m_shiftSeconds);
    shiftRow->addStretch();
    timeLayout->addLayout(shiftRow);

    for (QWidget* w : {static_cast<QWidget*>(m_shiftSign),
                       static_cast<QWidget*>(m_shiftDays),
                       static_cast<QWidget*>(m_shiftHours),
                       static_cast<QWidget*>(m_shiftMinutes),
                       static_cast<QWidget*>(m_shiftSeconds)}) {
        w->setEnabled(false);
        connect(m_shiftEnabled, &QCheckBox::toggled, w, &QWidget::setEnabled);
    }

    m_setMtime = new QCheckBox(tr("Set file modify time to capture time"), timeGroup);
    m_setMtime->setToolTip(tr("Makes the capture time visible to the file system "
                              "and to tools that sort by modification date."));
    timeLayout->addWidget(m_setMtime);

    // --- Tags ---
    auto* tagGroup = new QGroupBox(tr("Tags"), this);
    auto* tagLayout = new QVBoxLayout(tagGroup);

    auto* addRow = new QHBoxLayout;
    m_tagInput = new QComboBox(tagGroup);
    m_tagInput->setEditable(true);
    m_tagInput->setInsertPolicy(QComboBox::NoInsert);
    // Tags carrying a symbol first, then a separator, then the rest — same
    // split as the Tags settings page and the filter dialog. The icons make
    // the grouping self-explanatory; below the separator they are blank,
    // exactly like the (absent) grid badge.
    {
        QStringList ordered = knownTags;
        const int split = TagSymbols::partitionBySymbol(ordered);
        for (int i = 0; i < ordered.size(); ++i) {
            if (i == split && split > 0)
                m_tagInput->insertSeparator(m_tagInput->count());
            m_tagInput->addItem(
                TagSymbols::iconFor(ordered.at(i), tagIconSize()),
                ordered.at(i));
        }
    }
    clearTagInput();
    if (auto* completer = m_tagInput->completer()) {
        completer->setCompletionMode(QCompleter::PopupCompletion);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
    }
    auto* addBtn = new QPushButton(tr("&Add"), tagGroup);
    addRow->addWidget(m_tagInput, 1);
    addRow->addWidget(addBtn);
    tagLayout->addLayout(addRow);

    m_tagList = new QListWidget(tagGroup);
    tagLayout->addWidget(m_tagList);
    // Whole row is the hit target (see eventFilter). Filtering RELEASE, not
    // press, so the row still highlights on press as usual.
    m_tagList->viewport()->installEventFilter(this);

    for (const TagState& ts : presentTags) {
        const Qt::CheckState state = (ts.count >= fileCount)
            ? Qt::Checked : Qt::PartiallyChecked;
        auto* item = new QListWidgetItem(
            state == Qt::Checked || fileCount == 1
                ? ts.tag
                : tr("%1   (%2 of %3)").arg(ts.tag).arg(ts.count).arg(fileCount),
            m_tagList);
        // Tristate ONLY where "some" is a real state to return to. Qt cycles
        // such a row Unchecked → Partially → Checked, so starting from
        // partially checked the user reaches "add to all", then "remove from
        // all", then back to "leave as it is". Rows that are on every file,
        // and newly typed ones, stay two-state: this dialog cannot put a tag
        // on *some* files, so offering that state would be a lie.
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable
                       | (state == Qt::PartiallyChecked
                              ? Qt::ItemIsUserTristate : Qt::NoItemFlags));
        // Data roles BEFORE the check state: setCheckState is what the
        // itemChanged handler reacts to, and it reads these.
        item->setData(kTagRole, ts.tag);
        item->setData(kInitRole, int(state));
        item->setData(kCountRole, ts.count);
        item->setIcon(TagSymbols::iconFor(ts.tag, tagIconSize()));
        item->setCheckState(state);
        updateTagTooltip(item);
    }

    // Connected after populating: no point re-deriving tooltips for rows that
    // are still being built.
    connect(m_tagList, &QListWidget::itemChanged,
            this, [this](QListWidgetItem* item) { updateTagTooltip(item); });

    // The shortcut for "throw everything away" — it overrides the list, so
    // the list is disabled while it is on rather than showing a state that
    // will not be honoured.
    m_clearTags = new QCheckBox(tr("Remove all existing tags"), tagGroup);
    tagLayout->addWidget(m_clearTags);
    connect(m_clearTags, &QCheckBox::toggled, m_tagList, &QWidget::setDisabled);

    connect(addBtn, &QPushButton::clicked, this, &EditMetadataDialog::addTagFromInput);
    connect(m_tagInput->lineEdit(), &QLineEdit::returnPressed,
            this, &EditMetadataDialog::addTagFromInput);

    // --- Buttons ---
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(timeGroup);
    layout->addWidget(tagGroup);
    layout->addWidget(buttons);

    resize(460, sizeHint().height());
}

void EditMetadataDialog::clearTagInput()
{
    // setCurrentText("") alone leaves currentIndex on the first entry, whose
    // ICON then sits in the empty line edit — the combo looked as if a tag
    // were selected. Dropping the index first clears the icon too.
    m_tagInput->setCurrentIndex(-1);
    m_tagInput->setCurrentText(QString());
}

int EditMetadataDialog::tagIconSize() const
{
    return style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
}

void EditMetadataDialog::addTagFromInput()
{
    const QString tag = m_tagInput->currentText().trimmed();
    if (tag.isEmpty())
        return;

    // Already listed? Then just tick it — typing a tag that is on some of the
    // files means "put it on all of them", not "add a second row".
    for (int i = 0; i < m_tagList->count(); ++i) {
        QListWidgetItem* item = m_tagList->item(i);
        if (item->data(kTagRole).toString().compare(tag, Qt::CaseInsensitive) == 0) {
            item->setCheckState(Qt::Checked);
            m_tagList->scrollToItem(item);
            clearTagInput();
            return;
        }
    }

    auto* item = new QListWidgetItem(tag, m_tagList);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setData(kTagRole, tag);
    item->setData(kInitRole, int(Qt::Unchecked));   // on no file yet
    item->setData(kCountRole, 0);
    item->setIcon(TagSymbols::iconFor(tag, tagIconSize()));
    item->setCheckState(Qt::Checked);
    updateTagTooltip(item);
    clearTagInput();
}

void EditMetadataDialog::advanceTagState(QListWidgetItem* item)
{
    const Qt::CheckState now = item->checkState();
    // Same rule QStyledItemDelegate applies, so mouse and keyboard (Space,
    // which still goes through the delegate) stay in step.
    const Qt::CheckState next =
        (item->flags() & Qt::ItemIsUserTristate)
            ? Qt::CheckState((now + 1) % 3)
            : (now == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    item->setCheckState(next);
}

bool EditMetadataDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (m_tagList && watched == m_tagList->viewport()) {
        // Remember which row a tooltip was requested for; forget it once the
        // cursor leaves the list.
        if (event->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(event);
            m_tooltipItem = m_tagList->itemAt(he->pos());
        } else if (event->type() == QEvent::Leave) {
            m_tooltipItem = nullptr;
        }

        if (event->type() == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                QListWidgetItem* item =
                    m_tagList->itemAt(me->position().toPoint());
                if (item && (item->flags() & Qt::ItemIsUserCheckable)) {
                    // Consume it: the delegate would otherwise toggle a click
                    // on the check box a second time.
                    advanceTagState(item);
                    // Reading the outcome is the whole point of clicking, so
                    // put the tooltip back — now with the new text. The rect
                    // keeps it up while the cursor stays on the row instead
                    // of making it flicker.
                    if (m_tooltipItem == item)
                        QToolTip::showText(me->globalPosition().toPoint(),
                                           item->toolTip(),
                                           m_tagList->viewport(),
                                           m_tagList->visualItemRect(item));
                    return true;
                }
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void EditMetadataDialog::updateTagTooltip(QListWidgetItem* item) const
{
    if (!item)
        return;

    const QString tag = item->data(kTagRole).toString();
    const int count = item->data(kCountRole).toInt();
    const auto initial = Qt::CheckState(item->data(kInitRole).toInt());
    const Qt::CheckState now = item->checkState();

    QString text;
    if (now == Qt::PartiallyChecked) {
        text = tr("“%1” is on %2 of %3 selected files — unchanged.")
                   .arg(tag).arg(count).arg(m_fileCount);
    } else if (now == Qt::Checked) {
        if (initial == Qt::Checked) {
            text = m_fileCount > 1
                ? tr("“%1” is on all %2 selected files — unchanged.")
                      .arg(tag).arg(m_fileCount)
                : tr("“%1” is on this file — unchanged.").arg(tag);
        } else {
            text = m_fileCount > 1
                ? tr("“%1” will be added to all %2 selected files.")
                      .arg(tag).arg(m_fileCount)
                : tr("“%1” will be added to this file.").arg(tag);
        }
    } else {
        text = m_fileCount > 1
            ? tr("“%1” will be removed from all %2 selected files.")
                  .arg(tag).arg(m_fileCount)
            : tr("“%1” will be removed from this file.").arg(tag);
    }
    item->setToolTip(text);
}

EditMetadataDialog::Edits EditMetadataDialog::edits() const
{
    Edits e;
    if (m_shiftEnabled->isChecked()) {
        qint64 secs = qint64(m_shiftDays->value()) * 86400
                    + qint64(m_shiftHours->value()) * 3600
                    + qint64(m_shiftMinutes->value()) * 60
                    + m_shiftSeconds->value();
        if (m_shiftSign->currentIndex() == 1)
            secs = -secs;
        e.shiftSeconds = secs;
    }
    e.setMtimeToTaken = m_setMtime->isChecked();
    e.clearTags = m_clearTags->isChecked();

    for (int i = 0; i < m_tagList->count(); ++i) {
        const QListWidgetItem* item = m_tagList->item(i);
        const Qt::CheckState now = item->checkState();
        const auto initial = Qt::CheckState(item->data(kInitRole).toInt());
        // Unchanged rows produce no write at all — in particular a tag that
        // is on some files and was left partially checked stays untouched.
        if (now == initial || now == Qt::PartiallyChecked)
            continue;
        const QString tag = item->data(kTagRole).toString();
        if (now == Qt::Checked)
            e.addTags.append(tag);
        else
            e.removeTags.append(tag);
    }

    // "Remove all" makes individual removals redundant; keeping them would
    // only produce contradictory arguments.
    if (e.clearTags)
        e.removeTags.clear();
    return e;
}
