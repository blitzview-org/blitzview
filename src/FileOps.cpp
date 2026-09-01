#include "FileOps.h"

#include "Path.h"

#include <QClipboard>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QObject>

namespace {

// Recursive directory copy; returns false on the first failure.
bool copyDirRecursively(const QString& srcDir, const QString& dstDir)
{
    QDir dst(dstDir);
    if (!dst.exists() && !QDir().mkpath(dstDir))
        return false;

    QDirIterator it(srcDir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString dstPath = dstDir + QLatin1Char('/') + fi.fileName();
        if (fi.isDir()) {
            if (!copyDirRecursively(fi.absoluteFilePath(), dstPath))
                return false;
        } else {
            if (!QFile::copy(fi.absoluteFilePath(), dstPath))
                return false;
        }
    }
    return true;
}

bool copyEntry(const QFileInfo& src, const QString& dstPath)
{
    if (src.isDir())
        return copyDirRecursively(src.absoluteFilePath(), dstPath);
    return QFile::copy(src.absoluteFilePath(), dstPath);
}

bool moveEntry(const QFileInfo& src, const QString& dstPath)
{
    // rename works for files and directories on the same filesystem
    if (QFile::rename(src.absoluteFilePath(), dstPath))
        return true;

    // Cross-device fallback: copy, then delete the source
    if (!copyEntry(src, dstPath))
        return false;
    if (src.isDir())
        return QDir(src.absoluteFilePath()).removeRecursively();
    return QFile::remove(src.absoluteFilePath());
}

} // namespace

namespace FileOps {

std::optional<Op> askDropMode(QWidget* parent, const QPoint& globalPos)
{
    QMenu menu(parent);
    QAction* actCopy = menu.addAction(QObject::tr("&Copy Here"));
    QAction* actMove = menu.addAction(QObject::tr("&Move Here"));
    menu.addSeparator();
    menu.addAction(QObject::tr("C&ancel"));

    QAction* chosen = menu.exec(globalPos);
    if (chosen == actCopy)
        return Op::Copy;
    if (chosen == actMove)
        return Op::Move;
    return std::nullopt;
}

int perform(const QList<QUrl>& urls, const QString& targetDir, Op op,
            QWidget* parent)
{
    const QDir target(targetDir);
    if (!target.exists())
        return 0;

    int transferred = 0;
    QStringList skipped;
    QStringList failed;

    for (const QUrl& url : urls) {
        if (!url.isLocalFile())
            continue;

        const QFileInfo src(url.toLocalFile());
        if (!src.exists()) {
            failed.append(src.fileName());
            continue;
        }

        // Source already inside the target directory
        if (src.absolutePath() == target.absolutePath()) {
            skipped.append(src.fileName());
            continue;
        }

        const QString dstPath = target.absoluteFilePath(src.fileName());
        if (QFileInfo::exists(dstPath)) {
            skipped.append(src.fileName());
            continue;
        }

        const bool ok = (op == Op::Copy) ? copyEntry(src, dstPath)
                                         : moveEntry(src, dstPath);
        if (ok)
            ++transferred;
        else
            failed.append(src.fileName());
    }

    if (!skipped.isEmpty() || !failed.isEmpty()) {
        QString msg;
        if (!skipped.isEmpty())
            msg += QObject::tr("Skipped (already exists in target):\n%1")
                       .arg(skipped.join(QLatin1String("\n")));
        if (!failed.isEmpty()) {
            if (!msg.isEmpty())
                msg += QLatin1String("\n\n");
            msg += QObject::tr("Failed:\n%1").arg(failed.join(QLatin1String("\n")));
        }
        QMessageBox::warning(parent, QObject::tr("File Transfer"), msg);
    }

    return transferred;
}

QMimeData* makeClipboardMime(const QList<QUrl>& urls, Op op, Path::Flavor flavor)
{
    const bool cut = (op == Op::Move);

    auto* mime = new QMimeData;
    mime->setUrls(urls);

    // GNOME/Nautilus convention: first line "cut"/"copy", then the URIs.
    // MATE/Caja forked the format under its own MIME name — same payload.
    QByteArray gnome = cut ? QByteArrayLiteral("cut") : QByteArrayLiteral("copy");
    for (const QUrl& u : urls) {
        gnome += '\n';
        gnome += u.toEncoded();
    }
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnome);
    mime->setData(QStringLiteral("x-special/mate-copied-files"), gnome);

    // KDE convention: "1" = cut, "0" = copy
    mime->setData(QStringLiteral("application/x-kde-cutselection"),
                  cut ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));

    // Windows shell convention: a little-endian DWORD DROPEFFECT.
    // DROPEFFECT_MOVE = 2, DROPEFFECT_COPY|LINK = 1|4 = 5. Without this,
    // Explorer treats every paste as a copy.
    //
    // A runtime check rather than #ifdef, so the same binary can be tested
    // both ways — the extra format is inert on X11/Wayland anyway.
    if (flavor == Path::Flavor::Windows) {
        QByteArray effect(4, '\0');
        effect[0] = cut ? char(2) : char(5);
        mime->setData(QStringLiteral("Preferred DropEffect"), effect);
    }

    return mime;
}

void setClipboardFiles(const QList<QUrl>& urls, Op op)
{
    if (urls.isEmpty())
        return;

    QGuiApplication::clipboard()->setMimeData(makeClipboardMime(urls, op)); // takes ownership
}

Op opFromMime(const QMimeData* mime)
{
    if (!mime)
        return Op::Copy;

    const QByteArray gnome = mime->data(QStringLiteral("x-special/gnome-copied-files"));
    const QByteArray mate  = mime->data(QStringLiteral("x-special/mate-copied-files"));
    const QByteArray kde   = mime->data(QStringLiteral("application/x-kde-cutselection"));
    if (gnome.startsWith("cut") || mate.startsWith("cut")
        || kde == QByteArrayLiteral("1"))
        return Op::Move;

    const QByteArray effect = mime->data(QStringLiteral("Preferred DropEffect"));
    if (effect.size() >= 1 && (effect.at(0) & 2))
        return Op::Move;

    return Op::Copy;
}

ClipboardFiles clipboardFiles()
{
    ClipboardFiles result;
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls())
        return result;

    result.urls = mime->urls();
    result.op   = opFromMime(mime);
    return result;
}

} // namespace FileOps
