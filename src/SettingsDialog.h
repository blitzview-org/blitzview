#pragma once

#include <QDialog>
#include <QList>
#include <QString>

class QLabel;
class QLineEdit;
class QListWidget;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class SettingsPage;

// Application settings dialog: page list on the left, page content on the
// right, search field on top.
//
// The Settings menu opens this dialog either on a specific page (sidebar
// collapsed to icons) or as the general entrance (sidebar expanded).
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    // Select a page by its SettingsPage::pageId(). Unknown ids are ignored.
    void showPage(const QString& pageId);
    void setSidebarCollapsed(bool collapsed);

    void accept() override;
    void done(int result) override;

signals:
    // Forwarded from the pages: something changed that the grid must repaint.
    void repaintNeeded();

private:
    void addPage(SettingsPage* page);
    void onCurrentPageChanged(int row);
    void onSearchChanged(const QString& text);

    QLineEdit*      m_search    = nullptr;
    QToolButton*    m_collapse  = nullptr;
    QListWidget*    m_list      = nullptr;
    QStackedWidget* m_stack     = nullptr;
    QLabel*         m_heading   = nullptr;

    QList<SettingsPage*> m_pages;
    QList<QScrollArea*>  m_scrolls;   // parallel to m_pages
};
