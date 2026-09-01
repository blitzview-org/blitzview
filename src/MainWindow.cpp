#include "MainWindow.h"
#include "DirectoryPanel.h"
#include "DetailsPanel.h"
#include "MediaModel.h"
#include "GridView.h"
#include "TableView.h"
#include "MediaViewer.h"
#include "SettingsDialog.h"
#include "ThumbnailDiskCache.h"
#include "FileOps.h"
#include "ElidedLabel.h"
#include "EditMetadataDialog.h"
#include "ExifToolService.h"
#include "MetadataCache.h"
#include "FullscreenTransition.h"
#include "RenameDialog.h"
#include "TagFilterDialog.h"
#include "SlideTrace.h"
#include "AboutDialog.h"

#include <QToolBar>
#include <QStatusBar>
#include <QStackedWidget>
#include <QSplitter>
#include <QSlider>
#include <QLabel>
#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QActionGroup>
#include "AppSettings.h"
#include <QMenuBar>
#include <QMenu>
#include <QKeySequence>

#include "Shortcuts.h"
#include <QApplication>
#include <QShortcut>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QComboBox>
#include <QClipboard>
#include <QDialog>
#include <QFile>
#include <QFrame>
#include <QGuiApplication>
#include <QScreen>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QStyle>
#include <QToolButton>
#include <QCloseEvent>
#include <QCursor>
#include <QEnterEvent>
#include <QPolygonF>
#include <QTimer>
#include <QEventLoop>
#include <QSet>
#include <QVariantAnimation>
#include <memory>

#include <functional>

// Thin reserved border around a Details dialog's content: the margin is
// ALWAYS reserved (no jumping) and colored in Highlight while the dialog
// drives the grids' focus frame.
class FocusBorderFrame : public QWidget
{
public:
    explicit FocusBorderFrame(QWidget* parent = nullptr) : QWidget(parent)
    {
        const int b = AppSettings::focusBorderWidth();
        setContentsMargins(b, b, b, b);
    }

    void setActive(bool active)
    {
        if (m_active == active)
            return;
        m_active = active;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QWidget::paintEvent(event);
        if (!m_active)
            return;
        QPainter p(this);
        const int b = contentsMargins().left();
        const QColor c = palette().color(QPalette::Highlight);
        p.fillRect(QRect(0, 0, width(), b), c);
        p.fillRect(QRect(0, height() - b, width(), b), c);
        p.fillRect(QRect(0, b, b, height() - 2 * b), c);
        p.fillRect(QRect(width() - b, b, b, height() - 2 * b), c);
    }

private:
    bool m_active = false;
};

// A changed focus-border width (Settings → Input) applies to every OPEN aux
// window immediately: viewers re-run their manual geometry, Details dialogs
// get the FocusBorderFrame margins updated (relayouts automatically).
static void applyFocusBorderWidth()
{
    const int b = AppSettings::focusBorderWidth();
    const QWidgetList topLevels = QApplication::topLevelWidgets();
    for (QWidget* w : topLevels) {
        if (auto* viewer = qobject_cast<MediaViewer*>(w)) {
            viewer->refreshFocusBorder();
        } else if (auto* frame = static_cast<FocusBorderFrame*>(
                       w->property("focusBorderFrame").value<QWidget*>())) {
            frame->setContentsMargins(b, b, b, b);
            frame->update();
        }
    }
}

// Aux windows (Details dialogs, viewers) belong to ALL MainWindows: every
// open grid shows the same external focus, so the state is app-global.
QList<MainWindow*> MainWindow::s_allWindows;
QHash<QString, QPointer<QDialog>> MainWindow::s_detailsDialogs;
QPointer<QWidget> MainWindow::s_frameMouseWin;
QPointer<QWidget> MainWindow::s_frameActiveWin;
QPointer<QWidget> MainWindow::s_frameDrivingWin;
int      MainWindow::s_fullscreenWindows = 0;
QPalette MainWindow::s_systemPalette;

// Invisible strip at the top screen edge while fullscreen chrome is hidden —
// entering it reveals the menu bar and tool bar. A dedicated widget instead
// of an application-wide event filter: MainWindow::eventFilter already has
// aux-window semantics that must not see extra watched objects.
class FullscreenHotZone : public QWidget
{
public:
    FullscreenHotZone(std::function<void()> onEnter, QWidget* parent)
        : QWidget(parent), m_onEnter(std::move(onEnter))
    {
        hide();
    }

protected:
    void enterEvent(QEnterEvent* event) override
    {
        QWidget::enterEvent(event);
        m_onEnter();
    }

private:
    std::function<void()> m_onEnter;
};

QPalette MainWindow::makeDarkPalette(const QPalette& system)
{
    // Derived from the system palette so Highlight/HighlightedText keep the
    // familiar system accent. setColor(role, …) covers Active+Inactive+
    // Disabled; the Disabled group is then dimmed explicitly.
    // Backgrounds stay pure black (the point of the mode); the control roles
    // (Button, bevel shades) sit noticeably above black so scrollbars,
    // checkboxes and frames remain legible against it.
    QPalette pal = system;
    pal.setColor(QPalette::Window,          QColor(0x00, 0x00, 0x00));
    pal.setColor(QPalette::WindowText,      QColor(0xe4, 0xe4, 0xe4));
    pal.setColor(QPalette::Base,            QColor(0x00, 0x00, 0x00));
    pal.setColor(QPalette::AlternateBase,   QColor(0x16, 0x16, 0x16));
    pal.setColor(QPalette::Text,            QColor(0xe4, 0xe4, 0xe4));
    pal.setColor(QPalette::Button,          QColor(0x2e, 0x2e, 0x2e));
    pal.setColor(QPalette::ButtonText,      QColor(0xe4, 0xe4, 0xe4));
    pal.setColor(QPalette::BrightText,      QColor(0xff, 0x55, 0x55));
    pal.setColor(QPalette::ToolTipBase,     QColor(0x2e, 0x2e, 0x2e));
    pal.setColor(QPalette::ToolTipText,     QColor(0xe4, 0xe4, 0xe4));
    pal.setColor(QPalette::PlaceholderText, QColor(0x90, 0x90, 0x90));
    // Bevel roles: many styles derive scrollbar handles and control borders
    // from these; DirectoryPanel uses Midlight for its selection pills
    pal.setColor(QPalette::Light,           QColor(0x52, 0x52, 0x52));
    pal.setColor(QPalette::Midlight,        QColor(0x3a, 0x3a, 0x3a));
    pal.setColor(QPalette::Mid,             QColor(0x56, 0x56, 0x56));
    pal.setColor(QPalette::Dark,            QColor(0x26, 0x26, 0x26));
    pal.setColor(QPalette::Shadow,          QColor(0x10, 0x10, 0x10));
    pal.setColor(QPalette::Link,            QColor(0x3d, 0xae, 0xe9));
    pal.setColor(QPalette::Disabled, QPalette::WindowText,      QColor(0x6a, 0x6a, 0x6a));
    pal.setColor(QPalette::Disabled, QPalette::Text,            QColor(0x6a, 0x6a, 0x6a));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText,      QColor(0x6a, 0x6a, 0x6a));
    pal.setColor(QPalette::Disabled, QPalette::Base,            QColor(0x08, 0x08, 0x08));
    pal.setColor(QPalette::Disabled, QPalette::Button,          QColor(0x1c, 0x1c, 0x1c));
    pal.setColor(QPalette::Disabled, QPalette::Highlight,       QColor(0x38, 0x38, 0x38));
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(0x90, 0x90, 0x90));
    return pal;
}

QPalette MainWindow::lerpPalette(const QPalette& from, const QPalette& to,
                                 qreal t)
{
    const auto lerp = [t](const QColor& a, const QColor& b) {
        return QColor(a.red()   + qRound((b.red()   - a.red())   * t),
                      a.green() + qRound((b.green() - a.green()) * t),
                      a.blue()  + qRound((b.blue()  - a.blue())  * t),
                      a.alpha() + qRound((b.alpha() - a.alpha()) * t));
    };
    static const QPalette::ColorRole roles[] = {
        QPalette::Window,        QPalette::WindowText,
        QPalette::Base,          QPalette::AlternateBase,
        QPalette::Text,          QPalette::Button,
        QPalette::ButtonText,    QPalette::BrightText,
        QPalette::ToolTipBase,   QPalette::ToolTipText,
        QPalette::PlaceholderText,
        QPalette::Light,         QPalette::Midlight,
        QPalette::Mid,           QPalette::Dark,
        QPalette::Shadow,        QPalette::Link,
        QPalette::Highlight,     QPalette::HighlightedText,
    };
    static const QPalette::ColorGroup groups[] = {
        QPalette::Active, QPalette::Inactive, QPalette::Disabled,
    };
    QPalette pal = to;
    for (const auto g : groups)
        for (const auto r : roles)
            pal.setColor(g, r, lerp(from.color(g, r), to.color(g, r)));
    return pal;
}

void MainWindow::acquireDarkPalette()
{
    if (s_fullscreenWindows++ > 0)
        return;
    s_systemPalette = QApplication::palette();
    // Trade-off: after this first setPalette Qt treats the app palette as
    // explicitly set, so a SYSTEM theme change mid-session no longer
    // propagates automatically. Normal mode before the first fullscreen use
    // stays byte-identical to the untouched system theme.
    QApplication::setPalette(makeDarkPalette(s_systemPalette));
}

void MainWindow::releaseDarkPalette()
{
    if (s_fullscreenWindows > 0 && --s_fullscreenWindows == 0)
        QApplication::setPalette(s_systemPalette);
}

// Sidebar glyph: window outline with a filled left band. Qt has no standard
// icon for this, so it is drawn in the theme's text color.
static QIcon makeSidePanelIcon(const QPalette& pal)
{
    QIcon icon;
    const QColor color = pal.color(QPalette::ButtonText);

    for (const int size : {16, 24, 32, 48}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);

        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal margin = size / 8.0;
        const QRectF outer(margin, margin * 1.5, size - 2 * margin, size - 3 * margin);
        const qreal radius = size / 12.0;
        const qreal penW = qMax(1.0, size / 16.0);

        p.setPen(QPen(color, penW));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(outer, radius, radius);

        QRectF band = outer.adjusted(penW, penW, 0, -penW);
        band.setWidth(outer.width() * 0.38);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRect(band);
        p.end();

        icon.addPixmap(pm);
    }
    return icon;
}

// BLITZVIEW_TRACE_SLIDE diagnostics: logs geometry-relevant events of the
// watched widget (side panel / content stack) with timestamps
class SlideTraceFilter : public QObject
{
public:
    SlideTraceFilter(const char* tag, QObject* parent)
        : QObject(parent), m_tag(tag) {}

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::LayoutRequest: {
            static const char* names[] = {"?", "Resize", "Move", "Show",
                                          "Hide", "LayoutRequest"};
            int n = 0;
            if (e->type() == QEvent::Resize) n = 1;
            else if (e->type() == QEvent::Move) n = 2;
            else if (e->type() == QEvent::Show) n = 3;
            else if (e->type() == QEvent::Hide) n = 4;
            else if (e->type() == QEvent::LayoutRequest) n = 5;
            const auto* w = qobject_cast<QWidget*>(obj);
            const QRect g = w ? w->geometry() : QRect();
            TRACE_SLIDE("%s %-13s geo=%d,%d %dx%d visible=%d",
                        m_tag, names[n], g.x(), g.y(), g.width(), g.height(),
                        w ? int(w->isVisible()) : -1);
            break;
        }
        default:
            break;
        }
        return QObject::eventFilter(obj, e);
    }

private:
    const char* m_tag;
};

// Fullscreen glyph: four corner brackets (⛶ style), drawn in the theme's
// text color like the side-panel icon — re-baked on PaletteChange.
static QIcon makeFullscreenIcon(const QPalette& pal)
{
    QIcon icon;
    const QColor color = pal.color(QPalette::ButtonText);

    for (const int size : {16, 24, 32, 48}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);

        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal m = size / 5.0;             // outer margin
        const qreal len = size / 4.5;           // bracket arm length
        const qreal penW = qMax(1.0, size / 14.0);
        const qreal r = size - m;
        p.setPen(QPen(color, penW, Qt::SolidLine, Qt::FlatCap));
        p.drawPolyline(QPolygonF({{m, m + len}, {m, m}, {m + len, m}}));
        p.drawPolyline(QPolygonF({{r - len, m}, {r, m}, {r, m + len}}));
        p.drawPolyline(QPolygonF({{m, r - len}, {m, r}, {m + len, r}}));
        p.drawPolyline(QPolygonF({{r - len, r}, {r, r}, {r, r - len}}));
        p.end();

        icon.addPixmap(pm);
    }
    return icon;
}

MainWindow::MainWindow(const QStringList& initialDirs, QWidget* parent,
                       const MainWindow* cloneSource) : QMainWindow(parent)
{
    setWindowTitle(tr("BlitzView"));
    resize(1200, 750);

    // Apply persisted disk-cache settings before the first thumbnail request,
    // and enforce the size limit left over from previous sessions.
    auto& diskCache = ThumbnailDiskCache::instance();
    diskCache.setEnabled(AppSettings::diskCacheEnabled());
    diskCache.setMaxBytes(AppSettings::diskCacheMaxBytes());
    diskCache.trimIfNeeded();

    m_model = new MediaModel(this);
    m_model->setSyncModeEnabled(AppSettings::syncModeEnabled());

    m_dirPanel = new DirectoryPanel(this);
    m_dirPanel->setMinimumWidth(180);

    m_detailsPanel = new DetailsPanel(this);
    m_detailsPanel->setVisible(AppSettings::detailsPanelVisible());

    // Side panel: directory panel on top, optional details panel below
    m_sidePanel = new QSplitter(Qt::Vertical, this);
    m_sidePanel->addWidget(m_dirPanel);
    m_sidePanel->addWidget(m_detailsPanel);
    m_sidePanel->setStretchFactor(0, 1);
    m_sidePanel->setStretchFactor(1, 0);
    m_sidePanel->setCollapsible(0, false);
    m_sidePanel->setVisible(false);  // hidden by default

    m_gridView  = new GridView(this);
    m_tableView = new TableView(this);
    s_allWindows.append(this);   // grid exists — external focus may apply

    m_gridView->setSourceModel(m_model);
    m_tableView->setSourceModel(m_model);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_gridView);
    m_stack->addWidget(m_tableView);

    // Main layout: horizontal splitter with side panel | content
    m_hSplitter = new QSplitter(Qt::Horizontal, this);
    m_hSplitter->addWidget(m_sidePanel);
    m_hSplitter->addWidget(m_stack);
    m_hSplitter->setStretchFactor(0, 0);
    m_hSplitter->setStretchFactor(1, 1);
    m_hSplitter->setSizes({220, 980});

    setCentralWidget(m_hSplitter);
    buildToolBar();
    buildMenuBar();
    buildStatusBar();
    onSortChanged();

    // Clone: adopt the source's sort BEFORE the directories are loaded, so
    // the first scan lands already sorted the way the source shows it (the
    // combos re-fire onSortChanged themselves).
    if (cloneSource) {
        m_sortColumnCombo->setCurrentIndex(cloneSource->m_sortColumnCombo->currentIndex());
        m_sortOrderCombo->setCurrentIndex(cloneSource->m_sortOrderCombo->currentIndex());
        m_tableSizeSlider->setValue(cloneSource->m_tableSizeSlider->value());
    }

    if (slideTraceEnabled()) {
        m_sidePanel->installEventFilter(
            new SlideTraceFilter("sidePanel", this));
        m_stack->installEventFilter(new SlideTraceFilter("stack    ", this));
    }

    auto* pageDownShortcut = new QShortcut(QKeySequence(Qt::Key_PageDown), this);
    pageDownShortcut->setContext(Qt::WindowShortcut);
    connect(pageDownShortcut, &QShortcut::activated, this, [this]() {
        if (!isActiveWindow()) return;
        if (!m_stack) return;
        auto* area = qobject_cast<QAbstractScrollArea*>(m_stack->currentWidget());
        if (!area || !area->verticalScrollBar()) return;
        area->verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepAdd);
    });

    auto* pageUpShortcut = new QShortcut(QKeySequence(Qt::Key_PageUp), this);
    pageUpShortcut->setContext(Qt::WindowShortcut);
    connect(pageUpShortcut, &QShortcut::activated, this, [this]() {
        if (!isActiveWindow()) return;
        if (!m_stack) return;
        auto* area = qobject_cast<QAbstractScrollArea*>(m_stack->currentWidget());
        if (!area || !area->verticalScrollBar()) return;
        area->verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepSub);
    });

    auto* homeShortcut = new QShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_Home), this);
    homeShortcut->setContext(Qt::WindowShortcut);
    connect(homeShortcut, &QShortcut::activated, this, [this]() {
        if (!isActiveWindow()) return;
        if (!m_stack) return;
        auto* area = qobject_cast<QAbstractScrollArea*>(m_stack->currentWidget());
        if (!area || !area->verticalScrollBar()) return;
        area->verticalScrollBar()->setValue(area->verticalScrollBar()->minimum());
    });

    auto* endShortcut = new QShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_End), this);
    endShortcut->setContext(Qt::WindowShortcut);
    connect(endShortcut, &QShortcut::activated, this, [this]() {
        if (!isActiveWindow()) return;
        if (!m_stack) return;
        auto* area = qobject_cast<QAbstractScrollArea*>(m_stack->currentWidget());
        if (!area || !area->verticalScrollBar()) return;
        area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum());
    });

    // Fullscreen presentation mode: Escape leaves it (shortcut exists only
    // while fullscreen — Escape stays unclaimed in normal mode)
    m_escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    m_escShortcut->setContext(Qt::WindowShortcut);
    m_escShortcut->setEnabled(false);
    connect(m_escShortcut, &QShortcut::activated, this, [this]() {
        setFullscreenMode(false);
    });

    m_fsHotZone = new FullscreenHotZone([this]() { revealFullscreenChrome(); }, this);

    // Re-hide revealed chrome by polling the cursor position — avoids the
    // Leave/popup-grab corner cases of enter/leave tracking
    m_fsChromeTimer = new QTimer(this);
    m_fsChromeTimer->setInterval(250);
    connect(m_fsChromeTimer, &QTimer::timeout, this, [this]() {
        if (!isFullScreen()) {
            m_fsChromeTimer->stop();
            return;
        }
        if (QApplication::activePopupWidget())
            return;   // an open menu keeps the chrome
        const QPoint pos = mapFromGlobal(QCursor::pos());
        if (!rect().contains(pos)
            || pos.y() > m_toolBar->geometry().bottom() + 8) {
            m_fsChromeTimer->stop();
            hideFullscreenChrome();
        }
    });

    // Chrome slide: one progress value drives the max-height of menu bar
    // and toolbar together (the QMainWindow layout follows the constraint)
    m_fsChromeAnim = new QVariantAnimation(this);
    m_fsChromeAnim->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_fsChromeAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
        // Guard against the stopped-state re-evaluation emission of
        // setStartValue/setEndValue (see side-panel anim)
        if (m_fsChromeAnim->state() != QAbstractAnimation::Running)
            return;
        const qreal t = v.toReal();
        menuBar()->setMaximumHeight(qMax(0, qRound(t * m_fsMenuNaturalH)));
        m_toolBar->setMaximumHeight(qMax(0, qRound(t * m_fsToolNaturalH)));
    });
    connect(m_fsChromeAnim, &QVariantAnimation::finished, this, [this]() {
        menuBar()->setMaximumHeight(QWIDGETSIZE_MAX);
        m_toolBar->setMaximumHeight(QWIDGETSIZE_MAX);
        if (m_fsChromeAnim->endValue().toReal() > 0.5) {
            m_fsChromeTimer->start();   // fully revealed — arm the re-hide poll
        } else {
            menuBar()->hide();
            m_toolBar->hide();
            if (isFullScreen()) {
                updateFsHotZoneGeometry();
                m_fsHotZone->show();
                m_fsHotZone->raise();
            }
        }
    });

    // Side-panel slide: drives max width AND the splitter allocation every
    // frame; the grid runs ONE frozen from→to flow transition on the same
    // clock (setPanelSlideProgress), so its painted fold width equals the
    // moving panel edge exactly and new columns emerge continuously
    // through the flow wrap instead of popping in at a width threshold
    m_sidePanelAnim = new QVariantAnimation(this);
    m_sidePanelAnim->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_sidePanelAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
        // setStartValue/setEndValue on a STOPPED animation re-evaluate the
        // current value and emit too — acting on that would slam the
        // splitter to the new end value for one event-loop pass (visible
        // flash) before the real run starts at the start value
        if (m_sidePanelAnim->state() != QAbstractAnimation::Running)
            return;
        const int w = v.toInt();
        TRACE_SLIDE("anim value w=%d  panelW=%d stackW=%d", w,
                    m_sidePanel->width(), m_stack->width());
        m_sidePanel->setMaximumWidth(w);
        const int total = m_hSplitter->width() - m_hSplitter->handleWidth();
        m_hSplitter->setSizes({w, qMax(0, total - w)});
        const qreal a = m_sidePanelAnim->startValue().toReal();
        const qreal b = m_sidePanelAnim->endValue().toReal();
        if (qAbs(b - a) >= 1.0)
            m_gridView->setPanelSlideProgress((v.toReal() - a) / (b - a));
    });
    connect(m_sidePanelAnim, &QVariantAnimation::finished, this, [this]() {
        TRACE_SLIDE("anim finished end=%d",
                    m_sidePanelAnim->endValue().toInt());
        // Splitter finalization FIRST, while the slide is still active:
        // hiding the panel removes its 1 px remnant plus the splitter
        // handle — a final +4 px grid resize. Under the still-active
        // slide suppression that SNAPS (invisible); after endPanelSlide
        // it would start a full-duration reflow drift for those 4 px,
        // right when the end-of-slide hover re-evaluation pops the focus
        // frame in (reads as a jerk).
        if (m_sidePanelAnim->endValue().toInt() == 0)
            m_sidePanel->setVisible(false);
        m_sidePanel->setMaximumWidth(QWIDGETSIZE_MAX);
        // Drop the 1 px slide minimum (back to hint-based)
        m_sidePanel->setMinimumWidth(0);
        // Force the splitter redistribution NOW — after hiding, it defers
        // the reallocation to a later LayoutRequest (observed 160 ms after
        // the slide, whenever hover happened to invalidate a layout),
        // which would fall OUTSIDE the slide suppression
        m_hSplitter->refresh();
        // Only now: slider maximum and the single hover re-evaluation run
        // on the FINAL raster
        m_gridView->endPanelSlide();
    });

    // Palette fade between system theme and dark presentation theme:
    // a window-level palette override on top of the (already switched)
    // app palette, cleared when the fade completes
    m_paletteAnim = new QVariantAnimation(this);
    m_paletteAnim->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_paletteAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
        if (m_paletteAnim->state() != QAbstractAnimation::Running)
            return;
        setPalette(lerpPalette(m_paletteFadeFrom, m_paletteFadeTo, v.toReal()));
    });
    connect(m_paletteAnim, &QVariantAnimation::finished, this, [this]() {
        setPalette(QPalette());   // drop the override → inherit the app palette
    });

    // Connect directory panel
    connect(m_dirPanel, &DirectoryPanel::selectedDirectoriesChanged,
            this, &MainWindow::onSelectedDirectoriesChanged);
    connect(m_dirPanel, &DirectoryPanel::hideRequested,
            this, [this]() {
        setSidePanelVisibleAnimated(false);
        if (m_actSidePanel)
            m_actSidePanel->setChecked(false);
    });

    // The item carrying the focus frame (mouse hover, or the file of the
    // Details/viewer window driving the external focus) feeds the details
    // panel and the status bar path. Independent of the selection.
    connect(m_gridView, &GridView::focusItemChanged,
            this, &MainWindow::onFocusItemChanged);
    // The list view has the same hover focus frame and feeds the same two
    // widgets — only the visible view ever emits (hideEvent clears its hover).
    connect(m_tableView, &TableView::focusItemChanged,
            this, &MainWindow::onFocusItemChanged);
    connect(m_gridView, &GridView::selectionChanged, this, [this](const QList<int>& rows) {
        if (m_actCopy) m_actCopy->setEnabled(!rows.isEmpty());
        if (m_actCut)  m_actCut->setEnabled(!rows.isEmpty());
        if (!m_selectionSyncBlocked)
            m_tableView->setSelectedRows(rows);
    });
    // Grid and list share ONE selection — mirrored in both directions. Each
    // setter is a no-op when the selection already matches, which ends the
    // echo after one hop.
    connect(m_tableView, &TableView::selectionChangedRows, this,
            [this](const QList<int>& rows) {
        if (!m_selectionSyncBlocked)
            m_gridView->setSelectedRows(rows);
    });
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
        m_selectionSyncBlocked = true;
    });
    // Queued: runs AFTER every direct modelReset handler, in particular the
    // grid's restoreViewState (which re-selects by path).
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
        m_selectionSyncBlocked = false;
        syncSelectionToTable();
    }, Qt::QueuedConnection);
    // Whether THIS grid currently displays the external focus — feeds the
    // driving aux window's border (on while ANY open grid displays it)
    connect(m_gridView, &GridView::externalFocusDisplayChanged, this,
            [this](bool displayed) {
        m_externalDisplayed = displayed;
        applyDriverBorder();
    });

    // External file drops: into the grid (single-root target) and onto
    // directories in the tree. Rescan afterwards so the grid reflects the
    // new/removed files.
    connect(m_gridView, &GridView::filesDropped, this, [this]() {
        m_model->loadDirectories(m_selectedRecursive, m_selectedIndividual);
        reapplySort();
    });
    connect(m_tableView, &TableView::filesDropped, this, [this]() {
        m_model->loadDirectories(m_selectedRecursive, m_selectedIndividual);
        reapplySort();
    });
    connect(m_dirPanel, &DirectoryPanel::openInNewWindowRequested,
            this, [](const QString& path) {
        // Independent top-level window; frees itself on close. The app quits
        // when the last window closes (Qt default).
        auto* win = new MainWindow(QStringList{path});
        win->setAttribute(Qt::WA_DeleteOnClose);
        win->show();
    });
    connect(m_dirPanel, &DirectoryPanel::filesDroppedOnDir, this,
            [this](const QList<QUrl>& urls, const QString& targetDir) {
        const auto op = FileOps::askDropMode(this, QCursor::pos());
        if (!op)
            return;
        if (FileOps::perform(urls, targetDir, *op, this) > 0) {
            m_model->loadDirectories(m_selectedRecursive, m_selectedIndividual);
            reapplySort();
        }
    });

    connect(m_gridView, &GridView::detailsRequested,
            this, &MainWindow::onDetailsRequested);
    connect(m_gridView, &GridView::editMetadataRequested,
            this, &MainWindow::onEditMetadataRequested);
    connect(m_gridView, &GridView::renameRequested,
            this, &MainWindow::onRenameRequested);
    connect(m_gridView, &GridView::deleteRequested,
            this, &MainWindow::onDeleteRequested);
    connect(m_gridView, &GridView::filterToSelectionRequested,
            this, &MainWindow::onFilterToSelection);
    connect(m_gridView, &GridView::clearFilterRequested,
            m_model, &MediaModel::clearFilter);

    // The list view offers the same item context menu and routes into the
    // same handlers; clipboard actions go through the grid, which owns the
    // drop target directory.
    connect(m_tableView, &TableView::detailsRequested,
            this, &MainWindow::onDetailsRequested);
    connect(m_tableView, &TableView::editMetadataRequested,
            this, &MainWindow::onEditMetadataRequested);
    connect(m_tableView, &TableView::renameRequested,
            this, &MainWindow::onRenameRequested);
    connect(m_tableView, &TableView::deleteRequested,
            this, &MainWindow::onDeleteRequested);
    connect(m_tableView, &TableView::filterToSelectionRequested,
            this, &MainWindow::onFilterToSelection);
    connect(m_tableView, &TableView::clearFilterRequested,
            m_model, &MediaModel::clearFilter);
    connect(m_tableView, &TableView::copyRequested,
            m_gridView, &GridView::copySelection);
    connect(m_tableView, &TableView::cutRequested,
            m_gridView, &GridView::cutSelection);
    connect(m_tableView, &TableView::pasteRequested,
            m_gridView, &GridView::pasteFromClipboard);
    connect(m_tableView, &TableView::sortRequested,
            this, &MainWindow::onTableSortRequested);
    // Ctrl+wheel in the list drives the same slider a drag would — direct
    // connection, so the row height is already applied when wheelEvent
    // re-anchors the scroll position on the row under the cursor.
    connect(m_tableView, &TableView::thumbnailSizeChangeRequested, this,
            [this](int size) {
        if (m_tableSizeSlider)
            m_tableSizeSlider->setValue(size);
    });
    connect(m_model, &MediaModel::filterChanged, this, [this]() {
        if (m_filterButton)
            m_filterButton->setChecked(m_model->isFiltered());
        updateStatusBar(m_model->rowCount());
        onThumbnailLoaded();
    });
    connect(&ExifToolService::instance(), &ExifToolService::writeFinished,
            this, [this](const QStringList&) {
        if (m_pendingMetaWrites <= 0)
            return;
        if (--m_pendingMetaWrites > 0)
            return;
        finalizeMetadataWrites();
    });
    connect(&ExifToolService::instance(), &ExifToolService::metadataReady,
            this, &MainWindow::onMetadataReadyForMtime);

    // Connect grid/table double-click
    connect(m_gridView,  &GridView::itemDoubleClicked,
            this, &MainWindow::onItemDoubleClicked);
    connect(m_tableView, &TableView::itemDoubleClicked,
            this, &MainWindow::onItemDoubleClicked);

    // Status bar updates
    connect(m_model, &MediaModel::loadingFinished,
            this, &MainWindow::updateStatusBar);
    m_statusThrottle = new QTimer(this);
    m_statusThrottle->setSingleShot(true);
    m_statusThrottle->setInterval(200);
    connect(m_statusThrottle, &QTimer::timeout, this, &MainWindow::onThumbnailLoaded);
    connect(m_model, &QAbstractItemModel::dataChanged,
            this, [this](const QModelIndex&, const QModelIndex&, const QList<int>&) {
        if (!m_statusThrottle->isActive())
            m_statusThrottle->start();
    });

    // Window layout: a clone mirrors its source (offset so it does not hide
    // it), everything else comes from the persisted session state.
    if (cloneSource) {
        // A fullscreen/maximized source clones at its windowed size — the
        // clone opens as a normal window.
        const QRect srcGeo = (cloneSource->isFullScreen() || cloneSource->isMaximized())
                                 ? cloneSource->normalGeometry()
                                 : cloneSource->geometry();
        if (srcGeo.isValid())
            setGeometry(srcGeo.translated(30, 30));
        m_hSplitter->restoreState(cloneSource->m_hSplitter->saveState());
        m_sidePanel->restoreState(cloneSource->m_sidePanel->saveState());
        // In fullscreen the source hides its side panel — clone the state
        // it will return to
        const bool sideVisible = cloneSource->m_fsUiApplied
                                     ? cloneSource->m_fsSidePanelWasVisible
                                     : cloneSource->m_sidePanel->isVisible();
        m_sidePanel->setVisible(sideVisible);
        if (m_actSidePanel)
            m_actSidePanel->setChecked(sideVisible);
        if (m_actDetails)
            m_actDetails->setChecked(cloneSource->m_detailsPanel->isVisible());
        m_detailsPanel->setVisible(cloneSource->m_detailsPanel->isVisible());
    } else {
        // Restore window geometry
        const QByteArray geo = AppSettings::windowGeometry();
        if (!geo.isEmpty())
            restoreGeometry(geo);

        // Restore splitter state (captures side panel width)
        const QByteArray splState = AppSettings::splitterState();
        if (!splState.isEmpty())
            m_hSplitter->restoreState(splState);

        const QByteArray sideState = AppSettings::sideSplitterState();
        if (!sideState.isEmpty())
            m_sidePanel->restoreState(sideState);

        // Restore side panel visibility
        if (AppSettings::dirPanelVisible()) {
            m_sidePanel->setVisible(true);
            if (m_actSidePanel)
                m_actSidePanel->setChecked(true);
        }
    }

    // Determine initial directories
    if (cloneSource) {
        const QString anchor = cloneSource->m_dirPanel->treeAnchor();
        if (!anchor.isEmpty())
            m_dirPanel->restoreTreeAnchor(anchor);

        // Selection and scroll position ride the grid's by-path view state:
        // injected BEFORE the first scan, applied by the model reset that
        // delivers the scan result (the empty intermediate reset keeps it).
        m_gridView->setPendingViewState(cloneSource->m_gridView->viewStateSnapshot());

        // Filter and list scroll offset can only land AFTER the scan: a
        // filter set earlier would be cleared by loadDirectories (different
        // roots), and the list has no rows to scroll to yet.
        const QStringList filterPaths = cloneSource->m_model->activeFilterPaths();
        const QStringList filterTags  = cloneSource->m_model->activeTagFilter();
        const int tableScroll = cloneSource->m_tableView->verticalScrollBar()->value();
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = connect(m_model, &MediaModel::scanningFinished, this,
                        [this, filterPaths, filterTags, tableScroll, conn]() {
            disconnect(*conn);   // clone state applies to the FIRST scan only
            // Filtering resets the model; the grid carries selection and
            // top-left item through that reset by path on its own.
            if (!filterPaths.isEmpty())
                m_model->filterToPaths(filterPaths);
            if (!filterTags.isEmpty())
                m_model->filterToTags(filterTags);
            // The list's row geometry settles one event-loop pass after the
            // reset — set its scroll offset there (row heights match, the
            // thumbnail size is cloned too)
            QTimer::singleShot(0, this, [this, tableScroll]() {
                m_tableView->verticalScrollBar()->setValue(tableScroll);
            });
        });

        m_dirPanel->setSelectedDirectories(cloneSource->m_selectedRecursive,
                                           cloneSource->m_selectedIndividual);
    } else if (!initialDirs.isEmpty()) {
        if (initialDirs.size() == 1)
            m_dirPanel->setCurrentDirectory(initialDirs.first());
        else
            m_dirPanel->setSelectedDirectories(initialDirs);
    } else {
        QString savedAnchor = AppSettings::treeAnchor();
        if (!savedAnchor.isEmpty())
            m_dirPanel->restoreTreeAnchor(savedAnchor);

        QStringList savedRecursive = AppSettings::recursiveDirs();
        QStringList savedIndividual = AppSettings::individualDirs();
        if (!savedRecursive.isEmpty() || !savedIndividual.isEmpty()) {
            m_dirPanel->setSelectedDirectories(savedRecursive, savedIndividual);
        } else {
            QString startDir = AppSettings::lastDir();
            if (!startDir.isEmpty())
                m_dirPanel->setCurrentDirectory(startDir);
        }
    }

    // Restore the view that was active last (a clone takes the source's).
    // Goes through the same actions as a click, so toolbar sliders and
    // thumbnail size follow.
    const bool listActive = cloneSource
                                ? (cloneSource->m_stack->currentWidget() == cloneSource->m_tableView)
                                : AppSettings::listViewActive();
    if (listActive) {
        m_actTable->setChecked(true);
        switchToTable();
        m_tableView->setFocus();
    } else {
        m_gridView->setFocus();
    }
}

void MainWindow::buildToolBar()
{
    QToolBar* tb = addToolBar(tr("Tools"));
    m_toolBar = tb;   // fullscreen mode hides/reveals it
    tb->setMovable(false);
    tb->setFloatable(false);

    // Side panel button - toggles the side panel (directory + details panel)
    m_actSidePanel = tb->addAction(makeSidePanelIcon(palette()), tr("Side Panel"));
    m_actSidePanel->setCheckable(true);
    m_actSidePanel->setToolTip(tr("Toggle side panel"));
    connect(m_actSidePanel, &QAction::triggered, this, &MainWindow::toggleSidePanel);

    tb->addSeparator();

    m_actGrid  = tb->addAction(tr("Grid"));
    m_actTable = tb->addAction(tr("List"));
    m_actGrid->setCheckable(true);
    m_actTable->setCheckable(true);
    m_actGrid->setChecked(true);

    auto* grp = new QActionGroup(this);
    grp->addAction(m_actGrid);
    grp->addAction(m_actTable);
    grp->setExclusive(true);

    connect(m_actGrid,  &QAction::triggered, this, &MainWindow::switchToGrid);
    connect(m_actTable, &QAction::triggered, this, &MainWindow::switchToTable);
    tb->addSeparator();

    // ONE labelled slider slot, two INDEPENDENT zoom states: the grid's
    // column count and the list's row height are separate settings and
    // separate widgets — only the one belonging to the active view is
    // shown, so neither can drag the other along.
    tb->addWidget(new QLabel(tr("Size: ")));
    // The grid zoom slider carries the COLUMN COUNT, mirrored into the
    // value domain (columns = maximum + 1 − value) so that right = fewer
    // columns = larger previews while the groove still fills left to
    // right (invertedAppearance would fill it from the right). Discrete
    // steps, one per column; the maximum tracks the window width
    // (GridView::updateSliderMaximum, which also re-maps the value).
    m_sizeSlider = new QSlider(Qt::Horizontal);
    {
        const int cols = AppSettings::gridColumns();
        const int maxCols = qMax(16, cols);
        m_sizeSlider->setRange(1, maxCols);
        m_sizeSlider->setValue(maxCols + 1 - cols);
    }
    m_sizeSlider->setSingleStep(1);
    m_sizeSlider->setPageStep(1);
    m_sizeSlider->setFixedWidth(140);
    m_sizeSlider->setFocusPolicy(Qt::NoFocus);
    // Toolbar widgets are shown/hidden through the QAction addWidget
    // returns — hiding the WIDGET does not stick, QToolBar's layout shows
    // it again. (That bug left the grid slider visible in list view, so
    // dragging "the list slider" silently re-zoomed the GRID.)
    m_actSizeSlider = tb->addWidget(m_sizeSlider);

    m_tableSizeSlider = new QSlider(Qt::Horizontal);
    m_tableSizeSlider->setRange(TableView::kMinThumbSize, TableView::kMaxThumbSize);
    m_tableSizeSlider->setValue(AppSettings::tableIconSize());
    m_tableSizeSlider->setFixedWidth(140);
    m_tableSizeSlider->setFocusPolicy(Qt::NoFocus);
    m_actListSizeSlider = tb->addWidget(m_tableSizeSlider);
    m_actListSizeSlider->setVisible(false);

    tb->addSeparator();
    tb->addWidget(new QLabel(tr("Sort: ")));
    m_sortColumnCombo = new QComboBox(tb);
    m_sortColumnCombo->addItem(tr("Name"), MediaModel::Col_FileName);
    m_sortColumnCombo->addItem(tr("Size"), MediaModel::Col_FileSize);
    m_sortColumnCombo->addItem(tr("Modified"), MediaModel::Col_ModifiedDate);
    m_sortColumnCombo->addItem(tr("Created"), MediaModel::Col_CreatedDate);
    m_sortColumnCombo->addItem(tr("Taken"), MediaModel::Col_Taken);
    m_sortColumnCombo->addItem(tr("Type"), MediaModel::Col_FileType);
    tb->addWidget(m_sortColumnCombo);

    m_sortOrderCombo = new QComboBox(tb);
    m_sortOrderCombo->addItem(tr("Ascending"), Qt::AscendingOrder);
    m_sortOrderCombo->addItem(tr("Descending"), Qt::DescendingOrder);
    tb->addWidget(m_sortOrderCombo);

    m_gridView->setIconSizeSlider(m_sizeSlider);
    m_tableView->setThumbnailSize(m_tableSizeSlider->value());

    tb->addSeparator();
    m_filterButton = new QToolButton(tb);
    // "F" is taken by the &File menu — mnemonic is on the "i" instead
    m_filterButton->setText(tr("F&ilter"));
    m_filterButton->setPopupMode(QToolButton::InstantPopup);
    m_filterButton->setCheckable(true);   // checked = filter active (indicator)
    m_filterButton->setToolTip(tr("Filter (Alt+I)"));
    auto* filterMenu = new QMenu(m_filterButton);

    QAction* actFilterSel = filterMenu->addAction(tr("By &Selection"));
    connect(actFilterSel, &QAction::triggered,
            m_gridView, &GridView::requestFilterToSelection);

    QAction* actByTag = filterMenu->addAction(tr("By &Tag…"));
    connect(actByTag, &QAction::triggered, this, [this]() {
        // Tags offered are already scoped to any active selection filter
        // (MediaModel::tagsInDirectory) — narrowing further, not replacing it
        TagFilterDialog dlg(m_model->tagsInDirectory(), m_model->activeTagFilter(), this);
        if (dlg.exec() == QDialog::Accepted)
            m_model->filterToTags(dlg.checkedTags());   // empty = clears the tag criterion
    });

    // Tag SYMBOLS are a setting, not a filter — they live in Settings → Tags.
    filterMenu->addSeparator();
    QAction* actClearFilter = filterMenu->addAction(tr("&Clear Filter"));
    connect(actClearFilter, &QAction::triggered, this, [this]() {
        m_model->clearFilter();
    });
    m_filterButton->setMenu(filterMenu);
    tb->addWidget(m_filterButton);

    tb->addSeparator();
    // Reload: rescans the current directories and scrolls to the first
    // image. The dropdown arrow holds the sync-mode toggle — automatic
    // rescanning on filesystem changes (see MediaModel's QFileSystemWatcher).
    auto* reloadButton = new QToolButton(tb);
    reloadButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    reloadButton->setText(tr("Reload"));
    reloadButton->setPopupMode(QToolButton::MenuButtonPopup);
    connect(reloadButton, &QToolButton::clicked, this, &MainWindow::performManualReload);

    auto* reloadMenu = new QMenu(reloadButton);
    m_actSyncMode = reloadMenu->addAction(tr("Sync Automatically"));
    m_actSyncMode->setCheckable(true);
    m_actSyncMode->setChecked(AppSettings::syncModeEnabled());
    connect(m_actSyncMode, &QAction::toggled, this, [this](bool checked) {
        AppSettings::setSyncModeEnabled(checked);
        m_model->setSyncModeEnabled(checked);
        updateReloadButtonTooltip();
    });
    reloadButton->setMenu(reloadMenu);
    m_reloadButton = reloadButton;
    updateReloadButtonTooltip();
    tb->addWidget(reloadButton);

    // Fullscreen presentation mode — the action also appears in the View
    // menu (buildMenuBar) and is attached to the window itself so the
    // shortcut keeps firing while the menu bar is hidden in fullscreen
    tb->addSeparator();
    m_actFullscreen = tb->addAction(makeFullscreenIcon(palette()),
                                    tr("Fullscreen"));
    m_actFullscreen->setCheckable(true);
    m_actFullscreen->setToolTip(tr("Fullscreen presentation mode (F11)"));
    // Platform binding PLUS explicit F11 — the generic Unix theme leaves
    // QKeySequence::FullScreen empty (see Shortcuts.h)
    m_actFullscreen->setShortcuts(Shortcuts::fullScreen());
    // triggered (not toggled): programmatic setChecked from the
    // WindowStateChange path must not re-enter setFullscreenMode
    connect(m_actFullscreen, &QAction::triggered,
            this, &MainWindow::setFullscreenMode);
    addAction(m_actFullscreen);

    connect(m_tableSizeSlider, &QSlider::valueChanged,
            this, &MainWindow::onTableThumbSizeChanged);
    connect(m_tableSizeSlider, &QSlider::valueChanged, this, [](int value) {
        AppSettings::setTableIconSize(value);
    });
    // Grid zoom persistence lives in GridView::onColumnCountChanged — the
    // slider VALUE is a mirrored position, not the column count.
    connect(m_sortColumnCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSortChanged);
    connect(m_sortOrderCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSortChanged);

}

void MainWindow::openSettings(const QString& pageId)
{
    SettingsDialog dlg(this);
    connect(&dlg, &SettingsDialog::repaintNeeded, this, [this]() {
        m_gridView->viewport()->update();   // e.g. tag badges with new symbols
    });

    // Entry point decides the sidebar: a targeted jump keeps it out of the
    // way, the general entrance shows the full list.
    dlg.setSidebarCollapsed(!pageId.isEmpty());
    if (!pageId.isEmpty())
        dlg.showPage(pageId);

    if (dlg.exec() == QDialog::Accepted)
        applyFocusBorderWidth();
}

void MainWindow::buildMenuBar()
{
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    QAction* actNewWindow = fileMenu->addAction(tr("&New Window"));
    actNewWindow->setShortcut(QKeySequence::New);
    actNewWindow->setStatusTip(tr("Open a copy of this window"));
    connect(actNewWindow, &QAction::triggered, this, &MainWindow::openCloneWindow);
    // Also on the window itself — the shortcut must work while the menu bar
    // is hidden (fullscreen presentation mode), see actQuit below
    addAction(actNewWindow);

    QAction* actOpen = fileMenu->addAction(tr("&Open Folder..."));
    actOpen->setShortcut(QKeySequence::Open);
    connect(actOpen, &QAction::triggered, this, &MainWindow::openFolderDialog);

    fileMenu->addSeparator();
    QAction* actClose = fileMenu->addAction(tr("&Close Window"));
    actClose->setShortcuts(Shortcuts::closeWindow());
    // Window scope (the default) — each window closes itself; closing the
    // last one quits the app (Qt default). Menu-bar-only actions lose their
    // shortcut while the bar is hidden, so register it on the window too.
    connect(actClose, &QAction::triggered, this, &MainWindow::close);
    addAction(actClose);

    QAction* actQuit = fileMenu->addAction(tr("&Quit"));
    // Not setShortcut(QKeySequence::Quit): that is EMPTY on Windows, which
    // would leave the menu entry without an accelerator (see Shortcuts.h).
    actQuit->setShortcuts(Shortcuts::quit());
    // Deliberately WINDOW scope (the default): with multiple main windows
    // ("Open in New Window"), two application-wide Ctrl+Q registrations
    // would be ambiguous and Qt would fire neither. closeAllWindows (not
    // quit) so every window runs its closeEvent and saves its settings.
    connect(actQuit, &QAction::triggered, []() { QApplication::closeAllWindows(); });
    // Also registered on the window itself: actions whose only widget is
    // the menu bar lose their shortcut while the bar is HIDDEN (fullscreen
    // presentation mode) — Ctrl+Q must quit there too.
    addAction(actQuit);

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    m_actCopy = editMenu->addAction(tr("&Copy"));
    m_actCopy->setShortcut(QKeySequence::Copy);
    m_actCopy->setEnabled(false);
    connect(m_actCopy, &QAction::triggered, m_gridView, &GridView::copySelection);

    m_actCut = editMenu->addAction(tr("Cu&t"));
    m_actCut->setShortcut(QKeySequence::Cut);
    m_actCut->setEnabled(false);
    connect(m_actCut, &QAction::triggered, m_gridView, &GridView::cutSelection);

    m_actPaste = editMenu->addAction(tr("&Paste"));
    m_actPaste->setShortcut(QKeySequence::Paste);
    m_actPaste->setEnabled(m_gridView->canPaste());
    connect(m_actPaste, &QAction::triggered, m_gridView, &GridView::pasteFromClipboard);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this]() {
        m_actPaste->setEnabled(m_gridView->canPaste());
    });

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    if (m_actGrid)
        viewMenu->addAction(m_actGrid);
    if (m_actTable)
        viewMenu->addAction(m_actTable);

    viewMenu->addSeparator();
    // Created in buildToolBar (toolbar button); menu entry shares the action
    viewMenu->addAction(m_actFullscreen);

    viewMenu->addSeparator();
    m_actDetails = viewMenu->addAction(tr("&Details Panel"));
    m_actDetails->setCheckable(true);
    m_actDetails->setChecked(AppSettings::detailsPanelVisible());
    connect(m_actDetails, &QAction::toggled, this, [this](bool checked) {
        m_detailsPanel->setVisible(checked);
        AppSettings::setDetailsPanelVisible(checked);
    });

    // Own menu bar section, KDE-style (before Help). Deliberately SHORT: only
    // the settings that get revisited often get their own entry — they open
    // the one settings dialog on their page. Everything else is reached
    // through "Configure BlitzView…". No checkable toggles here: a menu
    // closes on every click, which makes it a poor home for switches (those
    // stay in View). Future entries go above the separator, in the same
    // style: Configure Toolbar…, Configure Shortcuts….
    QMenu* settingsMenu = menuBar()->addMenu(tr("&Settings"));
    QAction* actTagSettings = settingsMenu->addAction(tr("&Tags..."));
    connect(actTagSettings, &QAction::triggered, this, [this]() {
        openSettings(QStringLiteral("tags"));
    });

    settingsMenu->addSeparator();
    QAction* actSettings = settingsMenu->addAction(tr("&Configure BlitzView..."));
    actSettings->setShortcut(QKeySequence(tr("Ctrl+Shift+,")));
    connect(actSettings, &QAction::triggered, this, [this]() { openSettings(); });

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction* actAbout = helpMenu->addAction(tr("&About BlitzView..."));
    connect(actAbout, &QAction::triggered, this, [this]() {
        AboutDialog dlg(this);
        dlg.exec();
    });
}

void MainWindow::buildStatusBar()
{
    m_statusLabel = new QLabel(tr("Ready"));
    statusBar()->addWidget(m_statusLabel);

    // Separator between the counter and the focus path
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Sunken);
    statusBar()->addPermanentWidget(sep);

    // Full path of the focused item (hover / single selection). Left-aligned
    // within the remaining status bar space, so it starts right after the
    // separator. ElidedLabel guarantees the content never changes the window
    // width.
    m_focusPathLabel = new ElidedLabel(this);
    m_focusPathLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_focusPathLabel->setContentsMargins(4, 0, 0, 0);
    statusBar()->addPermanentWidget(m_focusPathLabel, 1);
}

void MainWindow::toggleSidePanel()
{
    const bool visible = !m_sidePanel->isVisible();

    // If showing and no tree anchor is set yet, initialize to home directory
    if (visible && m_dirPanel->treeAnchor().isEmpty()) {
        m_dirPanel->goHome();
    }

    setSidePanelVisibleAnimated(visible);
    if (m_actSidePanel)
        m_actSidePanel->setChecked(visible);
}

void MainWindow::showSidePanel()
{
    // Only show if not already visible - don't toggle
    if (!m_sidePanel->isVisible()) {
        // If no tree anchor is set yet, initialize to home directory
        if (m_dirPanel->treeAnchor().isEmpty()) {
            m_dirPanel->goHome();
        }

        setSidePanelVisibleAnimated(true);
        if (m_actSidePanel)
            m_actSidePanel->setChecked(true);
    }
}

void MainWindow::openFolderDialog()
{
    QStringList sel = m_dirPanel->selectedDirectories();
    QString startDir = sel.isEmpty() ? QDir::homePath() : sel.last();
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Folder"), startDir);
    if (!dir.isEmpty()) {
        m_dirPanel->setCurrentDirectory(dir);
        showSidePanel();
    }
}

void MainWindow::onSelectedDirectoriesChanged(const QStringList& recursive,
                                               const QStringList& individual)
{
    AppSettings::setRecursiveDirs(recursive);
    AppSettings::setIndividualDirs(individual);
    QStringList all = recursive + individual;
    AppSettings::setLastDir(all.isEmpty() ? QString() : all.last());

    m_selectedRecursive = recursive;
    m_selectedIndividual = individual;
    updateWindowTitle();

    // Drops/paste are only possible when exactly one root dir is shown —
    // both views take the same target
    const QStringList allRoots = recursive + individual;
    const QString dropTarget = allRoots.size() == 1 ? allRoots.first() : QString();
    m_gridView->setDropTargetDir(dropTarget);
    m_tableView->setDropTargetDir(dropTarget);
    if (m_actPaste)
        m_actPaste->setEnabled(m_gridView->canPaste());

    m_loadedCount = 0;
    m_model->loadDirectories(recursive, individual);
    onSortChanged();
}

void MainWindow::performManualReload()
{
    // Rescan, then scroll to the first image once it lands — a sync-mode
    // triggered reload does NOT do this, it keeps the current scroll
    // position (GridView restores it by path automatically on every
    // model reset). Self-disconnecting one-shot: fires once for THIS
    // reload only.
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_model, &MediaModel::loadingFinished, this, [this, conn](int) {
        QObject::disconnect(*conn);
        m_gridView->resetGridOffset();
    });
    m_model->loadDirectories(m_selectedRecursive, m_selectedIndividual);
}

void MainWindow::updateReloadButtonTooltip()
{
    if (!m_reloadButton)
        return;
    const bool syncOn = m_actSyncMode && m_actSyncMode->isChecked();
    m_reloadButton->setToolTip(syncOn
        ? tr("Reload (auto-sync on)")
        : tr("Reload (auto-sync off)"));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    const qint64 tClose = slideTraceMs();
    // Stop this window's background work right away: a library of tens of
    // thousands of files keeps the loader busy for minutes, and the pool is
    // only waited for (not cancelled) in ~MediaModel. The exiftool daemon is
    // shut down centrally in aboutToQuit — doing it here would kill it for
    // the OTHER windows too when just one clone closes.
    if (m_model)
        m_model->cancelBackgroundWork();

    // A transition overlay must not outlive its window
    if (m_fsOverlay) {
        m_fsOverlay->close();
        setWindowOpacity(1.0);
        m_gridView->setHoverSuspended(false);
    }

    // Closing while in fullscreen mode: leave it first — restores the app
    // palette (refcounted) and the remembered side-panel visibility, so the
    // persisted state below is the pre-fullscreen one. The geometry must
    // come from the snapshot; saveGeometry() would restore into fullscreen.
    if (m_fsUiApplied) {
        applyFullscreenUi(false);
        if (!m_fsWindowedGeometry.isEmpty())
            AppSettings::setWindowGeometry(m_fsWindowedGeometry);
    } else {
        AppSettings::setWindowGeometry(saveGeometry());
    }
    AppSettings::setSplitterState(m_hSplitter->saveState());
    AppSettings::setSideSplitterState(m_sidePanel->saveState());
    AppSettings::setDirPanelVisible(m_sidePanel->isVisible());
    AppSettings::setTreeAnchor(m_dirPanel->treeAnchor());
    TRACE_SLIDE("shutdown closeEvent dur=%lldms", (long long)(slideTraceMs() - tClose));
    QMainWindow::closeEvent(event);
}

void MainWindow::updateWindowTitle()
{
    QStringList parts;
    for (const QString& p : m_selectedRecursive + m_selectedIndividual)
        parts.append(QFileInfo(p).fileName());
    parts.sort(Qt::CaseInsensitive);

    if (parts.isEmpty())
        setWindowTitle(QStringLiteral("BlitzView"));
    else
        setWindowTitle(parts.join(QStringLiteral(", ")) + QStringLiteral(" - BlitzView"));
}

void MainWindow::openCloneWindow()
{
    // Independent top-level window; frees itself on close. The app quits
    // when the last window closes (Qt default).
    auto* win = new MainWindow(QStringList{}, nullptr, this);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->show();
    win->raise();
    win->activateWindow();
}

void MainWindow::switchToGrid()
{
    m_stack->setCurrentWidget(m_gridView);
    AppSettings::setListViewActive(false);
    if (m_actSizeSlider)     m_actSizeSlider->setVisible(true);
    if (m_actListSizeSlider) m_actListSizeSlider->setVisible(false);
    // The grid's EFFECTIVE (fitted) icon size, not the slider's nominal one
    if (m_model)
        m_model->invalidateThumbnails(
            QSize(m_gridView->iconSize(), m_gridView->iconSize()));
}

void MainWindow::switchToTable()
{
    m_stack->setCurrentWidget(m_tableView);
    AppSettings::setListViewActive(true);
    if (m_actSizeSlider)     m_actSizeSlider->setVisible(false);
    if (m_actListSizeSlider) m_actListSizeSlider->setVisible(true);
    if (m_model && m_tableSizeSlider)
        m_model->invalidateThumbnails(QSize(m_tableSizeSlider->value(), m_tableSizeSlider->value()));
    // The shared selection should be where the user left it in the grid
    m_tableView->scrollToSelection();
}

void MainWindow::onSortChanged()
{
    if (!m_model || !m_sortColumnCombo || !m_sortOrderCombo)
        return;

    const int col = m_sortColumnCombo->currentData().toInt();
    const auto order = static_cast<Qt::SortOrder>(m_sortOrderCombo->currentData().toInt());
    m_model->sort(col, order);

    if (m_gridView) {
        m_gridView->resetGridOffset();
    }
    if (m_tableView) {
        m_tableView->showSortIndicator(col, order);
        m_tableView->scrollToTop();
    }
}

void MainWindow::onTableSortRequested(int column, Qt::SortOrder order)
{
    if (!m_sortColumnCombo || !m_sortOrderCombo)
        return;
    const int colIdx = m_sortColumnCombo->findData(column);
    if (colIdx < 0)
        return;   // column MediaModel cannot sort by (preview, resolution, duration)
    const int orderIdx = m_sortOrderCombo->findData(static_cast<int>(order));

    // Adopt into the toolbar combos without firing onSortChanged twice
    {
        const QSignalBlocker b1(m_sortColumnCombo);
        const QSignalBlocker b2(m_sortOrderCombo);
        m_sortColumnCombo->setCurrentIndex(colIdx);
        if (orderIdx >= 0)
            m_sortOrderCombo->setCurrentIndex(orderIdx);
    }
    onSortChanged();
}

void MainWindow::onFocusItemChanged(int row)
{
    if (row >= 0 && row < m_model->rowCount()) {
        const MediaItem& item = m_model->item(row);
        m_detailsPanel->showItem(item);
        m_focusPathLabel->setFullText(item.filePath);
    } else {
        m_detailsPanel->clearItem();
        m_focusPathLabel->setFullText(QString());
    }
}

void MainWindow::onFilterToSelection(const QList<int>& rows)
{
    QStringList paths;
    for (int r : rows)
        if (r >= 0 && r < m_model->rowCount())
            paths.append(m_model->item(r).filePath);
    if (!paths.isEmpty())
        m_model->filterToPaths(paths);
}

void MainWindow::syncSelectionToTable()
{
    if (m_gridView && m_tableView)
        m_tableView->setSelectedRows(m_gridView->selectedRows());
}

void MainWindow::onTableThumbSizeChanged(int value)
{
    if (!m_tableView || !m_model)
        return;

    m_tableView->setThumbnailSize(value);
    if (m_stack && m_stack->currentWidget() == m_tableView)
        m_model->invalidateThumbnails(QSize(value, value));
}

void MainWindow::onItemDoubleClicked(int sourceRow)
{
    if (sourceRow < 0 || sourceRow >= m_model->rowCount()) return;

    // Unified viewer navigates the full mixed list — images and videos alike
    if (AppSettings::multipleViewers()) {
        // Every double-click gets its own window, freed on close
        auto* viewer = new MediaViewer(this);
        viewer->setAttribute(Qt::WA_DeleteOnClose);
        watchViewerFocus(viewer);
        viewer->setMedia(m_model->items(), sourceRow);
        // Presentation mode: the viewer joins fullscreen; Escape there
        // closes it, returning to the fullscreen main window
        viewer->setCloseOnFullscreenExit(isFullScreen());
        if (isFullScreen())
            viewer->enterFullscreen();
        else
            viewer->show();
        viewer->raise();
        viewer->activateWindow();
        return;
    }

    if (!m_mediaViewer) {
        m_mediaViewer = new MediaViewer(this);
        watchViewerFocus(m_mediaViewer);
    }
    m_mediaViewer->setMedia(m_model->items(), sourceRow);
    // Un-minimize if needed; raise() alone does not transfer keyboard focus
    // to an already-open window — activateWindow() does
    m_mediaViewer->setWindowState(m_mediaViewer->windowState() & ~Qt::WindowMinimized);
    m_mediaViewer->setCloseOnFullscreenExit(isFullScreen());
    if (isFullScreen() && !m_mediaViewer->isFullScreen())
        m_mediaViewer->enterFullscreen();
    else
        m_mediaViewer->show();
    m_mediaViewer->raise();
    m_mediaViewer->activateWindow();
}

void MainWindow::changeEvent(QEvent* event)
{
    // The main window becoming active ends the aux-window focus fallback —
    // now nothing in blitzview justifies an external focus frame
    if (event->type() == QEvent::ActivationChange && isActiveWindow()
        && s_frameActiveWin) {
        s_frameActiveWin.clear();
        updateExternalFocus();
    }
    // Single entry point for the fullscreen UI: F11/Escape and WM-initiated
    // state changes all land here
    if (event->type() == QEvent::WindowStateChange
        && isFullScreen() != m_fsUiApplied) {
        applyFullscreenUi(isFullScreen());
        // Overlay transition: the switched layout is about to settle —
        // capture the REAL target rects and start the flight. NOT for the
        // switch-back of a reversal (awaiting flag cleared): its settle
        // runs in finishOverlayReversal, no new flight starts.
        if (m_fsOverlay && m_fsOverlayAwaitingTarget)
            QTimer::singleShot(0, this,
                               &MainWindow::finishFullscreenOverlay);
    }
    // The toolbar icons bake their color — re-bake when the dark palette
    // arrives/leaves (and on every palette-fade frame), otherwise they
    // become invisible on the dark toolbar
    if (event->type() == QEvent::PaletteChange) {
        if (m_actSidePanel)
            m_actSidePanel->setIcon(makeSidePanelIcon(palette()));
        if (m_actFullscreen)
            m_actFullscreen->setIcon(makeFullscreenIcon(palette()));
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (m_fsHotZone && m_fsHotZone->isVisible())
        updateFsHotZoneGeometry();
}

void MainWindow::setFullscreenMode(bool on)
{
    // Checked BEFORE the isFullScreen guard below: while the overlay
    // lives, the toggle relates to the VISUAL heading (which may oppose
    // the real window state — that switches at the start of the flight)
    if (m_fsOverlay) {
        // A transition is in flight: REVERSE it from its current position
        // instead of ignoring the toggle — only the end sequence
        // (settle/handover) is uninterruptible
        bool heading = m_fsOverlayEnter
            != (m_fsOverlay->isBackward() != m_fsOverlayReversePending);
        if (!m_fsOverlayFinishing && on != heading) {
            if (m_fsOverlaySwitchPending) {
                // The real window has not switched yet — cancel
                // outright: the overlay still shows the untouched
                // screen state, removing it changes nothing visible
                m_fsOverlaySwitchPending = false;
                m_gridView->setReflowSuppressed(false);
                m_gridView->setHoverSuspended(false);
                m_fsOverlay->deleteLater();
            } else if (m_fsOverlayAwaitingTarget) {
                // Settle phase, flight not started — reverse the
                // moment it starts (finishFullscreenOverlay)
                m_fsOverlayReversePending = !m_fsOverlayReversePending;
            } else {
                m_fsOverlay->reverse();
            }
            heading = on;
        }
        // Sync the action to the heading, not to isFullScreen(): the
        // NEXT press must request the opposite of where we are going
        m_actFullscreen->setChecked(heading);
        return;
    }
    if (on == isFullScreen())
        return;
    // Desktop-overlay transition: cells fly from their current screen
    // positions to the (real, post-switch) target raster while the
    // background fades transparent<->black (see startFullscreenOverlay).
    // The technique needs global overlay positioning, override-redirect,
    // windowOpacity and a screen grab — X11/Windows only; Wayland takes
    // the instant path.
    const QString platform = QGuiApplication::platformName();
    const bool overlayCapable = platform == QLatin1String("xcb")
        || platform == QLatin1String("windows")
        || platform == QLatin1String("offscreen");
    if (AppSettings::fullscreenAnimationMs() > 0 && overlayCapable) {
        // The overlay performs the window switch itself, deferred until
        // its first frame is verifiably on screen — switching here, in
        // the same event-loop pass, let the compositor present the bare
        // switch before the overlay covered it (title-bar flicker
        // entering, restored-window flash leaving)
        startFullscreenOverlay(on);
        return;
    }
    applyFullscreenWindowState(on);
    // All UI/palette work happens in applyFullscreenUi via changeEvent
}

void MainWindow::applyFullscreenWindowState(bool on)
{
    if (on) {
        // Captured BEFORE entering — showNormal() is asynchronous, so
        // saving on exit would still record the fullscreen size
        m_fsWindowedGeometry = saveGeometry();
        m_fsWindowedStackW = m_stack->width();
        showFullScreen();
    } else {
        showNormal();
    }
}

void MainWindow::startFullscreenOverlay(bool enteringFullscreen)
{
    // Source cells only from the grid; with the table view active the
    // overlay still provides the background fade
    QList<FullscreenTransitionOverlay::Cell> cells;
    m_fsOverlaySourceRows.clear();
    m_fsOverlaySourceLayout = GridView::LayoutSnapshot();
    const bool gridActive = (m_stack->currentWidget() == m_gridView);
    QRect vpRect;
    if (gridActive) {
        const auto visible = m_gridView->captureCellRects();
        cells.reserve(visible.size());
        for (const auto& vc : visible) {
            FullscreenTransitionOverlay::Cell c;
            c.row = vc.row;
            c.from = QRect(
                m_gridView->viewport()->mapToGlobal(vc.rect.topLeft()),
                vc.rect.size());
            c.selected = vc.selected;
            cells.append(c);
            m_fsOverlaySourceRows.insert(vc.row);
        }
        // Pre-switch raster: rows visible only in the target fly in from
        // the position they would occupy here (finishFullscreenOverlay)
        m_fsOverlaySourceLayout = m_gridView->captureLayoutSnapshot();
        vpRect = QRect(m_gridView->viewport()->mapToGlobal(QPoint(0, 0)),
                       m_gridView->viewport()->size());
    }

    auto* overlay = new FullscreenTransitionOverlay(m_model, screen(),
                                                    enteringFullscreen);
    overlay->setAttribute(Qt::WA_DeleteOnClose, true);
    m_fsOverlay = overlay;
    m_fsOverlaySettleRetries = 0;
    m_fsOverlayLastWidth = -1;
    m_fsOverlayEnter = enteringFullscreen;
    m_fsOverlayAwaitingTarget = true;
    m_fsOverlayReversePending = false;
    m_fsOverlayFinishing = false;
    m_fsOverlaySourceStackW = m_stack->width();
    overlay->setSource(cells, m_gridView->iconSize(),
                       QApplication::palette());
    // Entering: backdrop of the still-windowed window (decorated frame,
    // empty grid) under the black fade — it disappears only once the fade
    // is fully opaque. Captured BEFORE the switch. Leaving needs no
    // backdrop: the real window itself stays visible (see below).
    if (enteringFullscreen) {
        QRect bdRect;
        const QPixmap bd = grabWindowBackdrop(&bdRect);
        overlay->setWindowBackdrop(bd, bdRect);
        overlay->setViewportClip(vpRect);
    }
    overlay->show();
    overlay->raise();
    TRACE_SLIDE("overlay start enter=%d cells=%d", int(enteringFullscreen),
                int(cells.size()));

    connect(overlay, &FullscreenTransitionOverlay::transitionFinished, this,
            [this](bool backward) {
        TRACE_SLIDE("overlay finished backward=%d", int(backward));
        m_fsOverlayFinishing = true;
        if (backward) {
            // The flight returned to its source: undo the real window's
            // switch behind the overlay's end state (opaque black when an
            // exit was aborted, invisible window + backdrop when an enter
            // was aborted), then wait for the settled layout
            applyFullscreenWindowState(!m_fsOverlayEnter);
            m_fsOverlaySettleRetries = 0;
            m_fsOverlayLastWidth = -1;
            QTimer::singleShot(0, this, &MainWindow::finishOverlayReversal);
            return;
        }
        restoreWindowAfterOverlay();
    });

    // The overlay is mouse-transparent and the real window stays mapped —
    // keep the grid's hover machinery quiet, and let the switch resize
    // SNAP the raster (a reflow would run unseen and could still be
    // mid-flight at handover)
    m_gridView->setHoverSuspended(true);
    m_gridView->setReflowSuppressed(true);

    // The real window mutates only once the overlay's first frame is ON
    // SCREEN — the static source frame is pixel-identical to the current
    // screen content, so the wait is invisible. The fallback timer keeps
    // the switch from wedging if the paint signal never arrives.
    m_fsOverlaySwitchPending = true;
    const auto doSwitch = [this, enteringFullscreen]() {
        if (!m_fsOverlaySwitchPending || !m_fsOverlay)
            return;
        m_fsOverlaySwitchPending = false;
        TRACE_SLIDE("overlay presented, switching enter=%d",
                    int(enteringFullscreen));
        if (enteringFullscreen) {
            // The window must BECOME the fullscreen window for the target
            // measurement — it switches invisibly, the backdrop stands in
            setWindowOpacity(0.0);
        } else {
            // Leaving: the real window stays VISIBLE — it restores to its
            // windowed decorated self behind the (initially opaque)
            // overlay and is revealed live as the black clears. Only the
            // grid images are suppressed: they are the flying tiles, and
            // they land exactly where the real grid repaints them at
            // handover.
            m_gridView->setSuppressItemPaint(true);
            m_gridView->viewport()->update();
        }
        applyFullscreenWindowState(enteringFullscreen);
    };
    connect(overlay, &FullscreenTransitionOverlay::sourceFramePresented,
            this, doSwitch);
    QTimer::singleShot(250, this, doSwitch);
}

void MainWindow::finishFullscreenOverlay()
{
    if (!m_fsOverlay)
        return;
    // Wait until the switched layout reached its EXPECTED width (screen
    // width entering; the remembered pre-fullscreen stack width leaving).
    // Width STABILITY alone is not enough: the WM restore can pause at
    // intermediate geometries (panel re-shown, height restored) for more
    // than one sample period and fake stability — target rects captured
    // there point at a layout that never survives (visible end snap).
    // Stability sampling remains the fallback when no expectation exists
    // (e.g. exit after a WM-initiated enter) or it never materializes.
    const int expected = isFullScreen()
        ? (screen() ? screen()->geometry().width() : -1)
        : m_fsWindowedStackW;
    const int w = m_stack->width();
    // Entering, the screen width is only a HEURISTIC (a WM-less setup
    // never reaches it) — expectation OR stability. Leaving, the
    // remembered width is exact: stability alone may not settle early.
    const bool ready = isFullScreen()
        ? (w == expected || w == m_fsOverlayLastWidth)
        : (expected > 0 ? (w == expected) : (w == m_fsOverlayLastWidth));
    if (!ready && m_fsOverlaySettleRetries++ < 30) {
        m_fsOverlayLastWidth = w;
        QTimer::singleShot(16, this, &MainWindow::finishFullscreenOverlay);
        return;
    }
    TRACE_SLIDE("overlay target capture stackW=%d expected=%d retries=%d",
                w, expected, m_fsOverlaySettleRetries);

    QList<FullscreenTransitionOverlay::Cell> cells;
    QRect vpRect;
    if (m_stack->currentWidget() == m_gridView) {
        const auto targetLayout = m_gridView->captureLayoutSnapshot();
        const auto visible = m_gridView->captureCellRects();
        cells.reserve(visible.size());
        QSet<int> targetRows;
        for (const auto& vc : visible) {
            FullscreenTransitionOverlay::Cell c;
            c.row = vc.row;
            c.to = QRect(
                m_gridView->viewport()->mapToGlobal(vc.rect.topLeft()),
                vc.rect.size());
            c.selected = vc.selected;
            // Visible only in the target: fly in (fading) from the
            // position the row would occupy in the pre-switch raster
            if (!m_fsOverlaySourceRows.contains(vc.row))
                c.from = m_fsOverlaySourceLayout.globalRectForRow(vc.row);
            cells.append(c);
            targetRows.insert(vc.row);
        }
        // Visible only in the source: fly out (fading) towards the
        // position the row would occupy in the new raster
        for (const int row : std::as_const(m_fsOverlaySourceRows)) {
            if (targetRows.contains(row))
                continue;
            FullscreenTransitionOverlay::Cell c;
            c.row = row;
            c.to = targetLayout.globalRectForRow(row);
            c.fadeOut = true;
            if (c.to.isValid())
                cells.append(c);
        }
        vpRect = QRect(m_gridView->viewport()->mapToGlobal(QPoint(0, 0)),
                       m_gridView->viewport()->size());
    }
    // Leaving: clip the flight to the (now settled) windowed grid
    // viewport — entering set the clip from the pre-switch layout
    if (!isFullScreen())
        m_fsOverlay->setViewportClip(vpRect);
    m_fsOverlay->startTo(cells, m_gridView->iconSize(),
                         QApplication::palette(),
                         AppSettings::fullscreenAnimationMs());
    m_fsOverlayAwaitingTarget = false;
    if (m_fsOverlayReversePending) {
        // Toggled during the settle phase: reverse right at launch —
        // the flight turns around after a frame at most
        m_fsOverlayReversePending = false;
        m_fsOverlay->reverse();
    }
}

void MainWindow::finishOverlayReversal()
{
    if (!m_fsOverlay)
        return;
    // The reversal returns to the EXACT layout the transition started
    // from — wait for that stack width (the overlay holds its source
    // frame meanwhile, pixel-identical to the window below). Trusting
    // width stability here restored visibility at an intermediate WM
    // geometry once (un-fullscreen pauses mid-restore), which snapped
    // the wrong-size window into place. Stability sampling remains the
    // fallback if the expected width never comes back.
    const int w = m_stack->width();
    const bool ready = m_fsOverlaySourceStackW > 0
        ? (w == m_fsOverlaySourceStackW)
        : (w == m_fsOverlayLastWidth);
    if (!ready && m_fsOverlaySettleRetries++ < 30) {
        m_fsOverlayLastWidth = w;
        QTimer::singleShot(16, this, &MainWindow::finishOverlayReversal);
        return;
    }
    TRACE_SLIDE("overlay reversal settled stackW=%d expected=%d retries=%d",
                w, m_fsOverlaySourceStackW, m_fsOverlaySettleRetries);
    restoreWindowAfterOverlay();
}

void MainWindow::restoreWindowAfterOverlay()
{
    setWindowOpacity(1.0);
    m_gridView->setSuppressItemPaint(false);
    m_gridView->viewport()->update();
    m_gridView->setReflowSuppressed(false);
    // Tear the overlay down only once the RESTORED window is actually on
    // screen: unmapping it in the same event-loop pass let the compositor
    // remove it BEFORE the grid's repaint (images back, opacity back) was
    // presented — the still-empty grid flashed white at handover. The
    // overlay's final frame equals the window state below, so the hold is
    // invisible.
    QTimer::singleShot(50, this, [this]() {
        TRACE_SLIDE("overlay teardown");
        m_gridView->setHoverSuspended(false);
        if (m_fsOverlay)
            m_fsOverlay->deleteLater();
    });
}

QPixmap MainWindow::grabWindowBackdrop(QRect* globalRect)
{
    // Client area rendered with an EMPTY grid; no update() is triggered,
    // so the real on-screen window never shows the suppressed state
    m_gridView->setSuppressItemPaint(true);
    const QPixmap client = grab();
    m_gridView->setSuppressItemPaint(false);
    const QRect clientRect(mapToGlobal(QPoint(0, 0)), size());

    // WM decorations cannot be rendered — take their pixels from the
    // SCREEN: entering fullscreen the decorated window really is on
    // screen right now (the overlay does not exist yet). Foreign windows
    // overlapping the frame band would be baked in — accepted, the user
    // just interacted with this window. A failed grab (e.g. Wayland)
    // degrades to the client-area-only backdrop.
    const QRect frameRect = frameGeometry();
    QPixmap frame;
    if (screen() && frameRect.isValid() && frameRect.contains(clientRect))
        frame = screen()->grabWindow(0, frameRect.x(), frameRect.y(),
                                     frameRect.width(),
                                     frameRect.height());
    if (!frame.isNull()) {
        QPainter p(&frame);
        p.drawPixmap(clientRect.topLeft() - frameRect.topLeft(), client);
        p.end();
        *globalRect = frameRect;
        return frame;
    }
    *globalRect = clientRect;
    return client;
}

void MainWindow::applyFullscreenUi(bool on)
{
    m_fsUiApplied = on;

    // A mode switch invalidates any running slide/fade — snap to clean state
    m_fsChromeAnim->stop();
    m_sidePanelAnim->stop();
    m_paletteAnim->stop();
    setPalette(QPalette());
    menuBar()->setMaximumHeight(QWIDGETSIZE_MAX);
    m_toolBar->setMaximumHeight(QWIDGETSIZE_MAX);
    m_sidePanel->setMaximumWidth(QWIDGETSIZE_MAX);

    const int animMs = AppSettings::fullscreenAnimationMs();
    if (on) {
        // Only the FIRST fullscreen window switches the app palette —
        // fade this window from the system look into the dark one then.
        // With the overlay transition the window is invisible (opacity 0)
        // during the switch: palette applies instantly, the overlay does
        // the visual cross-fade itself.
        const bool wasSystem = (s_fullscreenWindows == 0);
        acquireDarkPalette();
        if (wasSystem && animMs > 0 && !m_fsOverlay)
            startPaletteFade(s_systemPalette,
                             makeDarkPalette(s_systemPalette));
        menuBar()->hide();
        m_toolBar->hide();
        statusBar()->hide();
        // Hide the side panel for the presentation look, but remember the
        // state — the toggle stays functional via the revealed toolbar
        m_fsSidePanelWasVisible = m_sidePanel->isVisible();
        if (m_fsSidePanelWasVisible)
            m_sidePanelLastWidth = qMax(50, m_sidePanel->width());
        m_sidePanel->setVisible(false);
        m_actSidePanel->setChecked(false);
        updateFsHotZoneGeometry();
        m_fsHotZone->show();
        m_fsHotZone->raise();
    } else {
        const bool toSystem = (s_fullscreenWindows == 1);
        m_fsChromeTimer->stop();
        m_fsHotZone->hide();
        menuBar()->show();
        m_toolBar->show();
        statusBar()->show();
        // Restore the REMEMBERED visibility, not whatever the user toggled
        // meanwhile inside fullscreen
        m_sidePanel->setVisible(m_fsSidePanelWasVisible);
        m_actSidePanel->setChecked(m_fsSidePanelWasVisible);
        releaseDarkPalette();
        if (toSystem && animMs > 0 && !m_fsOverlay)
            startPaletteFade(makeDarkPalette(s_systemPalette),
                             s_systemPalette);
    }
    m_escShortcut->setEnabled(on);
    m_actFullscreen->setChecked(on);
}

void MainWindow::startPaletteFade(const QPalette& from, const QPalette& to)
{
    m_paletteAnim->stop();
    m_paletteFadeFrom = from;
    m_paletteFadeTo = to;
    setPalette(from);
    m_paletteAnim->setStartValue(0.0);
    m_paletteAnim->setEndValue(1.0);
    m_paletteAnim->setDuration(AppSettings::fullscreenAnimationMs());
    m_paletteAnim->start();
}

void MainWindow::setSidePanelVisibleAnimated(bool visible)
{
    const int ms = AppSettings::fullscreenAnimationMs();
    const bool wasAnimating =
        m_sidePanelAnim->state() == QAbstractAnimation::Running;
    // Capture BEFORE stop(): stopping a running animation emits finished,
    // whose handler may hide the panel (interrupted hide slide)
    const int cur = m_sidePanel->isVisible() ? m_sidePanel->width() : 0;
    TRACE_SLIDE("setSidePanelVisibleAnimated(%d) cur=%d wasAnim=%d "
                "panelVisible=%d sizes=%d/%d",
                int(visible), cur, int(wasAnimating),
                int(m_sidePanel->isVisible()), m_hSplitter->sizes().value(0),
                m_hSplitter->sizes().value(1));
    m_sidePanelAnim->stop();

    // Slide in normal mode too: an instant panel show/hide would leave the
    // grid to animate a raster change against an already-jumped viewport
    // (whitespace sliding out on hide, an instant jump on show) — the
    // synchronized slide is what keeps grid content and panel edge together
    if (ms <= 0) {
        m_sidePanel->setMaximumWidth(QWIDGETSIZE_MAX);
        m_sidePanel->setVisible(visible);
        return;
    }
    if (visible) {
        if (!m_sidePanel->isVisible()) {
            m_sidePanel->setMaximumWidth(0);
            m_sidePanel->setVisible(true);
            // Pin the splitter allocation to 0 NOW: showing the widget
            // makes the splitter reserve its remembered width on the next
            // layout pass — that would flash a full-size panel for one
            // frame before the first animation tick takes it back
            const int total = m_hSplitter->width() - m_hSplitter->handleWidth();
            m_hSplitter->setSizes({0, total});
            TRACE_SLIDE("pinned sizes after show: %d/%d panelW=%d",
                        m_hSplitter->sizes().value(0),
                        m_hSplitter->sizes().value(1), m_sidePanel->width());
        }
        m_sidePanelAnim->setStartValue(cur);
        m_sidePanelAnim->setEndValue(m_sidePanelLastWidth);
    } else {
        if (!m_sidePanel->isVisible()) {
            m_sidePanel->setMaximumWidth(QWIDGETSIZE_MAX);
            return;
        }
        // Remember the fully-open width as the next slide-in target —
        // not a partial width from an interrupted slide
        if (!wasAnimating)
            m_sidePanelLastWidth = qMax(50, cur);
        m_sidePanelAnim->setStartValue(cur);
        m_sidePanelAnim->setEndValue(0);
    }
    // Slide guard against mid-slide relayouts: a deferred LayoutRequest
    // (e.g. queued DetailsPanel label updates delivered on show) makes the
    // splitter re-allocate between two animation ticks, and its layout
    // engine floors the panel at the children-derived minimum size HINT
    // (180 px explicit tree minimum, ~70 px without it) regardless of the
    // animated maximum — one such frame reads as a jerk. An EXPLICIT
    // minimum overrides the hint in qSmartMinSize, so pinning it to 1 px
    // makes any rogue relayout reproduce the animated allocation instead.
    // (The panel has no explicit minimum otherwise — reset to 0 at the end.)
    m_sidePanel->setMinimumWidth(1);
    // Arm the grid's flow transition to the final raster — driven by the
    // slide's progress from the valueChanged handler above
    const int handle = m_hSplitter->handleWidth();
    const int finalPanel = visible ? m_sidePanelLastWidth : 0;
    const int finalGrid = m_hSplitter->width()
        - (finalPanel > 0 ? finalPanel + handle : 0);
    m_gridView->beginPanelSlide(finalGrid - m_gridView->width());
    m_sidePanelAnim->setDuration(ms);
    m_sidePanelAnim->start();
}

void MainWindow::revealFullscreenChrome()
{
    if (!isFullScreen())
        return;
    if (m_fsChromeAnim->state() == QAbstractAnimation::Running)
        return;
    m_fsHotZone->hide();

    const int ms = AppSettings::fullscreenAnimationMs();
    if (ms <= 0) {
        menuBar()->show();
        m_toolBar->show();
        m_fsChromeTimer->start();
        return;
    }
    // Slide in: grow from 0 to the natural heights via the max-height
    // constraint; released when the slide completes
    m_fsMenuNaturalH = menuBar()->sizeHint().height();
    m_fsToolNaturalH = m_toolBar->sizeHint().height();
    menuBar()->setMaximumHeight(0);
    m_toolBar->setMaximumHeight(0);
    menuBar()->show();
    m_toolBar->show();
    m_fsChromeAnim->setStartValue(0.0);
    m_fsChromeAnim->setEndValue(1.0);
    m_fsChromeAnim->setDuration(ms);
    m_fsChromeAnim->start();
}

void MainWindow::hideFullscreenChrome()
{
    if (m_fsChromeAnim->state() == QAbstractAnimation::Running)
        return;

    const int ms = AppSettings::fullscreenAnimationMs();
    if (ms <= 0) {
        menuBar()->hide();
        m_toolBar->hide();
        if (isFullScreen()) {
            updateFsHotZoneGeometry();
            m_fsHotZone->show();
            m_fsHotZone->raise();
        }
        return;
    }
    m_fsMenuNaturalH = menuBar()->height();
    m_fsToolNaturalH = m_toolBar->height();
    m_fsChromeAnim->setStartValue(1.0);
    m_fsChromeAnim->setEndValue(0.0);
    m_fsChromeAnim->setDuration(ms);
    m_fsChromeAnim->start();
}

void MainWindow::updateFsHotZoneGeometry()
{
    if (m_fsHotZone)
        m_fsHotZone->setGeometry(0, 0, width(), 3);
}

MainWindow::~MainWindow()
{
    s_allWindows.removeOne(this);
}

QString MainWindow::auxWindowPath(QWidget* w)
{
    if (!w)
        return QString();
    if (auto* viewer = qobject_cast<MediaViewer*>(w))
        return viewer->currentFilePath();
    return w->property("blitzFilePath").toString();
}

void MainWindow::setAuxBorderActive(QWidget* w, bool on)
{
    if (!w)
        return;
    if (auto* viewer = qobject_cast<MediaViewer*>(w))
        viewer->setFocusBorderActive(on);
    else if (auto* frame = static_cast<FocusBorderFrame*>(
                 w->property("focusBorderFrame").value<QWidget*>()))
        frame->setActive(on);
}

void MainWindow::applyDriverBorder()
{
    // The driving window's border is on while ANY open grid displays the
    // external focus (local hover overrides only that one grid)
    bool displayed = false;
    for (MainWindow* mw : std::as_const(s_allWindows))
        displayed = displayed || mw->m_externalDisplayed;
    setAuxBorderActive(s_frameDrivingWin, displayed);
}

void MainWindow::updateExternalFocus()
{
    // The aux window under the mouse wins; the focused one is the fallback.
    // Only when the mouse is in none AND none has focus is there no frame.
    QWidget* driver = s_frameMouseWin;
    QString path = auxWindowPath(driver);
    if (path.isEmpty()) {
        driver = s_frameActiveWin;
        path = auxWindowPath(driver);
    }
    if (path.isEmpty())
        driver = nullptr;

    // Updates every grid's m_externalDisplayed via
    // externalFocusDisplayChanged before the border is applied below
    for (MainWindow* mw : std::as_const(s_allWindows))
        mw->m_gridView->setExternalFocusPath(path);

    if (s_frameDrivingWin && s_frameDrivingWin != driver)
        setAuxBorderActive(s_frameDrivingWin, false);
    s_frameDrivingWin = driver;
    applyDriverBorder();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    // Watched objects are exclusively aux windows (Details dialogs,
    // viewers). Enter/Leave arrive at the top-level widget whenever the
    // cursor enters/leaves the window as a whole; over the grid itself the
    // mouse drives the frame directly via hover (which takes precedence).
    if (auto* w = qobject_cast<QWidget*>(watched)) {
        switch (event->type()) {
        case QEvent::Enter:
            s_frameMouseWin = w;
            updateExternalFocus();
            break;
        case QEvent::Leave:
            if (s_frameMouseWin == w) {
                s_frameMouseWin.clear();
                updateExternalFocus();
            }
            break;
        case QEvent::WindowActivate:
            s_frameActiveWin = w;
            updateExternalFocus();
            break;
        // Deliberately NO clearing on WindowDeactivate: window managers
        // revoke focus with a grab while a window is dragged by its title
        // bar — clearing would blank the frame for the whole move. The
        // fallback ends when something else in blitzview activates
        // (WindowActivate above overwrites; the main window clears in
        // changeEvent) or when the window closes (Hide below). Tradeoff:
        // switching to ANOTHER APPLICATION leaves the frame standing.
        case QEvent::Hide:
            // Returning from the viewer in fullscreen presentation mode:
            // keep the frame standing on the just-viewed item for
            // orientation (freeze — released on deliberate mouse move),
            // BEFORE updateExternalFocus drops the viewer's external path
            if (isFullScreen()) {
                if (auto* viewer = qobject_cast<MediaViewer*>(w))
                    m_gridView->freezeFocusOnPath(viewer->currentFilePath());
            }
            if (s_frameActiveWin == w)
                s_frameActiveWin.clear();
            if (s_frameMouseWin == w)
                s_frameMouseWin.clear();
            updateExternalFocus();
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::watchViewerFocus(MediaViewer* viewer)
{
    viewer->installEventFilter(this);
    // Navigating (prev/next) moves the frame along while this viewer is
    // the one driving it (mouse inside or focused)
    connect(viewer, &MediaViewer::currentMediaChanged, this,
            [viewer](const QString&) {
        if (s_frameMouseWin == viewer || s_frameActiveWin == viewer)
            updateExternalFocus();
    });
}

void MainWindow::onDetailsRequested(int row)
{
    if (row < 0 || row >= m_model->rowCount()) return;

    const MediaItem item = m_model->item(row);

    // At most one dialog per file — a second request raises the existing
    // one. The registry is app-global: dialogs belong to all MainWindows.
    if (QDialog* existing = s_detailsDialogs.value(item.filePath)) {
        existing->setWindowState(existing->windowState() & ~Qt::WindowMinimized);
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }

    // Modeless snapshot of the item's details. The embedded DetailsPanel
    // subscribes to MetadataCache itself, so taken/tags arriving later fill
    // in automatically. Rename/delete of the file is not tracked.
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(item.fileName);
    // While this dialog is the active window, the grid frames its file
    dialog->setProperty("blitzFilePath", item.filePath);
    dialog->installEventFilter(this);
    // Content sits inside a FocusBorderFrame: a thin reserved margin that
    // lights up while this dialog drives the grids' focus frame
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* frame = new FocusBorderFrame(dialog);
    auto* frameLayout = new QVBoxLayout(frame);
    auto* panel = new DetailsPanel(frame);
    panel->showItem(item);
    frameLayout->addWidget(panel);
    layout->addWidget(frame);
    // FocusBorderFrame is moc-less (local class) — findChild cannot see
    // it, so setAuxBorderActive retrieves it via this property
    dialog->setProperty("focusBorderFrame",
                        QVariant::fromValue(static_cast<QWidget*>(frame)));
    // The panel's ElidedLabels have an Ignored horizontal policy — the
    // dialog gets no width from the content and would collapse to the
    // caption column, eliding every value away.
    dialog->setMinimumWidth(360);
    dialog->resize(460, dialog->sizeHint().height());

    s_detailsDialogs.insert(item.filePath, dialog);
    connect(dialog, &QObject::destroyed, this, [path = item.filePath]() {
        s_detailsDialogs.remove(path);
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

// exiftool time-shift value from absolute seconds: "0:0:D H:M:S"
static QString exifShiftValue(qint64 absSeconds)
{
    const qint64 d = absSeconds / 86400;
    const qint64 h = (absSeconds % 86400) / 3600;
    const qint64 m = (absSeconds % 3600) / 60;
    const qint64 s = absSeconds % 60;
    return QStringLiteral("0:0:%1 %2:%3:%4").arg(d).arg(h).arg(m).arg(s);
}

void MainWindow::onEditMetadataRequested(const QList<int>& rows)
{
    if (rows.isEmpty() || m_pendingMetaWrites > 0 || !m_pendingMtimeVideos.isEmpty())
        return;

    if (!ExifToolService::instance().isAvailable()) {
        QMessageBox::warning(this, tr("Edit Metadata"),
            tr("exiftool is not installed (package perl-image-exiftool)."));
        return;
    }

    QStringList imagePaths, videoPaths;
    for (int r : rows) {
        if (r < 0 || r >= m_model->rowCount())
            continue;
        const MediaItem& item = m_model->item(r);
        (item.isVideo ? videoPaths : imagePaths).append(item.filePath);
    }
    if (imagePaths.isEmpty() && videoPaths.isEmpty())
        return;

    const QStringList allPaths = imagePaths + videoPaths;

    // The tag list shows STATE, so it has to be complete before the dialog
    // opens. get() already covers memory and disk; only real misses go to the
    // exiftool daemon, and those are awaited with a bounded wait rather than
    // showing a list that is quietly wrong.
    QStringList missing;
    for (const QString& p : allPaths)
        if (!MetadataCache::instance().get(p))
            missing.append(p);
    if (!missing.isEmpty()) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QEventLoop loop;
        QSet<QString> pending(missing.cbegin(), missing.cend());
        auto conn = connect(&ExifToolService::instance(),
                            &ExifToolService::metadataReady, &loop,
                            [&](const QStringList& done) {
            for (const QString& p : done)
                pending.remove(p);
            if (pending.isEmpty())
                loop.quit();
        });
        QTimer::singleShot(4000, &loop, &QEventLoop::quit);   // never hang
        for (const QString& p : missing)
            ExifToolService::instance().requestNow(p);
        loop.exec();
        disconnect(conn);
        QApplication::restoreOverrideCursor();
    }

    // tag → how many selected files carry it. Grouped case-insensitively
    // (casing variants of one tag coexist in the wild), remembering every
    // spelling seen: exiftool's "-=" is CASE-SENSITIVE, so removal has to
    // name each variant explicitly.
    QHash<QString, int>         tagCounts;      // folded → count
    QHash<QString, QString>     tagDisplay;     // folded → first spelling
    QHash<QString, QStringList> tagVariants;    // folded → spellings seen
    for (const QString& p : allPaths) {
        const auto meta = MetadataCache::instance().peek(p);
        if (!meta)
            continue;
        QSet<QString> seenInFile;
        for (const QString& t : meta->tags) {
            const QString folded = t.toCaseFolded();
            if (seenInFile.contains(folded))
                continue;
            seenInFile.insert(folded);
            tagCounts[folded] += 1;
            if (!tagDisplay.contains(folded))
                tagDisplay.insert(folded, t);
            if (!tagVariants[folded].contains(t))
                tagVariants[folded].append(t);
        }
    }
    QList<EditMetadataDialog::TagState> presentTags;
    for (auto it = tagCounts.cbegin(); it != tagCounts.cend(); ++it)
        presentTags.append({tagDisplay.value(it.key()), it.value()});
    std::sort(presentTags.begin(), presentTags.end(),
              [](const auto& a, const auto& b) {
                  return a.tag.localeAwareCompare(b.tag) < 0;
              });

    EditMetadataDialog dlg(allPaths.size(), presentTags,
                           MetadataCache::instance().knownTags(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const EditMetadataDialog::Edits e = dlg.edits();
    if (e.isEmpty())
        return;

    // Tag operations are identical for both groups. AVES semantics for the
    // primary store: tags live in XMP dc:subject (IPTC stays untouched).
    // exiftool list quirk: mixing "=" (clear) and "+=" (add) in ONE command
    // appends without clearing — so clear+add uses plain "=" assignments,
    // which replace the whole list.
    QStringList tagArgs;
    if (e.clearTags && e.addTags.isEmpty()) {
        tagArgs.append(QStringLiteral("-XMP-dc:Subject="));
    } else if (e.clearTags) {
        for (const QString& t : e.addTags)
            tagArgs.append(QStringLiteral("-XMP-dc:Subject=") + t);
    } else {
        // Pure list operations here — no "=" in sight, so the clear/append
        // trap above does not apply. Both directions must name every casing
        // variant found, because "-=" matches case-sensitively.
        auto variantsOf = [&](const QString& tag) {
            const QStringList v = tagVariants.value(tag.toCaseFolded());
            return v.isEmpty() ? QStringList{tag} : v;
        };
        for (const QString& t : e.removeTags)
            for (const QString& v : variantsOf(t))
                tagArgs.append(QStringLiteral("-XMP-dc:Subject-=") + v);
        for (const QString& t : e.addTags) {
            // Remove-then-add: idempotent for files that already carry the
            // tag (exiftool's "+=" would happily create a duplicate), and it
            // collapses casing variants onto the spelling chosen here.
            for (const QString& v : variantsOf(t))
                tagArgs.append(QStringLiteral("-XMP-dc:Subject-=") + v);
            tagArgs.append(QStringLiteral("-XMP-dc:Subject+=") + t);
        }
    }

    const QString shiftOp = (e.shiftSeconds >= 0) ? QStringLiteral("+=")
                                                  : QStringLiteral("-=");
    const QString shiftVal = exifShiftValue(qAbs(e.shiftSeconds));

    auto queueGroup = [&](const QStringList& paths, bool isVideo) {
        if (paths.isEmpty())
            return;
        QStringList args = tagArgs;
        if (e.shiftSeconds != 0) {
            if (isVideo) {
                args.append(QStringLiteral("-QuickTime:CreateDate") + shiftOp + shiftVal);
                args.append(QStringLiteral("-QuickTime:MediaCreateDate") + shiftOp + shiftVal);
            } else {
                args.append(QStringLiteral("-AllDates") + shiftOp + shiftVal);
            }
        }
        if (!args.isEmpty()) {
            ExifToolService::instance().queueWrite(paths, args);
            ++m_pendingMetaWrites;
        }
        if (e.setMtimeToTaken) {
            if (isVideo) {
                // exiftool cannot combine the UTC instant with the per-file
                // capture-site offset (Keys:AndroidTimeZone) in a batch —
                // the capture-site WALL CLOCK is computed by our own read
                // pipeline. So: after the writes, re-read these videos and
                // set the mtime via QFile::setFileTime (see finalize below).
                m_pendingMtimeVideos += paths;
            } else {
                // Images: DateTimeOriginal IS the wall clock; a separate job
                // AFTER the shift job (FIFO) copies the shifted value
                ExifToolService::instance().queueWrite(paths,
                    {QStringLiteral("-FileModifyDate<DateTimeOriginal")});
                ++m_pendingMetaWrites;
            }
        }
    };

    m_pendingMetaPaths = imagePaths + videoPaths;
    queueGroup(imagePaths, false);
    queueGroup(videoPaths, true);

    if (m_pendingMetaWrites == 0) {
        // No exiftool writes queued (e.g. only "mtime ← taken" for videos)
        if (!m_pendingMtimeVideos.isEmpty())
            finalizeMetadataWrites();
        else
            m_pendingMetaPaths.clear();
    }
}

void MainWindow::finalizeMetadataWrites()
{
    // All exiftool writes are done. Drop stale cache entries (the disk
    // entries miss automatically via the changed mtimes).
    MetadataCache::instance().remove(m_pendingMetaPaths);

    if (m_pendingMtimeVideos.isEmpty()) {
        finishMetadataEdit();
        return;
    }
    // Video mtime phase: re-read the (possibly shifted) metadata; the
    // arriving capture-site wall clocks drive setFileTime in
    // onMetadataReadyForMtime, which then finishes the edit.
    ExifToolService::instance().request(m_pendingMtimeVideos);
}

void MainWindow::onMetadataReadyForMtime(const QStringList& filePaths)
{
    if (m_pendingMtimeVideos.isEmpty())
        return;

    for (const QString& fp : filePaths) {
        if (!m_pendingMtimeVideos.removeOne(fp))
            continue;
        const auto meta = MetadataCache::instance().peek(fp);
        if (!meta || !meta->taken.isValid())
            continue;
        // taken is the capture-site wall clock as a local QDateTime — the
        // file manager will display exactly that wall clock
        QFile f(fp);
        if (f.open(QIODevice::ReadWrite)) {
            f.setFileTime(meta->taken, QFileDevice::FileModificationTime);
            f.close();
        }
    }

    if (m_pendingMtimeVideos.isEmpty())
        finishMetadataEdit();
}

void MainWindow::finishMetadataEdit()
{
    m_pendingMetaPaths.clear();
    m_model->loadDirectories(m_selectedRecursive, m_selectedIndividual);
    reapplySort();
}

void MainWindow::onRenameRequested(const QList<int>& rows)
{
    if (rows.isEmpty())
        return;

    // Capture time for a file: taken (capture-site wall clock), mtime fallback
    auto takenFor = [this](int row) {
        const MediaItem& item = m_model->item(row);
        if (auto meta = MetadataCache::instance().peek(item.filePath);
            meta && meta->taken.isValid())
            return meta->taken;
        return item.modifiedDate;
    };

    const MediaItem& firstItem = m_model->item(rows.first());
    RenameDialog dlg(rows.size(), firstItem.fileName, takenFor(rows.first()), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString pattern = dlg.pattern().trimmed();
    if (pattern.isEmpty())
        return;
    AppSettings::setLastRenamePattern(pattern);

    int failed = 0;
    QSet<QString> producedNames;
    for (int row : rows) {
        if (row < 0 || row >= m_model->rowCount())
            continue;
        const MediaItem& item = m_model->item(row);
        const QFileInfo fi(item.fileName);

        QString newName = RenameDialog::expand(pattern, takenFor(row),
                                               fi.completeBaseName(), fi.suffix());
        if (newName.isEmpty() || newName.contains(QLatin1Char('/'))) {
            ++failed;
            continue;
        }
        if (newName == item.fileName)
            continue;

        // Collisions (burst shots with identical capture second, existing
        // files): append _1, _2, … before the extension
        const QString dir = QFileInfo(item.filePath).absolutePath() + QLatin1Char('/');
        QString candidate = newName;
        for (int n = 1;
             producedNames.contains(candidate) || QFileInfo::exists(dir + candidate);
             ++n) {
            const QFileInfo cfi(newName);
            candidate = cfi.completeBaseName() + QStringLiteral("_%1").arg(n);
            if (!cfi.suffix().isEmpty())
                candidate += QLatin1Char('.') + cfi.suffix();
            if (n > 999) { candidate.clear(); break; }
        }
        if (candidate.isEmpty() || !m_model->renameItem(row, candidate)) {
            ++failed;
            continue;
        }
        producedNames.insert(candidate);
    }

    if (failed > 0)
        QMessageBox::warning(this, tr("Rename"),
            tr("%n file(s) could not be renamed.", nullptr, failed));

    // Name sort may reorder — the grid restores selection and scroll by path
    reapplySort();
}

void MainWindow::onDeleteRequested(const QList<int>& rows, bool permanent)
{
    if (rows.isEmpty())
        return;

    QStringList paths;
    for (int r : rows)
        if (r >= 0 && r < m_model->rowCount())
            paths.append(m_model->item(r).filePath);
    if (paths.isEmpty())
        return;

    if (permanent) {
        const auto answer = QMessageBox::warning(this, tr("Delete Permanently"),
            tr("Permanently delete %n file(s)? This cannot be undone.",
               nullptr, paths.size()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
            return;
    }

    int failed = 0;
    for (const QString& fp : paths) {
        QFile f(fp);
        const bool ok = permanent ? f.remove() : f.moveToTrash();
        if (!ok)
            ++failed;
    }
    if (failed > 0)
        QMessageBox::warning(this, permanent ? tr("Delete") : tr("Move to Trash"),
            tr("%n file(s) could not be removed.", nullptr, failed));

    m_model->loadDirectories(m_selectedRecursive, m_selectedIndividual);
    reapplySort();
}

void MainWindow::reapplySort()
{
    if (!m_model || !m_sortColumnCombo || !m_sortOrderCombo)
        return;
    const int col = m_sortColumnCombo->currentData().toInt();
    const auto order = static_cast<Qt::SortOrder>(m_sortOrderCombo->currentData().toInt());
    m_model->sort(col, order);
    if (m_tableView)
        m_tableView->showSortIndicator(col, order);
}

void MainWindow::updateStatusBar(int total)
{
    m_loadedCount = 0;
    if (m_model->isFiltered())
        m_statusLabel->setText(tr("%1 of %2 files (filtered)  |  0 loaded")
                                   .arg(total).arg(m_model->masterCount()));
    else
        m_statusLabel->setText(tr("%1 files  |  0 loaded").arg(total));
}

void MainWindow::onThumbnailLoaded()
{
    const int total = m_model->rowCount();
    const int loaded = total - m_model->unloadedCount();
    m_loadedCount = loaded;
    if (m_model->isFiltered())
        m_statusLabel->setText(tr("%1 of %2 files (filtered)  |  %3 loaded")
                                   .arg(total).arg(m_model->masterCount()).arg(loaded));
    else
        m_statusLabel->setText(tr("%1 files  |  %2 loaded").arg(total).arg(loaded));
}
