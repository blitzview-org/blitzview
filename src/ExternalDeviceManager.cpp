// Backend-independent part of ExternalDeviceManager. The platform backends
// live in ExternalDeviceManagerUdisks.cpp (Linux) and
// ExternalDeviceManagerDrives.cpp (everything else); exactly one of them is
// compiled in, see CMakeLists.txt.

#include "ExternalDeviceManager.h"
#include "ExternalDevice.h"

ExternalDeviceManager::ExternalDeviceManager(QObject* parent)
    : QObject(parent)
{
    initBackend();
}

ExternalDeviceManager* ExternalDeviceManager::instance()
{
    static ExternalDeviceManager inst; // function-static singleton
    // Populate known mounts on first call (after construction is complete)
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        const auto devices = allDevices();
        for (const auto& dev : devices) {
            if (!dev.mountPath.isEmpty())
                inst.m_knownMounts.insert(dev.mountPath);
        }
    }
    return &inst;
}

QList<QString> ExternalDeviceManager::allMountPaths()
{
    QList<ExternalDevice> devices = allDevices();
    QList<QString> result = {};
    for (const ExternalDevice& dev : devices) {
        if (!dev.mountPath.isEmpty()) {
            result.append(dev.mountPath);
        }
    }
    return result;
}

void ExternalDeviceManager::refreshMounts()
{
    QSet<QString> currentMounts;
    const auto devices = allDevices();
    for (const auto& dev : devices) {
        if (!dev.mountPath.isEmpty())
            currentMounts.insert(dev.mountPath);
    }

    // Detect disappeared mounts
    for (const QString& path : m_knownMounts) {
        if (!currentMounts.contains(path)) {
            emit mountChanged(path, false);
        }
    }

    // Detect new mounts
    for (const QString& path : currentMounts) {
        if (!m_knownMounts.contains(path)) {
            emit mountChanged(path, true);
        }
    }

    m_knownMounts = currentMounts;
}
