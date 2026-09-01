#pragma once

#include <QList>
#include <QPoint>
#include <QWidget>
#include "MediaItem.h"

class QAudioOutput;
class QGraphicsScene;
class QGraphicsVideoItem;
class QGraphicsView;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QScrollArea;
class QSlider;
class QStackedWidget;
class QTimer;
class VideoSurface;
class EdgeNavZone;

// Unified single-media view for images AND videos. No overlays: the control
// bar occupies a bottom strip in both modes (manual geometry, no QLayout).
// Windowed: bar always visible. Fullscreen: bar hidden by default (medium
// gets the full height); it appears while the mouse is in the bottom zone
// and hides when the mouse leaves it. The cursor hides when idle in
// fullscreen. Prev/next buttons sit at the outer ends of the bar; arrow keys
// navigate too. Navigates the full mixed media list — crossing between
// images and videos switches the stacked page.
//
// Video renders via the CPU-painting VideoSurface (verified pixel-correct,
// handles rotation/mirroring; HW decoding still applies). The GL path
// (QGraphicsVideoItem on a QOpenGLWidget viewport, Gwenview-style) is kept
// behind BLITZVIEW_VIDEO_GL=1 but renders BLACK in Qt 6.11 — retest with
// future Qt versions before making it the default.
class MediaViewer : public QWidget
{
    Q_OBJECT
public:
    explicit MediaViewer(QWidget* parent = nullptr);
    ~MediaViewer() override;

    void setMedia(const QList<MediaItem>& items, int currentIndex);
    // Path of the currently displayed medium (empty while no media set)
    QString currentFilePath() const;
    // Colors the reserved window border while this viewer drives the
    // grid's focus frame (windowed only — fullscreen has no border)
    void setFocusBorderActive(bool active);
    // Re-applies AppSettings::focusBorderWidth after a settings change —
    // the reserved margin adapts immediately, no reopen needed
    void refreshFocusBorder();
    // Enter fullscreen directly (also valid on a not-yet-shown window);
    // used when the main window is in presentation fullscreen mode
    void enterFullscreen();
    // While set, Escape in fullscreen CLOSES the viewer instead of merely
    // leaving fullscreen — the presentation-mode flow returns straight to
    // the fullscreen main window
    void setCloseOnFullscreenExit(bool on);

signals:
    // Emitted whenever the displayed medium changes (setMedia, prev/next)
    void currentMediaChanged(const QString& filePath);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void showPrev();
    void showNext();
    void onPlayPause();
    void onMuteToggle();
    void onSeek(int value);
    void onPositionChanged(qint64 pos);
    void onDurationChanged(qint64 dur);
    void toggleFullscreen();
    void fitToWindow();

private:
    void loadCurrent();
    void applyZoom();
    void updateGeometry_();    // manual geometry for stack and bar
    void setControlBarVisible(bool visible);
    void onIdleTimeout();      // fullscreen: hides the cursor when idle
    void parkNow();            // corner dwell elapsed: hide bar/zones/cursor
    QString formatTime(qint64 ms);

    // Pages. Video page: either the software surface (default) or the
    // experimental GL view (BLITZVIEW_VIDEO_GL=1) — exactly one is non-null.
    QStackedWidget*     m_stack        = nullptr;
    QScrollArea*        m_scrollArea   = nullptr;   // image page
    QLabel*             m_imageLabel   = nullptr;
    QGraphicsView*      m_videoView    = nullptr;   // video page (GL viewport)
    QGraphicsScene*     m_videoScene   = nullptr;
    QGraphicsVideoItem* m_videoItem    = nullptr;
    VideoSurface*       m_videoSurface = nullptr;   // video page (CPU fallback)
    QWidget*            m_videoPage    = nullptr;   // whichever of the two is active
    QMediaPlayer*       m_player       = nullptr;
    QAudioOutput*       m_audio        = nullptr;

    // Control bar: video transport left, fullscreen pinned at the right end
    // (same position for image and video). Prev/next are edge overlays.
    QWidget*     m_controlBar     = nullptr;
    QPushButton* m_btnPlayPause   = nullptr;
    QPushButton* m_btnMute        = nullptr;
    QPushButton* m_btnFullscreen  = nullptr;
    QSlider*     m_posSlider      = nullptr;
    QSlider*     m_volSlider      = nullptr;
    QLabel*      m_timeLabel      = nullptr;

    // Hover chevron overlays at the left/right edge of the medium
    EdgeNavZone* m_edgeLeft  = nullptr;
    EdgeNavZone* m_edgeRight = nullptr;
    // X disc top-right, fullscreen only (appears with the right chevron)
    class CloseOverlay* m_closeOverlay = nullptr;

    QTimer* m_idleTimer   = nullptr;
    QTimer* m_parkTimer   = nullptr;  // corner dwell before parking
    // Windowed geometry captured when ENTERING fullscreen — showNormal() is
    // asynchronous, so saving right after leaving fullscreen would still
    // record the fullscreen size
    QByteArray m_windowedGeometry;
    // Escape-in-fullscreen closes instead of un-fullscreening; cleared on
    // any explicit fullscreen exit (F11/button) and on close
    bool    m_closeOnFullscreenExit = false;
    bool    m_parked      = false;
    QPoint  m_lastMovePos;   // suppress synthetic zero-distance move events

    QList<MediaItem> m_items;
    int              m_current   = 0;

    // Reserved focus border (windowed): width as of the last
    // updateGeometry_ (0 in fullscreen), colored only while active
    int  m_focusBorderPx     = 0;
    bool m_focusBorderActive = false;

    // Image zoom state (ported from the former ImageViewer)
    QPixmap m_original;
    double  m_zoom      = 1.0;
    bool    m_fitted    = true;
    bool    m_fitOnShow = false;
};
