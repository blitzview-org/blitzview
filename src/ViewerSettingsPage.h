#pragma once

#include "SettingsPage.h"

class QCheckBox;

// Settings page "Media Viewer": viewer window policy and the opt-in for
// permanent deletion in the grid context menu.
class ViewerSettingsPage : public SettingsPage
{
    Q_OBJECT
public:
    explicit ViewerSettingsPage(QWidget* parent = nullptr);

    QString pageId() const override { return QStringLiteral("viewer"); }
    QString title() const override { return tr("Media Viewer"); }
    void    apply() override;

private:
    QCheckBox* m_multipleViewers = nullptr;
    QCheckBox* m_permanentDelete = nullptr;
};
