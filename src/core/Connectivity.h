#pragma once

#include "core/Schematic.h"

#include <QHash>
#include <QList>
#include <QPoint>
#include <QString>
#include <QStringList>

#include <functional>

namespace lvd {

// Nets of one sheet, derived from geometry: docs/file-format.md, section 3.3.

using DefinitionLookup = std::function<DefinitionPtr(const QString &key)>;

struct NetPin {
    QString componentId;
    QString pin;
    QPoint at;

    bool operator==(const NetPin &) const = default;
};

struct SheetNet {
    enum class NameSource { Global, Label, Port, Pin, Item };

    QString name;
    NameSource nameSource = NameSource::Item;
    QString globalName;      // netName of a component on the net, if any
    QStringList labelNames;  // distinct label names, sorted
    QStringList portNames;   // names of sheet ports on the net, sorted
    QList<NetPin> pins;      // sorted by reference, then pin
    QStringList wireIds;
    QStringList junctionIds;
    QStringList labelIds;
};

struct Connectivity {
    QList<SheetNet> nets; // sorted by name

    // Index into nets, or -1.
    int netOfPin(const QString &componentId, const QString &pin) const;
    int netOfItem(const QString &itemId) const; // wires, junctions and labels
    const SheetNet *net(const QString &name) const;

    // Components whose definition could not be resolved; their pins are absent.
    QStringList unresolvedComponents;

    QHash<QString, int> pinIndex;  // "componentId/pin" -> net
    QHash<QString, int> itemIndex; // item id -> net
};

Connectivity computeConnectivity(const Schematic &schematic, const DefinitionLookup &definitionOf);

// Connection point of a pin of a placed component, in sheet coordinates.
QPoint pinPosition(const PlacedComponent &component, const SymbolPin &pin);
// Connection points of all pins, in symbol pin order.
QList<QPoint> pinPositions(const PlacedComponent &component, const ComponentDefinition &definition);

// The text of a parameter on a placed component: its own value or the default.
QString parameterText(const PlacedComponent &component, const ComponentDefinition &definition, const QString &name);

// True if p lies on the segment from a to b, end points included.
bool segmentContains(QPoint a, QPoint b, QPoint p);
// True if p lies on any segment of the wire.
bool wireContains(const Wire &wire, QPoint p);

// Points where three or more connections meet (wire ends, pins, or a wire end
// or pin in the middle of another wire) and no junction exists yet.
QList<QPoint> missingJunctions(const Schematic &schematic, const DefinitionLookup &definitionOf);
// Junctions where fewer than three connections meet.
QStringList redundantJunctions(const Schematic &schematic, const DefinitionLookup &definitionOf);

} // namespace lvd
