#include "FileSystemModel.h"

#include <QDirIterator>
#include <QMimeData>

bool FileSystemModel::hasChildren(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return QFileSystemModel::hasChildren(parent);

    const QString path = filePath(parent);
    if (path.isEmpty())
        return false;

    QDirIterator it(path, QDir::AllDirs | QDir::NoDotAndDotDot);
    return it.hasNext();
}

Qt::ItemFlags FileSystemModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags f = QFileSystemModel::flags(index);
    if (index.isValid() && isDir(index))
        f |= Qt::ItemIsDropEnabled;
    return f;
}

Qt::DropActions FileSystemModel::supportedDropActions() const
{
    return Qt::CopyAction | Qt::MoveAction;
}

bool FileSystemModel::canDropMimeData(const QMimeData* data, Qt::DropAction /*action*/,
                                      int /*row*/, int /*column*/,
                                      const QModelIndex& parent) const
{
    return data && data->hasUrls() && parent.isValid() && isDir(parent);
}

bool FileSystemModel::dropMimeData(const QMimeData* data, Qt::DropAction action,
                                   int row, int column, const QModelIndex& parent)
{
    if (!canDropMimeData(data, action, row, column, parent))
        return false;

    emit filesDroppedOnDir(data->urls(), filePath(parent));

    // Deliberately report "not performed": the source must never delete
    // dragged files itself. The receiver of the signal does the copy/move.
    return false;
}