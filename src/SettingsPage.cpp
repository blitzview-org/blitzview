#include "SettingsPage.h"

#include <QAbstractButton>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

SettingsPage::SettingsPage(QWidget* parent) : QWidget(parent)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    // Permanent trailing stretch: groups are inserted above it, so short
    // pages stay top-aligned instead of stretching their rows apart.
    m_layout->addStretch(1);
}

QFormLayout* SettingsPage::addForm()
{
    auto* host = new QWidget(this);
    auto* form = new QFormLayout(host);
    form->setContentsMargins(0, 0, 0, 0);
    // Spin boxes and combo boxes keep their natural width instead of being
    // stretched across the whole page.
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    form->setVerticalSpacing(8);
    addPageWidget(host);
    return form;
}

QFormLayout* SettingsPage::addGroup(const QString& title)
{
    auto* group = new QGroupBox(title, this);
    auto* form = new QFormLayout(group);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    addPageWidget(group);
    return form;
}

void SettingsPage::addPageWidget(QWidget* widget, int stretch)
{
    m_layout->insertWidget(m_layout->count() - 1, widget, stretch);
    if (stretch > 0) {
        // A stretching child (e.g. a scroll area) owns the free space —
        // the trailing spacer must not compete for it.
        m_layout->setStretch(m_layout->count() - 1, 0);
    }
}

QList<QPair<QWidget*, QString>> SettingsPage::searchIndex() const
{
    QList<QPair<QWidget*, QString>> index;

    const QList<QWidget*> children = findChildren<QWidget*>();
    for (QWidget* w : children) {
        QStringList texts;

        if (auto* label = qobject_cast<QLabel*>(w)) {
            texts << label->text();
            // A form label describes its FIELD — a hit should highlight the
            // spin box, not the caption next to it.
            if (QWidget* buddy = label->buddy())
                w = buddy;
        } else if (auto* button = qobject_cast<QAbstractButton*>(w)) {
            texts << button->text();
        } else if (auto* group = qobject_cast<QGroupBox*>(w)) {
            texts << group->title();
        }
        texts << w->toolTip();

        const QString joined = texts.join(QLatin1Char(' ')).simplified();
        if (!joined.isEmpty())
            index.append({w, joined});
    }

    return index;
}
