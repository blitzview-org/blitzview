#pragma once

#include "SettingsPage.h"

class QCheckBox;
class QSpinBox;

// Settings page "Input & Animation": mouse/click behaviour, the focus border
// width and the durations of the two UI animations.
class InputSettingsPage : public SettingsPage
{
    Q_OBJECT
public:
    explicit InputSettingsPage(QWidget* parent = nullptr);

    QString pageId() const override { return QStringLiteral("input"); }
    QString title() const override { return tr("Input & Animation"); }
    void    apply() override;

private:
    QSpinBox*  m_doubleClickMs = nullptr;
    QCheckBox* m_doubleClickSystem = nullptr;
    QSpinBox*  m_focusBorderPx = nullptr;
    QSpinBox*  m_mouseThresholdPx = nullptr;
    QSpinBox*  m_reflowAnimMs = nullptr;
    QSpinBox*  m_scrollAnimMs = nullptr;
    QCheckBox* m_scrollSnap = nullptr;
    QSpinBox*  m_fullscreenAnimMs = nullptr;
};
