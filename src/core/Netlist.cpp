#include "core/Netlist.h"

#include "core/Connectivity.h"
#include "core/Expression.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace lvd {

namespace {

constexpr int MaxDepth = 32;

// Union-find over flat net names.
class NetMerger
{
public:
    void add(const QString &name) { find(name); }

    QString find(const QString &name)
    {
        auto it = m_parent.find(name);
        if (it == m_parent.end()) {
            m_parent.insert(name, name);
            return name;
        }
        QString root = name;
        while (m_parent.value(root) != root)
            root = m_parent.value(root);
        m_parent.insert(name, root);
        return root;
    }

    void unite(const QString &a, const QString &b)
    {
        const QString ra = find(a), rb = find(b);
        if (ra == rb)
            return;
        if (preferred(ra, rb))
            m_parent.insert(rb, ra);
        else
            m_parent.insert(ra, rb);
    }

private:
    // Names closer to the top of the hierarchy win, then the shorter, then
    // the alphabetically first.
    static bool preferred(const QString &a, const QString &b)
    {
        const auto depth = [](const QString &name) { return name.count(QLatin1Char('/')); };
        if (depth(a) != depth(b))
            return depth(a) < depth(b);
        if (a.size() != b.size())
            return a.size() < b.size();
        return a < b;
    }

    QHash<QString, QString> m_parent;
};

class Flattener
{
public:
    Flattener(const Project &project, Netlist &netlist) : m_project(project), m_netlist(netlist) {}

    void flatten(const Document &document, const QString &prefix, const QHash<QString, QString> &portNets,
                 const QHash<QString, double> &overrides, const Expression::Lookup &outer, QStringList chain)
    {
        const Schematic *schematic = document.schematic();
        if (!schematic) {
            error(prefix, QStringLiteral("document \"%1\" is not a schematic").arg(document.name()));
            return;
        }
        chain.append(document.name());
        const DefinitionLookup lookup = m_project.lookupFor(*schematic);
        const Connectivity connectivity = computeConnectivity(*schematic, lookup);

        // Variables: overrides from the instance win; the rest may refer to
        // the instance overrides and to the enclosing scope.
        QList<QPair<QString, QString>> definitions;
        for (const Variable &variable : schematic->variables()) {
            if (!overrides.contains(variable.name))
                definitions.append({variable.name, variable.value});
        }
        const Expression::Lookup parentScope = [overrides, outer](const QString &name) -> std::optional<double> {
            if (const auto it = overrides.constFind(name); it != overrides.cend())
                return *it;
            return outer ? outer(name) : std::nullopt;
        };
        const VariableResults variables = evaluateVariables(definitions, parentScope);
        for (auto it = variables.errors.begin(); it != variables.errors.end(); ++it)
            error(prefix, QStringLiteral("%1: variable %2: %3").arg(document.name(), it.key(), it.value()));
        const QHash<QString, double> values = variables.values;
        const Expression::Lookup scope = [values, parentScope](const QString &name) -> std::optional<double> {
            if (const auto it = values.constFind(name); it != values.cend())
                return *it;
            return parentScope(name);
        };

        // Flat names of this sheet's nets.
        QStringList netNames;
        for (const SheetNet &net : connectivity.nets) {
            const QString flat = net.nameSource == SheetNet::NameSource::Global ? net.name : prefix + net.name;
            m_merger.add(flat);
            for (const QString &port : net.portNames) {
                if (const auto it = portNets.constFind(port); it != portNets.cend())
                    m_merger.unite(flat, *it);
            }
            netNames.append(flat);
        }
        const auto netOfPin = [&](const QString &componentId, const QString &pin) {
            const int index = connectivity.netOfPin(componentId, pin);
            return index >= 0 ? netNames.at(index) : QString();
        };

        QList<const PlacedComponent *> components;
        for (const PlacedComponent &component : schematic->components())
            components.append(&component);
        std::sort(components.begin(), components.end(),
                  [](const PlacedComponent *a, const PlacedComponent *b) { return a->reference < b->reference; });

        for (const PlacedComponent *component : components) {
            const QString path = prefix + component->reference;
            const DefinitionPtr definition = lookup(component->definition);
            if (!definition) {
                error(prefix, QStringLiteral("%1: definition \"%2\" not found").arg(path, component->definition));
                continue;
            }
            if (definition->isSheetPort() || !definition->netName.isEmpty())
                continue;

            QString subDocument;
            const bool isSheet = component->definition.startsWith(QStringLiteral("sheet:"));
            if (isSheet)
                subDocument = component->definition.mid(6);
            else if (const ModelDefinition *model = definition->model(QStringLiteral("subschematic")))
                subDocument = model->data.value(QStringLiteral("document")).toString();

            // Parameter values, evaluated in this sheet's scope.
            QMap<QString, QString> texts;
            QMap<QString, double> numbers;
            for (const ParameterDefinition &parameter : definition->parameters) {
                const QString text = parameterText(*component, *definition, parameter.name);
                texts.insert(parameter.name, text);
                if (!parameter.isExpression())
                    continue;
                // A sheet parameter left at its default is evaluated inside the sheet.
                if (isSheet && !component->parameters.contains(parameter.name))
                    continue;
                QString evaluationError;
                const auto value = Expression::parse(text).evaluate(scope, &evaluationError);
                if (value)
                    numbers.insert(parameter.name, *value);
                else
                    error(prefix, QStringLiteral("%1: parameter %2: %3").arg(path, parameter.name, evaluationError));
            }

            if (!subDocument.isEmpty()) {
                if (chain.contains(subDocument)) {
                    error(prefix, QStringLiteral("%1: sheet \"%2\" contains itself (%3)")
                                      .arg(path, subDocument, (chain + QStringList{subDocument}).join(QStringLiteral(" > "))));
                    continue;
                }
                if (chain.size() >= MaxDepth) {
                    error(prefix, QStringLiteral("%1: hierarchy deeper than %2 levels").arg(path).arg(MaxDepth));
                    continue;
                }
                const Document *sub = m_project.document(subDocument);
                if (!sub) {
                    error(prefix, QStringLiteral("%1: sheet document \"%2\" not found").arg(path, subDocument));
                    continue;
                }
                QHash<QString, QString> childPorts;
                for (const QString &pin : definition->pins) {
                    const QString net = netOfPin(component->id, pin);
                    if (!net.isEmpty())
                        childPorts.insert(pin, net);
                }
                QHash<QString, double> childOverrides;
                for (auto it = numbers.begin(); it != numbers.end(); ++it)
                    childOverrides.insert(it.key(), it.value());
                flatten(*sub, path + QLatin1Char('/'), childPorts, childOverrides, scope, chain);
                continue;
            }

            FlatComponent flat;
            flat.path = path;
            flat.instancePath = prefix;
            flat.document = document.name();
            flat.componentId = component->id;
            flat.definitionKey = component->definition;
            flat.definition = definition;
            flat.parameterText = texts;
            flat.parameterValues = numbers;
            for (const QString &pin : definition->pins)
                flat.pinNets.insert(pin, netOfPin(component->id, pin));
            m_netlist.components.append(flat);
        }
    }

    void finish()
    {
        QSet<QString> nets;
        for (FlatComponent &component : m_netlist.components) {
            for (auto it = component.pinNets.begin(); it != component.pinNets.end(); ++it) {
                it.value() = m_merger.find(it.value());
                nets.insert(it.value());
            }
        }
        m_netlist.nets = QStringList(nets.begin(), nets.end());
        m_netlist.nets.sort();
        std::sort(m_netlist.components.begin(), m_netlist.components.end(),
                  [](const FlatComponent &a, const FlatComponent &b) { return a.path < b.path; });
    }

private:
    void error(const QString &prefix, const QString &message)
    {
        Q_UNUSED(prefix)
        m_netlist.errors.append(message);
    }

    const Project &m_project;
    Netlist &m_netlist;
    NetMerger m_merger;
};

} // namespace

const FlatComponent *Netlist::component(const QString &path) const
{
    for (const FlatComponent &candidate : components) {
        if (candidate.path == path)
            return &candidate;
    }
    return nullptr;
}

QStringList Netlist::pinsOn(const QString &net) const
{
    QStringList pins;
    for (const FlatComponent &flat : components) {
        for (auto it = flat.pinNets.begin(); it != flat.pinNets.end(); ++it) {
            if (it.value() == net)
                pins.append(flat.path + QLatin1Char('.') + it.key());
        }
    }
    pins.sort();
    return pins;
}

QString Netlist::toText() const
{
    QString text = QStringLiteral("* lvd RF cad netlist of %1\n").arg(topDocument);
    for (const FlatComponent &flat : components) {
        QStringList fields{flat.path, flat.definitionKey};
        for (const QString &pin : flat.definition->pins)
            fields.append(pin + QLatin1Char('=') + flat.pinNets.value(pin));
        for (auto it = flat.parameterText.begin(); it != flat.parameterText.end(); ++it) {
            QString value = it.value();
            if (value.contains(QLatin1Char(' ')) || value.isEmpty())
                value = QLatin1Char('"') + value + QLatin1Char('"');
            fields.append(it.key() + QLatin1Char('=') + value);
        }
        text += fields.join(QLatin1Char(' ')) + QLatin1Char('\n');
    }
    for (const QString &message : errors)
        text += QStringLiteral("* error: ") + message + QLatin1Char('\n');
    return text;
}

Netlist buildNetlist(const Project &project, const Document &top)
{
    Netlist netlist;
    netlist.topDocument = top.name();
    Flattener flattener(project, netlist);
    flattener.flatten(top, QString(), {}, {}, {}, {});
    flattener.finish();
    return netlist;
}

} // namespace lvd
