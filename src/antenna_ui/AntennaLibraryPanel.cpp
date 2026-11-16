#include "antenna_ui/AntennaLibraryPanel.h"

#include "antenna/AntennaLibrary.h"

#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lvd::antui {

using namespace ant;

namespace {
constexpr int KeyRole = Qt::UserRole + 1;
}

AntennaLibraryPanel::AntennaLibraryPanel(AntennaLibraryManager *library, QWidget *parent) : QWidget(parent), m_library(library)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter antenna library"));
    m_filter->setClearButtonEnabled(true);
    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_description = new QLabel;
    m_description->setWordWrap(true);
    m_description->setMargin(4);
    auto *use = new QPushButton(tr("Use in Current Document"));
    v->addWidget(m_filter);
    v->addWidget(m_tree, 1);
    v->addWidget(m_description);
    v->addWidget(use);

    connect(m_filter, &QLineEdit::textChanged, this, &AntennaLibraryPanel::reload);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        const QString key = item->data(0, KeyRole).toString();
        if (!key.isEmpty())
            Q_EMIT entryActivated(key);
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        m_description->setText(item ? item->toolTip(0) : QString());
    });
    connect(use, &QPushButton::clicked, this, [this] {
        if (QTreeWidgetItem *item = m_tree->currentItem(); item && !item->data(0, KeyRole).toString().isEmpty())
            Q_EMIT entryActivated(item->data(0, KeyRole).toString());
    });
    if (m_library)
        connect(m_library, &AntennaLibraryManager::librariesChanged, this, &AntennaLibraryPanel::reload);
    reload();
}

void AntennaLibraryPanel::reload()
{
    m_tree->clear();
    if (!m_library)
        return;
    const QString filter = m_filter->text().trimmed();
    for (const AntennaLibrary &lib : m_library->libraries()) {
        auto *libItem = new QTreeWidgetItem(m_tree, {lib.name});
        libItem->setToolTip(0, lib.description);
        QFont bold = libItem->font(0);
        bold.setBold(true);
        libItem->setFont(0, bold);
        for (AntennaEntryKind kind : {AntennaEntryKind::Element, AntennaEntryKind::Pattern, AntennaEntryKind::TrModule, AntennaEntryKind::Array}) {
            QTreeWidgetItem *kindItem = nullptr;
            for (const AntennaLibraryEntry &e : lib.entries) {
                if (e.kind != kind)
                    continue;
                if (!filter.isEmpty() && !e.name.contains(filter, Qt::CaseInsensitive) && !e.description.contains(filter, Qt::CaseInsensitive))
                    continue;
                if (!kindItem)
                    kindItem = new QTreeWidgetItem(libItem, {antennaEntryKindName(kind)});
                auto *item = new QTreeWidgetItem(kindItem, {e.name});
                item->setData(0, KeyRole, AntennaLibraryManager::makeKey(lib.name, e.id));
                item->setToolTip(0, e.description.isEmpty() ? e.name : e.description);
            }
            if (kindItem)
                kindItem->setExpanded(true);
        }
        if (libItem->childCount() == 0)
            delete libItem;
        else
            libItem->setExpanded(true);
    }
}

int AntennaLibraryPanel::entryCount() const
{
    int n = 0;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        if (!(*it)->data(0, KeyRole).toString().isEmpty())
            ++n;
    return n;
}

} // namespace lvd::antui
