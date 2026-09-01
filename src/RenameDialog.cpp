#include "RenameDialog.h"
#include "AppSettings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QToolButton>
#include <QVBoxLayout>

QString RenameDialog::defaultPattern()
{
    return QStringLiteral("%Y-%m-%d_%a_%H-%M-%S.%%e");
}

QString RenameDialog::expand(const QString& pattern, const QDateTime& taken,
                             const QString& baseName, const QString& extension)
{
    const QLocale locale;
    QString out;
    out.reserve(pattern.size() + 16);

    for (int i = 0; i < pattern.size(); ++i) {
        const QChar ch = pattern.at(i);
        if (ch != QLatin1Char('%') || i + 1 >= pattern.size()) {
            out += ch;
            continue;
        }

        const QChar code = pattern.at(++i);
        if (code == QLatin1Char('%')) {
            // "%%f" / "%%e" — exiftool filename codes; bare "%%" = literal %
            if (i + 1 < pattern.size() && pattern.at(i + 1) == QLatin1Char('f')) {
                out += baseName;
                ++i;
            } else if (i + 1 < pattern.size() && pattern.at(i + 1) == QLatin1Char('e')) {
                out += extension;
                ++i;
            } else {
                out += QLatin1Char('%');
            }
            continue;
        }

        switch (code.toLatin1()) {
        case 'Y': out += taken.toString(QStringLiteral("yyyy")); break;
        case 'y': out += taken.toString(QStringLiteral("yy"));   break;
        case 'm': out += taken.toString(QStringLiteral("MM"));   break;
        case 'd': out += taken.toString(QStringLiteral("dd"));   break;
        case 'H': out += taken.toString(QStringLiteral("HH"));   break;
        case 'M': out += taken.toString(QStringLiteral("mm"));   break;
        case 'S': out += taken.toString(QStringLiteral("ss"));   break;
        case 'a': {
            // Some locales abbreviate with a trailing period ("Do.") —
            // strip it, periods don't belong in the middle of file names
            QString wd = locale.toString(taken.date(), QStringLiteral("ddd"));
            wd.remove(QLatin1Char('.'));
            out += wd;
            break;
        }
        default:
            // Unknown code: keep literally so mistakes stay visible
            out += QLatin1Char('%');
            out += code;
        }
    }
    return out;
}

RenameDialog::RenameDialog(int fileCount,
                           const QString& previewName, const QDateTime& previewTaken,
                           QWidget* parent)
    : QDialog(parent)
    , m_previewTaken(previewTaken)
{
    setWindowTitle(tr("Rename — %n file(s)", nullptr, fileCount));

    const QFileInfo fi(previewName);
    m_previewBase = fi.completeBaseName();
    m_previewExt  = fi.suffix();

    auto* layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(tr("Pattern:"), this));

    // Editable combo over the user-maintained preset list. The built-in
    // default is seeded once and stays retrievable through the list.
    QStringList presets = AppSettings::renamePatterns();
    if (presets.isEmpty()) {
        presets = {defaultPattern()};
        AppSettings::setRenamePatterns(presets);
    }

    m_pattern = new QComboBox(this);
    m_pattern->setEditable(true);
    m_pattern->addItems(presets);
    const QString last = AppSettings::lastRenamePattern();
    m_pattern->setEditText(last.isEmpty() ? defaultPattern() : last);

    auto* btnAdd = new QToolButton(this);
    btnAdd->setText(QStringLiteral("+"));
    btnAdd->setToolTip(tr("Save the current pattern to the preset list"));
    auto* btnRemove = new QToolButton(this);
    btnRemove->setText(QStringLiteral("−"));
    btnRemove->setToolTip(tr("Remove the current pattern from the preset list"));

    auto* patternRow = new QHBoxLayout;
    patternRow->addWidget(m_pattern, 1);
    patternRow->addWidget(btnAdd);
    patternRow->addWidget(btnRemove);
    layout->addLayout(patternRow);

    connect(btnAdd, &QToolButton::clicked, this, [this]() {
        const QString p = m_pattern->currentText().trimmed();
        if (p.isEmpty() || m_pattern->findText(p) >= 0)
            return;
        m_pattern->insertItem(0, p);
        QStringList list;
        for (int i = 0; i < m_pattern->count(); ++i)
            list.append(m_pattern->itemText(i));
        AppSettings::setRenamePatterns(list);
        m_pattern->setEditText(p);
    });
    connect(btnRemove, &QToolButton::clicked, this, [this]() {
        const QString p = m_pattern->currentText();
        const int idx = m_pattern->findText(p);
        if (idx < 0)
            return;
        m_pattern->removeItem(idx);
        QStringList list;
        for (int i = 0; i < m_pattern->count(); ++i)
            list.append(m_pattern->itemText(i));
        AppSettings::setRenamePatterns(list);
        m_pattern->setEditText(p);   // keep the text, it just left the list
    });

    auto* help = new QLabel(
        tr("Placeholders (dates use the capture time „Taken“):\n"
           "  %Y   year (2026)          %H   hour\n"
           "  %m   month (04)           %M   minute\n"
           "  %d   day (23)             %S   second\n"
           "  %y   two-digit year       %a   weekday (Do)\n"
           "  %%f  original name without extension\n"
           "  %%e  original extension"), this);
    QFont mono = help->font();
    mono.setFamilies({QStringLiteral("monospace")});
    help->setFont(mono);
    help->setStyleSheet(QStringLiteral("color: gray;"));
    layout->addWidget(help);

    // Two-line preview; the file names sit in their own column so old and
    // new name start at exactly the same x position
    auto* previewGrid = new QGridLayout;
    previewGrid->setHorizontalSpacing(8);
    previewGrid->setVerticalSpacing(2);
    previewGrid->addWidget(new QLabel(tr("Preview:"), this), 0, 0);
    // Right-aligned: the arrow hugs the target name instead of sitting at
    // the left edge of the (wider) "Preview:" column
    previewGrid->addWidget(new QLabel(QStringLiteral("→"), this), 1, 0,
                           Qt::AlignRight);
    m_previewOld = new QLabel(this);
    m_previewNew = new QLabel(this);
    previewGrid->addWidget(m_previewOld, 0, 1);
    previewGrid->addWidget(m_previewNew, 1, 1);
    previewGrid->setColumnStretch(1, 1);
    layout->addLayout(previewGrid);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_pattern, &QComboBox::editTextChanged, this, &RenameDialog::updatePreview);
    updatePreview();

    resize(520, sizeHint().height());
}

QString RenameDialog::pattern() const
{
    return m_pattern->currentText();
}

void RenameDialog::updatePreview()
{
    const QString result = expand(m_pattern->currentText(), m_previewTaken,
                                  m_previewBase, m_previewExt);
    m_previewOld->setText(m_previewBase + QLatin1Char('.') + m_previewExt);
    m_previewNew->setText(result);
}
