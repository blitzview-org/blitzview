#include "ViewerSettingsPage.h"

#include "AppSettings.h"

#include <QCheckBox>
#include <QFormLayout>

ViewerSettingsPage::ViewerSettingsPage(QWidget* parent) : SettingsPage(parent)
{
    QFormLayout* form = addForm();

    m_multipleViewers = new QCheckBox(tr("Allow multiple viewer windows"), this);
    m_multipleViewers->setToolTip(
        tr("Each double-click opens a new viewer window instead of reusing one."));
    m_multipleViewers->setChecked(AppSettings::multipleViewers());
    form->addRow(m_multipleViewers);

    m_permanentDelete = new QCheckBox(
        tr("Enable \"Delete Permanently\" in the context menu"), this);
    m_permanentDelete->setToolTip(
        tr("When disabled, only \"Move to Trash\" is offered."));
    m_permanentDelete->setChecked(AppSettings::permanentDeleteEnabled());
    form->addRow(m_permanentDelete);
}

void ViewerSettingsPage::apply()
{
    // Both settings are read on demand (viewer creation, context menu build)
    // — nothing to push into running objects.
    AppSettings::setMultipleViewers(m_multipleViewers->isChecked());
    AppSettings::setPermanentDeleteEnabled(m_permanentDelete->isChecked());
}
