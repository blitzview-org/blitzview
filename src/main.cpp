#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    app.setApplicationName("BlitzView");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("BlitzView");
    
    MainWindow window;
    window.show();
    
    return app.exec();
}
