#pragma once

#include "SettingsPage.h"

#include <QHash>
#include <QList>

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;

// Settings page "Tags": assigns a symbol from the curated palette (see
// TagSymbols) to each known tag. One combo box per tag; "(none)" = no symbol
// and no indicator in the grid at all, which is the default.
//
// Rows are split in two: tags that HAVE a symbol first, then the rest,
// separated by a rule — with "(none)" as the default the configured tags
// would otherwise be scattered through a long alphabetical list.
//
// The tag list comes from MetadataCache and is read when the page is built.
// It can be long, so the rows scroll below a fixed header with a filter box
// — hence wantsScrollArea() == false, the page does its own scrolling.
class TagSettingsPage : public SettingsPage
{
    Q_OBJECT
public:
    explicit TagSettingsPage(QWidget* parent = nullptr);

    QString pageId() const override { return QStringLiteral("tags"); }
    QString title() const override { return tr("Tags"); }
    void    apply() override;
    bool    wantsScrollArea() const override { return false; }

private:
    struct Row {
        QString     tag;
        QComboBox*  combo;
        int         formRow;
        bool        hasSymbol;   // group it was placed in when built
    };

    // tag → chosen symbol; empty string = none
    QHash<QString, QString> symbolMap() const;
    void onFilterChanged(const QString& text);

    QLineEdit*   m_filter   = nullptr;
    QLabel*      m_status   = nullptr;
    QFormLayout* m_form     = nullptr;
    // Form row of the rule between the two groups; -1 when one group is empty.
    // Only shown while BOTH groups still have a visible row under the filter.
    int          m_splitRow = -1;
    QList<Row>   m_rows;
};
