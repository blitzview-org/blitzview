#include "TagFilterDialog.h"

#include "TagSymbols.h"

#include <QCheckBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QStyle>
#include <QVBoxLayout>

TagFilterDialog::TagFilterDialog(const QStringList& tags,
                                 const QStringList& activeTags,
                                 QWidget* parent)
    : QDialog(parent)
{
    // Tags carrying a symbol first, the rest after a rule — same split as
    // the Tags settings page, so the two lists read alike
    QStringList availableTags = tags;
    const int split = TagSymbols::partitionBySymbol(availableTags);

    setWindowTitle(tr("Filter by Tag"));

    auto* layout = new QVBoxLayout(this);

    if (availableTags.isEmpty()) {
        layout->addWidget(new QLabel(tr("No tags found."), this));
    } else {
        layout->addWidget(new QLabel(tr("Show items with any of the checked tags:"), this));

        m_selectAll = new QCheckBox(tr("Select &all"), this);
        layout->addWidget(m_selectAll);

        m_list = new QListWidget(this);
        const int iconPx = style()->pixelMetric(QStyle::PM_SmallIconSize,
                                                nullptr, m_list);
        for (int i = 0; i < availableTags.size(); ++i) {
            if (i == split && split > 0) {
                // Inert row carrying a QFrame rule. No checkable flag, so
                // checkedTags() cannot pick it up.
                auto* sep = new QListWidgetItem(m_list);
                sep->setFlags(Qt::NoItemFlags);
                sep->setSizeHint(QSize(0, 9));
                auto* rule = new QFrame(m_list);
                rule->setFrameShape(QFrame::HLine);
                rule->setFrameShadow(QFrame::Sunken);
                m_list->setItemWidget(sep, rule);
            }
            const QString& tag = availableTags.at(i);
            auto* item = new QListWidgetItem(tag, m_list);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(activeTags.contains(tag) ? Qt::Checked : Qt::Unchecked);
            // Tag symbol — same visual as the grid badges; blank for the
            // tags below the rule, which show no badge either
            item->setIcon(TagSymbols::iconFor(tag, iconPx));
        }
        layout->addWidget(m_list);

        // Clicking the label text toggles the checkbox too, not just the
        // indicator box. A click ON the indicator already toggled the state
        // via QListWidget's built-in handling before this signal fires — do
        // not toggle again there, or the click would cancel itself out.
        // The row layout is indicator → icon → text: the icon region lies
        // past labelStart and intentionally counts as label (toggles).
        connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            const QRect r = m_list->visualItemRect(item);
            const int clickX = m_list->viewport()->mapFromGlobal(QCursor::pos()).x();
            const int indicatorWidth = m_list->style()->pixelMetric(
                QStyle::PM_IndicatorWidth, nullptr, m_list);
            const int spacing = m_list->style()->pixelMetric(
                QStyle::PM_CheckBoxLabelSpacing, nullptr, m_list);
            const int labelStart = r.left() + indicatorWidth + spacing;
            if (clickX >= labelStart)
                item->setCheckState(item->checkState() == Qt::Checked
                                       ? Qt::Unchecked : Qt::Checked);
        });

        // Master checkbox instead of a button: it also SHOWS the state
        // (all / some / none), which a button cannot, and one control covers
        // both directions. A button whose label flips between "Check all" and
        // "Uncheck all" changes under the cursor, which reads as a glitch.
        m_selectAll->setTristate(true);
        connect(m_selectAll, &QCheckBox::clicked, this, [this]() {
            // Qt cycles a tristate box Unchecked → Partially → Checked on
            // click, but "partially" is not a command. So the target is
            // derived from the LIST — anything less than all checked means
            // "check everything" — and the box is corrected afterwards.
            int checkable = 0;
            // Separate statements on purpose: the operands of `<` have no
            // guaranteed evaluation order, so `checkable` must be filled in
            // before it is read.
            const int checked = countChecked(&checkable);
            const Qt::CheckState target = (checked < checkable)
                ? Qt::Checked : Qt::Unchecked;
            for (int i = 0; i < m_list->count(); ++i) {
                QListWidgetItem* item = m_list->item(i);
                // Skip the group rule: setting a check state on it would
                // give the separator row a checkbox of its own
                if (item->flags() & Qt::ItemIsUserCheckable)
                    item->setCheckState(target);
            }
            syncSelectAll();
        });
        // Connected after populating — no point recomputing per initial row
        connect(m_list, &QListWidget::itemChanged,
                this, [this]() { syncSelectAll(); });
        syncSelectAll();
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    resize(340, qMin(480, 160 + availableTags.size() * 24));
}

int TagFilterDialog::countChecked(int* checkable) const
{
    int checked = 0;
    if (checkable)
        *checkable = 0;
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem* item = m_list->item(i);
        // The group rule is not a row the user can check
        if (!(item->flags() & Qt::ItemIsUserCheckable))
            continue;
        if (checkable)
            ++*checkable;
        if (item->checkState() == Qt::Checked)
            ++checked;
    }
    return checked;
}

void TagFilterDialog::syncSelectAll()
{
    if (!m_selectAll || !m_list)
        return;
    int checkable = 0;
    const int checked = countChecked(&checkable);
    // setCheckState emits stateChanged/toggled, NOT clicked — the handler
    // above is bound to clicked(), so this cannot loop back into itself.
    m_selectAll->setCheckState(checked == 0                        ? Qt::Unchecked
                               : checked == checkable              ? Qt::Checked
                                                                   : Qt::PartiallyChecked);
}

QStringList TagFilterDialog::checkedTags() const
{
    QStringList out;
    if (!m_list)
        return out;
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem* item = m_list->item(i);
        if ((item->flags() & Qt::ItemIsUserCheckable)
            && item->checkState() == Qt::Checked)
            out.append(item->text());
    }
    return out;
}
