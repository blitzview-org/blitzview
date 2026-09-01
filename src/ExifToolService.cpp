#include "ExifToolService.h"
#include "MetadataCache.h"
#include "SlideTrace.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStandardPaths>
#include <QTimeZone>

namespace {

// exiftool timestamps: "YYYY:MM:DD HH:MM:SS" with optional subseconds and
// timezone suffix ("+02:00" / "Z"). Local time when no zone is given.
QDateTime parseExifDateTime(QString s)
{
    s = s.trimmed();
    if (s.isEmpty() || s.startsWith(QLatin1String("0000")))
        return {};

    // Normalize "YYYY:MM:DD" date part to ISO so QDateTime can parse it
    if (s.size() >= 10 && s[4] == QLatin1Char(':') && s[7] == QLatin1Char(':')) {
        s[4] = QLatin1Char('-');
        s[7] = QLatin1Char('-');
    }
    s.replace(QLatin1Char(' '), QLatin1Char('T'));

    QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(s.left(19), Qt::ISODate);
    return dt;
}

// "+0100" / "-0530" / "+01:00" → offset seconds; nullopt when unparseable
std::optional<int> parseTzOffsetSeconds(QString s)
{
    s = s.trimmed().remove(QLatin1Char(':'));
    if (s.size() != 5 || (s[0] != QLatin1Char('+') && s[0] != QLatin1Char('-')))
        return std::nullopt;
    bool okH = false, okM = false;
    const int h = s.mid(1, 2).toInt(&okH);
    const int m = s.mid(3, 2).toInt(&okM);
    if (!okH || !okM)
        return std::nullopt;
    const int secs = h * 3600 + m * 60;
    return (s[0] == QLatin1Char('-')) ? -secs : secs;
}

// The wall clock at the capture site, represented as a LOCAL QDateTime —
// the same convention as naive EXIF DateTimeOriginal, so photos and videos
// display and sort consistently.
QDateTime toCaptureWallClock(const QDateTime& instant, int siteOffsetSeconds)
{
    const QDateTime onSite = instant.toOffsetFromUtc(siteOffsetSeconds);
    return QDateTime(onSite.date(), onSite.time());
}

// Tag values come as JSON string ("single") or array (["a","b"])
QStringList jsonToTagList(const QJsonValue& v)
{
    QStringList out;
    if (v.isArray()) {
        const QJsonArray arr = v.toArray();
        for (const QJsonValue& e : arr) {
            const QString s = e.toString().trimmed();
            if (!s.isEmpty())
                out.append(s);
        }
    } else if (v.isString()) {
        const QString s = v.toString().trimmed();
        if (!s.isEmpty())
            out.append(s);
    }
    return out;
}

} // namespace

ExifToolService& ExifToolService::instance()
{
    static ExifToolService service;
    return service;
}

ExifToolService::ExifToolService() = default;

ExifToolService::~ExifToolService()
{
    shutdown();
}

bool ExifToolService::isAvailable()
{
    return ensureStarted();
}

bool ExifToolService::ensureStarted()
{
    if (m_proc && m_proc->state() == QProcess::Running)
        return true;
    if (m_startFailed || m_shutdown)
        return false;

    // Look next to the executable first: that is where a bundled exiftool
    // sits in the portable Windows package. Falls back to PATH, which is how
    // it is found on Linux (perl-image-exiftool) and in a dev build.
    QString exe = QStandardPaths::findExecutable(QStringLiteral("exiftool"),
                      { QCoreApplication::applicationDirPath() });
    if (exe.isEmpty())
        exe = QStandardPaths::findExecutable(QStringLiteral("exiftool"));
    if (exe.isEmpty()) {
        qWarning() << "ExifToolService: exiftool not found — metadata features disabled";
        m_startFailed = true;
        return false;
    }

    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_proc, &QProcess::readyReadStandardOutput,
            this, &ExifToolService::onReadyRead);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        qWarning() << "ExifToolService: exiftool process error —"
                   << (m_proc ? m_proc->errorString() : QString());
        m_startFailed = true;
    });

    const qint64 t0 = slideTraceMs();
    m_proc->start(exe, {QStringLiteral("-stay_open"), QStringLiteral("True"),
                        QStringLiteral("-@"), QStringLiteral("-")});
    const bool started = m_proc->waitForStarted(3000);
    TRACE_SLIDE("exiftool spawn ok=%d dur=%lldms", started,
                (long long)(slideTraceMs() - t0));
    if (!started) {
        qWarning() << "ExifToolService: failed to start exiftool";
        m_proc->deleteLater();
        m_proc = nullptr;
        m_startFailed = true;
        return false;
    }
    return true;
}

void ExifToolService::request(const QStringList& filePaths)
{
    if (filePaths.isEmpty())
        return; // don't spawn the daemon for nothing (fully cached directory)
    if (!ensureStarted())
        return;

    auto& cache = MetadataCache::instance();
    for (const QString& fp : filePaths) {
        if (!cache.peek(fp) && !m_queuedSet.contains(fp) && !m_inFlightSet.contains(fp)) {
            m_queue.append(fp);
            m_queuedSet.insert(fp);
        }
    }
    pumpQueue();
}

void ExifToolService::requestNow(const QString& filePath)
{
    if (!ensureStarted())
        return;
    if (MetadataCache::instance().peek(filePath) || m_inFlightSet.contains(filePath))
        return;
    m_queue.removeAll(filePath);
    m_queue.prepend(filePath);
    m_queuedSet.insert(filePath);
    pumpQueue();
}

void ExifToolService::queueWrite(const QStringList& filePaths, const QStringList& args)
{
    if (filePaths.isEmpty() || args.isEmpty() || !ensureStarted())
        return;
    m_writeJobs.append({filePaths, args});
    pumpQueue();
}

void ExifToolService::pumpQueue()
{
    if (m_busy || !m_proc)
        return;

    // Writes first — they are user-initiated and small
    if (!m_writeJobs.isEmpty()) {
        const WriteJob job = m_writeJobs.takeFirst();
        m_inFlight = job.paths;
        m_inFlightSet = QSet<QString>(m_inFlight.cbegin(), m_inFlight.cend());
        m_currentIsWrite = true;
        m_busy = true;
        m_buffer.clear();

        QByteArray cmd;
        cmd += "-q\n-overwrite_original\n";
        for (const QString& a : job.args) {
            cmd += a.toUtf8();
            cmd += '\n';
        }
        for (const QString& fp : job.paths) {
            cmd += fp.toUtf8();
            cmd += '\n';
        }
        cmd += "-echo3\n{batchdone}\n-execute\n";
        m_proc->write(cmd);
        return;
    }

    if (m_queue.isEmpty())
        return;

    const int n = qMin<qsizetype>(kBatchSize, m_queue.size());
    m_inFlight = m_queue.mid(0, n);
    m_inFlightSet = QSet<QString>(m_inFlight.cbegin(), m_inFlight.cend());
    for (const QString& fp : std::as_const(m_inFlight))
        m_queuedSet.remove(fp);
    m_queue.remove(0, n);
    m_currentIsWrite = false;
    m_busy = true;
    m_buffer.clear();

    // One argument per line; -execute flushes the command. -q keeps the JSON
    // clean but ALSO suppresses the standard "{ready}" marker — so the batch
    // end is signalled by our own -echo3 marker instead.
    QByteArray cmd;
    cmd += "-j\n-q\n-fast\n";
    // QuickTime dates are stored in UTC; without this they'd be reported as
    // naive local time, off by the timezone for phone videos
    cmd += "-api\nQuickTimeUTC\n";
    cmd += "-DateTimeOriginal\n-CreateDate\n-MediaCreateDate\n";
    // Capture-site timezone of videos: Android writes Keys:AndroidTimeZone
    // ("+0100"), iPhones write Keys:CreationDate (full date incl. offset)
    cmd += "-Keys:AndroidTimeZone\n-Keys:CreationDate\n";
    cmd += "-XMP:Subject\n-IPTC:Keywords\n";
    for (const QString& fp : std::as_const(m_inFlight)) {
        cmd += fp.toUtf8();
        cmd += '\n';
    }
    cmd += "-echo3\n{batchdone}\n-execute\n";
    TRACE_SLIDE("exiftool batch sent n=%d queued=%d", int(m_inFlight.size()),
                int(m_queue.size()));
    m_proc->write(cmd);
}

void ExifToolService::onReadyRead()
{
    m_buffer += m_proc->readAllStandardOutput();

    const int readyPos = m_buffer.indexOf("{batchdone}");
    if (readyPos < 0)
        return;

    const QByteArray output = m_buffer.left(readyPos);
    m_buffer.clear();

    const qint64 t0 = slideTraceMs();
    if (m_currentIsWrite) {
        const QStringList paths = m_inFlight;
        m_inFlight.clear();
        m_inFlightSet.clear();
        emit writeFinished(paths);
    } else {
        handleResult(output);
    }
    TRACE_SLIDE("exiftool batch handled bytes=%lld dur=%lldms",
                (long long)output.size(), (long long)(slideTraceMs() - t0));

    m_busy = false;
    pumpQueue();
}

void ExifToolService::handleResult(const QByteArray& json)
{
    const QStringList batch = m_inFlight;
    m_inFlight.clear();
    m_inFlightSet.clear();

    auto& cache = MetadataCache::instance();
    QStringList readyPaths;

    const QJsonDocument doc = QJsonDocument::fromJson(json.trimmed());
    const QJsonArray entries = doc.array();
    for (const QJsonValue& ev : entries) {
        const QJsonObject o = ev.toObject();
        const QString path = o.value(QLatin1String("SourceFile")).toString();
        if (path.isEmpty())
            continue;

        MediaMetadata meta;
        meta.valid = true;

        // Goal: the wall clock AT THE CAPTURE SITE, consistently for photos
        // and videos.
        // Images: DateTimeOriginal is that wall clock by definition (naive).
        // Videos: the QuickTime dates are UTC instants — convert them using
        // the capture-site offset when the phone recorded one:
        //   iPhone:  Keys:CreationDate carries the full local date incl.
        //            offset — its wall clock is what we want.
        //   Android: Keys:AndroidTimeZone ("+0100") + the UTC instant.
        // Without either, fall back to the instant in viewer-local time.
        meta.taken = parseExifDateTime(
            o.value(QLatin1String("DateTimeOriginal")).toString());
        if (!meta.taken.isValid()) {
            const QDateTime creation = parseExifDateTime(
                o.value(QLatin1String("CreationDate")).toString());
            QDateTime instant;
            for (const char* key : {"MediaCreateDate", "CreateDate"}) {
                instant = parseExifDateTime(o.value(QLatin1String(key)).toString());
                if (instant.isValid())
                    break;
            }
            const auto siteOffset = parseTzOffsetSeconds(
                o.value(QLatin1String("AndroidTimeZone")).toString());

            if (creation.isValid()) {
                // Parsed with OffsetFromUTC spec: date()/time() are the
                // as-written on-site values — exactly the wanted wall clock
                meta.taken = QDateTime(creation.date(), creation.time());
            } else if (instant.isValid() && siteOffset) {
                meta.taken = toCaptureWallClock(instant, *siteOffset);
            } else {
                meta.taken = instant;
            }
        }

        // AVES convention: XMP dc:subject primary, IPTC keywords merged in
        meta.tags = jsonToTagList(o.value(QLatin1String("Subject")));
        const QStringList iptc = jsonToTagList(o.value(QLatin1String("Keywords")));
        for (const QString& t : iptc)
            if (!meta.tags.contains(t))
                meta.tags.append(t);

        cache.put(path, meta);
        readyPaths.append(path);
    }

    // Files exiftool produced no entry for (unreadable/no metadata section):
    // record an empty result so they are not re-queried forever
    for (const QString& fp : batch) {
        if (!readyPaths.contains(fp) && !cache.peek(fp)) {
            MediaMetadata empty;
            empty.valid = true;
            cache.put(fp, empty);
            readyPaths.append(fp);
        }
    }

    if (!readyPaths.isEmpty())
        emit metadataReady(readyPaths);
}

void ExifToolService::shutdown()
{
    m_shutdown = true;
    m_queue.clear();
    m_queuedSet.clear();
    m_writeJobs.clear();
    if (!m_proc)
        return;

    // The process dies ON PURPOSE here — detach all handlers first, or the
    // deliberate kill() below fires errorOccurred and gets reported as
    // "Process crashed" on every exit.
    disconnect(m_proc, nullptr, this, nullptr);

    // Only a WRITE in flight deserves the polite goodbye (killing exiftool
    // mid-write risks temp-file litter next to originals). Read batches are
    // idempotent — waiting for one to finish held the app's exit hostage
    // for up to 2 s whenever a large directory was still being read.
    if (m_busy && m_currentIsWrite) {
        m_proc->write("-stay_open\nFalse\n");
        m_proc->closeWriteChannel();
        if (!m_proc->waitForFinished(2000))
            m_proc->kill();
    } else {
        m_proc->kill();
        m_proc->waitForFinished(200);
    }
    m_proc->deleteLater();
    m_proc = nullptr;
}
