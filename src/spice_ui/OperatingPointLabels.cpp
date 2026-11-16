#include "spice_ui/OperatingPointLabels.h"

#include "core/Connectivity.h"
#include "core/Document.h"
#include "core/Expression.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QHash>
#include <QSet>

#include <cmath>

namespace lvd::spiceui {

namespace {

std::optional<double> valueOf(const spice::SpicePlot &plot, const QString &name)
{
    const spice::SpiceVector *v = plot.vector(name);
    if (!v || v->size() == 0)
        return std::nullopt;
    return v->at(0).real();
}

// The middle of the longest segment of the net's wires, or a pin.
std::optional<QPointF> netAnchor(const Schematic &schematic, const SheetNet &net)
{
    double best = -1;
    QPointF anchor;
    for (const QString &wireId : net.wireIds) {
        const auto found = schematic.wires().constFind(wireId);
        if (found == schematic.wires().cend())
            continue;
        const Wire *wire = &found.value();
        for (qsizetype i = 0; i + 1 < wire->points.size(); ++i) {
            const QPoint a = wire->points.at(i), b = wire->points.at(i + 1);
            const double length = std::hypot(double(b.x() - a.x()), double(b.y() - a.y()));
            if (length > best) {
                best = length;
                anchor = (QPointF(a) + QPointF(b)) / 2.0;
            }
        }
    }
    if (best >= 0)
        return anchor;
    if (!net.pins.isEmpty())
        return QPointF(net.pins.first().at);
    return std::nullopt;
}

} // namespace

QList<OperatingPointLabel> operatingPointLabels(const Project &project, const Document &document,
                                                const spice::SpiceSimulation &simulation)
{
    QList<OperatingPointLabel> labels;
    const Schematic *schematic = document.schematic();
    const spice::SpicePlot *op = simulation.run.plot(QStringLiteral("op"));
    if (!schematic || !op)
        return labels;

    const DefinitionLookup lookup = [&](const QString &key) { return project.resolve(*schematic, key).definition; };
    const Connectivity connectivity = computeConnectivity(*schematic, lookup);

    // Elements of this sheet, by component id.
    QHash<QString, const spice::SpiceElement *> elementOf;
    for (const spice::SpiceElement &element : simulation.deck.elements)
        if (element.document == document.name() && !element.path.contains(QLatin1Char('/')))
            elementOf.insert(element.componentId, &element);

    // Voltages.
    for (const SheetNet &net : connectivity.nets) {
        QString node;
        for (const NetPin &pin : net.pins) {
            if (const spice::SpiceElement *element = elementOf.value(pin.componentId)) {
                node = element->pinNodes.value(pin.pin);
                if (!node.isEmpty())
                    break;
            }
        }
        if (node.isEmpty() || node == QLatin1String("0"))
            continue;
        const auto voltage = valueOf(*op, QStringLiteral("v(%1)").arg(node));
        const auto anchor = netAnchor(*schematic, net);
        if (!voltage || !anchor)
            continue;
        OperatingPointLabel label;
        label.at = *anchor;
        label.text = formatEngineering(*voltage, QStringLiteral("V"));
        label.kind = OperatingPointLabel::Kind::Voltage;
        label.net = simulation.deck.netOfNode.value(node);
        labels << label;
    }

    // Currents and powers of primitive parts.
    for (const PlacedComponent &component : schematic->components()) {
        const spice::SpiceElement *element = elementOf.value(component.id);
        if (!element || element->name.isEmpty() || element->elementNames.size() != 1)
            continue;
        const QString name = element->name;
        QStringList lines;
        // Values below 1 pA or 1 pW are rounding noise (a resistor to an
        // unloaded output); they only clutter the drawing.
        const auto add = [&](const QString &label, const QString &vector, const QString &unit) {
            if (const auto value = valueOf(*op, vector); value && std::abs(*value) >= 1e-12)
                lines << QStringLiteral("%1 %2").arg(label, formatEngineering(*value, unit));
        };
        switch (name.at(0).unicode()) {
        case 'r':
        case 'l':
        case 'b':
            add(QStringLiteral("I"), QStringLiteral("@%1[i]").arg(name), QStringLiteral("A"));
            break;
        case 'v':
        case 'e':
        case 'h':
            add(QStringLiteral("I"), QStringLiteral("i(%1)").arg(name), QStringLiteral("A"));
            break;
        case 'i':
            break; // the current is the source's value
        case 'd':
        case 'm':
        case 'j':
            add(QStringLiteral("I"), QStringLiteral("@%1[id]").arg(name), QStringLiteral("A"));
            break;
        case 'q':
            add(QStringLiteral("Ic"), QStringLiteral("@%1[ic]").arg(name), QStringLiteral("A"));
            add(QStringLiteral("Ib"), QStringLiteral("@%1[ib]").arg(name), QStringLiteral("A"));
            break;
        default:
            break;
        }
        add(QStringLiteral("P"), QStringLiteral("@%1[p]").arg(name), QStringLiteral("W"));
        if (lines.isEmpty())
            continue;
        OperatingPointLabel label;
        // Below and right of the part's origin, clear of its pins on the axis.
        label.at = QPointF(component.at) + QPointF(14, 22);
        label.text = lines.join(QLatin1Char('\n'));
        label.kind = OperatingPointLabel::Kind::Current;
        label.componentId = component.id;
        labels << label;
    }
    return labels;
}

} // namespace lvd::spiceui
