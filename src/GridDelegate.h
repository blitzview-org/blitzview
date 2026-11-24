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

    // Fixed chrome around the iconSize×iconSize thumb area, in px: it does
    // NOT scale with the zoom. paint()/sizeHint() and GridView's pin math
    // (the Ctrl+wheel zoom pins an in-THUMB point, so it has to know where
    // the thumb starts inside the cell) share these — never duplicate the
    // numbers. The cell is iconSize + 2*thumbInsetX() wide, so centring
    // the thumb horizontally lands exactly at thumbInsetX().
    static constexpr int thumbPadTop() { return 6; }
    static constexpr int thumbInsetX() { return 4; }

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
