#pragma once

#include <QKeyEvent>
#include <QKeySequence>
#include <QList>

#include <initializer_list>

/// Platform-independent shortcuts.
///
/// Qt maps QKeySequence::StandardKey per platform, and leaves some of them
/// EMPTY where the platform convention is a window-manager gesture rather than
/// an accelerator. Measured against the two Qt builds BlitzView uses:
///
///     StandardKey   Linux (6.11)   Windows (6.8)
///     Quit          Ctrl+Q         (none — the convention is Alt+F4)
///     FullScreen    (none)         F11, Alt+Enter
///     Close         Ctrl+W         Ctrl+F4, Ctrl+W
///
/// BlitzView offers the same keys on every platform, so where the platform
/// binding is missing the expected sequence is appended rather than replacing
/// the platform's own — a Windows user keeps Ctrl+F4 and Alt+Enter and gains
/// Ctrl+Q.
namespace Shortcuts {

/// @p preferred first, then any platform binding for @p key not already in it.
///
/// Order matters: Qt shows only the FIRST sequence in menus, so this is what
/// decides which key a user is told about. BlitzView's own key comes first;
/// the platform's are kept as additional accepted aliases.
inline QList<QKeySequence> preferring(std::initializer_list<QKeySequence> preferred,
                                      QKeySequence::StandardKey key)
{
    QList<QKeySequence> keys(preferred);
    for (const QKeySequence& platform : QKeySequence::keyBindings(key)) {
        if (!keys.contains(platform))
            keys.append(platform);
    }
    return keys;
}

/// True if @p event is @p key OR one of @p extras.
///
/// Qt's own QKeyEvent::matches() is asked first and kept as-is: it applies
/// normalisation this code should not try to reproduce (keypad modifier,
/// shift variants). Only the extra sequences are checked separately, so on a
/// platform where the StandardKey is non-empty this is a strict superset of
/// the previous behaviour — it can start matching more, never less.
inline bool matches(const QKeyEvent* event, QKeySequence::StandardKey key,
                    std::initializer_list<QKeySequence> extras)
{
    if (!event)
        return false;
    if (event->matches(key))
        return true;
    const QKeySequence pressed(event->keyCombination());
    for (const QKeySequence& extra : extras) {
        if (pressed == extra)
            return true;
    }
    return false;
}

// The sequences BlitzView guarantees on every platform.

inline QList<QKeySequence> quit()
{
    return preferring({ QKeySequence(Qt::CTRL | Qt::Key_Q) }, QKeySequence::Quit);
}

inline bool matchesQuit(const QKeyEvent* event)
{
    return matches(event, QKeySequence::Quit, { QKeySequence(Qt::CTRL | Qt::Key_Q) });
}

/// Ctrl+W, because every BlitzView window is top-level — "close this window"
/// is the honest description, and that is what the menu should say.
///
/// Windows' own binding is Ctrl+F4, which means "close the DOCUMENT inside an
/// MDI parent". BlitzView has no MDI, so leading with it would promise
/// semantics that do not exist. It is kept as an accepted alias: a user who
/// presses it out of habit gets the sensible result, and it costs nothing.
///
/// Alt+F4 is deliberately not bound — closing a window is the window
/// manager's job, and Windows already does it.
inline QList<QKeySequence> closeWindow()
{
    return preferring({ QKeySequence(Qt::CTRL | Qt::Key_W) }, QKeySequence::Close);
}

inline bool matchesClose(const QKeyEvent* event)
{
    return matches(event, QKeySequence::Close, { QKeySequence(Qt::CTRL | Qt::Key_W) });
}

inline QList<QKeySequence> fullScreen()
{
    return preferring({ QKeySequence(Qt::Key_F11) }, QKeySequence::FullScreen);
}

} // namespace Shortcuts
