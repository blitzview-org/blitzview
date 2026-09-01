#include "SettingsDialog.h"

#include "AppSettings.h"
#include "CacheSettingsPage.h"
#include "InputSettingsPage.h"
#include "SettingsPage.h"
#include "TagSettingsPage.h"
#include "ViewerSettingsPage.h"

#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <utility>

namespace {
constexpr int kSidebarMaxWidth = 220;
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Settings"));

    m_collapse = new QToolButton(this);
    m_collapse->setAutoRaise(true);
    m_collapse->setCheckable(true);
    connect(m_collapse, &QToolButton::toggled,
            this, &SettingsDialog::setSidebarCollapsed);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search settings…"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged,
            this, &SettingsDialog::onSearchChanged);

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(m_collapse);
    topRow->addWidget(m_search, 1);
    // Absorbs the row while the search field is hidden, so the arrow button
    // stays parked at the left edge instead of drifting.
    topRow->addStretch(0);

    m_list = new QListWidget(this);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_list, &QListWidget::currentRowChanged,
            this, &SettingsDialog::onCurrentPageChanged);

    m_heading = new QLabel(this);
    QFont headingFont = m_heading->font();
    headingFont.setBold(true);
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.15);
    m_heading->setFont(headingFont);

    auto* headingRule = new QFrame(this);
    headingRule->setFrameShape(QFrame::HLine);
    headingRule->setFrameShadow(QFrame::Sunken);

    m_stack = new QStackedWidget(this);

    // The content gets its own panel: with the page list beside it, an
    // unframed column would just float in the dialog. The frame also gives
    // the padding that keeps fields off the sidebar.
    auto* contentPanel = new QFrame(this);
    contentPanel->setFrameShape(QFrame::StyledPanel);
    auto* rightColumn = new QVBoxLayout(contentPanel);
    rightColumn->setContentsMargins(12, 10, 12, 12);
    rightColumn->setSpacing(8);
    rightColumn->addWidget(m_heading);
    rightColumn->addWidget(headingRule);
    rightColumn->addWidget(m_stack, 1);

    auto* middleRow = new QHBoxLayout;
    middleRow->setSpacing(8);
    middleRow->addWidget(m_list);
    middleRow->addWidget(contentPanel, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    layout->addLayout(topRow);
    layout->addLayout(middleRow, 1);
    layout->addWidget(buttons);

    // Page order = sidebar order.
    addPage(new ViewerSettingsPage(this));
    addPage(new InputSettingsPage(this));
    addPage(new TagSettingsPage(this));
    addPage(new CacheSettingsPage(this));

    // Default; the caller overrides it via setSidebarCollapsed().
    setSidebarCollapsed(false);
    showPage(AppSettings::settingsLastPage());
    if (m_list->currentRow() < 0)
        m_list->setCurrentRow(0);

    // The content column must fit the widest page, otherwise long checkbox
    // labels get a horizontal scroll bar instead of simply being readable.
    int widest = 0;
    for (SettingsPage* page : std::as_const(m_pages))
        widest = qMax(widest, page->sizeHint().width());
    m_stack->setMinimumWidth(qMin(widest + 24, 620));

    const QByteArray geo = AppSettings::settingsGeometry();
    if (!geo.isEmpty())
        restoreGeometry(geo);
    else
        resize(qMax(680, sizeHint().width()), 480);
}

void SettingsDialog::addPage(SettingsPage* page)
{
    connect(page, &SettingsPage::repaintNeeded,
            this, &SettingsDialog::repaintNeeded);

    // Most pages are simply scrolled as a whole; a page that scrolls part of
    // itself (Tags) opts out so it does not end up inside two scroll areas.
    QScrollArea* scroll = nullptr;
    QWidget* stackEntry = page;
    if (page->wantsScrollArea()) {
        scroll = new QScrollArea(this);
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        stackEntry = scroll;
    }

    m_pages.append(page);
    m_scrolls.append(scroll);
    m_stack->addWidget(stackEntry);

    auto* item = new QListWidgetItem(page->title(), m_list);
    item->setData(Qt::UserRole, page->pageId());
}

void SettingsDialog::showPage(const QString& pageId)
{
    for (int i = 0; i < m_pages.size(); ++i) {
        if (m_pages.at(i)->pageId() == pageId) {
            m_list->setCurrentRow(i);
            return;
        }
    }
}

void SettingsDialog::setSidebarCollapsed(bool collapsed)
{
    // Collapsed is a focused single-page view: no page list and no search
    // either — both only make sense while browsing the whole dialog. The
    // arrow button is the way back.
    m_list->setVisible(!collapsed);
    m_search->setVisible(!collapsed);
    if (collapsed)
        m_search->clear();   // a hidden filter would silently hide pages

    if (!collapsed)
        m_list->setFixedWidth(qMin(kSidebarMaxWidth,
                                   m_list->sizeHintForColumn(0) + 24));

    m_collapse->setChecked(collapsed);
    m_collapse->setArrowType(collapsed ? Qt::RightArrow : Qt::LeftArrow);
    m_collapse->setToolTip(collapsed ? tr("Show all settings")
                                     : tr("Hide the page list"));
}

void SettingsDialog::onCurrentPageChanged(int row)
{
    if (row < 0 || row >= m_pages.size())
        return;
    m_stack->setCurrentIndex(row);
    m_heading->setText(m_pages.at(row)->title());
}

void SettingsDialog::onSearchChanged(const QString& text)
{
    const QString needle = text.trimmed();

    if (needle.isEmpty()) {
        for (int i = 0; i < m_list->count(); ++i)
            m_list->item(i)->setHidden(false);
        return;
    }

    int firstHitRow = -1;
    QWidget* firstHitWidget = nullptr;

    for (int i = 0; i < m_pages.size(); ++i) {
        SettingsPage* page = m_pages.at(i);
        QWidget* hit = nullptr;

        if (page->title().contains(needle, Qt::CaseInsensitive))
            hit = page;   // page-level match, no single field to jump to

        const auto index = page->searchIndex();
        for (const auto& [widget, haystack] : index) {
            if (haystack.contains(needle, Qt::CaseInsensitive)) {
                hit = widget;
                break;
            }
        }

        m_list->item(i)->setHidden(hit == nullptr);
        if (hit && firstHitRow < 0) {
            firstHitRow = i;
            firstHitWidget = (hit == page) ? nullptr : hit;
        }
    }

    if (firstHitRow < 0)
        return;

    m_list->setCurrentRow(firstHitRow);
    // Scroll the hit into view — but never move the FOCUS there: the user is
    // still typing, and stealing focus would swallow every character after
    // the first.
    if (firstHitWidget && m_scrolls.at(firstHitRow))
        m_scrolls.at(firstHitRow)->ensureWidgetVisible(firstHitWidget);
}

void SettingsDialog::accept()
{
    for (SettingsPage* page : std::as_const(m_pages))
        page->apply();

    QDialog::accept();
}

void SettingsDialog::done(int result)
{
    // Runs for OK, Cancel and window close alike — the dialog's own state is
    // remembered either way, it is not part of what Cancel discards.
    AppSettings::setSettingsGeometry(saveGeometry());
    if (m_list->currentRow() >= 0)
        AppSettings::setSettingsLastPage(m_pages.at(m_list->currentRow())->pageId());

    QDialog::done(result);
}
