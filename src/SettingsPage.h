#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QWidget>

class QFormLayout;
class QVBoxLayout;

// One page of the settings dialog.
//
// A page owns its widgets, loads their values from AppSettings in the
// constructor and writes them back in apply(). There is no settings-changed
// signal in this code base (AppSettings is a plain static class), so apply()
// must ALSO push the new value into whatever is already running — see the
// existing pages for the three flavors (QApplication property, singleton,
// repaint request).
class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(QWidget* parent = nullptr);

    // Stable identifier used by SettingsDialog::showPage() and persisted as
    // the last-visited page — never translate or rename it.
    virtual QString pageId() const = 0;
    // Shown in the sidebar and as the page heading (translated).
    virtual QString title() const = 0;
    // Persist and apply. Called for every page when the dialog is accepted.
    virtual void apply() = 0;

    // False for pages that scroll a part of themselves and want the rest to
    // stay put (see TagSettingsPage). Nesting the dialog's scroll area around
    // such a page would give it two scroll bars.
    virtual bool wantsScrollArea() const { return true; }

    // Searchable (widget, text) pairs: every label text, checkbox/button
    // text and tooltip found below this page. Collected by walking the child
    // widgets, so adding a field to a page keeps the search working without
    // a second list to maintain.
    QList<QPair<QWidget*, QString>> searchIndex() const;

signals:
    // Emitted from apply() when the change affects how items are drawn in
    // the grid. SettingsDialog forwards it; MainWindow repaints.
    void repaintNeeded();

protected:
    // Plain block of form rows — the usual case, since the dialog already
    // shows the page title as a heading.
    QFormLayout* addForm();
    // Titled block, for pages that need more than one visual group.
    QFormLayout* addGroup(const QString& title);
    // For pages that need a non-form widget (lists, scroll contents).
    void addPageWidget(QWidget* widget, int stretch = 0);

private:
    QVBoxLayout* m_layout = nullptr;
};
