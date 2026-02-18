#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QScrollArea>
#include <QTreeView>
#include <QFileSystemModel>
#include <QSplitter>
#include <QStatusBar>

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    
    void openImageFile(const QString &filePath);

private slots:
    void openImage();
    void openFolder();
    void showAbout();
    void onFileClicked(const QModelIndex &index);

private:
    void createMenus();
    void createWidgets();
    void loadImage(const QString &filePath);
    void updateStatusBar(const QString &filePath = QString());

    QLabel *imageLabel;
    QScrollArea *scrollArea;
    QTreeView *fileTreeView;
    QFileSystemModel *fileSystemModel;
    QSplitter *splitter;
    
    QString currentImagePath;
};

#endif // MAINWINDOW_H
