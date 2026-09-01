#include "AboutDialog.h"

#include "BuildInfo.h"
#include "Shortcuts.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QShortcut>
#include <QVBoxLayout>

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About BlitzView"));

    auto* quitShortcut = new QShortcut(this);
    quitShortcut->setKeys(Shortcuts::quit());
    connect(quitShortcut, &QShortcut::activated, qApp, &QApplication::quit);

    auto* iconLabel = new QLabel(this);
    iconLabel->setPixmap(QPixmap(QStringLiteral(":/BlitzViewLogo.png"))
                             .scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    // The human-readable version line is built in one place --
    // cmake/GenerateBuildInfo.cmake -- from the same git-describe fields the
    // portable archive name uses, so the two can never drift apart.
    QString versionText = QStringLiteral(BLITZVIEW_VERSION_LINE);

    // The license block is not decoration: GPLv3 asks an interactive program to
    // show the copyright and the warranty disclaimer, and the LGPL requires
    // naming the libraries that are used under it (Qt and FFmpeg are both
    // linked dynamically, so users may replace them). The bundled license
    // texts live in licenses/ next to the executable in the portable packages.
    auto* textLabel = new QLabel(
        QStringLiteral("<h2>BlitzView</h2>"
                       "<p>%1</p>"
                       "<p>A fast image and video browser with thumbnail preview.</p>"
                       "<p><b>License:</b> <a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">"
                       "GNU General Public License v3 or later</a><br>"
                       "This program comes with ABSOLUTELY NO WARRANTY.</p>"
                       "<p>Copyright &copy; 2026 Oliver&nbsp;Schmidt</p>"
                       "<p>Built with Qt %2 (LGPL&nbsp;v3) and FFmpeg (LGPL&nbsp;v2.1)<br>"
                       "Third-party license texts: see the <tt>licenses</tt> folder</p>"
                       "<p><a href=\"https://blitzview.org\">blitzview.org</a></p>")
            .arg(versionText, QString::fromLatin1(qVersion())),
        this);
    textLabel->setTextFormat(Qt::RichText);
    textLabel->setWordWrap(false);
    textLabel->setOpenExternalLinks(true);
    textLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);

    auto* columns = new QHBoxLayout;
    columns->setSpacing(12);
    columns->addWidget(iconLabel, 0, Qt::AlignTop | Qt::AlignHCenter);
    columns->addWidget(textLabel, 1, Qt::AlignVCenter);

    auto* rows = new QVBoxLayout(this);
    rows->addLayout(columns, 1);
    rows->addWidget(buttonBox);

    setFixedSize(sizeHint());
}
