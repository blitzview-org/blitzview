#pragma once

#include <QAbstractProxyModel>

// Adds one blank row before the first and one after the last source row.
// The list view uses them as the DUMMY SPACE its Ctrl+wheel zoom anchor
// needs to hold a row the natural scroll range cannot reach.
//
// They are REAL rows on purpose: Qt then derives the scroll range, the
// scrollbar's visibility and the painting from them by itself. The two
// earlier attempts each broke one of those — widening the scrollbar range
// fought Qt's own clamping, and a viewport margin produced no scrollbar at
// all (besides dragging the column header down with it).
//
// The rows always exist; their HEIGHT carries the amount (0 = not needed),
// so the row mapping is a constant offset and never changes underneath the
// view's selection or hover state.
class PaddedListProxy : public QAbstractProxyModel
{
    Q_OBJECT
public:
    explicit PaddedListProxy(QObject* parent = nullptr);

    // Blank rows per side (leading is row 0, trailing the last row)
    static constexpr int kPadRows = 1;

    bool isPadRow(int proxyRow) const;
    int  trailingPadRow() const;   // -1 without a source model

    void setSourceModel(QAbstractItemModel* source) override;
    QModelIndex mapToSource(const QModelIndex& proxyIndex) const override;
    QModelIndex mapFromSource(const QModelIndex& sourceIndex) const override;
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
};
