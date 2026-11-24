#pragma once

#include <QObject>
#include <QList>
#include <functional>
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

    /// Outcome of mountAsync() / unmountAsync().
    struct OpResult {
        bool ok = false;
        /// The user cancelled the authorization dialog — not an error worth
        /// reporting.
        bool dismissed = false;
        QString mountPath;  // mountAsync() only
        QString error;      // human-readable reason when !ok
    };
    using OpCallback = std::function<void(const OpResult&)>;

    /// Starts mounting @p device without blocking. @p done runs later in
    /// @p context's thread — never synchronously, and not at all if
    /// @p context is destroyed first. Backends without mount support always
    /// report failure.
    static void mountAsync(const ExternalDevice& device, QObject* context,
                           OpCallback done);

    /// Starts unmounting @p device; same callback contract as mountAsync().
    static void unmountAsync(const ExternalDevice& device, QObject* context,
                             OpCallback done);

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
