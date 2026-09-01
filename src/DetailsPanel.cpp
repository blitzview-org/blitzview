#include "DetailsPanel.h"
#include "ExifToolService.h"
#include "MediaItem.h"
#include "MetadataCache.h"
#include "ThumbnailCache.h"

#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLocale>
#include <QVBoxLayout>

namespace {
const QString kEmpty = QStringLiteral("–"); // en dash

// Weekday + locale short date + full time: QLocale::ShortFormat drops the
// seconds, but capture times (burst shots!) need them
QString formatDateTime(const QLocale& locale, const QDateTime& dt)
{
    if (!dt.isValid())
        return kEmpty;
    return locale.toString(dt.date(), QStringLiteral("ddd"))
        + QLatin1Char(' ')
        + locale.toString(dt.date(), QLocale::ShortFormat)
        + QLatin1Char(' ')
        + dt.time().toString(QStringLiteral("HH:mm:ss"));
}
}

DetailsPanel::DetailsPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 8);

    auto* header = new QLabel(tr("Details"), this);
    QFont f = header->font();
    f.setBold(true);
    header->setFont(f);
    layout->addWidget(header);

    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 4, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(2);
    layout->addLayout(form);
    layout->addStretch();

    m_name       = addRow(form, tr("Name:"));
    m_folder     = addRow(form, tr("Folder:"));
    m_type       = addRow(form, tr("Type:"));
    m_size       = addRow(form, tr("Size:"));
    m_resolution = addRow(form, tr("Resolution:"));
    m_taken      = addRow(form, tr("Taken:"));
    m_modified   = addRow(form, tr("Modified:"));
    m_tags       = addRow(form, tr("Tags:"));

    connect(&ExifToolService::instance(), &ExifToolService::metadataReady,
            this, &DetailsPanel::refreshMetadata);

    clearItem();
}

ElidedLabel* DetailsPanel::addRow(QFormLayout* form, const QString& caption)
{
    auto* value = new ElidedLabel(this);
    form->addRow(caption, value);
    return value;
}

void DetailsPanel::showItem(const MediaItem& item)
{
    const QLocale locale;
    m_currentPath = item.filePath;

    m_name->setFullText(item.fileName);
    m_folder->setFullText(QFileInfo(item.filePath).absolutePath());
    m_type->setFullText(QStringLiteral("%1 (%2)")
        .arg(item.fileType, item.isVideo ? tr("Video") : tr("Image")));
    m_size->setFullText(locale.formattedDataSize(item.fileSize));

    // Resolution is not part of the scan metadata; the thumbnail workers store
    // the oriented image dimensions in ThumbnailCache once a thumb is loaded.
    QSize res = item.resolution;
    if (res.isEmpty())
        res = ThumbnailCache::instance().orientedSize(item.filePath);
    m_resolution->setFullText(res.isEmpty()
        ? kEmpty
        : QStringLiteral("%1 × %2").arg(res.width()).arg(res.height()));

    m_modified->setFullText(formatDateTime(locale, item.modifiedDate));

    // Capture time and tags come from the metadata pipeline. Missing entry:
    // show placeholders and ask for a priority read — refreshMetadata fills
    // them in as soon as the daemon answers.
    if (auto meta = MetadataCache::instance().peek(item.filePath)) {
        m_taken->setFullText(formatDateTime(locale, meta->taken));
        m_tags->setFullText(meta->tags.isEmpty()
            ? kEmpty : meta->tags.join(QStringLiteral(", ")));
    } else {
        m_taken->setFullText(kEmpty);
        m_tags->setFullText(kEmpty);
        ExifToolService::instance().requestNow(item.filePath);
    }
}

void DetailsPanel::refreshMetadata(const QStringList& filePaths)
{
    if (m_currentPath.isEmpty() || !filePaths.contains(m_currentPath))
        return;
    const auto meta = MetadataCache::instance().peek(m_currentPath);
    if (!meta)
        return;
    const QLocale locale;
    m_taken->setFullText(formatDateTime(locale, meta->taken));
    m_tags->setFullText(meta->tags.isEmpty()
        ? kEmpty : meta->tags.join(QStringLiteral(", ")));
}

void DetailsPanel::clearItem()
{
    m_currentPath.clear();
    for (ElidedLabel* l : {m_name, m_folder, m_type, m_size,
                           m_resolution, m_taken, m_modified, m_tags})
        l->setFullText(kEmpty);
}
