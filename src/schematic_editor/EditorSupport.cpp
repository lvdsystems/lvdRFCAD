#include "schematic_editor/EditorSupport.h"

#include <QJsonArray>

#include <cmath>

namespace lvd {

namespace {

QJsonArray pointJson(QPoint point)
{
    return QJsonArray{point.x(), point.y()};
}

} // namespace

QPoint snapToGrid(QPointF point)
{
    return QPoint(int(std::lround(point.x() / GridStep)) * GridStep, int(std::lround(point.y() / GridStep)) * GridStep);
}

QString itemsMimeType()
{
    return QStringLiteral("application/x-lvdrfcad-items");
}

QString componentMimeType()
{
    return QStringLiteral("application/x-lvdrfcad-component");
}

QJsonObject itemsToJson(const Schematic &schematic, const QStringList &ids, const DefinitionLookup &definitionOf)
{
    QJsonArray components, wires, junctions, labels, texts;
    QJsonObject definitions;
    for (const QString &id : ids) {
        switch (Schematic::kindOf(id)) {
        case ItemKind::Component:
            if (const PlacedComponent *component = schematic.component(id)) {
                components.append(componentToJson(*component));
                // Sheet symbols are generated from the project, never cached.
                if (!component->definition.startsWith(QStringLiteral("sheet:"))
                    && !definitions.contains(component->definition)) {
                    if (const DefinitionPtr definition = definitionOf(component->definition))
                        definitions.insert(component->definition, definition->toJson());
                }
            }
            break;
        case ItemKind::Wire:
            if (schematic.wires().contains(id))
                wires.append(wireToJson(schematic.wires().value(id)));
            break;
        case ItemKind::Junction:
            if (schematic.junctions().contains(id)) {
                const Junction &junction = schematic.junctions()[id];
                junctions.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("at"), pointJson(junction.at)}});
            }
            break;
        case ItemKind::Label:
            if (schematic.labels().contains(id)) {
                const NetLabel &label = schematic.labels()[id];
                labels.append(QJsonObject{{QStringLiteral("id"), id},
                                          {QStringLiteral("net"), label.net},
                                          {QStringLiteral("at"), pointJson(label.at)}});
            }
            break;
        case ItemKind::Text:
            if (schematic.texts().contains(id)) {
                const TextNote &text = schematic.texts()[id];
                texts.append(QJsonObject{{QStringLiteral("id"), id},
                                         {QStringLiteral("text"), text.text},
                                         {QStringLiteral("at"), pointJson(text.at)},
                                         {QStringLiteral("size"), text.size}});
            }
            break;
        case ItemKind::None:
            break;
        }
    }
    return QJsonObject{{QStringLiteral("components"), components}, {QStringLiteral("wires"), wires},
                       {QStringLiteral("junctions"), junctions}, {QStringLiteral("labels"), labels},
                       {QStringLiteral("texts"), texts},         {QStringLiteral("definitions"), definitions}};
}

std::unique_ptr<Schematic> itemsFromJson(const QJsonObject &content)
{
    return Schematic::fromJson(content);
}

QRect anchorBounds(const Schematic &schematic, const QStringList &ids)
{
    const QSet<QString> wanted(ids.begin(), ids.end());
    const auto use = [&](const QString &id) { return wanted.isEmpty() || wanted.contains(id); };
    int left = 0, top = 0, right = 0, bottom = 0;
    bool first = true;
    const auto add = [&](QPoint p) {
        if (first) {
            left = right = p.x();
            top = bottom = p.y();
            first = false;
            return;
        }
        left = std::min(left, p.x());
        right = std::max(right, p.x());
        top = std::min(top, p.y());
        bottom = std::max(bottom, p.y());
    };
    for (const PlacedComponent &component : schematic.components()) {
        if (use(component.id))
            add(component.at);
    }
    for (const Wire &wire : schematic.wires()) {
        if (use(wire.id)) {
            for (const QPoint &point : wire.points)
                add(point);
        }
    }
    for (const Junction &junction : schematic.junctions()) {
        if (use(junction.id))
            add(junction.at);
    }
    for (const NetLabel &label : schematic.labels()) {
        if (use(label.id))
            add(label.at);
    }
    for (const TextNote &text : schematic.texts()) {
        if (use(text.id))
            add(text.at);
    }
    if (first)
        return {};
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

QPoint gridCenter(const QRect &bounds)
{
    // QRect is inclusive; the centre of the end points is wanted.
    return snapToGrid(QPointF((bounds.left() + bounds.right()) / 2.0, (bounds.top() + bounds.bottom()) / 2.0));
}

void fixJunctions(Schematic &schematic, const DefinitionLookup &definitionOf)
{
    const QStringList redundant = redundantJunctions(schematic, definitionOf);
    if (!redundant.isEmpty())
        schematic.removeItems(redundant);
    for (const QPoint &point : missingJunctions(schematic, definitionOf))
        schematic.addJunction(point);
}

QList<QPoint> simplifyPolyline(const QList<QPoint> &points)
{
    QList<QPoint> result;
    for (const QPoint &point : points) {
        if (!result.isEmpty() && result.last() == point)
            continue;
        if (result.size() >= 2) {
            const QPoint a = result.at(result.size() - 2);
            const QPoint b = result.last();
            // b lies between a and point on one straight line: drop it.
            if (segmentContains(a, point, b))
                result.removeLast();
        }
        result.append(point);
    }
    return result;
}

QList<QPoint> orthogonalRoute(QPoint from, QPoint to, bool horizontalFirst)
{
    if (from.x() == to.x() || from.y() == to.y())
        return {to};
    const QPoint corner = horizontalFirst ? QPoint(to.x(), from.y()) : QPoint(from.x(), to.y());
    return {corner, to};
}

} // namespace lvd
