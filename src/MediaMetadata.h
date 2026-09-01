#pragma once

#include <QDataStream>
#include <QDateTime>
#include <QStringList>

// Media metadata read via exiftool: capture time and user tags.
// Tags follow the AVES convention: XMP dc:subject is the primary store,
// IPTC keywords are merged in when present.
struct MediaMetadata {
    QDateTime   taken;   // capture time (invalid = unknown)
    QStringList tags;    // user tags (XMP subject ∪ IPTC keywords)
    bool        valid = false;  // true once read (even if fields are empty)
};

inline QDataStream& operator<<(QDataStream& ds, const MediaMetadata& m)
{
    ds << (m.taken.isValid() ? m.taken.toMSecsSinceEpoch() : qint64(0));
    ds << m.tags;
    return ds;
}

inline QDataStream& operator>>(QDataStream& ds, MediaMetadata& m)
{
    qint64 msecs = 0;
    ds >> msecs >> m.tags;
    m.taken = (msecs != 0) ? QDateTime::fromMSecsSinceEpoch(msecs) : QDateTime();
    m.valid = true;
    return ds;
}
