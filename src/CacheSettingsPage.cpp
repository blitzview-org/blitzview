#include "CacheSettingsPage.h"

#include "AppSettings.h"
#include "ThumbnailDiskCache.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QtConcurrentRun>

CacheSettingsPage::CacheSettingsPage(QWidget* parent) : SettingsPage(parent)
{
    QFormLayout* form = addForm();

    m_enabled = new QCheckBox(tr("Enable disk cache"), this);
    m_enabled->setChecked(AppSettings::diskCacheEnabled());
    form->addRow(m_enabled);

    const qint64 maxBytes = AppSettings::diskCacheMaxBytes();

    m_unlimited = new QCheckBox(tr("Unlimited"), this);
    m_unlimited->setChecked(maxBytes <= 0);

    m_maxMb = new QSpinBox(this);
    m_maxMb->setRange(64, 1000000);
    m_maxMb->setSuffix(tr(" MB"));
    m_maxMb->setValue(maxBytes > 0 ? int(maxBytes / (1024 * 1024)) : 1024);
    m_maxMb->setEnabled(!m_unlimited->isChecked());
    connect(m_unlimited, &QCheckBox::toggled, m_maxMb, &QSpinBox::setDisabled);

    form->addRow(tr("Size limit:"), m_maxMb);
    form->addRow(QString(), m_unlimited);

    m_usage = new QLabel(tr("Current size: computing…"), this);
    form->addRow(m_usage);

    m_clear = new QPushButton(tr("Clear Cache"), this);
    connect(m_clear, &QPushButton::clicked, this, &CacheSettingsPage::onClearCache);
    form->addRow(m_clear);

    refreshUsageAsync();
}

void CacheSettingsPage::apply()
{
    const bool enabled = m_enabled->isChecked();
    const qint64 maxBytes = m_unlimited->isChecked()
        ? 0
        : qint64(m_maxMb->value()) * 1024 * 1024;

    AppSettings::setDiskCacheEnabled(enabled);
    AppSettings::setDiskCacheMaxBytes(maxBytes);

    auto& disk = ThumbnailDiskCache::instance();
    disk.setEnabled(enabled);
    disk.setMaxBytes(maxBytes);
    disk.trimIfNeeded();
}

void CacheSettingsPage::refreshUsageAsync()
{
    QPointer<CacheSettingsPage> guard(this);
    auto future = QtConcurrent::run([guard]() {
        const qint64 bytes = ThumbnailDiskCache::instance().computeCurrentBytes();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, bytes]() {
            if (!guard)
                return;
            guard->m_usage->setText(tr("Current size: %1")
                .arg(QLocale().formattedDataSize(bytes)));
        }, Qt::QueuedConnection);
    });
    Q_UNUSED(future);
}

void CacheSettingsPage::onClearCache()
{
    const auto answer = QMessageBox::question(
        this, tr("Clear Cache"),
        tr("Delete all cached thumbnails from disk?"));
    if (answer != QMessageBox::Yes)
        return;

    ThumbnailDiskCache::instance().clear();
    refreshUsageAsync();
}
