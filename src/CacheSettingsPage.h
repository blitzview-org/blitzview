#pragma once

#include "SettingsPage.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;

// Settings page "Thumbnail Cache": on/off, size limit, current usage and the
// clear button.
class CacheSettingsPage : public SettingsPage
{
    Q_OBJECT
public:
    explicit CacheSettingsPage(QWidget* parent = nullptr);

    QString pageId() const override { return QStringLiteral("cache"); }
    QString title() const override { return tr("Thumbnail Cache"); }
    void    apply() override;

private:
    void refreshUsageAsync();
    void onClearCache();

    QCheckBox*   m_enabled   = nullptr;
    QCheckBox*   m_unlimited = nullptr;
    QSpinBox*    m_maxMb     = nullptr;
    QLabel*      m_usage     = nullptr;
    QPushButton* m_clear     = nullptr;
};
