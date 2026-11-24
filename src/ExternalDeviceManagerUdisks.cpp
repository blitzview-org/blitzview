// Linux backend: removable devices via UDisks2 over the D-Bus system bus.
// Everything D-Bus is confined to this file so ExternalDeviceManager.h stays
// parsable on platforms without it.

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusArgument>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>

#include "ExternalDeviceManager.h"
#include "DeviceId.h"
#include "ExternalDevice.h"

using InterfaceMap = QMap<QString, QVariantMap>;
using ManagedObjectMap = QMap<QDBusObjectPath, InterfaceMap>;

Q_DECLARE_METATYPE(InterfaceMap)
Q_DECLARE_METATYPE(ManagedObjectMap)

namespace {

/// Carries the D-Bus slot. It cannot live on ExternalDeviceManager itself:
/// the slot signature is moc-visible and would drag QDBusMessage into the
/// public header.
class UDisksWatcher : public QObject
{
    Q_OBJECT
public:
    explicit UDisksWatcher(ExternalDeviceManager* owner)
        : QObject(owner), m_owner(owner) {}

private slots:
    void onPropertiesChanged(const QDBusMessage& message)
    {
        // Filter: only react to Filesystem interface changes
        const QList<QVariant> args = message.arguments();
        if (args.size() < 2)
            return;
        if (args.at(0).toString() != "org.freedesktop.UDisks2.Filesystem")
            return;

        m_owner->refreshMounts();
    }

private:
    ExternalDeviceManager* m_owner;
};

} // namespace

void ExternalDeviceManager::initBackend()
{
    qDBusRegisterMetaType<InterfaceMap>();
    qDBusRegisterMetaType<ManagedObjectMap>();

    // Listen for property changes on UDisks2 Filesystem interfaces.
    // When MountPoints changes, we detect mount/unmount events.
    auto* watcher = new UDisksWatcher(this);
    QDBusConnection::systemBus().connect(
        "org.freedesktop.UDisks2",           // service
        QString(),                            // any object path
        "org.freedesktop.DBus.Properties",    // interface
        "PropertiesChanged",                  // signal name
        watcher,
        SLOT(onPropertiesChanged(QDBusMessage)));
}

static QString cleanByteArray(const QVariant& v)
{
    const QByteArray raw = v.toByteArray();
    // Use the full QByteArray (handles embedded NULs) when converting
    return QString::fromUtf8(raw);
}

static QStringList extractMountPoints(const QVariant& variant)
{
    QStringList result;
    const QDBusArgument& dbusArg = variant.value<QDBusArgument>();
    dbusArg.beginArray();
    while (!dbusArg.atEnd())
    {
        QByteArray path;
        dbusArg >> path;
        result << QString::fromUtf8(path.constData());
    }
    dbusArg.endArray();
    return result;
}

QList<ExternalDevice> ExternalDeviceManager::allDevices()
{
    instance();

    QDBusInterface manager(
        "org.freedesktop.UDisks2",
        "/org/freedesktop/UDisks2",
        "org.freedesktop.DBus.ObjectManager",
        QDBusConnection::systemBus());

    if (!manager.isValid()) {
        qCritical() << "UDisks2 not available.";
        return {};
    }

    QDBusReply<ManagedObjectMap> reply = manager.call("GetManagedObjects");
    if (!reply.isValid()) {
        qCritical() << "DBus error:" << reply.error().message();
        return {};
    }

    const ManagedObjectMap objects = reply.value();

    QList<ExternalDevice> result;
    for (auto driveIt = objects.begin(); driveIt != objects.end(); ++driveIt)
    {
        const InterfaceMap &interfaces = driveIt.value();
        if (!interfaces.contains("org.freedesktop.UDisks2.Drive"))
            continue;

        const QVariantMap driveProps = interfaces["org.freedesktop.UDisks2.Drive"];
        QString connectionBus = driveProps.value("ConnectionBus").toString();
        bool hintSystem = driveProps.value("HintSystem").toBool();

        if (connectionBus != "usb" || hintSystem)
            continue;

        for (auto blockIt = objects.begin(); blockIt != objects.end(); ++blockIt)
        {
            const InterfaceMap &blockIfaces = blockIt.value();
            if (!blockIfaces.contains("org.freedesktop.UDisks2.Block"))
                continue;

            const QVariantMap blockProps = blockIfaces["org.freedesktop.UDisks2.Block"];
            // Defensive: ensure the Block object references a Drive
            if (!blockProps.contains("Drive"))
                continue;
            QDBusObjectPath drivePath = blockProps.value("Drive").value<QDBusObjectPath>();
            if (drivePath.path() != driveIt.key().path())
                continue;

            if (!blockIfaces.contains("org.freedesktop.UDisks2.Partition"))
                continue;

            QDBusObjectPath blockPath = blockIt.key();
            QString mountPoint;
            if (blockIfaces.contains("org.freedesktop.UDisks2.Filesystem"))
            {
                const QVariantMap fsProps = blockIfaces["org.freedesktop.UDisks2.Filesystem"];
                QVariant mpVariant = fsProps.value("MountPoints");
                if (!mpVariant.isNull())
                    mountPoint = extractMountPoints(mpVariant).value(0, QString());
            }

            result.append(ExternalDevice(DeviceId(blockPath.path()),
                                         blockProps.value("IdLabel").toString().trimmed(),
                                         driveProps.value("Model").toString(),
                                         cleanByteArray(blockProps.value("Device")),
                                         mountPoint));
        }
    }
    return result;
}

namespace {

// Generous on purpose: the call stays pending while the user types a password
// into the polkit dialog, and a timeout there would report a bogus failure
// for a mount that still goes through.
constexpr int kMountCallTimeoutMs = 5 * 60 * 1000;

/// Calls @p method on the device's Filesystem interface and hands the reply
/// to @p onReply once it arrives. The empty options map leaves
/// auth.no_user_interaction off, so the session's polkit agent may ask.
void callFilesystem(const ExternalDevice& device, const QString& method,
                    QObject* context,
                    std::function<void(const QDBusPendingCall&)> onReply)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        "org.freedesktop.UDisks2",
        device.deviceId.toString(),
        "org.freedesktop.UDisks2.Filesystem",
        method);
    msg << QVariantMap();

    const QDBusPendingCall call =
        QDBusConnection::systemBus().asyncCall(msg, kMountCallTimeoutMs);
    // Parented to the context: if it dies first, the watcher goes with it
    // and the reply is dropped.
    auto* watcher = new QDBusPendingCallWatcher(call, context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
                     [watcher, onReply = std::move(onReply)]() {
        onReply(*watcher);
        watcher->deleteLater();
    });
}

void fillError(ExternalDeviceManager::OpResult& result, const QDBusError& error)
{
    result.error = error.message();
    // org.freedesktop.UDisks2.Error.NotAuthorizedDismissed: the user closed
    // the polkit dialog.
    result.dismissed = error.name().endsWith("NotAuthorizedDismissed");
}

} // namespace

void ExternalDeviceManager::mountAsync(const ExternalDevice& device,
                                       QObject* context, OpCallback done)
{
    callFilesystem(device, "Mount", context,
                   [done = std::move(done)](const QDBusPendingCall& call) {
        const QDBusPendingReply<QString> reply = call;
        OpResult result;
        if (reply.isError()) {
            fillError(result, reply.error());
        } else {
            result.ok = true;
            result.mountPath = reply.value();
        }
        done(result);
    });
}

void ExternalDeviceManager::unmountAsync(const ExternalDevice& device,
                                         QObject* context, OpCallback done)
{
    callFilesystem(device, "Unmount", context,
                   [done = std::move(done)](const QDBusPendingCall& call) {
        const QDBusPendingReply<> reply = call;
        OpResult result;
        if (reply.isError())
            fillError(result, reply.error());
        else
            result.ok = true;
        done(result);
    });
}

#include "ExternalDeviceManagerUdisks.moc"
