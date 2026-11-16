#include "schematic_editor/PropertiesPanel.h"

#include "core/Expression.h"
#include "schematic_editor/EditorIcons.h"
#include "schematic_editor/SchematicScene.h"

#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lvd {

namespace {

enum Column { LabelColumn, ValueColumn, UnitColumn, EvaluatedColumn, ResetColumn, StandardColumn, ColumnCount };

QLabel *wrappingLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}

} // namespace

std::optional<QString> standardValueText(const QString &text, const QString &unit, ESeries series, Rounding rounding)
{
    const QString trimmed = text.trimmed();
    std::optional<double> value = parseNumber(trimmed);
    if (!value) {
        const Expression expression = Expression::parse(trimmed);
        if (!expression.isValid() || !expression.variables().isEmpty())
            return std::nullopt;
        value = expression.evaluate();
    }
    if (!value || !std::isfinite(*value) || *value <= 0)
        return std::nullopt;
    const double rounded = snapToSeries(*value, series, rounding);

    // Keep the style of the original text. A one-letter unit is matched
    // case-sensitively ("1f" is one femto, not one farad).
    const bool withUnit = !unit.isEmpty()
                          && (trimmed.endsWith(unit) || (unit.size() > 1 && trimmed.endsWith(unit, Qt::CaseInsensitive)))
                          && trimmed.size() > unit.size();
    QString result = formatEngineering(rounded, withUnit ? unit : QString(), 4);
    if (!trimmed.contains(QLatin1Char(' ')))
        result.remove(QLatin1Char(' '));
    if (!trimmed.contains(QChar(0x00B5)) && !trimmed.contains(QChar(0x03BC))) {
        result.replace(QChar(0x00B5), QLatin1Char('u'));
        result.replace(QChar(0x03BC), QLatin1Char('u'));
    }
    return result;
}

PropertiesPanel::PropertiesPanel(SchematicScene *scene, QWidget *parent) : QWidget(parent), m_scene(scene)
{
    setObjectName(QStringLiteral("schematicProperties"));
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(0);
    connect(&m_refreshTimer, &QTimer::timeout, this, &PropertiesPanel::refresh);

    m_pages = new QStackedWidget(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_pages);

    m_emptyLabel = wrappingLabel(this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setText(tr("Select an item to see its properties."));
    m_emptyLabel->setForegroundRole(QPalette::PlaceholderText);
    m_pages->addWidget(m_emptyLabel);

    m_multiLabel = wrappingLabel(this);
    m_multiLabel->setAlignment(Qt::AlignCenter);
    m_pages->addWidget(m_multiLabel);

    // Component
    m_componentPage = new QWidget(this);
    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    m_reference = new QLineEdit(m_componentPage);
    m_reference->setObjectName(QStringLiteral("reference"));
    connect(m_reference, &QLineEdit::editingFinished, this, [this] {
        const PlacedComponent *component = m_scene->schematic()->component(m_tableComponent);
        const QString text = m_reference->text().trimmed();
        if (!component || text == component->reference)
            return;
        if (text.isEmpty()) {
            m_reference->setText(component->reference);
            return;
        }
        if (const PlacedComponent *other = m_scene->schematic()->componentByReference(text); other && other != component)
            Q_EMIT statusMessage(tr("Another component already uses the reference %1").arg(text));
        m_scene->schematic()->setReference(component->id, text);
    });
    m_definition = wrappingLabel(m_componentPage);
    m_source = wrappingLabel(m_componentPage);
    form->addRow(tr("Reference:"), m_reference);
    form->addRow(tr("Definition:"), m_definition);
    form->addRow(tr("Source:"), m_source);

    m_parameters = new QTableWidget(0, ColumnCount, m_componentPage);
    m_parameters->setObjectName(QStringLiteral("parameters"));
    m_parameters->setHorizontalHeaderLabels({tr("Parameter"), tr("Value"), tr("Unit"), tr("Evaluated"), QString(), QString()});
    m_parameters->verticalHeader()->hide();
    m_parameters->setSelectionMode(QAbstractItemView::NoSelection);
    m_parameters->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_parameters->horizontalHeader()->setSectionResizeMode(LabelColumn, QHeaderView::ResizeToContents);
    m_parameters->horizontalHeader()->setSectionResizeMode(ValueColumn, QHeaderView::Stretch);
    m_parameters->horizontalHeader()->setSectionResizeMode(UnitColumn, QHeaderView::ResizeToContents);
    m_parameters->horizontalHeader()->setSectionResizeMode(EvaluatedColumn, QHeaderView::ResizeToContents);
    m_parameters->horizontalHeader()->setSectionResizeMode(ResetColumn, QHeaderView::ResizeToContents);
    m_parameters->horizontalHeader()->setSectionResizeMode(StandardColumn, QHeaderView::ResizeToContents);

    auto *componentLayout = new QVBoxLayout(m_componentPage);
    componentLayout->setContentsMargins(0, 0, 0, 0);
    componentLayout->addLayout(form);
    componentLayout->addWidget(m_parameters, 1);
    m_pages->addWidget(m_componentPage);

    // Net label
    m_labelPage = new QWidget(this);
    auto *labelForm = new QFormLayout(m_labelPage);
    m_net = new QLineEdit(m_labelPage);
    m_net->setObjectName(QStringLiteral("net"));
    connect(m_net, &QLineEdit::editingFinished, this, [this] {
        if (m_ids.size() != 1 || !m_scene->schematic()->labels().contains(m_ids.first()))
            return;
        NetLabel label = m_scene->schematic()->labels().value(m_ids.first());
        const QString name = m_net->text().trimmed();
        if (name.isEmpty()) {
            m_net->setText(label.net);
            return;
        }
        if (name != label.net) {
            label.net = name;
            m_scene->schematic()->updateLabel(label);
        }
    });
    labelForm->addRow(tr("Net name:"), m_net);
    m_pages->addWidget(m_labelPage);

    // Text note
    m_textPage = new QWidget(this);
    auto *textForm = new QFormLayout(m_textPage);
    m_text = new QPlainTextEdit(m_textPage);
    m_text->setObjectName(QStringLiteral("text"));
    m_text->installEventFilter(this);
    m_textSize = new QSpinBox(m_textPage);
    m_textSize->setObjectName(QStringLiteral("textSize"));
    m_textSize->setRange(4, 100);
    connect(m_textSize, &QSpinBox::editingFinished, this, &PropertiesPanel::applyText);
    textForm->addRow(tr("Text:"), m_text);
    textForm->addRow(tr("Size:"), m_textSize);
    m_pages->addWidget(m_textPage);

    Schematic *schematic = m_scene->schematic();
    connect(schematic, &Schematic::changed, this, &PropertiesPanel::scheduleRefresh);
    connect(schematic, &Schematic::variablesChanged, this, &PropertiesPanel::scheduleRefresh);
    connect(schematic, &Schematic::itemRemoved, this, [this](const QString &id) {
        if (m_ids.contains(id)) {
            m_ids.removeAll(id);
            scheduleRefresh();
        }
    });
    if (LibraryManager *libraries = m_scene->project()->libraryManager())
        connect(libraries, &LibraryManager::librariesChanged, this, &PropertiesPanel::scheduleRefresh);
    refresh();
}

void PropertiesPanel::setSelection(const QStringList &ids)
{
    if (ids == m_ids)
        return;
    // Pending edits in the old selection's fields are applied first.
    if (m_text->hasFocus())
        applyText();
    m_ids = ids;
    refresh();
}

void PropertiesPanel::activate()
{
    for (QWidget *ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        if (auto *dock = qobject_cast<QDockWidget *>(ancestor)) {
            dock->show();
            dock->raise();
            break;
        }
    }
    if (m_pages->currentWidget() == m_componentPage)
        m_reference->setFocus();
}

void PropertiesPanel::scheduleRefresh()
{
    m_refreshTimer.start();
}

bool PropertiesPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_text && event->type() == QEvent::FocusOut)
        applyText();
    return QWidget::eventFilter(watched, event);
}

void PropertiesPanel::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange)
        scheduleRefresh();
    QWidget::changeEvent(event);
}

void PropertiesPanel::refresh()
{
    Schematic *schematic = m_scene->schematic();
    QStringList existing;
    for (const QString &id : std::as_const(m_ids)) {
        if (schematic->contains(id))
            existing.append(id);
    }
    m_ids = existing;

    if (m_ids.isEmpty()) {
        m_tableComponent.clear();
        m_pages->setCurrentWidget(m_emptyLabel);
        return;
    }
    if (m_ids.size() > 1) {
        m_tableComponent.clear();
        int counts[6] = {};
        for (const QString &id : std::as_const(m_ids))
            ++counts[int(Schematic::kindOf(id))];
        QStringList parts;
        if (counts[int(ItemKind::Component)])
            parts.append(tr("%n component(s)", nullptr, counts[int(ItemKind::Component)]));
        if (counts[int(ItemKind::Wire)])
            parts.append(tr("%n wire(s)", nullptr, counts[int(ItemKind::Wire)]));
        if (counts[int(ItemKind::Junction)])
            parts.append(tr("%n junction(s)", nullptr, counts[int(ItemKind::Junction)]));
        if (counts[int(ItemKind::Label)])
            parts.append(tr("%n net label(s)", nullptr, counts[int(ItemKind::Label)]));
        if (counts[int(ItemKind::Text)])
            parts.append(tr("%n text(s)", nullptr, counts[int(ItemKind::Text)]));
        m_multiLabel->setText(tr("%n items selected", nullptr, int(m_ids.size())) + QStringLiteral("\n")
                              + parts.join(QStringLiteral(", ")));
        m_pages->setCurrentWidget(m_multiLabel);
        return;
    }

    const QString id = m_ids.first();
    switch (Schematic::kindOf(id)) {
    case ItemKind::Component: showComponent(id); return;
    case ItemKind::Label:
        m_tableComponent.clear();
        if (!m_net->hasFocus())
            m_net->setText(schematic->labels().value(id).net);
        m_pages->setCurrentWidget(m_labelPage);
        return;
    case ItemKind::Text: {
        m_tableComponent.clear();
        const TextNote text = schematic->texts().value(id);
        if (!m_text->hasFocus() && m_text->toPlainText() != text.text)
            m_text->setPlainText(text.text);
        if (!m_textSize->hasFocus())
            m_textSize->setValue(text.size);
        m_pages->setCurrentWidget(m_textPage);
        return;
    }
    case ItemKind::Wire:
    case ItemKind::Junction:
    case ItemKind::None:
        m_tableComponent.clear();
        m_multiLabel->setText(Schematic::kindOf(id) == ItemKind::Wire ? tr("Wire %1").arg(id)
                                                                      : tr("Junction %1").arg(id));
        m_pages->setCurrentWidget(m_multiLabel);
        return;
    }
}

void PropertiesPanel::showComponent(const QString &id)
{
    const PlacedComponent component = *m_scene->schematic()->component(id);
    const Resolution resolution = m_scene->resolution(component.definition);
    const SchematicStyle style = SchematicStyle::fromPalette(palette());

    if (!m_reference->hasFocus())
        m_reference->setText(component.reference);
    m_definition->setText(resolution.definition
                              ? QStringLiteral("%1 (%2)").arg(component.definition, resolution.definition->name)
                              : component.definition);

    QString source;
    QColor color = palette().color(QPalette::WindowText);
    switch (resolution.source) {
    case Resolution::Source::Library:
        if (resolution.changedInLibrary) {
            source = tr("Changed in library: the library definition differs from the copy saved with this "
                        "schematic. The saved copy is updated when the document is saved.");
            if (!resolution.removedPins.isEmpty())
                source += QLatin1Char(' ')
                          + tr("Pins no longer in the library: %1.").arg(resolution.removedPins.join(QStringLiteral(", ")));
            color = style.warning;
        } else {
            source = tr("Library: the current library definition is used.");
        }
        break;
    case Resolution::Source::Cache:
        source = tr("From cache: the library or the component is not available, so the copy saved with this "
                    "schematic is used. A machine with the library may give different results.");
        color = style.warning;
        break;
    case Resolution::Source::Sheet:
        source = tr("Sheet: a block generated from the schematic document %1.")
                     .arg(component.definition.mid(component.definition.indexOf(QLatin1Char(':')) + 1));
        break;
    case Resolution::Source::Missing:
        source = tr("Missing: neither the libraries nor this schematic's cache have this definition.");
        color = style.error;
        break;
    }
    m_source->setText(source);
    QPalette sourcePalette = m_source->palette();
    sourcePalette.setColor(QPalette::WindowText, color);
    m_source->setPalette(sourcePalette);

    if (!resolution.definition) {
        m_parameters->setRowCount(0);
        m_tableComponent = id;
        m_tableDefinition.reset();
    } else if (m_tableComponent != id || m_tableDefinition != resolution.definition) {
        buildParameterTable(component, *resolution.definition);
        m_tableComponent = id;
        m_tableDefinition = resolution.definition;
    }
    if (resolution.definition)
        updateParameterRows(component, *resolution.definition);
    m_pages->setCurrentWidget(m_componentPage);
}

void PropertiesPanel::buildParameterTable(const PlacedComponent &, const ComponentDefinition &definition)
{
    m_parameters->setRowCount(0);
    m_parameters->setRowCount(int(definition.parameters.size()));
    const QPalette pal = palette();
    for (int row = 0; row < definition.parameters.size(); ++row) {
        const ParameterDefinition &parameter = definition.parameters.at(row);
        const QString name = parameter.name;

        auto *label = readOnlyItem(parameter.label.isEmpty() ? name : parameter.label);
        label->setToolTip(name);
        m_parameters->setItem(row, LabelColumn, label);
        m_parameters->setItem(row, UnitColumn, readOnlyItem(parameter.unit));
        m_parameters->setItem(row, EvaluatedColumn, readOnlyItem(QString()));

        QWidget *editor = nullptr;
        if (!parameter.choices.isEmpty()) {
            auto *combo = new QComboBox;
            combo->setObjectName(QStringLiteral("value:") + name);
            combo->addItems(parameter.choices);
            connect(combo, &QComboBox::activated, this,
                    [this, combo, name](int index) { applyParameter(name, combo->itemText(index)); });
            editor = combo;
        } else {
            auto *line = new QLineEdit;
            line->setObjectName(QStringLiteral("value:") + name);
            line->setFrame(false);
            line->setPlaceholderText(parameter.defaultValue);
            connect(line, &QLineEdit::editingFinished, this, [this, line, name] { applyParameter(name, line->text()); });
            if (parameter.isExpression()) {
                // The field's own context menu, with the standard values.
                line->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(line, &QLineEdit::customContextMenuRequested, this, [this, line, name](const QPoint &position) {
                    QMenu *menu = line->createStandardContextMenu();
                    menu->addSeparator();
                    QMenu *standard = menu->addMenu(tr("Round to Standard Value"));
                    fillStandardValueMenu(standard, name);
                    menu->exec(line->mapToGlobal(position));
                    delete menu;
                });
            }
            if (parameter.isFile) {
                auto *box = new QWidget;
                auto *layout = new QHBoxLayout(box);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(2);
                auto *browse = new QToolButton(box);
                browse->setIcon(editorIcon(EditorIcon::Browse, pal));
                browse->setToolTip(tr("Choose a file in the project folder"));
                connect(browse, &QToolButton::clicked, this, [this, name] { browseFile(name); });
                layout->addWidget(line, 1);
                layout->addWidget(browse);
                editor = box;
            } else {
                editor = line;
            }
        }
        m_parameters->setCellWidget(row, ValueColumn, editor);

        auto *reset = new QToolButton;
        reset->setObjectName(QStringLiteral("reset:") + name);
        reset->setAutoRaise(true);
        reset->setIcon(editorIcon(EditorIcon::Reset, pal));
        reset->setToolTip(tr("Reset to the default value"));
        connect(reset, &QToolButton::clicked, this, [this, name] {
            if (!m_tableComponent.isEmpty())
                m_scene->schematic()->resetParameter(m_tableComponent, name);
        });
        m_parameters->setCellWidget(row, ResetColumn, reset);

        if (parameter.isExpression()) {
            auto *standard = new QToolButton;
            standard->setObjectName(QStringLiteral("standard:") + name);
            standard->setAutoRaise(true);
            standard->setText(tr("E"));
            standard->setToolTip(tr("Round to a standard value (E12, E24, E48, E96)"));
            standard->setPopupMode(QToolButton::InstantPopup);
            standard->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0px; }"));
            auto *menu = new QMenu(standard);
            menu->setObjectName(QStringLiteral("standardMenu:") + name);
            connect(menu, &QMenu::aboutToShow, this, [this, menu, name] { fillStandardValueMenu(menu, name); });
            standard->setMenu(menu);
            m_parameters->setCellWidget(row, StandardColumn, standard);
        }
    }
}

void PropertiesPanel::updateParameterRows(const PlacedComponent &component, const ComponentDefinition &definition)
{
    QList<QPair<QString, QString>> variables;
    for (const Variable &variable : m_scene->schematic()->variables())
        variables.append({variable.name, variable.value});
    const VariableResults results = evaluateVariables(variables);
    const Expression::Lookup lookup = [&results](const QString &name) -> std::optional<double> {
        const auto it = results.values.constFind(name);
        return it == results.values.cend() ? std::nullopt : std::optional<double>(*it);
    };
    const SchematicStyle style = SchematicStyle::fromPalette(palette());
    const QColor normal = palette().color(QPalette::Text);

    for (int row = 0; row < definition.parameters.size() && row < m_parameters->rowCount(); ++row) {
        const ParameterDefinition &parameter = definition.parameters.at(row);
        const bool overridden = component.parameters.contains(parameter.name);
        const QString value = parameterText(component, definition, parameter.name);

        const QString editorName = QStringLiteral("value:") + parameter.name;
        QWidget *cell = m_parameters->cellWidget(row, ValueColumn);
        if (auto *combo = qobject_cast<QComboBox *>(cell)) {
            const QSignalBlocker blocker(combo);
            if (combo->findText(value) < 0)
                combo->addItem(value);
            combo->setCurrentText(value);
            QFont font = combo->font();
            font.setItalic(!overridden);
            combo->setFont(font);
            combo->setToolTip(overridden ? QString() : tr("Default value"));
        } else {
            auto *line = qobject_cast<QLineEdit *>(cell);
            if (!line)
                line = cell->findChild<QLineEdit *>(editorName);
            if (line && !line->hasFocus())
                line->setText(overridden ? value : QString());
        }

        QTableWidgetItem *evaluated = m_parameters->item(row, EvaluatedColumn);
        if (parameter.isExpression()) {
            QString error;
            const auto result = Expression::parse(value).evaluate(lookup, &error);
            if (result) {
                evaluated->setText(formatEngineering(*result, parameter.unit));
                evaluated->setForeground(normal);
                evaluated->setToolTip(QString());
            } else {
                evaluated->setText(error);
                evaluated->setForeground(style.error);
                evaluated->setToolTip(error);
            }
        } else {
            evaluated->setText(value);
            evaluated->setForeground(normal);
        }

        if (auto *reset = qobject_cast<QToolButton *>(m_parameters->cellWidget(row, ResetColumn)))
            reset->setEnabled(overridden);
        if (auto *standard = qobject_cast<QToolButton *>(m_parameters->cellWidget(row, StandardColumn)))
            standard->setEnabled(standardValueText(value, parameter.unit, ESeries::E96).has_value());
    }
}

void PropertiesPanel::applyParameter(const QString &name, const QString &text)
{
    Schematic *schematic = m_scene->schematic();
    const PlacedComponent *component = schematic->component(m_tableComponent);
    if (!component || !m_tableDefinition)
        return;
    const ParameterDefinition *parameter = m_tableDefinition->parameter(name);
    if (!parameter)
        return;
    const QString value = text.trimmed();
    const bool overridden = component->parameters.contains(name);
    // Only values that differ from the default are stored.
    if (value.isEmpty() || value == parameter->defaultValue) {
        if (overridden)
            schematic->resetParameter(component->id, name);
        return;
    }
    if (value != parameterText(*component, *m_tableDefinition, name))
        schematic->setParameter(component->id, name, value);
}

QString PropertiesPanel::currentParameterText(const QString &name) const
{
    const PlacedComponent *component = m_scene->schematic()->component(m_tableComponent);
    if (!component || !m_tableDefinition)
        return {};
    return parameterText(*component, *m_tableDefinition, name);
}

bool PropertiesPanel::roundToStandardValue(const QString &name, ESeries series, Rounding rounding)
{
    const PlacedComponent *component = m_scene->schematic()->component(m_tableComponent);
    if (!component || !m_tableDefinition)
        return false;
    const ParameterDefinition *parameter = m_tableDefinition->parameter(name);
    if (!parameter || !parameter->isExpression())
        return false;
    const QString text = parameterText(*component, *m_tableDefinition, name);
    const auto rounded = standardValueText(text, parameter->unit, series, rounding);
    if (!rounded)
        return false;
    // Only a changed value is written; a standard value stays as it is.
    const auto before = Expression::parse(text).evaluate();
    const auto after = parseNumber(*rounded);
    if (before && after && std::abs(*before - *after) <= 1e-9 * std::abs(*before))
        return true;
    const QString reference = component->reference;
    applyParameter(name, *rounded);
    Q_EMIT statusMessage(tr("%1.%2 rounded to %3 (%4)").arg(reference, name, *rounded, seriesName(series)));
    return true;
}

QMenu *PropertiesPanel::standardValueMenu(const QString &parameter) const
{
    for (int row = 0; row < m_parameters->rowCount(); ++row) {
        if (auto *button = qobject_cast<QToolButton *>(m_parameters->cellWidget(row, StandardColumn));
            button && button->objectName() == QStringLiteral("standard:") + parameter)
            return button->menu();
    }
    return nullptr;
}

void PropertiesPanel::fillStandardValueMenu(QMenu *menu, const QString &name)
{
    menu->clear();
    const ParameterDefinition *parameter = m_tableDefinition ? m_tableDefinition->parameter(name) : nullptr;
    const QString text = currentParameterText(name);
    if (!parameter || !standardValueText(text, parameter->unit, ESeries::E96)) {
        menu->addAction(tr("Only for a positive number or a constant expression"))->setEnabled(false);
        return;
    }
    const std::pair<Rounding, QString> roundings[] = {
        {Rounding::Nearest, tr("Nearest")}, {Rounding::Up, tr("Up")}, {Rounding::Down, tr("Down")}};
    for (ESeries series : {ESeries::E12, ESeries::E24, ESeries::E48, ESeries::E96}) {
        QMenu *submenu = menu->addMenu(seriesName(series));
        for (const auto &[rounding, label] : roundings) {
            const QString result = standardValueText(text, parameter->unit, series, rounding).value_or(QString());
            QAction *action = submenu->addAction(tr("%1: %2").arg(label, result));
            action->setObjectName(QStringLiteral("%1-%2").arg(seriesName(series)).arg(static_cast<int>(rounding)));
            connect(action, &QAction::triggered, this,
                    [this, name, series, rounding] { roundToStandardValue(name, series, rounding); });
        }
    }
}

void PropertiesPanel::browseFile(const QString &name)
{
    const PlacedComponent *component = m_scene->schematic()->component(m_tableComponent);
    if (!component || !m_tableDefinition)
        return;
    const QDir projectDir(m_scene->project()->directory());
    const QString current = parameterText(*component, *m_tableDefinition, name);
    const QString start = current.isEmpty() ? projectDir.path() : projectDir.absoluteFilePath(current);
    const QString chosen = QFileDialog::getOpenFileName(this, tr("Choose File"), start);
    if (chosen.isEmpty())
        return;
    const QString relative = projectDir.relativeFilePath(chosen);
    if (relative.startsWith(QStringLiteral("..")))
        Q_EMIT statusMessage(tr("The file is outside the project folder; the project will not be self-contained"));
    applyParameter(name, relative);
}

void PropertiesPanel::applyText()
{
    if (m_ids.size() != 1 || !m_scene->schematic()->texts().contains(m_ids.first()))
        return;
    TextNote text = m_scene->schematic()->texts().value(m_ids.first());
    const QString changed = m_text->toPlainText();
    if ((changed == text.text || changed.trimmed().isEmpty()) && m_textSize->value() == text.size)
        return;
    if (!changed.trimmed().isEmpty())
        text.text = changed;
    text.size = m_textSize->value();
    m_scene->schematic()->updateText(text);
}

} // namespace lvd
