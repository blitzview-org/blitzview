#include "Path.h"

#include <QDir>
#include <QFileInfo>

namespace {

// "C:/" and friends — the only root form Qt hands out on Windows.
bool isDriveRoot(const QString& p)
{
    return p.length() == 3 && p.at(0).isLetter() && p.at(1) == QLatin1Char(':')
           && p.at(2) == QLatin1Char('/');
}

} // namespace

namespace Path {

Flavor flavor()
{
    static const Flavor f = [] {
        const QByteArray override = qgetenv("BLITZVIEW_PATH_FLAVOR");
        if (override.compare("windows", Qt::CaseInsensitive) == 0)
            return Flavor::Windows;
        if (override.compare("posix", Qt::CaseInsensitive) == 0)
            return Flavor::Posix;
#ifdef Q_OS_WIN
        return Flavor::Windows;
#else
        return Flavor::Posix;
#endif
    }();
    return f;
}

Qt::CaseSensitivity caseSensitivity(Flavor f)
{
    return f == Flavor::Windows ? Qt::CaseInsensitive : Qt::CaseSensitive;
}

QStringList roots(Flavor f)
{
    if (f == Flavor::Posix)
        return { QStringLiteral("/") };

    QStringList out;
    const QFileInfoList drives = QDir::drives();
    out.reserve(drives.size());
    for (const QFileInfo& fi : drives)
        out.append(normalize(fi.absoluteFilePath(), f));
    return out;
}

bool isRoot(const QString& path, Flavor f)
{
    if (f == Flavor::Posix)
        return path == QLatin1String("/");
    return isDriveRoot(path);
}

QString rootOf(const QString& path, Flavor f)
{
    if (f == Flavor::Posix)
        return path.startsWith(QLatin1Char('/')) ? QStringLiteral("/") : QString();

    if (path.length() >= 2 && path.at(0).isLetter() && path.at(1) == QLatin1Char(':'))
        return path.left(1).toUpper() + QStringLiteral(":/");
    return QString();
}

QString parentOf(const QString& path, Flavor f)
{
    const QString clean = normalize(path, f);
    if (clean.isEmpty() || isRoot(clean, f))
        return QString();

    const int slash = clean.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return QString();

    QString parent = clean.left(slash);
    const QString root = rootOf(clean, f);
    // "/foo" -> "" and "C:/foo" -> "C:" both need to become the root itself.
    if (parent.isEmpty() || (!root.isEmpty() && parent.length() < root.length()))
        return root.isEmpty() ? QStringLiteral("/") : root;
    return parent;
}

QString withSlash(const QString& path)
{
    if (path.isEmpty() || path.endsWith(QLatin1Char('/')))
        return path;
    return path + QLatin1Char('/');
}

bool equal(const QString& a, const QString& b, Flavor f)
{
    return QString::compare(a, b, caseSensitivity(f)) == 0;
}

bool isUnder(const QString& child, const QString& parent, Flavor f)
{
    if (parent.isEmpty() || child.isEmpty())
        return false;
    const QString prefix = withSlash(parent);
    return child.length() > prefix.length() && child.startsWith(prefix, caseSensitivity(f));
}

bool isSelfOrUnder(const QString& child, const QString& parent, Flavor f)
{
    return equal(child, parent, f) || isUnder(child, parent, f);
}

QString nextChildInChain(const QString& anchor, const QString& target, Flavor f)
{
    if (!isUnder(target, anchor, f))
        return QString();

    const QString prefix = withSlash(anchor);
    const QString rel    = target.mid(prefix.length());
    const int slash      = rel.indexOf(QLatin1Char('/'));
    return slash == -1 ? target : prefix + rel.left(slash);
}

QString normalize(const QString& path, Flavor f)
{
    if (path.isEmpty())
        return path;

    QString p = path;
    if (f == Flavor::Windows)
        p.replace(QLatin1Char('\\'), QLatin1Char('/'));

    p = QDir::cleanPath(p);

    if (f == Flavor::Windows) {
        // A bare "C:" denotes the drive's current directory in Win32, but
        // BlitzView only ever means the drive root by it.
        if (p.length() == 2 && p.at(0).isLetter() && p.at(1) == QLatin1Char(':'))
            p += QLatin1Char('/');
        if (p.length() >= 2 && p.at(0).isLetter() && p.at(1) == QLatin1Char(':'))
            p[0] = p.at(0).toUpper();
    }
    return p;
}

QString display(const QString& path, Flavor f)
{
    if (f == Flavor::Windows)
        return path;

    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (isUnder(path, home, f))
        return QLatin1Char('~') + path.mid(home.length());
    return path;
}

} // namespace Path
