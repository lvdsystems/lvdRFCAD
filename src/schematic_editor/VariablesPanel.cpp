#include "schematic_editor/VariablesPanel.h"

#include "core/Expression.h"
#include "core/Schematic.h"
#include "schematic_editor/EditorIcons.h"
#include "widgets/SchematicStyle.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace lvd {

namespace {

enum Column { NameColumn, ExpressionColumn, ValueColumn, ColumnCount };

} // namespace

VariablesPanel::VariablesPanel(Schematic *schematic, QWidget *parent) : QWidget(parent), m_schematic(schematic)
{
    setObjectName(QStringLiteral("schematicVariables"));
    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setObjectName(QStringLiteral("variables"));
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Expression"), tr("Value")});
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                             | QAbstractItemView::AnyKeyPressed);
    m_table->horizontalHeader()->setSectionResizeMode(NameColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ExpressionColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ValueColumn, QHeaderView::ResizeToContents);

    const auto button = [this](EditorIcon icon, const QString &tip, const QString &name) {
        auto *b = new QToolButton(this);
        b->setIcon(editorIcon(icon, palette()));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setObjectName(name);
        return b;
    };
    m_add = button(EditorIcon::Add, tr("Add a variable"), QStringLiteral("add"));
    m_remove = button(EditorIcon::Remove, tr("Remove the selected variables"), QStringLiteral("remove"));
    m_up = button(EditorIcon::Up, tr("Move up"), QStringLiteral("up"));
    m_down = button(EditorIcon::Down, tr("Move down"), QStringLiteral("down"));
    connect(m_add, &QToolButton::clicked, this, &VariablesPanel::addVariable);
    connect(m_remove, &QToolButton::clicked, this, &VariablesPanel::removeSelected);
    connect(m_up, &QToolButton::clicked, this, [this] { moveSelected(-1); });
    connect(m_down, &QToolButton::clicked, this, [this] { moveSelected(1); });

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->addWidget(m_add);
    buttons->addWidget(m_remove);
    buttons->addWidget(m_up);
    buttons->addWidget(m_down);
    buttons->addStretch();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(buttons);
    layout->addWidget(m_table, 1);

    connect(m_table, &QTableWidget::itemChanged, this, [this] {
        if (!m_updating)
            applyTable();
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &VariablesPanel::updateButtons);
    connect(m_schematic, &Schematic::variablesChanged, this, &VariablesPanel::refresh);
    refresh();
}

void VariablesPanel::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange) {
        m_add->setIcon(editorIcon(EditorIcon::Add, palette()));
        m_remove->setIcon(editorIcon(EditorIcon::Remove, palette()));
        m_up->setIcon(editorIcon(EditorIcon::Up, palette()));
        m_down->setIcon(editorIcon(EditorIcon::Down, palette()));
        refresh();
    }
    QWidget::changeEvent(event);
}

void VariablesPanel::refresh()
{
    const QList<Variable> &variables = m_schematic->variables();
    QList<QPair<QString, QString>> definitions;
    for (const Variable &variable : variables)
        definitions.append({variable.name, variable.value});
    const VariableResults results = evaluateVariables(definitions);
    const QColor error = SchematicStyle::fromPalette(palette()).error;

    m_updating = true;
    const int current = m_table->currentRow();
    m_table->setRowCount(int(variables.size()));
    for (int row = 0; row < variables.size(); ++row) {
        const Variable &variable = variables.at(row);
        const auto setText = [&](int column, const QString &text) {
            QTableWidgetItem *item = m_table->item(row, column);
            if (!item) {
                item = new QTableWidgetItem;
                m_table->setItem(row, column, item);
            }
            if (item->text() != text)
                item->setText(text);
            return item;
        };
        setText(NameColumn, variable.name);
        setText(ExpressionColumn, variable.value);
        QString value;
        QColor color = palette().color(QPalette::Text);
        QString tip;
        if (const auto it = results.errors.constFind(variable.name); it != results.errors.cend()) {
            value = *it;
            tip = *it;
            color = error;
        } else if (const auto found = results.values.constFind(variable.name); found != results.values.cend()) {
            value = formatEngineering(*found);
        }
        QTableWidgetItem *valueItem = setText(ValueColumn, value);
        valueItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        valueItem->setForeground(color);
        valueItem->setToolTip(tip);
    }
    if (current >= 0 && current < m_table->rowCount())
        m_table->setCurrentCell(current, m_table->currentColumn() < 0 ? 0 : m_table->currentColumn());
    m_updating = false;
    updateButtons();
}

void VariablesPanel::applyTable()
{
    QList<Variable> variables;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *name = m_table->item(row, NameColumn);
        const QTableWidgetItem *value = m_table->item(row, ExpressionColumn);
        variables.append(Variable{name ? name->text().trimmed() : QString(), value ? value->text().trimmed() : QString()});
    }
    if (variables != m_schematic->variables())
        m_schematic->setVariables(variables);
}

void VariablesPanel::addVariable()
{
    QList<Variable> variables = m_schematic->variables();
    QString name;
    for (int n = int(variables.size()) + 1;; ++n) {
        name = QStringLiteral("var%1").arg(n);
        if (std::none_of(variables.begin(), variables.end(), [&](const Variable &v) { return v.name == name; }))
            break;
    }
    variables.append(Variable{name, QStringLiteral("0")});
    m_schematic->setVariables(variables);
    const int row = int(variables.size()) - 1;
    m_table->setCurrentCell(row, NameColumn);
    if (isVisible())
        m_table->editItem(m_table->item(row, NameColumn));
}

void VariablesPanel::removeSelected()
{
    QSet<int> rows;
    for (const QModelIndex &index : m_table->selectionModel()->selectedIndexes())
        rows.insert(index.row());
    if (rows.isEmpty() && m_table->currentRow() >= 0)
        rows.insert(m_table->currentRow());
    if (rows.isEmpty())
        return;
    QList<Variable> variables;
    for (int row = 0; row < m_schematic->variables().size(); ++row) {
        if (!rows.contains(row))
            variables.append(m_schematic->variables().at(row));
    }
    m_schematic->setVariables(variables);
}

void VariablesPanel::moveSelected(int offset)
{
    const int row = m_table->currentRow();
    QList<Variable> variables = m_schematic->variables();
    const int target = row + offset;
    if (row < 0 || target < 0 || target >= variables.size())
        return;
    variables.swapItemsAt(row, target);
    m_schematic->setVariables(variables);
    m_table->setCurrentCell(target, std::max(0, m_table->currentColumn()));
    m_table->selectRow(target);
}

void VariablesPanel::updateButtons()
{
    const int row = m_table->currentRow();
    const bool any = row >= 0 && row < m_table->rowCount();
    m_remove->setEnabled(any);
    m_up->setEnabled(any && row > 0);
    m_down->setEnabled(any && row + 1 < m_table->rowCount());
}

} // namespace lvd
