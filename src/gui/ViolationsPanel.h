#pragma once

#include "core/RuleCheck.h"

#include <QWidget>

class QLabel;
class QTreeWidget;

namespace lvd {

// Lists rule check results. Activating a row asks to show the item.
class ViolationsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ViolationsPanel(QWidget *parent = nullptr);

    void setViolations(const QString &documentName, const QList<RuleViolation> &violations);
    void clear();
    int count() const;

Q_SIGNALS:
    void itemActivated(const QString &documentName, const QString &itemId);

private:
    QLabel *m_summary;
    QTreeWidget *m_list;
};

} // namespace lvd
