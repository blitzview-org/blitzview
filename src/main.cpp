#include "mainwindow.h"
#include <QApplication>
#include <QFileInfo>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    app.setApplicationName("BlitzView");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("BlitzView");
    
    MainWindow window;
    window.show();
    
    // Load image from command-line argument if provided
    if (argc > 1) {
        QString filePath = QString::fromLocal8Bit(argv[1]);
        QFileInfo fileInfo(filePath);
        if (fileInfo.exists() && fileInfo.isFile()) {
            window.openImageFile(filePath);
        }
    }
    
    return app.exec();
}
