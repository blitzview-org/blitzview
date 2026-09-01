#pragma once

#include <QDateTime>
#include <QDialog>
#include <QString>

class QLabel;

class QComboBox;

// Batch rename with exiftool-style placeholders. Date codes use the item's
// capture time (taken, capture-site wall clock; mtime fallback):
//   %Y %m %d  year/month/day        %H %M %S  hour/minute/second
//   %y        two-digit year        %a        weekday abbreviation (locale)
//   %%f       original name (without extension)
//   %%e       original extension
//   %%        literal %
// Example: "%Y-%m-%d_%a_%H-%M-%S_%%f.%%e"
//
// The pattern box is an editable combo backed by a user-maintained preset
// list (AppSettings::renamePatterns, +/- buttons; the built-in default is
// seeded on first use and stays retrievable that way).
class RenameDialog : public QDialog
{
    Q_OBJECT
public:
    RenameDialog(int fileCount,
                 const QString& previewName, const QDateTime& previewTaken,
                 QWidget* parent = nullptr);

    QString pattern() const;

    // Built-in default pattern (first entry of a freshly seeded preset list)
    static QString defaultPattern();

    // Expands the placeholder pattern for one file.
    // baseName/extension are from the ORIGINAL file name.
    static QString expand(const QString& pattern, const QDateTime& taken,
                          const QString& baseName, const QString& extension);

private:
    void updatePreview();

    QComboBox* m_pattern = nullptr;
    // Preview as two grid rows so old and new file name align exactly
    QLabel*    m_previewOld = nullptr;
    QLabel*    m_previewNew = nullptr;
    QString    m_previewBase;
    QString    m_previewExt;
    QDateTime  m_previewTaken;
};
