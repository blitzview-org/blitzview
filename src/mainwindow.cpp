#include "mainwindow.h"
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QPixmap>
#include <QFileInfo>
#include <QImageReader>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("BlitzView");
    resize(1024, 768);
    
    createWidgets();
    createMenus();
    
    // Set up status bar
    statusBar()->showMessage("Ready");
}

MainWindow::~MainWindow()
{
}

void MainWindow::createWidgets()
{
    // Create central widget with splitter
    splitter = new QSplitter(Qt::Horizontal, this);
    setCentralWidget(splitter);
    
    // Create file browser sidebar
    fileTreeView = new QTreeView(splitter);
    fileSystemModel = new QFileSystemModel(this);
    
    // Set up file system model with image filters
    QStringList filters;
    filters << "*.jpg" << "*.jpeg" << "*.png" << "*.bmp" << "*.gif" 
            << "*.tiff" << "*.tif" << "*.webp"
            << "*.JPG" << "*.JPEG" << "*.PNG" << "*.BMP" << "*.GIF"
            << "*.TIFF" << "*.TIF" << "*.WEBP";
    fileSystemModel->setNameFilters(filters);
    fileSystemModel->setNameFilterDisables(false);
    
    fileTreeView->setModel(fileSystemModel);
    fileTreeView->setRootIndex(fileSystemModel->index(QDir::homePath()));
    
    // Hide all columns except name
    for (int i = 1; i < fileSystemModel->columnCount(); ++i) {
        fileTreeView->hideColumn(i);
    }
    
    connect(fileTreeView, &QTreeView::clicked, this, &MainWindow::onFileClicked);
    
    // Create image display area
    imageLabel = new QLabel;
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setStyleSheet("QLabel { background-color: #2b2b2b; color: #888; }");
    imageLabel->setText("No image loaded\n\nUse File > Open Image or File > Open Folder to get started");
    imageLabel->setMinimumSize(400, 300);
    
    scrollArea = new QScrollArea(splitter);
    scrollArea->setWidget(imageLabel);
    scrollArea->setWidgetResizable(false);
    scrollArea->setAlignment(Qt::AlignCenter);
    
    // Add widgets to splitter
    splitter->addWidget(fileTreeView);
    splitter->addWidget(scrollArea);
    
    // Set initial splitter sizes (sidebar: 250px, image view: rest)
    splitter->setSizes(QList<int>() << 250 << 774);
}

void MainWindow::createMenus()
{
    // File menu
    QMenu *fileMenu = menuBar()->addMenu("&File");
    
    QAction *openImageAction = new QAction("&Open Image...", this);
    openImageAction->setShortcut(QKeySequence::Open);
    connect(openImageAction, &QAction::triggered, this, &MainWindow::openImage);
    fileMenu->addAction(openImageAction);
    
    QAction *openFolderAction = new QAction("Open &Folder...", this);
    openFolderAction->setShortcut(QKeySequence("Ctrl+Shift+O"));
    connect(openFolderAction, &QAction::triggered, this, &MainWindow::openFolder);
    fileMenu->addAction(openFolderAction);
    
    fileMenu->addSeparator();
    
    QAction *exitAction = new QAction("E&xit", this);
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QMainWindow::close);
    fileMenu->addAction(exitAction);
    
    // View menu
    QMenu *viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction("(View options coming soon)");
    
    // Help menu
    QMenu *helpMenu = menuBar()->addMenu("&Help");
    
    QAction *aboutAction = new QAction("&About BlitzView", this);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    helpMenu->addAction(aboutAction);
}

void MainWindow::openImage()
{
    QString fileName = QFileDialog::getOpenFileName(
        this,
        "Open Image",
        QDir::homePath(),
        "Image Files (*.jpg *.jpeg *.png *.bmp *.gif *.tiff *.tif *.webp);;All Files (*)"
    );
    
    if (!fileName.isEmpty()) {
        loadImage(fileName);
    }
}

void MainWindow::openFolder()
{
    QString folderPath = QFileDialog::getExistingDirectory(
        this,
        "Open Folder",
        QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );
    
    if (!folderPath.isEmpty()) {
        fileSystemModel->setRootPath(folderPath);
        fileTreeView->setRootIndex(fileSystemModel->index(folderPath));
        statusBar()->showMessage(QString("Opened folder: %1").arg(folderPath));
    }
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this,
        "About BlitzView",
        "<h2>BlitzView</h2>"
        "<p>Version 0.1.0</p>"
        "<p>A fast desktop image browser and manager with EXIF tag support.</p>"
        "<p>Copyright © 2026</p>"
        "<p>Licensed under the MIT License</p>"
    );
}

void MainWindow::onFileClicked(const QModelIndex &index)
{
    if (fileSystemModel->isDir(index)) {
        return; // Ignore directories
    }
    
    QString filePath = fileSystemModel->filePath(index);
    loadImage(filePath);
}

void MainWindow::loadImage(const QString &filePath)
{
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    
    QImage image = reader.read();
    if (image.isNull()) {
        QMessageBox::warning(
            this,
            "Error Loading Image",
            QString("Cannot load image: %1\n%2").arg(filePath, reader.errorString())
        );
        return;
    }
    
    currentImagePath = filePath;
    
    // Display the image
    QPixmap pixmap = QPixmap::fromImage(image);
    imageLabel->setPixmap(pixmap);
    imageLabel->resize(pixmap.size());
    
    // Update status bar with image information
    updateStatusBar(filePath);
    
    // Update window title
    QFileInfo fileInfo(filePath);
    setWindowTitle(QString("BlitzView - %1").arg(fileInfo.fileName()));
}

void MainWindow::updateStatusBar(const QString &filePath)
{
    if (filePath.isEmpty()) {
        statusBar()->showMessage("Ready");
        return;
    }
    
    QFileInfo fileInfo(filePath);
    QImageReader reader(filePath);
    QSize imageSize = reader.size();
    
    QString sizeText;
    qint64 bytes = fileInfo.size();
    if (bytes < 1024) {
        sizeText = QString("%1 bytes").arg(bytes);
    } else if (bytes < 1024 * 1024) {
        sizeText = QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    } else {
        sizeText = QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
    }
    
    QString statusText = QString("%1 | %2 x %3 | %4")
        .arg(fileInfo.fileName())
        .arg(imageSize.width())
        .arg(imageSize.height())
        .arg(sizeText);
    
    statusBar()->showMessage(statusText);
}
