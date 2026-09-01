#include "DeviceId.h"

DeviceId::DeviceId()
    : m_id() {}

DeviceId::DeviceId(const QString& id)
    : m_id(id) {}

bool DeviceId::isValid() const {
    return !m_id.isEmpty();
}

const QString& DeviceId::toString() const {
    return m_id;
}

bool DeviceId::operator==(const DeviceId& other) const {
    return m_id == other.m_id;
}

bool DeviceId::operator!=(const DeviceId& other) const {
    return m_id != other.m_id;
}

bool DeviceId::operator<(const DeviceId& other) const {
    return m_id < other.m_id;
}
