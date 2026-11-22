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

// Substrate parts (rf-circuit:substrate, see core/Substrates.h) are not
// components of the flattened design: they are collected in `substrates`
// (with "er" and "tand" already taken from the material catalog), and every
// component with a non-empty text parameter "substrate" gets the named
// substrate's values in place of its own parameters h, t, er, tand, sigma
// and rough (and b, the stripline ground spacing). The name is looked up in
// the component's own sheet instance first, then in each enclosing sheet up
// to the top level.
struct Netlist {
    QString topDocument;
    QList<FlatComponent> components; // sorted by path
    QList<FlatComponent> substrates; // Substrate parts, sorted by path
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
