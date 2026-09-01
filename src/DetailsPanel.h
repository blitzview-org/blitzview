#pragma once

#include <QWidget>
#include "ElidedLabel.h"
// Full include, not a forward declaration: the moc-generated metatype code
// for showItem(const MediaItem&) must see the complete type. With only a
// forward declaration, Qt's SFINAE completeness check takes its fallback
// path (GCC 16: -Wsfinae-incomplete) — harmless for direct connections,
// wrong for queued ones.
#include "MediaItem.h"

class QFormLayout;

// Optional panel below the DirectoryPanel in the side panel.
// Shows extended information about the item currently hovered in the grid.
// Also reused as the content of the modeless "Details…" dialog (context menu).
class DetailsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DetailsPanel(QWidget* parent = nullptr);

public slots:
    void showItem(const MediaItem& item);
    void clearItem();
    // Re-reads taken/tags from MetadataCache when they arrived after showItem
    void refreshMetadata(const QStringList& filePaths);

private:
    ElidedLabel* addRow(QFormLayout* form, const QString& caption);

    ElidedLabel* m_name       = nullptr;
    ElidedLabel* m_folder     = nullptr;
    ElidedLabel* m_type       = nullptr;
    ElidedLabel* m_size       = nullptr;
    ElidedLabel* m_resolution = nullptr;
    ElidedLabel* m_taken      = nullptr;
    ElidedLabel* m_modified   = nullptr;
    ElidedLabel* m_tags       = nullptr;

    QString m_currentPath;  // shown item; guards async metadata refresh
};
