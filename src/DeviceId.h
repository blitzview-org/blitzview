#pragma once

#include <QString>

class DeviceId
{
public:
    DeviceId();
    explicit DeviceId(const QString& id);
    DeviceId(const DeviceId& other) = default;
    DeviceId& operator=(const DeviceId& other) = default;
    ~DeviceId() = default;

    bool isValid() const;
    const QString& toString() const;

    bool operator==(const DeviceId& other) const;
    bool operator!=(const DeviceId& other) const;
    bool operator<(const DeviceId& other) const;

private:
    QString m_id;
};

// Optional hash function for unordered/hash based containers.
// Qt 6 requires the size_t signature — the old uint one silently would not
// be picked up by QHash.
#include <QHash>
inline size_t qHash(const DeviceId& key, size_t seed = 0)
{
    return qHash(key.toString(), seed);
}
