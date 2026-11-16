#include "core/Connectivity.h"

#include <QMap>
#include <QSet>

#include <algorithm>
#include <numeric>

namespace lvd {

namespace {

class UnionFind
{
public:
    int add()
    {
        m_parent.push_back(static_cast<int>(m_parent.size()));
        return m_parent.back();
    }
    int find(int x)
    {
        while (m_parent[static_cast<size_t>(x)] != x) {
            m_parent[static_cast<size_t>(x)] = m_parent[static_cast<size_t>(m_parent[static_cast<size_t>(x)])];
            x = m_parent[static_cast<size_t>(x)];
        }
        return x;
    }
    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            m_parent[static_cast<size_t>(std::max(a, b))] = std::min(a, b);
    }

private:
    std::vector<int> m_parent;
};

struct Node {
    enum class Kind { Wire, Pin, Junction, Label };
    Kind kind;
    QString itemId;     // wire, junction, label or component id
    QString pin;        // for pins
    QPoint at;          // pins, junctions and labels
};

// A connection point and what sits there, used for junction detection.
struct PointUse {
    int wireEnds = 0;
    int pins = 0;
    int wiresThrough = 0; // wires passing through the point between their ends
    QStringList junctions;
};

QHash<QPoint, PointUse> pointUses(const Schematic &schematic, const DefinitionLookup &definitionOf)
{
    QHash<QPoint, PointUse> uses;
    for (const Wire &wire : schematic.wires()) {
        ++uses[wire.points.first()].wireEnds;
        ++uses[wire.points.last()].wireEnds;
    }
    for (const PlacedComponent &component : schematic.components()) {
        if (const DefinitionPtr definition = definitionOf(component.definition)) {
            for (const QPoint &point : pinPositions(component, *definition))
                ++uses[point].pins;
        }
    }
    for (const Junction &junction : schematic.junctions())
        uses[junction.at].junctions.append(junction.id);
    for (auto it = uses.begin(); it != uses.end(); ++it) {
        for (const Wire &wire : schematic.wires()) {
            if (wire.points.first() != it.key() && wire.points.last() != it.key() && wireContains(wire, it.key()))
                ++it->wiresThrough;
        }
    }
    return uses;
}

int connectionCount(const PointUse &use)
{
    return use.wireEnds + use.pins + 2 * use.wiresThrough;
}

} // namespace

int Connectivity::netOfPin(const QString &componentId, const QString &pin) const
{
    return pinIndex.value(componentId + QLatin1Char('/') + pin, -1);
}

int Connectivity::netOfItem(const QString &itemId) const
{
    return itemIndex.value(itemId, -1);
}

const SheetNet *Connectivity::net(const QString &name) const
{
    for (const SheetNet &candidate : nets) {
        if (candidate.name == name)
            return &candidate;
    }
    return nullptr;
}

QPoint pinPosition(const PlacedComponent &component, const SymbolPin &pin)
{
    return component.at + component.orientation().map(pin.at);
}

QList<QPoint> pinPositions(const PlacedComponent &component, const ComponentDefinition &definition)
{
    QList<QPoint> positions;
    for (const SymbolPin &pin : definition.symbol.pins)
        positions.append(pinPosition(component, pin));
    return positions;
}

QString parameterText(const PlacedComponent &component, const ComponentDefinition &definition, const QString &name)
{
    const auto it = component.parameters.constFind(name);
    if (it != component.parameters.cend())
        return it.value();
    const ParameterDefinition *parameter = definition.parameter(name);
    return parameter ? parameter->defaultValue : QString();
}

bool segmentContains(QPoint a, QPoint b, QPoint p)
{
    const qint64 cross = qint64(b.x() - a.x()) * (p.y() - a.y()) - qint64(b.y() - a.y()) * (p.x() - a.x());
    if (cross != 0)
        return false;
    return p.x() >= std::min(a.x(), b.x()) && p.x() <= std::max(a.x(), b.x()) && p.y() >= std::min(a.y(), b.y())
           && p.y() <= std::max(a.y(), b.y());
}

bool wireContains(const Wire &wire, QPoint p)
{
    for (qsizetype i = 1; i < wire.points.size(); ++i) {
        if (segmentContains(wire.points.at(i - 1), wire.points.at(i), p))
            return true;
    }
    return false;
}

Connectivity computeConnectivity(const Schematic &schematic, const DefinitionLookup &definitionOf)
{
    Connectivity result;
    std::vector<Node> nodes;
    UnionFind sets;
    QHash<QPoint, QList<int>> atPoint;       // connection points -> nodes there
    QList<std::pair<int, const Wire *>> wireNodes;
    QHash<QString, int> labelByName, globalByName;
    QHash<int, QString> globalOfNode, portOfNode;

    const auto addNode = [&](Node node) {
        nodes.push_back(std::move(node));
        return sets.add();
    };

    for (const Wire &wire : schematic.wires()) {
        const int node = addNode(Node{Node::Kind::Wire, wire.id, {}, {}});
        wireNodes.append({node, &wire});
        atPoint[wire.points.first()].append(node);
        atPoint[wire.points.last()].append(node);
    }
    for (const PlacedComponent &component : schematic.components()) {
        const DefinitionPtr definition = definitionOf(component.definition);
        if (!definition) {
            result.unresolvedComponents.append(component.id);
            continue;
        }
        for (const SymbolPin &pin : definition->symbol.pins) {
            const QPoint at = pinPosition(component, pin);
            const int node = addNode(Node{Node::Kind::Pin, component.id, pin.pin, at});
            atPoint[at].append(node);
            if (!definition->netName.isEmpty()) {
                globalOfNode.insert(node, definition->netName);
                const auto existing = globalByName.constFind(definition->netName);
                if (existing != globalByName.cend())
                    sets.unite(node, *existing);
                else
                    globalByName.insert(definition->netName, node);
            }
            if (definition->isSheetPort()) {
                const QString name = parameterText(component, *definition, QStringLiteral("name")).trimmed();
                if (!name.isEmpty())
                    portOfNode.insert(node, name);
            }
        }
    }
    for (const Junction &junction : schematic.junctions())
        atPoint[junction.at].append(addNode(Node{Node::Kind::Junction, junction.id, {}, junction.at}));
    for (const NetLabel &label : schematic.labels()) {
        const int node = addNode(Node{Node::Kind::Label, label.id, {}, label.at});
        atPoint[label.at].append(node);
        const auto existing = labelByName.constFind(label.net);
        if (existing != labelByName.cend())
            sets.unite(node, *existing);
        else
            labelByName.insert(label.net, node);
    }

    // Everything at one point connects, and so does every wire passing through it.
    for (auto it = atPoint.begin(); it != atPoint.end(); ++it) {
        const QList<int> &here = it.value();
        for (qsizetype i = 1; i < here.size(); ++i)
            sets.unite(here.first(), here.at(i));
        for (const auto &[node, wire] : wireNodes) {
            if (wireContains(*wire, it.key()))
                sets.unite(here.first(), node);
        }
    }

    // Collect nets.
    QMap<int, SheetNet> byRoot;
    QHash<QString, QString> referenceOf;
    for (const PlacedComponent &component : schematic.components())
        referenceOf.insert(component.id, component.reference);
    QHash<int, QStringList> itemsOfRoot;
    for (int index = 0; index < static_cast<int>(nodes.size()); ++index) {
        const Node &node = nodes[static_cast<size_t>(index)];
        const int root = sets.find(index);
        SheetNet &net = byRoot[root];
        switch (node.kind) {
        case Node::Kind::Wire: net.wireIds.append(node.itemId); break;
        case Node::Kind::Junction: net.junctionIds.append(node.itemId); break;
        case Node::Kind::Label: {
            net.labelIds.append(node.itemId);
            const QString name = schematic.labels().value(node.itemId).net;
            if (!net.labelNames.contains(name))
                net.labelNames.append(name);
            break;
        }
        case Node::Kind::Pin:
            net.pins.append(NetPin{node.itemId, node.pin, node.at});
            break;
        }
        if (globalOfNode.contains(index) && net.globalName.isEmpty())
            net.globalName = globalOfNode.value(index);
        if (portOfNode.contains(index) && !net.portNames.contains(portOfNode.value(index)))
            net.portNames.append(portOfNode.value(index));
        if (node.kind != Node::Kind::Pin)
            itemsOfRoot[root].append(node.itemId);
    }

    for (auto it = byRoot.begin(); it != byRoot.end(); ++it) {
        SheetNet &net = it.value();
        net.labelNames.sort();
        net.portNames.sort();
        std::sort(net.pins.begin(), net.pins.end(), [&referenceOf](const NetPin &a, const NetPin &b) {
            const QString ra = referenceOf.value(a.componentId), rb = referenceOf.value(b.componentId);
            return ra != rb ? ra < rb : a.pin < b.pin;
        });
        if (!net.globalName.isEmpty()) {
            net.name = net.globalName;
            net.nameSource = SheetNet::NameSource::Global;
        } else if (!net.labelNames.isEmpty()) {
            net.name = net.labelNames.first();
            net.nameSource = SheetNet::NameSource::Label;
        } else if (!net.portNames.isEmpty()) {
            net.name = net.portNames.first();
            net.nameSource = SheetNet::NameSource::Port;
        } else if (!net.pins.isEmpty()) {
            QStringList candidates;
            for (const NetPin &pin : net.pins)
                candidates.append(referenceOf.value(pin.componentId) + QLatin1Char('-') + pin.pin);
            candidates.sort();
            net.name = QStringLiteral("Net-(%1)").arg(candidates.first());
            net.nameSource = SheetNet::NameSource::Pin;
        } else {
            QStringList items = itemsOfRoot.value(it.key());
            items.sort();
            net.name = QStringLiteral("Net-(%1)").arg(items.value(0));
            net.nameSource = SheetNet::NameSource::Item;
        }
    }

    result.nets = byRoot.values();
    std::sort(result.nets.begin(), result.nets.end(),
              [](const SheetNet &a, const SheetNet &b) { return a.name < b.name; });
    for (int index = 0; index < static_cast<int>(result.nets.size()); ++index) {
        const SheetNet &net = result.nets.at(index);
        for (const NetPin &pin : net.pins)
            result.pinIndex.insert(pin.componentId + QLatin1Char('/') + pin.pin, index);
        for (const QString &id : net.wireIds + net.junctionIds + net.labelIds)
            result.itemIndex.insert(id, index);
    }
    return result;
}

QList<QPoint> missingJunctions(const Schematic &schematic, const DefinitionLookup &definitionOf)
{
    QList<QPoint> result;
    const auto uses = pointUses(schematic, definitionOf);
    for (auto it = uses.begin(); it != uses.end(); ++it) {
        if (it->junctions.isEmpty() && connectionCount(*it) >= 3)
            result.append(it.key());
    }
    return result;
}

QStringList redundantJunctions(const Schematic &schematic, const DefinitionLookup &definitionOf)
{
    QStringList result;
    const auto uses = pointUses(schematic, definitionOf);
    for (auto it = uses.begin(); it != uses.end(); ++it) {
        if (it->junctions.isEmpty())
            continue;
        // Every junction beyond the first at one point is redundant too.
        const QStringList extra = it->junctions.mid(connectionCount(*it) >= 3 ? 1 : 0);
        result.append(extra);
    }
    result.sort();
    return result;
}

} // namespace lvd
