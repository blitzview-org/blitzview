#pragma once

#include <QStyledItemDelegate>
#include <QSize>

class GridDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit GridDelegate(QObject* parent = nullptr);

    void setIconSize(int size);
    int  iconSize() const { return m_iconSize; }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    void drawVideoOverlay(QPainter* painter, const QRect& dest) const;
    // uiFont: the same font the filename label uses — the badge size is
    // derived from it, so both stay constant across zoom (see .cpp)
    void drawTagBadges(QPainter* painter, const QRect& dest,
                       const QStringList& tags, const QFont& uiFont) const;

    int m_iconSize = 128;
};
