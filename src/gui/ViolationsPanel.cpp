#include "gui/ViolationsPanel.h"

#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lvd {

namespace {

const int DocumentRole = Qt::UserRole;
const int ItemRole = Qt::UserRole + 1;

QIcon severityIcon(RuleViolation::Severity severity)
{
    QStyle *style = QApplication::style();
    switch (severity) {
    case RuleViolation::Severity::Error: return style->standardIcon(QStyle::SP_MessageBoxCritical);
    case RuleViolation::Severity::Warning: return style->standardIcon(QStyle::SP_MessageBoxWarning);
    case RuleViolation::Severity::Info: return style->standardIcon(QStyle::SP_MessageBoxInformation);
    }
    return {};
}

} // namespace

ViolationsPanel::ViolationsPanel(QWidget *parent) : QWidget(parent)
{
    m_summary = new QLabel(tr("Run the rule check from the schematic toolbar."), this);
    m_summary->setContentsMargins(6, 4, 6, 4);

    m_list = new QTreeWidget(this);
    m_list->setHeaderLabels({tr("Severity"), tr("Sheet"), tr("Message")});
    m_list->setRootIsDecorated(false);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setAlternatingRowColors(true);
    m_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_list->header()->setStretchLastSection(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_summary);
    layout->addWidget(m_list);

    connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        Q_EMIT itemActivated(item->data(0, DocumentRole).toString(), item->data(0, ItemRole).toString());
    });
}

void ViolationsPanel::setViolations(const QString &documentName, const QList<RuleViolation> &violations)
{
    m_list->clear();
    int errors = 0, warnings = 0;
    for (const RuleViolation &violation : violations) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setIcon(0, severityIcon(violation.severity));
        switch (violation.severity) {
        case RuleViolation::Severity::Error:
            item->setText(0, tr("Error"));
            ++errors;
            break;
        case RuleViolation::Severity::Warning:
            item->setText(0, tr("Warning"));
            ++warnings;
            break;
        case RuleViolation::Severity::Info:
            item->setText(0, tr("Info"));
            break;
        }
        item->setText(1, violation.document);
        item->setText(2, violation.message);
        item->setToolTip(2, violation.message);
        item->setData(0, DocumentRole, violation.document);
        item->setData(0, ItemRole, violation.itemId);
    }
    if (violations.isEmpty())
        m_summary->setText(tr("Rule check of %1: no problems found.").arg(documentName));
    else
        m_summary->setText(tr("Rule check of %1: %2 errors, %3 warnings, %4 notes.")
                               .arg(documentName)
                               .arg(errors)
                               .arg(warnings)
                               .arg(violations.size() - errors - warnings));
}

void ViolationsPanel::clear()
{
    m_list->clear();
    m_summary->setText(tr("Run the rule check from the schematic toolbar."));
}

int ViolationsPanel::count() const
{
    return m_list->topLevelItemCount();
}

} // namespace lvd
