#pragma once

#include <QObject>
#include <QProcess>
#include <QSet>
#include <QStringList>

// Reads media metadata (capture time, user tags) through ONE persistent
// exiftool process (-stay_open mode): commands go in via stdin, batches of
// files per command, JSON comes back terminated by a "{ready}" marker.
// Per-file process spawns would cost ~100ms Perl startup each — the daemon
// amortizes that to a few ms per file in batches.
//
// Results land in MetadataCache; metadataReady() reports the file paths.
// If exiftool is not installed the service disables itself (one warning);
// runtime dependency: perl-image-exiftool.
class ExifToolService : public QObject
{
    Q_OBJECT
public:
    static ExifToolService& instance();

    bool isAvailable();

    // Queues paths for background metadata reading (skips paths already in
    // MetadataCache memory). Batches are processed FIFO.
    void request(const QStringList& filePaths);

    // Front-of-queue single file (details panel showing "–" right now).
    void requestNow(const QString& filePath);

    // Queues a metadata WRITE: args are exiftool arguments (one per entry,
    // e.g. "-XMP-dc:Subject+=beach"), applied to all paths in one command.
    // Jobs run FIFO on the same daemon, so multiple queued writes keep their
    // order. writeFinished() fires when the command completed.
    void queueWrite(const QStringList& filePaths, const QStringList& args);

    // Terminates the daemon AND permanently disables the service: nothing
    // may respawn exiftool after the app decided to quit (a late request()
    // from a finishing prime future used to start a fresh daemon that then
    // outlived the event loop).
    void shutdown();

signals:
    // Paths whose metadata just arrived in MetadataCache
    void metadataReady(const QStringList& filePaths);
    // A queued write command finished (files rewritten on disk)
    void writeFinished(const QStringList& filePaths);

private slots:
    void onReadyRead();

private:
    ExifToolService();
    ~ExifToolService() override;

    bool ensureStarted();
    void pumpQueue();          // send next batch when idle
    void handleResult(const QByteArray& json);

    struct WriteJob {
        QStringList paths;
        QStringList args;
    };

    QProcess*   m_proc = nullptr;
    bool        m_startFailed = false;
    bool        m_shutdown = false;     // quit requested — never respawn
    bool        m_busy = false;        // a command is in flight
    bool        m_currentIsWrite = false;
    QStringList m_queue;               // pending read paths
    // Membership mirror of m_queue/m_inFlight: request() is called with the
    // WHOLE directory (tens of thousands of paths) — a linear contains() per
    // path made queueing quadratic and blocked the UI thread for seconds.
    QSet<QString> m_queuedSet;
    QList<WriteJob> m_writeJobs;       // pending writes (processed before reads)
    QStringList m_inFlight;            // paths of the current command
    QSet<QString> m_inFlightSet;
    QByteArray  m_buffer;              // stdout accumulator until marker

    static constexpr int kBatchSize = 100;
};
