#include "MediaViewer.h"

#include "Shortcuts.h"
#include "AppSettings.h"
#include "PageBackground.h"

#include <QApplication>
#include <QAudioOutput>
#include <QCloseEvent>
#include <QGraphicsScene>
#include <QGraphicsVideoItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QOpenGLWidget>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QPainterPath>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWheelEvent>
#include <algorithm>
#include <functional>

// ---------------------------------------------------------------------------
// VideoSurface — plain widget that paints frames from a QVideoSink itself,
// handling rotation/mirroring manually via painter transforms. Verified
// pixel-correct against ffmpeg's autorotated reference; the default video
// page (see header for the experimental GL alternative).
// ---------------------------------------------------------------------------
class VideoSurface : public QWidget
{
public:
    explicit VideoSurface(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        connect(&m_sink, &QVideoSink::videoFrameChanged,
                this, qOverload<>(&QWidget::update));
    }

    QVideoSink* sink() { return &m_sink; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);

        const QVideoFrame frame = m_sink.videoFrame();
        if (!frame.isValid())
            return;

        const QImage img = frame.toImage(); // unrotated, unmirrored
        if (img.isNull())
            return;

        const int cwDeg = static_cast<int>(frame.rotation());
        const bool quarter = (cwDeg == 90 || cwDeg == 270);

        // Fit the DISPLAY size (post-rotation) into the widget
        QSizeF display = quarter ? QSizeF(img.height(), img.width())
                                 : QSizeF(img.size());
        display.scale(QSizeF(size()), Qt::KeepAspectRatio);

        // Draw the unrotated image through the rotation transform: its
        // width/height swap back for quarter rotations
        const QSizeF draw = quarter ? display.transposed() : display;

        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.translate(width() / 2.0, height() / 2.0);
        if (cwDeg != 0)
            p.rotate(cwDeg);
        if (frame.mirrored())
            p.scale(-1.0, 1.0); // horizontal mirror (front cameras)
        p.drawImage(QRectF(-draw.width() / 2.0, -draw.height() / 2.0,
                           draw.width(), draw.height()),
                    img);
    }

private:
    QVideoSink m_sink;
};

// ---------------------------------------------------------------------------
// EdgeNavZone — full-height sensitive strip at the left/right edge of the
// medium. Invisible until hovered; then shows a chevron in a dark disc,
// vertically centered. Permanently visible widget that only repaints on
// hover — toggling widget visibility on mouse moves causes flicker.
// ---------------------------------------------------------------------------
class EdgeNavZone : public QWidget
{
public:
    EdgeNavZone(bool pointsLeft, QWidget* parent)
        : QWidget(parent), m_pointsLeft(pointsLeft)
    {
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void()> onClick;

    void setNavEnabled(bool enabled)
    {
        if (m_enabled == enabled)
            return;
        m_enabled = enabled;
        update();
    }

    // Corner park position: chevron and pointer cursor are disabled so the
    // zone shows nothing even while the mouse rests inside it
    void setSuppressed(bool suppressed)
    {
        if (m_suppressed == suppressed)
            return;
        m_suppressed = suppressed;
        setCursor(suppressed ? Qt::BlankCursor : Qt::PointingHandCursor);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (m_suppressed || !m_enabled || !underMouse())
            return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const int d = 48; // disc diameter
        const QRectF disc((width() - d) / 2.0, (height() - d) / 2.0, d, d);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawEllipse(disc);

        QPen pen(QColor(255, 255, 255, 230), 3.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);

        const QPointF c = disc.center();
        const qreal dx = m_pointsLeft ? 4.0 : -4.0;
        QPainterPath path;
        path.moveTo(c.x() + dx, c.y() - 9.0);
        path.lineTo(c.x() - dx, c.y());
        path.lineTo(c.x() + dx, c.y() + 9.0);
        p.drawPath(path);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_enabled && onClick) {
            onClick();
            return;
        }
        event->ignore();
    }

    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    bool m_pointsLeft;
    bool m_enabled = false;
    bool m_suppressed = false;
};

// ---------------------------------------------------------------------------
// CloseOverlay — X disc at the top-right of the medium (fullscreen only).
// Painted while the mouse is in the right edge region (same moment the right
// chevron appears) or over the disc itself.
// ---------------------------------------------------------------------------
class CloseOverlay : public QWidget
{
public:
    explicit CloseOverlay(QWidget* parent) : QWidget(parent)
    {
        setFixedSize(56, 56);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void()> onClick;

    void setActive(bool active)
    {
        if (m_active == active)
            return;
        m_active = active;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (!m_active && !underMouse())
            return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const int d = 48;
        const QRectF disc((width() - d) / 2.0, (height() - d) / 2.0, d, d);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawEllipse(disc);

        QPen pen(QColor(255, 255, 255, 230), 3.0);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        const QPointF c = disc.center();
        p.drawLine(QPointF(c.x() - 8, c.y() - 8), QPointF(c.x() + 8, c.y() + 8));
        p.drawLine(QPointF(c.x() - 8, c.y() + 8), QPointF(c.x() + 8, c.y() - 8));
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && onClick) {
            onClick();
            return;
        }
        event->ignore();
    }

    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    bool m_active = false;
};

// ---------------------------------------------------------------------------
// ClickSlider — QSlider whose groove jumps to the clicked position instead of
// paging towards it. Used for the video progress bar: click = seek there.
// ---------------------------------------------------------------------------
class ClickSlider : public QSlider
{
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            const int val = QStyle::sliderValueFromPosition(
                minimum(), maximum(),
                static_cast<int>(event->position().x()), width());
            setValue(val);
            emit sliderMoved(val); // drives the seek, same as dragging
        }
        // Base handling afterwards: the handle now sits under the cursor,
        // so a press-and-drag continues as a normal handle drag
        QSlider::mousePressEvent(event);
    }
};

namespace {
// Park zone: only the EXACT bottom corners trigger parking. Small on purpose
// (size = the shared mouse threshold, Settings → Input, default 8 px) — in
// fullscreen the screen edges pin the cursor, so slamming the mouse into the
// corner is easy, while near-corner interaction (fullscreen button,
// play/pause) must never park.
bool inParkCorner(const QPoint& pos, int w, int h)
{
    const int corner = AppSettings::mouseThresholdPx();
    return pos.y() >= h - corner
        && (pos.x() <= corner || pos.x() >= w - corner);
}
} // namespace

// ---------------------------------------------------------------------------
// MediaViewer
// ---------------------------------------------------------------------------

MediaViewer::MediaViewer(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Media Viewer"));
    resize(960, 640);
    // Start with the size/position of the last closed viewer window
    const QByteArray geo = AppSettings::viewerGeometry();
    if (!geo.isEmpty())
        restoreGeometry(geo);
    setMouseTracking(true);

    // Dark, flat control look, scoped to children. The bar has a translucent
    // dark background so it reads as an overlay in fullscreen.
    const QString accent = palette().color(QPalette::Highlight).name();
    setStyleSheet(QStringLiteral(
        "MediaViewer { background: black; }"
        "QWidget#controlBar { background: rgba(20,20,20,0.85); }"
        "MediaViewer QScrollArea { background: transparent; border: none; }"
        "MediaViewer QScrollArea QWidget { background: transparent; }"
        "MediaViewer QPushButton {"
        "  background: transparent; color: #dddddd; border: none;"
        "  border-radius: 4px; padding: 6px 12px; font-size: 14px;"
        "}"
        "MediaViewer QPushButton:hover { background: rgba(255,255,255,0.15); }"
        "MediaViewer QPushButton:pressed { background: rgba(255,255,255,0.25); }"
        "MediaViewer QPushButton:disabled { color: #555555; }"
        "MediaViewer QLabel { color: #dddddd; }"
        "MediaViewer QSlider::groove:horizontal {"
        "  height: 4px; background: #444444; border-radius: 2px;"
        "}"
        "MediaViewer QSlider::sub-page:horizontal {"
        "  background: %1; border-radius: 2px;"
        "}"
        "MediaViewer QSlider::handle:horizontal {"
        "  width: 12px; height: 12px; margin: -4px 0;"
        "  border-radius: 6px; background: #dddddd;"
        "}"
        "MediaViewer QSlider::handle:horizontal:hover { background: #ffffff; }")
        .arg(accent));
    setAttribute(Qt::WA_StyledBackground, true);

    // --- Image page ---
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setAlignment(Qt::AlignCenter);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setFocusPolicy(Qt::NoFocus);

    m_imageLabel = new QLabel;
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_scrollArea->setWidget(m_imageLabel);

    m_player = new QMediaPlayer(this);
    m_audio  = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    m_audio->setVolume(0.7f);

    if (qEnvironmentVariableIntValue("BLITZVIEW_VIDEO_GL") != 1) {
        // --- Video page (default): CPU-painting surface, see VideoSurface
        m_videoSurface = new VideoSurface(this);
        m_videoSurface->setFocusPolicy(Qt::NoFocus);
        m_player->setVideoSink(m_videoSurface->sink());
        m_videoPage = m_videoSurface;
    } else {
        // --- Video page (EXPERIMENTAL, BLITZVIEW_VIDEO_GL=1):
        // QGraphicsVideoItem on a QOpenGLWidget viewport — would give GPU
        // texture rendering without a native window, but renders BLACK in
        // Qt 6.11 (verified under Xvfb/llvmpipe and on real AMD hardware,
        // AA_ShareOpenGLContexts does not help). Kept for retesting with
        // future Qt versions.
        m_videoScene = new QGraphicsScene(this);
        m_videoScene->setBackgroundBrush(Qt::black);
        m_videoItem = new QGraphicsVideoItem;
        m_videoItem->setAspectRatioMode(Qt::KeepAspectRatio);
        m_videoScene->addItem(m_videoItem);

        m_videoView = new QGraphicsView(m_videoScene, this);
        m_videoView->setViewport(new QOpenGLWidget);
        // GL viewports do not support partial updates
        m_videoView->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        m_videoView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_videoView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_videoView->setFrameShape(QFrame::NoFrame);
        m_videoView->setFocusPolicy(Qt::NoFocus);
        m_player->setVideoOutput(m_videoItem);
        m_videoPage = m_videoView;
    }

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_scrollArea);
    m_stack->addWidget(m_videoPage);

    // --- Control bar ---
    m_btnPlayPause  = new QPushButton(tr("▶"), this);
    m_btnMute       = new QPushButton(tr("🔊"), this);
    m_btnFullscreen = new QPushButton(tr("⛶"), this);
    m_btnFullscreen->setToolTip(tr("Fullscreen (F11)"));

    // Fix the play/pause width to the wider of both glyphs so toggling
    // ▶ ⇄ ⏸ does not shift the bar layout
    m_btnPlayPause->setText(tr("⏸"));
    int ppWidth = m_btnPlayPause->sizeHint().width();
    m_btnPlayPause->setText(tr("▶"));
    ppWidth = qMax(ppWidth, m_btnPlayPause->sizeHint().width());
    m_btnPlayPause->setFixedWidth(ppWidth);

    m_posSlider = new ClickSlider(Qt::Horizontal, this);
    m_posSlider->setRange(0, 0);

    m_volSlider = new QSlider(Qt::Horizontal, this);
    m_volSlider->setRange(0, 100);
    m_volSlider->setValue(70);
    m_volSlider->setMaximumWidth(100);

    m_timeLabel = new QLabel(QStringLiteral("0:00 / 0:00"), this);

    m_controlBar = new QWidget(this);
    m_controlBar->setObjectName(QStringLiteral("controlBar"));
    m_controlBar->setMouseTracking(true);
    // Video transport in an inner layout with stretch; the fullscreen button
    // sits outside it, pinned to the right end — its position and size are
    // identical whether the transport controls are visible (video) or
    // hidden (image)
    auto* controls = new QHBoxLayout(m_controlBar);
    controls->setContentsMargins(8, 4, 8, 4);
    controls->setSpacing(6);
    auto* transport = new QHBoxLayout;
    transport->setSpacing(6);
    transport->addWidget(m_btnPlayPause);
    transport->addWidget(m_posSlider, 1);
    transport->addWidget(m_timeLabel);
    transport->addWidget(m_btnMute);
    transport->addWidget(m_volSlider);
    controls->addLayout(transport, 1);
    controls->addWidget(m_btnFullscreen);

    // Hidden transport controls (image mode) must keep their space — the
    // fullscreen button then has the identical size and position for images
    // and videos, instead of expanding into the collapsed layout
    for (QWidget* w : {static_cast<QWidget*>(m_btnPlayPause),
                       static_cast<QWidget*>(m_posSlider),
                       static_cast<QWidget*>(m_timeLabel),
                       static_cast<QWidget*>(m_btnMute),
                       static_cast<QWidget*>(m_volSlider)}) {
        QSizePolicy sp = w->sizePolicy();
        sp.setRetainSizeWhenHidden(true);
        w->setSizePolicy(sp);
    }

    // The corner-park logic must also see mouse moves over the bar and its
    // controls — otherwise parking stops working while the bar is visible
    m_controlBar->installEventFilter(this);
    const auto barChildren = m_controlBar->findChildren<QWidget*>();
    for (QWidget* w : barChildren) {
        w->setMouseTracking(true);
        w->installEventFilter(this);
    }

    // None of the controls may take keyboard focus — arrows/space belong to
    // the viewer shortcuts
    const auto controlChildren = m_controlBar->findChildren<QWidget*>();
    for (QWidget* w : controlChildren)
        w->setFocusPolicy(Qt::NoFocus);

    // Mouse tracking over the media area (fullscreen bar zone + cursor hiding)
    QWidget* videoMouseTarget = m_videoView ? m_videoView->viewport()
                                            : static_cast<QWidget*>(m_videoSurface);
    videoMouseTarget->setMouseTracking(true);
    videoMouseTarget->installEventFilter(this);
    m_scrollArea->viewport()->setMouseTracking(true);
    m_scrollArea->viewport()->installEventFilter(this);
    m_imageLabel->setMouseTracking(true);
    m_imageLabel->installEventFilter(this);

    // --- Edge navigation overlays (over the medium, both modes) ---
    m_edgeLeft  = new EdgeNavZone(true, this);
    m_edgeRight = new EdgeNavZone(false, this);
    m_edgeLeft->onClick  = [this]() { showPrev(); };
    m_edgeRight->onClick = [this]() { showNext(); };
    // The zones sit over the media pages, so the fullscreen mouse logic
    // (cursor un-hide, bottom bar zone) must also see their moves
    m_edgeLeft->installEventFilter(this);
    m_edgeRight->installEventFilter(this);

    m_closeOverlay = new CloseOverlay(this);
    m_closeOverlay->onClick = [this]() { close(); };
    m_closeOverlay->setVisible(false);   // fullscreen only
    m_closeOverlay->installEventFilter(this);

    // Idle timer: hides the cursor in fullscreen (the bar is zone-driven)
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    m_idleTimer->setInterval(1200);
    connect(m_idleTimer, &QTimer::timeout, this, &MediaViewer::onIdleTimeout);

    // Corner dwell: parking waits briefly so the corner buttons (play/pause
    // left, fullscreen right) remain clickable when passing through
    m_parkTimer = new QTimer(this);
    m_parkTimer->setSingleShot(true);
    m_parkTimer->setInterval(400);
    connect(m_parkTimer, &QTimer::timeout, this, &MediaViewer::parkNow);

    // Arrow navigation must work no matter which child is focused
    auto* prevShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
    prevShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(prevShortcut, &QShortcut::activated, this, &MediaViewer::showPrev);
    auto* nextShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
    nextShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(nextShortcut, &QShortcut::activated, this, &MediaViewer::showNext);

    connect(m_btnPlayPause,  &QPushButton::clicked, this, &MediaViewer::onPlayPause);
    connect(m_btnMute,       &QPushButton::clicked, this, &MediaViewer::onMuteToggle);
    connect(m_btnFullscreen, &QPushButton::clicked, this, &MediaViewer::toggleFullscreen);
    connect(m_posSlider,     &QSlider::sliderMoved, this, &MediaViewer::onSeek);
    connect(m_volSlider, &QSlider::valueChanged, this, [this](int v) {
        m_audio->setVolume(v / 100.0f);
    });

    connect(m_player, &QMediaPlayer::positionChanged, this, &MediaViewer::onPositionChanged);
    connect(m_player, &QMediaPlayer::durationChanged, this, &MediaViewer::onDurationChanged);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
        m_btnPlayPause->setText(state == QMediaPlayer::PlayingState ? tr("⏸") : tr("▶"));
    });

}

MediaViewer::~MediaViewer()
{
    m_player->stop();
}

void MediaViewer::setMedia(const QList<MediaItem>& items, int currentIndex)
{
    m_items     = items;
    m_current   = qBound(0, currentIndex, int(items.size()) - 1);
    m_fitted    = true;
    m_fitOnShow = true;
    loadCurrent();
}

QString MediaViewer::currentFilePath() const
{
    return m_items.isEmpty() ? QString() : m_items.at(m_current).filePath;
}

void MediaViewer::loadCurrent()
{
    if (m_items.isEmpty())
        return;

    const MediaItem& item = m_items.at(m_current);
    setWindowTitle(item.filePath);
    emit currentMediaChanged(item.filePath);

    const bool isVideo = item.isVideo;

    // Video-only controls; fullscreen button is always there
    m_btnPlayPause->setVisible(isVideo);
    m_posSlider->setVisible(isVideo);
    m_timeLabel->setVisible(isVideo);
    m_btnMute->setVisible(isVideo);
    m_volSlider->setVisible(isVideo);

    m_edgeLeft->setNavEnabled(m_current > 0);
    m_edgeRight->setNavEnabled(m_current < m_items.size() - 1);

    if (isVideo) {
        m_stack->setCurrentWidget(m_videoPage);
        m_player->setSource(QUrl::fromLocalFile(item.filePath));
        m_player->play();
    } else {
        m_player->stop();
        m_player->setSource(QUrl());
        m_stack->setCurrentWidget(m_scrollArea);

        QImageReader reader(item.filePath);
        reader.setAutoTransform(true);
        applyPaperBackground(reader, item.fileType);
        const QImage img = reader.read();
        if (img.isNull()) {
            m_original = QPixmap();
            m_imageLabel->setText(tr("Failed to load image:\n%1").arg(item.filePath));
            m_imageLabel->adjustSize();
        } else {
            m_original = QPixmap::fromImage(img);
            if (m_fitted) {
                if (isVisible())
                    fitToWindow();
                else
                    m_fitOnShow = true;
            } else {
                applyZoom();
            }
        }
    }
}

// --- Manual geometry ----------------------------------------------------------

void MediaViewer::updateGeometry_()
{
    const int barH = m_controlBar->sizeHint().height();
    const bool fs = isFullScreen();

    // Windowed: a thin border strip is ALWAYS reserved around the content
    // (colored only while this viewer drives the grid's focus frame — no
    // jumping when that toggles). Fullscreen has no border.
    const int b = fs ? 0 : AppSettings::focusBorderWidth();
    m_focusBorderPx = b;
    const int innerW = width() - 2 * b;

    // Windowed: bar always visible below the medium. Fullscreen: the medium
    // always keeps the FULL height — showing the bar lays it over the bottom
    // strip instead of resizing the medium (no jumping). With letterboxed
    // media the bar covers only black; otherwise it clips the bottom edge.
    const int mediaH = fs ? height() : height() - barH - 2 * b;
    m_stack->setGeometry(b, b, innerW, mediaH);
    m_controlBar->setGeometry(b, height() - barH - b, innerW, barH);

    // Edge zones overlay the medium over its full height
    const int zoneW = qBound(48, width() / 10, 96);
    m_edgeLeft->setGeometry(b, b, zoneW, mediaH);
    m_edgeRight->setGeometry(b + innerW - zoneW, b, zoneW, mediaH);
    m_edgeLeft->raise();
    m_edgeRight->raise();

    m_closeOverlay->move(width() - m_closeOverlay->width() - 12 - b, 12 + b);
    m_closeOverlay->raise();

    if (fs)
        m_controlBar->raise();  // above the zones' bottom part
}

void MediaViewer::refreshFocusBorder()
{
    // The reserved margin resizes right away; the page viewports' resize
    // events re-fit a fitted image automatically (see eventFilter)
    updateGeometry_();
    update();
}

void MediaViewer::setFocusBorderActive(bool active)
{
    if (m_focusBorderActive == active)
        return;
    m_focusBorderActive = active;
    update();
}

void MediaViewer::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (!m_focusBorderActive || m_focusBorderPx <= 0 || isFullScreen())
        return;
    QPainter p(this);
    const int b = m_focusBorderPx;
    const QColor c = palette().color(QPalette::Highlight);
    p.fillRect(QRect(0, 0, width(), b), c);
    p.fillRect(QRect(0, height() - b, width(), b), c);
    p.fillRect(QRect(0, b, b, height() - 2 * b), c);
    p.fillRect(QRect(width() - b, b, b, height() - 2 * b), c);
}

void MediaViewer::setControlBarVisible(bool visible)
{
    if (m_controlBar->isVisible() == visible)
        return;
    m_controlBar->setVisible(visible);
    updateGeometry_();
}

// --- Navigation ------------------------------------------------------------

void MediaViewer::showPrev()
{
    if (m_current > 0) {
        --m_current;
        m_fitted = true;
        loadCurrent();
    }
}

void MediaViewer::showNext()
{
    if (m_current < m_items.size() - 1) {
        ++m_current;
        m_fitted = true;
        loadCurrent();
    }
}

// --- Image zoom (ported from the former ImageViewer) ------------------------

void MediaViewer::applyZoom()
{
    if (m_original.isNull())
        return;
    const QSize sz = m_original.size() * m_zoom;
    m_imageLabel->setPixmap(m_original.scaled(sz, Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation));
    m_imageLabel->resize(sz);
}

void MediaViewer::fitToWindow()
{
    if (m_original.isNull())
        return;
    const QSize avail = m_scrollArea->viewport()->size();
    const QPixmap scaled = m_original.scaled(avail, Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation);
    m_zoom = static_cast<double>(scaled.width()) / m_original.width();
    m_imageLabel->setPixmap(scaled);
    m_imageLabel->resize(scaled.size());
    m_fitted = true;
}

// --- Video controls ----------------------------------------------------------

void MediaViewer::onPlayPause()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();
}

void MediaViewer::onMuteToggle()
{
    const bool muted = m_audio->isMuted();
    m_audio->setMuted(!muted);
    m_btnMute->setText(!muted ? tr("🔇") : tr("🔊"));
}

void MediaViewer::onSeek(int value)
{
    m_player->setPosition(static_cast<qint64>(value));
}

void MediaViewer::onPositionChanged(qint64 pos)
{
    if (!m_posSlider->isSliderDown())
        m_posSlider->setValue(static_cast<int>(pos));
    m_timeLabel->setText(formatTime(pos) + QStringLiteral(" / ")
                         + formatTime(m_player->duration()));
}

void MediaViewer::onDurationChanged(qint64 dur)
{
    m_posSlider->setRange(0, static_cast<int>(dur));
}

QString MediaViewer::formatTime(qint64 ms)
{
    qint64 s = ms / 1000;
    const qint64 m = s / 60;
    s %= 60;
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

// --- Fullscreen & control-bar/cursor auto-hide --------------------------------

void MediaViewer::toggleFullscreen()
{
    if (isFullScreen()) {
        // Explicitly chosen windowed mode — Escape closes from here on
        m_closeOnFullscreenExit = false;
        showNormal();
        m_idleTimer->stop();
        m_parkTimer->stop();
        m_parked = false;
        setControlBarVisible(true);   // windowed: always visible
        m_edgeLeft->setSuppressed(false);
        m_edgeRight->setSuppressed(false);
        m_closeOverlay->setVisible(false);
        unsetCursor();
        updateGeometry_();
    } else {
        enterFullscreen();
    }
}

void MediaViewer::enterFullscreen()
{
    m_windowedGeometry = saveGeometry();
    showFullScreen();
    // Start clean: bar hidden (appears via the bottom zone), cursor
    // hidden (appears on movement)
    setControlBarVisible(false);
    m_closeOverlay->setActive(false);
    m_closeOverlay->setVisible(true);
    setCursor(Qt::BlankCursor);
    updateGeometry_();
}

void MediaViewer::setCloseOnFullscreenExit(bool on)
{
    m_closeOnFullscreenExit = on;
}

void MediaViewer::parkNow()
{
    if (!isFullScreen())
        return;

    // The dwell may have been outrun by a fast exit whose move event got
    // coalesced away — verify the cursor is still in a park corner
    if (!inParkCorner(mapFromGlobal(QCursor::pos()), width(), height()))
        return;

    m_parked = true;
    setControlBarVisible(false);
    m_edgeLeft->setSuppressed(true);
    m_edgeRight->setSuppressed(true);
    m_closeOverlay->setActive(false);
    m_idleTimer->stop();
    setCursor(Qt::BlankCursor);
}

void MediaViewer::onIdleTimeout()
{
    // Hide the cursor when idle in fullscreen — except while it is on the
    // bar (e.g. adjusting the volume)
    if (!isFullScreen())
        return;
    if (m_controlBar->isVisible() && m_controlBar->underMouse()) {
        m_idleTimer->start();
        return;
    }
    setCursor(Qt::BlankCursor);
}

// --- Events ------------------------------------------------------------------

bool MediaViewer::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::Resize:
        // Refit when the page viewports reach their FINAL size — the window's
        // resizeEvent can fire before children are repositioned.
        if (m_videoView && watched == m_videoView->viewport()) {
            // Gwenview-style sizing: item = full viewport, always. The item's
            // internal KeepAspectRatio letterboxes (black on black); sizing
            // from nativeSize instead breaks rotated portrait videos.
            const QSizeF s = static_cast<QResizeEvent*>(event)->size();
            m_videoItem->setSize(s);
            m_videoScene->setSceneRect(QRectF(QPointF(0, 0), s));
        } else if (watched == m_scrollArea->viewport() && m_fitted) {
            fitToWindow();
        }
        break;
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        const QPoint globalPos = me->globalPosition().toPoint();
        if (globalPos == m_lastMovePos)
            break; // synthetic move (widget shown/hidden under cursor)
        m_lastMovePos = globalPos;

        if (isFullScreen()) {
            const QPoint pos = mapFromGlobal(globalPos);
            const int barH = m_controlBar->sizeHint().height();

            // Park position: resting in the exact bottom corner hides
            // everything — bar, edge chevrons, cursor. Dwell-based (parkNow
            // after 400ms) so passing through never parks.
            if (inParkCorner(pos, width(), height())) {
                if (m_parked)
                    break;              // stay parked while inside the corner
                if (!m_parkTimer->isActive())
                    m_parkTimer->start();
            } else {
                m_parkTimer->stop();
                m_parked = false;
            }

            m_edgeLeft->setSuppressed(false);
            m_edgeRight->setSuppressed(false);

            // The close X appears together with the right chevron: whenever
            // the mouse is in the right edge region (top-right corner incl.)
            const int zoneW = qBound(48, width() / 10, 96);
            m_closeOverlay->setActive(pos.x() >= width() - zoneW);

            // Movement reveals the cursor; idle hides it again
            unsetCursor();
            m_idleTimer->start();

            // Bar is visible exactly while the mouse is in the bottom zone
            // (also fires for moves over the bar and its controls, which
            // keeps the bar up while using them)
            setControlBarVisible(pos.y() >= height() - barH - 8);
        }
        break;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void MediaViewer::keyPressEvent(QKeyEvent* event)
{
    // Deliberately no activity handling here: keyboard navigation must not
    // un-hide the controls or the cursor in fullscreen.
    // Not event->matches() directly: QKeySequence::Quit is EMPTY on Windows,
    // so it would never fire there. Shortcuts::matchesQuit asks Qt first and
    // only then the added sequences — a superset, never a replacement.
    if (Shortcuts::matchesQuit(event)) {
        QApplication::quit();
        return;
    }
    if (Shortcuts::matchesClose(event)) {
        close();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Escape:
        // In presentation-mode flow (m_closeOnFullscreenExit) Escape closes
        // entirely, returning to the fullscreen main window
        if (isFullScreen() && !m_closeOnFullscreenExit)
            toggleFullscreen();
        else
            close();
        break;
    case Qt::Key_Left:   showPrev(); break;
    case Qt::Key_Right:  showNext(); break;
    case Qt::Key_Space:
        if (!m_items.isEmpty() && m_items.at(m_current).isVideo)
            onPlayPause();
        break;
    case Qt::Key_F:
        if (!m_items.isEmpty() && !m_items.at(m_current).isVideo)
            fitToWindow();
        break;
    case Qt::Key_F11:    toggleFullscreen(); break;
    default: QWidget::keyPressEvent(event);
    }
}

void MediaViewer::wheelEvent(QWheelEvent* event)
{
    // Wheel zoom only for images
    if (m_items.isEmpty() || m_items.at(m_current).isVideo || m_original.isNull()) {
        QWidget::wheelEvent(event);
        return;
    }

    const double factor = (event->angleDelta().y() > 0) ? 1.1 : 0.9;
    const double newZoom = std::clamp(m_zoom * factor, 0.1, 10.0);
    if (qFuzzyCompare(newZoom, m_zoom))
        return;
    m_zoom   = newZoom;
    m_fitted = false;
    applyZoom();
}

void MediaViewer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateGeometry_();
    if (m_fitted)
        fitToWindow();
}

void MediaViewer::showEvent(QShowEvent* event)
{
    // The window is REUSED (hidden on close, shown again later) — re-apply
    // the persisted geometry on every show. After closing from fullscreen
    // the instance would otherwise keep the fullscreen dimensions.
    if (!isFullScreen()) {
        const QByteArray geo = AppSettings::viewerGeometry();
        if (!geo.isEmpty())
            restoreGeometry(geo);
    }

    QWidget::showEvent(event);
    updateGeometry_();
    if (m_fitOnShow && m_fitted) {
        m_fitOnShow = false;
        QTimer::singleShot(0, this, [this]() {
            if (isVisible() && m_fitted)
                fitToWindow();
        });
    }
}

void MediaViewer::closeEvent(QCloseEvent* event)
{
    // Persistent window, only hidden on close — stop playback explicitly.
    // Also leave fullscreen so the next open starts windowed.
    m_player->stop();
    m_idleTimer->stop();
    // Never carry the presentation-mode flag into a later windowed open of
    // the reused instance
    m_closeOnFullscreenExit = false;
    if (isFullScreen()) {
        // Persist the geometry captured when fullscreen was entered — the
        // current one is the fullscreen size
        if (!m_windowedGeometry.isEmpty())
            AppSettings::setViewerGeometry(m_windowedGeometry);
        toggleFullscreen();
    } else {
        AppSettings::setViewerGeometry(saveGeometry());
    }
    QWidget::closeEvent(event);
}
