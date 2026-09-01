#pragma once

#include <QString>
#include <QStringList>
#include <Qt>

/// Filesystem path semantics that differ between platforms.
///
/// Everything here is a pure string operation — nothing touches the disk.
/// Paths are always in Qt's internal form with forward slashes; a root is
/// "/" on POSIX and "C:/" on Windows.
///
/// Every function comes in two flavors: one that uses the platform the
/// binary was built for, and one that takes an explicit Flavor. The latter
/// exists so the Windows semantics can be exercised (and regression-tested)
/// on Linux.
namespace Path {

enum class Flavor { Posix, Windows };

/// Compiled-in default (Q_OS_WIN), overridable via BLITZVIEW_PATH_FLAVOR
/// ("windows" / "posix") for development and testing.
Flavor flavor();

Qt::CaseSensitivity caseSensitivity(Flavor f);
inline Qt::CaseSensitivity caseSensitivity() { return caseSensitivity(flavor()); }

/// The filesystem roots: {"/"} on POSIX, the drive list on Windows.
QStringList roots(Flavor f);
inline QStringList roots() { return roots(flavor()); }

/// True for "/" (POSIX) resp. "C:/" (Windows).
bool isRoot(const QString& path, Flavor f);
inline bool isRoot(const QString& path) { return isRoot(path, flavor()); }

/// The root @p path lives on, or QString() if @p path is not absolute.
QString rootOf(const QString& path, Flavor f);
inline QString rootOf(const QString& path) { return rootOf(path, flavor()); }

/// The parent directory, or QString() at a root (and for empty input).
QString parentOf(const QString& path, Flavor f);
inline QString parentOf(const QString& path) { return parentOf(path, flavor()); }

/// @p path with exactly one trailing slash. Roots are already terminated.
QString withSlash(const QString& path);

/// Path equality, case-insensitive under Windows semantics.
bool equal(const QString& a, const QString& b, Flavor f);
inline bool equal(const QString& a, const QString& b) { return equal(a, b, flavor()); }

/// True if @p child is a strict descendant of @p parent.
/// Component-aware: "/foobar" is NOT under "/foo".
bool isUnder(const QString& child, const QString& parent, Flavor f);
inline bool isUnder(const QString& child, const QString& parent)
{
    return isUnder(child, parent, flavor());
}

/// isUnder(), but @p child == @p parent also counts.
bool isSelfOrUnder(const QString& child, const QString& parent, Flavor f);
inline bool isSelfOrUnder(const QString& child, const QString& parent)
{
    return isSelfOrUnder(child, parent, flavor());
}

/// Walking from @p anchor towards @p target, the next single step down.
/// QString() if @p target is not below @p anchor. Works uniformly for
/// "/", "C:/" and ordinary directories.
QString nextChildInChain(const QString& anchor, const QString& target, Flavor f);
inline QString nextChildInChain(const QString& anchor, const QString& target)
{
    return nextChildInChain(anchor, target, flavor());
}

/// Canonical form for use as a map key: cleaned, forward slashes, and
/// under Windows semantics an upper-case drive letter.
QString normalize(const QString& path, Flavor f);
inline QString normalize(const QString& path) { return normalize(path, flavor()); }

/// Shortened form for display: "~/pictures" under POSIX, unchanged under
/// Windows (where "~" means nothing to the user).
QString display(const QString& path, Flavor f);
inline QString display(const QString& path) { return display(path, flavor()); }

} // namespace Path
