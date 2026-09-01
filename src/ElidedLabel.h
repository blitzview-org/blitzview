#pragma once

#include <QLabel>

// Single-line value label that elides instead of growing: its width hint is
// ignored by the layout, so content can NEVER change the parent's width.
// The full text is available as tooltip.
class ElidedLabel : public QLabel
{
public:
    explicit ElidedLabel(QWidget* parent = nullptr);
    void setFullText(const QString& text);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateElide();
    QString m_fullText;
};
