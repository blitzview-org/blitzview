#pragma once

#include <QFileSystemModel>
#include <QList>
#include <QUrl>

// Custom FileSystemModel that checks for actual subdirectories
// so empty directories don't show an expand arrow.
//
// Also accepts file drops onto directories: the drop is NOT performed by the
// model — it emits filesDroppedOnDir and returns false, so the drag source
// never deletes anything itself. The actual copy/move (chosen via popup) is
// done by the receiver of the signal.
class FileSystemModel : public QFileSystemModel
{
    Q_OBJECT
public:
    using QFileSystemModel::QFileSystemModel;

    bool hasChildren(const QModelIndex& parent) const override;

    Qt::ItemFlags flags(const QModelIndex& index) const override;
    Qt::DropActions supportedDropActions() const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action,
                         int row, int column, const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action,
                      int row, int column, const QModelIndex& parent) override;

signals:
    void filesDroppedOnDir(const QList<QUrl>& urls, const QString& targetDir);
};
