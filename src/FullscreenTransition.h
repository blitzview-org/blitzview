#pragma once

#include <QList>
#include <QPalette>
#include <QPixmap>
#include <QRect>
#include <QWidget>

class GridDelegate;
class MediaModel;
class QScreen;
class QVariantAnimation;

// Fullscreen transition overlay: a
// frameless, translucent, screen-sized top-level window on which the grid
// cells (image + label, selection kept) FLY from their windowed screen
// positions to the fullscreen raster (and back), while the background
// cross-fades between fully transparent — the desktop shows through — and
// opaque black. ENTERING, a backdrop image of the windowed main window
// (WM frame from a screen grab, client area re-rendered with an empty
// grid — its images are the flying cells) sits under the black fade, so
// the window visually disappears only once the background is fully
// opaque; the REAL window has switched modes underneath at windowOpacity
// 0, so the target rects are read from the FINISHED layout: no
// prediction, no visible seam at handover (the overlay's last frame
// equals the window's first). LEAVING needs no backdrop: the real window
// restores VISIBLY behind the initially opaque overlay (grid images
// suppressed) and is revealed — live, with real decorations — as the
// black clears. Without a compositor the translucent background degrades
// to opaque black; the flight still runs (deliberate fallback).
// Override-redirect flags keep the window manager's own open/close
// effects away from the overlay.
class FullscreenTransitionOverlay : public QWidget
{
    Q_OBJECT
public:
    // Rects in GLOBAL coordinates (converted internally). Rows present on
    // one side only fly between their real rect and the HYPOTHETICAL rect
    // the row would occupy in the other side's raster
    // (GridView::LayoutSnapshot), fading in/out on the way:
    //   startTo cell unknown to setSource -> fadeIn, `from` = hypothetical
    //   source rect (fade in place if the caller left it invalid);
    //   startTo cell with fadeOut set -> matches a setSource cell, `to` =
    //   hypothetical target rect.
    struct Cell {
        int   row = -1;
        QRect from;
        QRect to;
        bool  selected = false;
        bool  fadeIn = false;    // only present in the target raster
        bool  fadeOut = false;   // only present in the source raster
    };

    // toBlack: entering fullscreen (transparent -> black); false reverses
    FullscreenTransitionOverlay(MediaModel* model, QScreen* screen,
                                bool toBlack);

    // First (static) frame: cells at their source positions (Cell::from,
    // global), background at the start opacity
    void setSource(const QList<Cell>& cells, int iconSize,
                   const QPalette& palette);

    // The real window finished its switch: complete the cell list
    // (fade-in/out for rows present on one side only) and start the flight
    void startTo(const QList<Cell>& cells, int iconSize,
                 const QPalette& palette, int durationMs);

    // Mid-flight toggle: flip the running flight from its CURRENT
    // position — the same lerp runs backwards, so cells retrace their
    // paths and the fades invert. The reversed leg runs a FULL animation
    // period (each toggle is a new user interaction). A second call
    // flips forward again. transitionFinished(backward=true) then means
    // the flight returned to its source: the caller must undo the real
    // window's switch.
    void reverse();
    bool isBackward() const { return m_backward; }

    // Backdrop (entering only): image of the windowed main window (frame
    // + empty grid) at its global rect, painted under the black fade
    void setWindowBackdrop(const QPixmap& window, const QRect& windowRect);

    // Grid viewport (global, invalid = no clip): the flying cells are
    // clipped to it on the WINDOWED end of the flight, so they never
    // overlap the window's chrome and fade cells emerge from / vanish
    // behind the viewport edge
    void setViewportClip(const QRect& viewportRect);

signals:
    // The first overlay frame is on screen (first paintEvent plus one
    // compositor frame of latency): NOW the real window may mutate behind
    // it without a visible flash — earlier, the compositor could present
    // the window switch before the overlay covered it (title-bar flicker
    // entering, restored-window flash leaving)
    void sourceFramePresented();
    // backward=false: the flight reached its target (normal handover).
    // backward=true: a reverse() returned it to the SOURCE state — the
    // real window's switch must be undone behind the overlay.
    void transitionFinished(bool backward);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    MediaModel*        m_model = nullptr;
    GridDelegate*      m_delegate = nullptr;
    QVariantAnimation* m_anim = nullptr;
    QList<Cell>        m_cells;   // rects in overlay-local coordinates
    QPixmap  m_winPm;             // windowed-window backdrop (empty grid)
    QRect    m_winRect;           // its rect, overlay-local
    QRect    m_vpClip;            // grid viewport, overlay-local (may be invalid)
    int      m_fromIcon = 0;
    int      m_toIcon = 0;
    QPalette m_fromPal;
    QPalette m_toPal;
    bool     m_toBlack = true;
    bool     m_backward = false;             // flight direction (reverse())
    int      m_fullDurationMs = 1;           // full-distance flight time
    bool     m_firstPaintNotified = false;   // sourceFramePresented armed
    qreal    m_t = 0.0;
};
