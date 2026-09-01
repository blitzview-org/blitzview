#include "InputSettingsPage.h"

#include "AppSettings.h"

#include <QApplication>
#include <QCheckBox>
#include <QFormLayout>
#include <QSpinBox>

InputSettingsPage::InputSettingsPage(QWidget* parent) : SettingsPage(parent)
{
    QFormLayout* form = addForm();

    const int configuredMs = AppSettings::doubleClickIntervalMs();
    const int systemMs = AppSettings::systemDoubleClickInterval();

    m_doubleClickMs = new QSpinBox(this);
    m_doubleClickMs->setRange(100, 1000);
    m_doubleClickMs->setSingleStep(25);
    m_doubleClickMs->setSuffix(tr(" ms"));
    m_doubleClickMs->setValue(configuredMs > 0 ? configuredMs : systemMs);
    m_doubleClickMs->setToolTip(
        tr("Double-click detection time — how quickly two clicks must follow\n"
           "each other to count as a double-click (affects the whole\n"
           "application)."));

    m_doubleClickSystem = new QCheckBox(
        tr("System default (%1 ms)").arg(systemMs), this);
    m_doubleClickSystem->setChecked(configuredMs <= 0);
    m_doubleClickMs->setEnabled(configuredMs > 0);
    connect(m_doubleClickSystem, &QCheckBox::toggled,
            m_doubleClickMs, &QSpinBox::setDisabled);

    form->addRow(tr("Double-click time:"), m_doubleClickMs);
    form->addRow(QString(), m_doubleClickSystem);

    m_focusBorderPx = new QSpinBox(this);
    m_focusBorderPx->setRange(0, 8);
    m_focusBorderPx->setSuffix(tr(" px"));
    m_focusBorderPx->setSpecialValueText(tr("off"));
    m_focusBorderPx->setValue(AppSettings::focusBorderWidth());
    m_focusBorderPx->setToolTip(
        tr("Width of the border that Details and viewer windows show while\n"
           "they drive the focus frame in the grid. 0 disables the border.\n"
           "Takes effect immediately, including open windows."));
    form->addRow(tr("Focus border width:"), m_focusBorderPx);

    m_mouseThresholdPx = new QSpinBox(this);
    m_mouseThresholdPx->setRange(1, 64);
    m_mouseThresholdPx->setSuffix(tr(" px"));
    m_mouseThresholdPx->setValue(AppSettings::mouseThresholdPx());
    m_mouseThresholdPx->setToolTip(
        tr("How far the mouse must travel to count as a deliberate move.\n"
           "Releases the focus frame after Ctrl+wheel zooming in the grid,\n"
           "and sizes the bottom-corner park zones in the fullscreen\n"
           "viewer."));
    form->addRow(tr("Mouse move threshold:"), m_mouseThresholdPx);

    m_reflowAnimMs = new QSpinBox(this);
    m_reflowAnimMs->setRange(0, 1000);
    m_reflowAnimMs->setSingleStep(50);
    m_reflowAnimMs->setSuffix(tr(" ms"));
    m_reflowAnimMs->setSpecialValueText(tr("off"));
    m_reflowAnimMs->setValue(AppSettings::reflowAnimationMs());
    m_reflowAnimMs->setToolTip(
        tr("Animates the grid rearrangement when zooming or when a resize\n"
           "changes the column count — thumbnails travel along their row\n"
           "and wrap around the edges. 0 disables the animation."));
    form->addRow(tr("Reflow animation:"), m_reflowAnimMs);

    m_scrollAnimMs = new QSpinBox(this);
    m_scrollAnimMs->setRange(0, 1000);
    m_scrollAnimMs->setSingleStep(20);
    m_scrollAnimMs->setSuffix(tr(" ms"));
    m_scrollAnimMs->setSpecialValueText(tr("off"));
    m_scrollAnimMs->setValue(AppSettings::scrollAnimationMs());
    m_scrollAnimMs->setToolTip(
        tr("Glides the grid to the new position when scrolling with the\n"
           "mouse wheel instead of jumping there. 0 disables the\n"
           "animation."));
    form->addRow(tr("Scroll animation:"), m_scrollAnimMs);

    m_scrollSnap = new QCheckBox(tr("Snap scrolling to the grid"), this);
    m_scrollSnap->setChecked(AppSettings::scrollSnapToGrid());
    m_scrollSnap->setToolTip(
        tr("The grid always comes to rest on a whole row of thumbnails.\n"
           "Switched off, scrolling can stop between rows and hi-res\n"
           "wheels and touchpads scroll by smaller steps than one row."));
    form->addRow(QString(), m_scrollSnap);

    m_fullscreenAnimMs = new QSpinBox(this);
    m_fullscreenAnimMs->setRange(0, 1000);
    m_fullscreenAnimMs->setSingleStep(50);
    m_fullscreenAnimMs->setSuffix(tr(" ms"));
    m_fullscreenAnimMs->setSpecialValueText(tr("off"));
    m_fullscreenAnimMs->setValue(AppSettings::fullscreenAnimationMs());
    m_fullscreenAnimMs->setToolTip(
        tr("UI transition animations: the side-panel slide (windowed and\n"
           "fullscreen), the fullscreen toolbar/menu slide at the top edge,\n"
           "the fade between the system theme and the dark presentation\n"
           "theme, and the grid glide when entering/leaving fullscreen.\n"
           "0 disables these animations."));
    form->addRow(tr("UI animations:"), m_fullscreenAnimMs);
}

void InputSettingsPage::apply()
{
    AppSettings::setFocusBorderWidth(m_focusBorderPx->value());
    AppSettings::setMouseThresholdPx(m_mouseThresholdPx->value());
    AppSettings::setReflowAnimationMs(m_reflowAnimMs->value());
    // Both scroll settings are read on demand by GridView (per wheel event
    // and per glide start) — nothing to push.
    AppSettings::setScrollAnimationMs(m_scrollAnimMs->value());
    AppSettings::setScrollSnapToGrid(m_scrollSnap->isChecked());
    AppSettings::setFullscreenAnimationMs(m_fullscreenAnimMs->value());

    const int dblMs = m_doubleClickSystem->isChecked() ? 0 : m_doubleClickMs->value();
    AppSettings::setDoubleClickIntervalMs(dblMs);
    QApplication::setDoubleClickInterval(
        dblMs > 0 ? dblMs : AppSettings::systemDoubleClickInterval());

    // The focus border of already-open Details/viewer windows is refreshed by
    // MainWindow after the dialog closes (applyFocusBorderWidth) — the frame
    // widget type is MainWindow-local, so the walk stays there.
}
