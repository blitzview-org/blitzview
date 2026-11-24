// Non-Linux backend: removable devices via QStorageInfo. Used on Windows,
// where there is no UDisks2 and mounting is the operating system's job.
//
// QStorageInfo is cross-platform, so this file also compiles and runs on
// Linux — that is deliberate, it is how the Windows backend gets tested.

#include <QStorageInfo>
#include <QTimer>

#include "ExternalDeviceManager.h"
#include "DeviceId.h"
#include "ExternalDevice.h"

namespace {

// Windows has no cheap mount notification without a message-only window, so
// the volume list is polled. Slow enough to be free, fast enough that a
// plugged-in stick shows up while the user is still reaching for the mouse.
constexpr int kPollIntervalMs = 2000;

} // namespace

void ExternalDeviceManager::initBackend()
{
    auto* timer = new QTimer(this);
    timer->setInterval(kPollIntervalMs);
    connect(timer, &QTimer::timeout, this, &ExternalDeviceManager::refreshMounts);
    timer->start();
}

QList<ExternalDevice> ExternalDeviceManager::allDevices()
{
    instance();

    QList<ExternalDevice> result;
    const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo& si : volumes) {
        // The root volume is the system drive — never a removable device.
        if (si.isRoot() || !si.isReady() || si.rootPath().isEmpty())
            continue;

        const QString deviceNode = QString::fromUtf8(si.device());
        result.append(ExternalDevice(DeviceId(deviceNode),
                                     si.name().trimmed(),
                                     QString::fromUtf8(si.fileSystemType()),
                                     deviceNode,
                                     si.rootPath()));
    }
    return result;
}

void ExternalDeviceManager::mountAsync(const ExternalDevice& device,
                                       QObject* context, OpCallback done)
{
    // Windows mounts removable media automatically; there is nothing to do
    // and nothing sensible to offer if it did not happen.
    Q_UNUSED(device);
    QTimer::singleShot(0, context, [done]() {
        OpResult result;
        result.error = QObject::tr("Mounting is handled by the operating system.");
        done(result);
    });
}

void ExternalDeviceManager::unmountAsync(const ExternalDevice& device,
                                         QObject* context, OpCallback done)
{
    Q_UNUSED(device);
    QTimer::singleShot(0, context, [done]() {
        OpResult result;
        result.error = QObject::tr("Ejecting must be done through the system's own dialog.");
        done(result);
    });
}
