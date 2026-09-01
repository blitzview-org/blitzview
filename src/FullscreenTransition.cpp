#include "FullscreenTransition.h"
#include "GridDelegate.h"
#include "MediaModel.h"

#include <QHash>
#include <QPainter>
#include <QScreen>
#include <QSet>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QVariantAnimation>

namespace {

QColor lerpColor(const QColor& a, const QColor& b, qreal t)
{
    return QColor(a.red() + qRound((b.red() - a.red()) * t),
                  a.green() + qRound((b.green() - a.green()) * t),
                  a.blue() + qRound((b.blue() - a.blue()) * t),
                  a.alpha() + qRound((b.alpha() - a.alpha()) * t));
}

// Only the roles GridDelegate consumes
QPalette lerpPalette(const QPalette& from, const QPalette& to, qreal t)
{
    static const QPalette::ColorRole roles[] = {
        QPalette::Base,      QPalette::Text,
        QPalette::Window,    QPalette::WindowText,
        QPalette::Highlight, QPalette::HighlightedText,
    };
    QPalette pal = to;
    for (const auto r : roles)
        pal.setColor(r, lerpColor(from.color(r), to.color(r), t));
    return pal;
}

QRect lerpRect(const QRect& a, const QRect& b, qreal t)
{
    return QRect(qRound(a.x() + (b.x() - a.x()) * t),
                 qRound(a.y() + (b.y() - a.y()) * t),
                 qRound(a.width() + (b.width() - a.width()) * t),
                 qRound(a.height() + (b.height() - a.height()) * t));
}

} // namespace

FullscreenTransitionOverlay::FullscreenTransitionOverlay(MediaModel* model,
                                                         QScreen* screen,
                                                         bool toBlack)
    : QWidget(nullptr,
              Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
                  | Qt::X11BypassWindowManagerHint),
      m_model(model), m_toBlack(toBlack)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    if (screen)
        setGeometry(screen->geometry());

    m_delegate = new GridDelegate(this);

    m_anim = new QVariantAnimation(this);
    m_anim->setStartValue(0.0);
    m_anim->setEndValue(1.0);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_anim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
        m_t = v.toReal();
        update();
    });
    connect(m_anim, &QVariantAnimation::finished, this, [this]() {
        m_t = m_anim->endValue().toReal();
        update();
        emit transitionFinished(m_backward);
    });
}

void FullscreenTransitionOverlay::setSource(const QList<Cell>& cells,
                                            int iconSize,
                                            const QPalette& palette)
{
    m_cells = cells;
    for (Cell& c : m_cells) {
        c.from.moveTopLeft(mapFromGlobal(c.from.topLeft()));
        c.to = c.from;
    }
    m_fromIcon = iconSize;
    m_toIcon = iconSize;
    m_fromPal = palette;
    m_toPal = palette;
    m_t = 0.0;
    update();
}

void FullscreenTransitionOverlay::setWindowBackdrop(const QPixmap& window,
                                                    const QRect& windowRect)
{
    m_winPm = window;
    m_winRect = QRect(mapFromGlobal(windowRect.topLeft()),
                      windowRect.size());
    update();
}

void FullscreenTransitionOverlay::setViewportClip(const QRect& viewportRect)
{
    m_vpClip = viewportRect.isValid()
        ? QRect(mapFromGlobal(viewportRect.topLeft()), viewportRect.size())
        : QRect();
    update();
}

void FullscreenTransitionOverlay::startTo(const QList<Cell>& cells,
                                          int iconSize,
                                          const QPalette& palette,
                                          int durationMs)
{
    m_toIcon = iconSize;
    m_toPal = palette;

    QHash<int, int> indexOfRow;   // row -> index into m_cells
    indexOfRow.reserve(m_cells.size());
    for (int i = 0; i < m_cells.size(); ++i)
        indexOfRow.insert(m_cells.at(i).row, i);

    QSet<int> matched;
    QList<Cell> incoming;
    for (const Cell& t : cells) {
        const QRect toLocal(mapFromGlobal(t.to.topLeft()), t.to.size());
        const auto it = indexOfRow.constFind(t.row);
        if (it != indexOfRow.constEnd()) {
            Cell& c = m_cells[it.value()];
            c.to = toLocal;
            c.selected = t.selected;
            // fadeOut set by the caller: `to` is the HYPOTHETICAL rect the
            // row would occupy in the target raster — fly out towards it
            c.fadeOut = t.fadeOut;
            matched.insert(t.row);
        } else {
            // Only in the target raster: fly in from the hypothetical
            // source rect (fade in place if the caller supplied none)
            Cell c = t;
            c.to = toLocal;
            c.from = t.from.isValid()
                ? QRect(mapFromGlobal(t.from.topLeft()), t.from.size())
                : toLocal;
            c.fadeIn = true;
            incoming.append(c);
        }
    }
    for (Cell& c : m_cells) {
        if (!matched.contains(c.row)) {
            // Only in the source raster and no hypothetical target rect
            // arrived: fade out where it was
            c.to = c.from;
            c.fadeOut = true;
        }
    }
    m_cells += incoming;

    m_fullDurationMs = qMax(1, durationMs);
    m_anim->setStartValue(0.0);
    m_anim->setEndValue(1.0);
    m_anim->setDuration(m_fullDurationMs);
    m_anim->start();
}

void FullscreenTransitionOverlay::reverse()
{
    m_backward = !m_backward;
    const qreal target = m_backward ? 0.0 : 1.0;
    m_anim->stop();
    m_anim->setStartValue(m_t);
    m_anim->setEndValue(target);
    // A FULL animation period for the way back: the toggle is a new user
    // interaction (scaling by the remaining distance made an early abort
    // snap back almost instantly)
    m_anim->setDuration(m_fullDurationMs);
    m_anim->start();
}

void FullscreenTransitionOverlay::paintEvent(QPaintEvent*)
{
    // First paint: the frame is flushed when this event returns and on
    // screen one vblank later — signal after ~one compositor frame (16 ms)
    // so the caller can mutate the real window flicker-free behind it
    if (!m_firstPaintNotified) {
        m_firstPaintNotified = true;
        QTimer::singleShot(16, this,
                           [this]() { emit sourceFramePresented(); });
    }

    QPainter p(this);
    // Background: see-through base -> windowed-window backdrop (entering
    // only) -> black alpha fade on top. Entering, the backdrop under the
    // fade makes the window visually disappear only once the background
    // is fully opaque; leaving has no backdrop — the REAL restored window
    // shows through the translucent surface and is revealed by the fade.
    // CompositionMode_Source so the base alpha REPLACES the surface (true
    // see-through with a compositor; degrades to opaque black without one).
    const qreal bg = m_toBlack ? m_t : 1.0 - m_t;
    const QColor black(0, 0, 0, qBound(0, qRound(255.0 * bg), 255));
    p.setCompositionMode(QPainter::CompositionMode_Source);
    if (m_winPm.isNull()) {
        p.fillRect(rect(), black);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    } else {
        p.fillRect(rect(), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.drawPixmap(m_winRect, m_winPm);
        p.fillRect(rect(), black);
    }

    if (!m_model)
        return;

    const QPalette pal = lerpPalette(m_fromPal, m_toPal, m_t);
    m_delegate->setIconSize(
        qMax(1, qRound(m_fromIcon + (m_toIcon - m_fromIcon) * m_t)));

    // The flight is clipped to the grid viewport on the WINDOWED end:
    // tiles never overlap the backdrop's chrome (statusbar, toolbar), and
    // fade cells emerge from / vanish behind the viewport edge. The clip
    // edge lerps with the same t as the cell rects, so a cell can never
    // outrun it.
    if (m_vpClip.isValid())
        p.setClipRect(m_toBlack ? lerpRect(m_vpClip, rect(), m_t)
                                : lerpRect(rect(), m_vpClip, m_t));

    for (const Cell& c : std::as_const(m_cells)) {
        if (c.row < 0 || c.row >= m_model->rowCount())
            continue;
        const QRect r = lerpRect(c.from, c.to, m_t);

        qreal opacity = 1.0;
        if (c.fadeIn)
            opacity = m_t;
        else if (c.fadeOut)
            opacity = 1.0 - m_t;
        p.setOpacity(opacity);

        // Cells fly as solid tiles: Base-filled background keeps the label
        // readable over the desktop and reads as the window decomposing
        // into its cells; at t=1 the tiles are black on black — seamless.
        p.fillRect(r, pal.brush(QPalette::Base));

        QStyleOptionViewItem option;
        option.initFrom(this);
        option.rect = r;
        option.palette = pal;
        option.state |= QStyle::State_Enabled | QStyle::State_Active;
        option.state &= ~QStyle::State_MouseOver;
        if (c.selected)
            option.state |= QStyle::State_Selected;
        const QModelIndex mi =
            m_model->index(c.row, MediaModel::Col_Thumbnail);
        m_delegate->paint(&p, option, mi);
    }
    p.setOpacity(1.0);
}
