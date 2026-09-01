#include "ElidedLabel.h"

#include <QResizeEvent>

ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent)
{
    // Ignored horizontal policy: the layout never widens for the content
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTextInteractionFlags(Qt::TextSelectableByMouse);
}

void ElidedLabel::setFullText(const QString& text)
{
    m_fullText = text;
    setToolTip(text);
    updateElide();
}

void ElidedLabel::resizeEvent(QResizeEvent* event)
{
    QLabel::resizeEvent(event);
    updateElide();
}

void ElidedLabel::updateElide()
{
    const QFontMetrics fm(font());
    setText(fm.elidedText(m_fullText, Qt::ElideMiddle, qMax(0, width())));
}
