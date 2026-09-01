#include "TagSettingsPage.h"

#include "MetadataCache.h"
#include "TagSymbols.h"

#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QStyle>
#include <utility>

namespace {
// Below this many tags a filter box is more clutter than help.
constexpr int kFilterThreshold = 12;
}

TagSettingsPage::TagSettingsPage(QWidget* parent) : SettingsPage(parent)
{
    QStringList knownTags = MetadataCache::instance().knownTags();
    // Tags carrying a symbol first, the rest after the rule
    const int split = TagSymbols::partitionBySymbol(knownTags);

    if (knownTags.isEmpty()) {
        addPageWidget(new QLabel(tr("No tags known yet."), this));
        return;
    }

    addPageWidget(new QLabel(
        tr("Symbol shown on thumbnails for each tag:"), this));

    if (knownTags.size() > kFilterThreshold) {
        m_filter = new QLineEdit(this);
        m_filter->setPlaceholderText(tr("Filter tags…"));
        m_filter->setClearButtonEnabled(true);
        connect(m_filter, &QLineEdit::textChanged,
                this, &TagSettingsPage::onFilterChanged);
        addPageWidget(m_filter);
    }

    // Only the rows scroll — caption and filter stay put, which is the whole
    // point of opting out of the dialog's scroll area.
    auto* rowHost = new QWidget(this);
    m_form = new QFormLayout(rowHost);
    m_form->setContentsMargins(0, 0, 0, 0);
    m_form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);

    // A quarter above the standard icon size: 16 px makes the symbols
    // indistinguishable, but without the badge pill the glyph fills the box
    // almost entirely, so a modest bump already reads clearly.
    const int iconPx = qMax(18, style()->pixelMetric(QStyle::PM_SmallIconSize,
                                                     nullptr, this) * 5 / 4);
    for (int i = 0; i < knownTags.size(); ++i) {
        const QString& tag = knownTags.at(i);
        if (i == split && split > 0) {
            auto* rule = new QFrame(rowHost);
            rule->setFrameShape(QFrame::HLine);
            rule->setFrameShadow(QFrame::Sunken);
            m_form->addRow(rule);
            m_splitRow = m_form->rowCount() - 1;
        }
        auto* combo = new QComboBox(rowHost);
        // Without this the combo box scales every icon down to its own
        // default size and the larger pixmaps have no effect at all.
        combo->setIconSize(QSize(iconPx, iconPx));
        // "(none)" is the DEFAULT and shows a blank icon — no badge at all
        // for that tag. The neutral tag glyph below it is one symbol choice
        // among the palette, not a fallback.
        combo->addItem(TagSymbols::symbolIcon(QString(), iconPx),
                       tr("(none)"), QString());
        combo->addItem(
            TagSymbols::symbolIcon(TagSymbols::neutralSymbol(), iconPx),
            tr("Tag"), TagSymbols::neutralSymbol());
        for (const QString& symbol : TagSymbols::palette()) {
            // Icon renders the glyph; the TEXT is the symbol's name. Putting
            // displaySymbol() here too would show the same character twice.
            // Data stays the canonical emoji even in fallback mode.
            combo->addItem(TagSymbols::symbolIcon(symbol, iconPx),
                           TagSymbols::nameFor(symbol), symbol);
        }
        const int idx = combo->findData(TagSymbols::symbolFor(tag));
        combo->setCurrentIndex(idx < 0 ? 0 : idx);
        m_form->addRow(tag, combo);
        m_rows.append({tag, combo, m_form->rowCount() - 1, i < split});
    }

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(rowHost);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    addPageWidget(scroll, 1);

    if (m_filter) {
        m_status = new QLabel(this);
        m_status->setEnabled(false);   // secondary text, not a control
        addPageWidget(m_status);
        onFilterChanged(QString());
    }
}

void TagSettingsPage::onFilterChanged(const QString& text)
{
    const QString needle = text.trimmed();

    int shown = 0;
    int shownWith = 0;
    for (const Row& row : std::as_const(m_rows)) {
        const bool match = needle.isEmpty()
            || row.tag.contains(needle, Qt::CaseInsensitive);
        m_form->setRowVisible(row.formRow, match);
        if (match) {
            ++shown;
            if (row.hasSymbol)
                ++shownWith;
        }
    }

    // A rule with nothing on one side of it separates nothing
    if (m_splitRow >= 0)
        m_form->setRowVisible(m_splitRow, shownWith > 0 && shown > shownWith);

    if (m_status) {
        // No singular case to worry about: the filter only exists above
        // kFilterThreshold tags.
        m_status->setText(shown == m_rows.size()
            ? tr("%1 tags").arg(m_rows.size())
            : tr("%1 of %2 tags").arg(shown).arg(m_rows.size()));
    }
}

QHash<QString, QString> TagSettingsPage::symbolMap() const
{
    QHash<QString, QString> map;
    for (const Row& row : std::as_const(m_rows))
        map.insert(row.tag, row.combo->currentData().toString());
    return map;
}

void TagSettingsPage::apply()
{
    if (m_rows.isEmpty())
        return;

    // The whole map is written, filtered-away rows included — the filter is a
    // view over the list, not a selection.
    TagSymbols::setSymbols(symbolMap());
    emit repaintNeeded();   // grid badges must be redrawn
}
