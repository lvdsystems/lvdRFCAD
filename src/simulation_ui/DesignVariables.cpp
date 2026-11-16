#include "simulation_ui/DesignVariables.h"

#include "core/Document.h"
#include "core/Expression.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QCoreApplication>
#include <QMenu>

#include <algorithm>
#include <cmath>

namespace lvd::sim {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("lvd::sim::DesignVariables", text);
}

} // namespace

void setDefaultRange(rf::DesignVariable &variable, double value)
{
    if (value > 0) {
        variable.minimum = value / 2;
        variable.maximum = value * 2;
        variable.logarithmic = true;
    } else if (value < 0) {
        variable.minimum = value * 2;
        variable.maximum = value / 2;
        variable.logarithmic = false;
    } else {
        variable.minimum = 0;
        variable.maximum = 1;
        variable.logarithmic = false;
    }
}

bool sameDesignVariable(const rf::DesignVariable &a, const rf::DesignVariable &b)
{
    if (a.kind != b.kind || a.name != b.name)
        return false;
    return a.kind == rf::DesignVariable::Kind::Variable || (a.document == b.document && a.component == b.component);
}

QList<DesignCandidate> designCandidates(const Project &project, const Document &document)
{
    QList<DesignCandidate> result;
    const Schematic *schematic = document.schematic();
    if (!schematic)
        return result;

    for (const Variable &item : schematic->variables()) {
        rf::DesignVariable variable;
        variable.kind = rf::DesignVariable::Kind::Variable;
        variable.name = item.name;
        const auto value = rf::currentValue(project, document, variable);
        if (!value || !std::isfinite(*value))
            continue;
        setDefaultRange(variable, *value);
        result.append({variable, *value, variable.label(), QStringLiteral("%1 = %2").arg(item.name, formatValue(*value, {}))});
    }

    QList<const PlacedComponent *> components;
    for (const PlacedComponent &component : schematic->components())
        components.append(&component);
    std::sort(components.begin(), components.end(), [](const PlacedComponent *a, const PlacedComponent *b) {
        // R2 before R10: compare the prefix, then the number.
        const auto split = [](const QString &reference) {
            qsizetype i = reference.size();
            while (i > 0 && reference.at(i - 1).isDigit())
                --i;
            return std::pair<QString, int>(reference.left(i), reference.mid(i).toInt());
        };
        return split(a->reference) < split(b->reference);
    });
    for (const PlacedComponent *component : std::as_const(components)) {
        const DefinitionPtr definition = project.resolve(*schematic, component->definition).definition;
        if (!definition)
            continue;
        for (const ParameterDefinition &parameter : definition->parameters) {
            if (!parameter.isExpression())
                continue;
            rf::DesignVariable variable;
            variable.kind = rf::DesignVariable::Kind::Parameter;
            variable.name = parameter.name;
            variable.component = component->reference;
            variable.unit = parameter.unit;
            const auto value = rf::currentValue(project, document, variable);
            if (!value || !std::isfinite(*value))
                continue;
            setDefaultRange(variable, *value);
            const QString label = parameter.label.isEmpty() ? parameter.name : parameter.label;
            result.append({variable, *value, variable.label(),
                           QStringLiteral("%1 = %2").arg(label, formatValue(*value, parameter.unit))});
        }
    }
    return result;
}

QList<Schematic *> touchedSchematics(Project &project, Document &document, const QList<rf::DesignVariable> &variables)
{
    QList<Schematic *> result;
    for (const rf::DesignVariable &variable : variables) {
        Document *sheet = &document;
        if (variable.kind == rf::DesignVariable::Kind::Parameter && !variable.document.isEmpty())
            sheet = project.document(variable.document);
        Schematic *schematic = sheet ? sheet->schematic() : nullptr;
        if (schematic && !result.contains(schematic))
            result.append(schematic);
    }
    return result;
}

bool applyDesignValuesUndoable(Project &project, Document &document, const QList<rf::DesignVariable> &variables,
                               const std::vector<double> &values, const QString &description, QString *error)
{
    const QList<Schematic *> schematics = touchedSchematics(project, document, variables);
    for (Schematic *schematic : schematics)
        schematic->beginEdit(description);
    const bool ok = rf::applyDesignValues(project, document, variables, values, error);
    // An edit that changed nothing records no step; a failed apply may have
    // changed some values already, which stay undoable in the same step.
    for (auto it = schematics.crbegin(); it != schematics.crend(); ++it)
        (*it)->endEdit();
    return ok;
}

void fillDesignVariableMenu(QMenu *menu, const Project *project, const Document *document,
                            const QList<rf::DesignVariable> &existing,
                            const std::function<void(const rf::DesignVariable &)> &chosen)
{
    menu->clear();
    if (!project || !document || !document->schematic()) {
        menu->addAction(tr("No schematic"))->setEnabled(false);
        return;
    }
    const QList<DesignCandidate> candidates = designCandidates(*project, *document);
    if (candidates.isEmpty()) {
        menu->addAction(tr("No variables or numeric parameters"))->setEnabled(false);
        return;
    }
    const auto contains = [&existing](const rf::DesignVariable &variable) {
        return std::any_of(existing.begin(), existing.end(),
                           [&variable](const rf::DesignVariable &other) { return sameDesignVariable(variable, other); });
    };
    bool variablesSection = false;
    QMenu *submenu = nullptr;
    QString submenuComponent;
    for (const DesignCandidate &candidate : candidates) {
        QAction *action = nullptr;
        if (candidate.variable.kind == rf::DesignVariable::Kind::Variable) {
            if (!variablesSection) {
                menu->addSection(tr("Variables"));
                variablesSection = true;
            }
            action = menu->addAction(candidate.description);
        } else {
            if (!submenu || submenuComponent != candidate.variable.component) {
                if (!submenu)
                    menu->addSection(tr("Components"));
                submenuComponent = candidate.variable.component;
                submenu = menu->addMenu(submenuComponent);
            }
            action = submenu->addAction(candidate.description);
        }
        action->setEnabled(!contains(candidate.variable));
        const rf::DesignVariable variable = candidate.variable;
        QObject::connect(action, &QAction::triggered, menu, [chosen, variable] { chosen(variable); });
    }
}

std::optional<double> parseValue(const QString &text)
{
    if (const auto number = parseNumber(text))
        return number;
    const Expression expression = Expression::parse(text);
    if (!expression.isValid() || !expression.variables().isEmpty())
        return std::nullopt;
    return expression.evaluate();
}

QString formatValue(double value, const QString &unit, int significantDigits)
{
    return formatEngineering(value, unit, significantDigits);
}

QString seriesLabel(const std::optional<ESeries> &series)
{
    return series ? seriesName(*series) : tr("None");
}

std::vector<ESeries> offeredSeries()
{
    return {ESeries::E12, ESeries::E24, ESeries::E48, ESeries::E96};
}

} // namespace lvd::sim
