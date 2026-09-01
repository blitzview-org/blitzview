#pragma once

#include <QObject>
#include <QList>
#include <QSet>
#include <QString>
#include "DeviceId.h"
#include "ExternalDevice.h"

/// Removable storage devices, with a per-platform backend.
///
/// Linux uses UDisks2 over D-Bus (ExternalDeviceManagerUdisks.cpp), everything
/// else enumerates volumes via QStorageInfo (ExternalDeviceManagerDrives.cpp).
/// The public surface here must stay free of any backend type — it is what
/// keeps the D-Bus dependency out of every translation unit that includes
/// this header.
class ExternalDeviceManager : public QObject
{
    Q_OBJECT

    explicit ExternalDeviceManager(QObject* parent = nullptr);

public:
    // function-static singleton
    static ExternalDeviceManager* instance();

    /// All currently known removable devices, mounted or not.
    static QList<ExternalDevice> allDevices();

    static QList<QString> allMountPaths();

    /// Mounts @p deviceName and returns the resulting mount path, or an empty
    /// string on failure (then @p errorOut, if given, holds the reason).
    /// Backends without mount support always fail.
    static QString mount(const QString& deviceName, QString* errorOut = nullptr);

    /// Unmounts @p deviceName. Returns false on failure, with the reason in
    /// @p errorOut if given.
    static bool unmount(const QString& deviceName, QString* errorOut = nullptr);

    /// Re-reads the mount state and emits mountChanged() for every difference.
    /// Called by the backends when they notice something may have changed.
    void refreshMounts();

signals:
    /// Emitted when a mount point appears or disappears.
    /// @p mountPath is the filesystem path (e.g. "/media/user/LABEL").
    /// @p mounted is true if the path was just mounted, false if unmounted.
    void mountChanged(const QString& mountPath, bool mounted);

private:
    /// Starts the platform backend's change notification. Defined per backend.
    void initBackend();

    QSet<QString> m_knownMounts;  // tracks currently mounted paths
};
