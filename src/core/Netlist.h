#pragma once

#include "core/Library.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace lvd {

class Document;
class Project;

// A component of the flattened design. Sheet instances, sheet ports and
// global-net symbols (ground) are resolved into connectivity and do not
// appear as components.
struct FlatComponent {
    QString path;          // hierarchical reference, such as "X1/A1"
    QString instancePath;  // "X1/", empty at the top level
    QString document;      // name of the sheet document holding the component
    QString componentId;   // id inside that sheet
    QString definitionKey;
    DefinitionPtr definition;
    QMap<QString, QString> pinNets;       // pin -> flat net name
    QMap<QString, QString> parameterText; // every parameter, own value or default
    QMap<QString, double> parameterValues; // successfully evaluated expression parameters
};

struct Netlist {
    QString topDocument;
    QList<FlatComponent> components; // sorted by path
    QStringList nets;                // nets with at least one pin, sorted
    QStringList errors;
    QStringList warnings;

    const FlatComponent *component(const QString &path) const;
    // "path.pin" entries on a net, sorted.
    QStringList pinsOn(const QString &net) const;
    // Human-readable and deterministic, for tests and debugging.
    QString toText() const;
};

// Flattens the hierarchy below a schematic document and evaluates all
// parameters. Problems are collected in errors and warnings; whatever can be
// flattened still is.
Netlist buildNetlist(const Project &project, const Document &top);

} // namespace lvd
