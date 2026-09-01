#pragma once

#include <QImage>
#include <QSize>
#include <QString>

// Extract a video thumbnail using FFmpeg libraries directly.
// Much faster than QMediaPlayer (~10-50ms vs 100-3000ms).
// First checks for attached_pic (cover art), then decodes first keyframe.
// Returns a null QImage on failure.
QImage extractVideoThumbnail(const QString& filePath, const QSize& targetSize);
