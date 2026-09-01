#include "PaddedListProxy.h"

#include <QApplication>
#include <QPalette>

PaddedListProxy::PaddedListProxy(QObject* parent)
    : QAbstractProxyModel(parent)
{
}

bool PaddedListProxy::isPadRow(int proxyRow) const
{
    return proxyRow < kPadRows || proxyRow >= rowCount() - kPadRows;
}

int PaddedListProxy::trailingPadRow() const
{
    return sourceModel() ? rowCount() - 1 : -1;
}

void PaddedListProxy::setSourceModel(QAbstractItemModel* source)
{
    if (sourceModel())
        sourceModel()->disconnect(this);

    beginResetModel();
    QAbstractProxyModel::setSourceModel(source);
    endResetModel();

    if (!source)
        return;

    // MediaModel signals every structural change as a full reset (scan,
    // sort, filter); dataChanged carries thumbnails and metadata.
    connect(source, &QAbstractItemModel::modelAboutToBeReset,
            this, [this]() { beginResetModel(); });
    connect(source, &QAbstractItemModel::modelReset,
            this, [this]() { endResetModel(); });
    connect(source, &QAbstractItemModel::layoutAboutToBeChanged,
            this, [this]() { beginResetModel(); });
    connect(source, &QAbstractItemModel::layoutChanged,
            this, [this]() { endResetModel(); });
    connect(source, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& tl, const QModelIndex& br,
                   const QList<int>& roles) {
        emit dataChanged(mapFromSource(tl), mapFromSource(br), roles);
    });
}

QModelIndex PaddedListProxy::mapToSource(const QModelIndex& proxyIndex) const
{
    if (!proxyIndex.isValid() || !sourceModel() || isPadRow(proxyIndex.row()))
        return {};
    return sourceModel()->index(proxyIndex.row() - kPadRows,
                                proxyIndex.column());
}

QModelIndex PaddedListProxy::mapFromSource(const QModelIndex& sourceIndex) const
{
    if (!sourceIndex.isValid())
        return {};
    return index(sourceIndex.row() + kPadRows, sourceIndex.column());
}

QModelIndex PaddedListProxy::index(int row, int column,
                                    const QModelIndex& parent) const
{
    if (parent.isValid() || row < 0 || row >= rowCount()
        || column < 0 || column >= columnCount())
        return {};
    return createIndex(row, column);
}

QModelIndex PaddedListProxy::parent(const QModelIndex&) const
{
    return {};
}

int PaddedListProxy::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid() || !sourceModel())
        return 0;
    return sourceModel()->rowCount() + 2 * kPadRows;
}

int PaddedListProxy::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid() || !sourceModel())
        return 0;
    return sourceModel()->columnCount();
}

QVariant PaddedListProxy::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};
    if (isPadRow(index.row())) {
        // Plain background: the pad rows are blank SPACE, so the view's
        // alternating row colors must not stripe them.
        if (role == Qt::BackgroundRole)
            return QApplication::palette().base();
        return {};
    }
    return QAbstractProxyModel::data(index, role);
}

QVariant PaddedListProxy::headerData(int section, Qt::Orientation orientation,
                                      int role) const
{
    if (!sourceModel())
        return {};
    if (orientation == Qt::Vertical)
        return {};   // hidden anyway, and the pad rows would shift the numbers
    return sourceModel()->headerData(section, orientation, role);
}

Qt::ItemFlags PaddedListProxy::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    if (isPadRow(index.row()))
        return Qt::NoItemFlags;   // not selectable, not hoverable, no drag
    return QAbstractProxyModel::flags(index);
}
