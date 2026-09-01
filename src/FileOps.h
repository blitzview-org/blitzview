#pragma once

#include <QList>
#include <QPoint>
#include <QString>
#include <QUrl>
#include <optional>

#include "Path.h"

class QMimeData;
class QWidget;

// File operations for drag & drop. Synchronous, local files/directories only.
namespace FileOps {

enum class Op { Copy, Move };

// Dolphin-style popup at the drop position: "Copy Here / Move Here / Cancel".
// Returns nullopt when cancelled.
std::optional<Op> askDropMode(QWidget* parent, const QPoint& globalPos);

// Copies or moves the given URLs into targetDir. Directories are handled
// recursively; sources already inside targetDir and name conflicts are
// skipped. Shows a summary message box when something was skipped or failed.
// Returns the number of transferred entries.
int perform(const QList<QUrl>& urls, const QString& targetDir, Op op,
            QWidget* parent);

// Puts the files on the system clipboard as copy or cut, using the GNOME and
// KDE conventions so file managers perform the paste (move for cut) — the
// app itself never deletes anything.
void setClipboardFiles(const QList<QUrl>& urls, Op op);

// Reads file URLs and the cut/copy marker (GNOME/KDE/Windows conventions)
// from the system clipboard. Empty urls = no files on the clipboard.
struct ClipboardFiles {
    QList<QUrl> urls;
    Op op = Op::Copy;
};
ClipboardFiles clipboardFiles();

// The two above split into pure functions, so the marker round trip can be
// tested without touching the real clipboard.
// makeClipboardMime returns a newly allocated QMimeData; the caller owns it.
// The explicit flavor is what lets one binary check both platforms.
QMimeData* makeClipboardMime(const QList<QUrl>& urls, Op op, Path::Flavor flavor);
inline QMimeData* makeClipboardMime(const QList<QUrl>& urls, Op op)
{
    return makeClipboardMime(urls, op, Path::flavor());
}
Op opFromMime(const QMimeData* mime);

} // namespace FileOps
