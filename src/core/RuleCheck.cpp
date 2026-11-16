#include "core/RuleCheck.h"

#include "core/Connectivity.h"
#include "core/Expression.h"
#include "core/Netlist.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QHash>

#include <algorithm>

namespace lvd {

QString severityName(RuleViolation::Severity severity)
{
    switch (severity) {
    case RuleViolation::Severity::Error: return QStringLiteral("error");
    case RuleViolation::Severity::Warning: return QStringLiteral("warning");
    case RuleViolation::Severity::Info: return QStringLiteral("info");
    }
    return {};
}

namespace {

using Severity = RuleViolation::Severity;

void checkSheet(const Project &project, const Document &document, QList<RuleViolation> &violations)
{
    const Schematic &schematic = *document.schematic();
    const QString name = document.name();
    const auto add = [&](Severity severity, const QString &itemId, const QString &message) {
        violations.append(RuleViolation{severity, name, itemId, message});
    };

    // Definitions
    QHash<QString, DefinitionPtr> definitions;
    for (const PlacedComponent &component : schematic.components()) {
        const Resolution resolution = project.resolve(schematic, component.definition);
        switch (resolution.source) {
        case Resolution::Source::Missing:
            add(Severity::Error, component.id,
                QStringLiteral("%1: definition \"%2\" not found").arg(component.reference, component.definition));
            continue;
        case Resolution::Source::Cache:
            add(Severity::Warning, component.id,
                QStringLiteral("%1: library of \"%2\" not found; using the copy stored in the schematic")
                    .arg(component.reference, component.definition));
            break;
        case Resolution::Source::Library:
            if (resolution.changedInLibrary)
                add(Severity::Info, component.id,
                    QStringLiteral("%1: \"%2\" changed in its library since the schematic was saved")
                        .arg(component.reference, component.definition));
            for (const QString &pin : resolution.removedPins)
                add(Severity::Error, component.id,
                    QStringLiteral("%1: pin \"%2\" no longer exists in the library").arg(component.reference, pin));
            break;
        case Resolution::Source::Sheet:
            break;
        }
        definitions.insert(component.id, resolution.definition);

        for (auto it = component.parameters.begin(); it != component.parameters.end(); ++it) {
            const ParameterDefinition *parameter = resolution.definition->parameter(it.key());
            if (!parameter) {
                add(Severity::Warning, component.id,
                    QStringLiteral("%1: \"%2\" is not a parameter of %3")
                        .arg(component.reference, it.key(), resolution.definition->name));
            } else if (!parameter->choices.isEmpty() && !parameter->choices.contains(it.value())) {
                add(Severity::Error, component.id,
                    QStringLiteral("%1: \"%2\" is not a valid value of %3").arg(component.reference, it.value(), it.key()));
            } else if (parameter->isExpression()) {
                const Expression expression = Expression::parse(it.value());
                if (!expression.isValid())
                    add(Severity::Error, component.id,
                        QStringLiteral("%1: %2: %3").arg(component.reference, it.key(), expression.error()));
            }
        }
    }

    // References
    QHash<QString, QStringList> byReference;
    for (const PlacedComponent &component : schematic.components()) {
        const DefinitionPtr definition = definitions.value(component.id);
        if (definition && (!definition->netName.isEmpty() || definition->isSheetPort()))
            continue; // grounds and ports may share references
        byReference[component.reference].append(component.id);
    }
    for (auto it = byReference.begin(); it != byReference.end(); ++it) {
        if (it.value().size() > 1) {
            for (const QString &id : it.value())
                add(Severity::Error, id, QStringLiteral("reference %1 is used %2 times").arg(it.key()).arg(it.value().size()));
        }
    }

    // Connectivity
    const Connectivity connectivity = computeConnectivity(schematic, project.lookupFor(schematic));
    for (const SheetNet &net : connectivity.nets) {
        if (net.labelNames.size() > 1)
            add(Severity::Warning, net.labelIds.value(0),
                QStringLiteral("net %1 has several names: %2").arg(net.name, net.labelNames.join(QStringLiteral(", "))));
        if (net.portNames.size() > 1)
            add(Severity::Warning, net.labelIds.value(0),
                QStringLiteral("sheet ports %1 are connected together").arg(net.portNames.join(QStringLiteral(", "))));
        const bool namedElsewhere = !net.labelNames.isEmpty() || !net.globalName.isEmpty() || !net.portNames.isEmpty();
        if (net.pins.size() == 1 && net.wireIds.isEmpty() && !namedElsewhere) {
            const NetPin &pin = net.pins.first();
            const PlacedComponent *component = schematic.component(pin.componentId);
            add(Severity::Warning, pin.componentId,
                QStringLiteral("%1: pin %2 is not connected").arg(component ? component->reference : pin.componentId, pin.pin));
        } else if (net.pins.size() == 1 && !namedElsewhere) {
            const NetPin &pin = net.pins.first();
            const PlacedComponent *component = schematic.component(pin.componentId);
            add(Severity::Warning, pin.componentId,
                QStringLiteral("%1: pin %2 is connected to a wire that leads nowhere")
                    .arg(component ? component->reference : pin.componentId, pin.pin));
        }
        if (net.pins.isEmpty() && !net.labelIds.isEmpty() && net.wireIds.isEmpty())
            add(Severity::Warning, net.labelIds.first(), QStringLiteral("label %1 is not attached to a wire or pin").arg(net.name));
    }

    // Dangling wire ends: an end point that touches nothing else.
    QHash<QPoint, int> endCount;
    QSet<QPoint> anchors;
    for (const Wire &wire : schematic.wires()) {
        ++endCount[wire.points.first()];
        ++endCount[wire.points.last()];
    }
    for (const PlacedComponent &component : schematic.components()) {
        if (const DefinitionPtr definition = definitions.value(component.id)) {
            for (const QPoint &point : pinPositions(component, *definition))
                anchors.insert(point);
        }
    }
    for (const NetLabel &label : schematic.labels())
        anchors.insert(label.at);
    for (const Junction &junction : schematic.junctions())
        anchors.insert(junction.at);
    for (const Wire &wire : schematic.wires()) {
        for (const QPoint &end : {wire.points.first(), wire.points.last()}) {
            if (endCount.value(end) > 1 || anchors.contains(end))
                continue;
            bool onOtherWire = false;
            for (const Wire &other : schematic.wires()) {
                if (other.id != wire.id && wireContains(other, end)) {
                    onOtherWire = true;
                    break;
                }
            }
            if (!onOtherWire)
                add(Severity::Warning, wire.id,
                    QStringLiteral("wire end at (%1, %2) is not connected").arg(end.x()).arg(end.y()));
        }
    }

    // Variables
    QList<QPair<QString, QString>> variableDefinitions;
    for (const Variable &variable : schematic.variables())
        variableDefinitions.append({variable.name, variable.value});
    const VariableResults results = evaluateVariables(variableDefinitions, [](const QString &) { return 1.0; });
    // Undefined names may come from a parent sheet, so only syntax and cycles are reported here.
    for (auto it = results.errors.begin(); it != results.errors.end(); ++it)
        add(Severity::Error, QString(), QStringLiteral("variable %1: %2").arg(it.key(), it.value()));
}

} // namespace

QList<RuleViolation> checkSchematic(const Project &project, const Document &top)
{
    QList<RuleViolation> violations;
    if (!top.schematic())
        return violations;

    // Every sheet below the top, once.
    QStringList visited;
    QList<const Document *> pending{&top};
    while (!pending.isEmpty()) {
        const Document *document = pending.takeFirst();
        if (visited.contains(document->name()))
            continue;
        visited.append(document->name());
        checkSheet(project, *document, violations);
        const Schematic &schematic = *document->schematic();
        for (const PlacedComponent &component : schematic.components()) {
            QString sub;
            if (component.definition.startsWith(QStringLiteral("sheet:"))) {
                sub = component.definition.mid(6);
            } else if (const DefinitionPtr definition = project.resolve(schematic, component.definition).definition) {
                if (const ModelDefinition *model = definition->model(QStringLiteral("subschematic")))
                    sub = model->data.value(QStringLiteral("document")).toString();
            }
            if (sub.isEmpty())
                continue;
            const Document *subDocument = project.document(sub);
            if (subDocument && subDocument->schematic())
                pending.append(subDocument);
        }
    }

    // Hierarchy and parameter evaluation problems.
    const Netlist netlist = buildNetlist(project, top);
    for (const QString &message : netlist.errors)
        violations.append(RuleViolation{Severity::Error, top.name(), QString(), message});

    std::stable_sort(violations.begin(), violations.end(), [](const RuleViolation &a, const RuleViolation &b) {
        if (a.severity != b.severity)
            return a.severity < b.severity;
        if (a.document != b.document)
            return a.document < b.document;
        return a.itemId < b.itemId;
    });
    // Drop exact duplicates (the netlist repeats some sheet-level findings).
    violations.erase(std::unique(violations.begin(), violations.end(),
                                 [](const RuleViolation &a, const RuleViolation &b) {
                                     return a.severity == b.severity && a.document == b.document
                                            && a.itemId == b.itemId && a.message == b.message;
                                 }),
                     violations.end());
    return violations;
}

} // namespace lvd
