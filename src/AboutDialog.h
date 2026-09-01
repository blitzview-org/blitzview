#pragma once

#include <QDialog>

// The Help → About BlitzView dialog. Kept as its own class so that the version
// string (which changes on every build from git) compiles in isolation instead
// of dragging the whole MainWindow translation unit along.
class AboutDialog final : public QDialog
{
public:
    explicit AboutDialog(QWidget* parent = nullptr);
};
