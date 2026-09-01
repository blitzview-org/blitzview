#pragma once

#include <QString>
#include "DeviceId.h"

struct ExternalDevice
{
    DeviceId deviceId;
    QString volumeLabel;
    QString modelName;
    QString deviceName;
    QString mountPath;

    QString displayName() const {
        if (!volumeLabel.isEmpty()) return volumeLabel;
        else if (!modelName.isEmpty()) return modelName;
        else if (!mountPath.isEmpty()) return mountPath;
        else return deviceId.toString();
    }

    ExternalDevice() = default;
    ExternalDevice(const DeviceId& id,
                   const QString& label,
                   const QString& model,
                   const QString& device,
                   const QString& mount)
        : deviceId(id),
          volumeLabel(label),
          modelName(model),
          deviceName(device),
          mountPath(mount) {}
};
