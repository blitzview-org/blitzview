#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLoggingCategory>
#include <QTimer>
#include <QtGlobal>
#include "AppSettings.h"
#include "AppShutdown.h"
#include "ExifToolService.h"
#include "MainWindow.h"
#include "SlideTrace.h"

extern "C" {
#include <libavutil/log.h>
}

namespace {
QtMessageHandler g_prevMessageHandler = nullptr;

void filteredMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    if (context.category && qstrcmp(context.category, "qt.gui.imageio.jpeg") == 0)
        return;
    if (msg.contains(QStringLiteral("Invalid SOS parameters for sequential JPEG"), Qt::CaseInsensitive))
        return;
    if (msg.contains(QStringLiteral("FFmpeg log:"), Qt::CaseInsensitive))
        return;

    if (g_prevMessageHandler) {
        g_prevMessageHandler(type, context, msg);
        return;
    }

    qt_message_output(type, context, msg);
}
} // namespace

#include "QtDebugStacktrace.h"

int main(int argc, char* argv[])
{
    // Chain: drop noisy messages (FFmpeg log, jpeg warnings) first, then pass
    // the rest to the stacktrace debug handler which prints them.
    g_prevMessageHandler = qtDebugStacktraceHandler;
    qInstallMessageHandler(filteredMessageHandler);

    // Keep Qt Multimedia FFmpeg backend quiet in normal app usage.
    // Hardware DECODING stays enabled: vaapi covers AMD/Intel, cuda covers
    // NVIDIA (NVDEC). Only VDPAU is excluded from probing — it is obsolete
    // (NVIDIA uses NVDEC, Mesa is dropping it) and libvdpau prints a raw
    // stderr message ("Failed to open VDPAU backend ...") when the driver is
    // missing, bypassing av_log and Qt logging entirely, so it cannot be
    // filtered. A user-provided value is respected. Non-Linux platforms keep
    // Qt's defaults (D3D11/VideoToolbox).
#ifdef Q_OS_LINUX
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "vaapi,cuda");
#endif
    // BlitzView never encodes; the encoder capability probe would create HW
    // devices of ALL types — including VDPAU, with the same unfilterable
    // stderr noise (verified: this probe, not decoding, loads libvdpau).
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", "");
    qputenv("QT_FFMPEG_DEBUG", "0");
    qputenv("QT_LOGGING_RULES", "qt.multimedia.ffmpeg=false;qt.multimedia.ffmpeg.*=false;qt.gui.imageio.jpeg=false;qt.gui.imageio.jpeg.*=false;*.ffmpeg.*=false;*.multimedia.*=false");
    QLoggingCategory::setFilterRules(
        QStringLiteral("qt.multimedia.ffmpeg=false\n"
                       "qt.multimedia.ffmpeg.*=false\n"
                       "qt.gui.imageio.jpeg=false\n"
                       "qt.gui.imageio.jpeg.*=false\n"
                       "*.ffmpeg.*=false\n"
                       "*.multimedia.*=false"));

    QApplication app(argc, argv);
    av_log_set_level(AV_LOG_QUIET);
    app.setApplicationName("BlitzView");
    app.setOrganizationName("BlitzView");
#ifndef Q_OS_MACOS
    // On macOS, QApplication::setWindowIcon() does not just set a per-window
    // icon -- Qt's Cocoa platform plugin forwards it to
    // NSApplication.applicationIconImage, which overrides the Dock/Cmd+Tab
    // icon at runtime and hides the app bundle's own Info.plist icon
    // (CFBundleIconFile, see CMakeLists.txt's if(APPLE) block). Skipping this
    // call there lets the bundle's BlitzView.icns show through undisturbed,
    // same as every other properly bundled macOS app.
    app.setWindowIcon(QIcon(QStringLiteral(":/BlitzViewIcon.png")));
#endif

    AppSettings::rememberSystemDoubleClickInterval(QApplication::doubleClickInterval());
    if (const int ms = AppSettings::doubleClickIntervalMs(); ms > 0)
        QApplication::setDoubleClickInterval(ms);

    // Parse command-line arguments: optional directory paths
    QStringList initialDirs;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        QFileInfo info(args.at(i));
        if (info.isDir())
            initialDirs.append(info.canonicalFilePath());
    }

    // Quit handling for everything that outlives a single window: the
    // exiftool daemon dies here (while the event loop is still alive, and
    // exactly once even with several windows open), and the cache-maintenance
    // jobs on the global thread pool learn to bail out — Qt waits for that
    // pool in ~QCoreApplication, which is what kept the process alive for a
    // long time after the last window closed.
    QObject::connect(&app, &QApplication::aboutToQuit, []() {
        TRACE_SLIDE("shutdown aboutToQuit");
        setAppShuttingDown();
        ExifToolService::instance().shutdown();
    });

    MainWindow w(initialDirs);
    w.show();

    // Event-loop latency probe (trace builds only): a 50 ms timer whose
    // actual gap exceeds 200 ms means something blocked the GUI thread —
    // the log line brackets the stall for correlation with the other trace
    // points.
    if (slideTraceEnabled()) {
        auto* probe = new QTimer(&w);
        probe->setInterval(50);
        auto last = std::make_shared<qint64>(slideTraceMs());
        QObject::connect(probe, &QTimer::timeout, [last]() {
            const qint64 now = slideTraceMs();
            if (now - *last > 200)
                TRACE_SLIDE("UI STALL: event loop blocked ~%lld ms",
                            (long long)(now - *last));
            *last = now;
        });
        probe->start();
    }

    const int rc = app.exec();
    TRACE_SLIDE("shutdown event loop left");
    return rc;
}
