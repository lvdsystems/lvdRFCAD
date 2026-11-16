#pragma once

#include "core/Netlist.h"

#include <QJsonObject>
#include <QMap>
#include <QVariant>

#include <functional>
#include <optional>
#include <QString>
#include <QStringList>

namespace lvd {
class Document;
class Project;
} // namespace lvd

namespace lvd::spice {

// What a probe part measures (model field "probe"); probes add no circuit
// element except the current probe's 0 V source.
enum class ProbeKind { None, Voltage, Differential, Current };

// One schematic component in the deck.
struct SpiceElement {
    QString path;        // flat hierarchical reference, "X1/R1"
    QString document;    // sheet holding the component
    QString componentId; // id inside that sheet
    QString name;        // first element's SPICE name, lower case ("rr1"); empty for probes without element
    QStringList elementNames; // every element line's name, lower case
    QMap<QString, QString> pinNodes; // pin -> SPICE node
    ProbeKind probe = ProbeKind::None;
    // Virtual instrument (requirement SP-5): oscilloscope, voltmeter, ammeter,
    // function-generator, spectrum-analyzer, bode-plotter; empty otherwise.
    QString instrument;
};

struct SpiceNetlistOptions {
    QString title;
    QStringList cards;          // analysis and control cards placed before ".end"
    // Saves node voltages and every primitive element's current and power
    // ("@rr1[i]", "@rr1[p]", ...) for the annotated schematic (SP-6).
    bool saveDeviceCurrents = false;
};

// A SPICE netlist generated from a schematic (requirement SP-10), with the
// maps needed to show results on the schematic.
struct SpiceDeck {
    QString text;
    QList<SpiceElement> elements;
    QMap<QString, QString> nodeOfNet; // flat net name -> SPICE node ("0" for ground)
    QMap<QString, QString> netOfNode; // SPICE node -> flat net name
    QStringList errors;
    QStringList warnings;

    bool ok() const { return errors.isEmpty(); }
    const SpiceElement *element(const QString &path) const;            // case-insensitive
    const SpiceElement *elementByName(const QString &spiceName) const; // case-insensitive
    // SPICE node of a net given as typed by a user: a net label ("out") or a
    // flat net name ("X1/n3"), case-insensitive. Empty when unknown.
    QString node(const QString &net) const;
};

// Python-defined models (requirement SP-8): a component whose "python"
// model (docs/file-format.md, section 4.4) defines a function
// spice(ref, nodes, params) returning netlist lines. The netlister knows no
// Python; the application registers a handler that runs the code.
struct PythonModelRequest {
    QJsonObject model;                  // the "python" model object ("source" or "file")
    QString reference;                  // the {ref} name, e.g. "U1"
    QMap<QString, QString> nodes;       // pin -> SPICE node
    QMap<QString, QVariant> parameters; // expression parameters as double, others as text
    QString projectDirectory;
};
using PythonModelHandler = std::function<std::optional<QString>(const PythonModelRequest &request, QString *error)>;
void setPythonModelHandler(PythonModelHandler handler);
bool hasPythonModelHandler();

// Number in SPICE syntax: plain decimal or exponent form, never SI suffixes
// (SPICE reads "1M" as milli).
QString formatSpiceNumber(double value);

// Flattens the schematic and renders every component's "spice" model
// (docs/file-format.md, section 4.4).
SpiceDeck buildSpiceDeck(const Project &project, const Document &top, const SpiceNetlistOptions &options = {});
SpiceDeck buildSpiceDeck(const Netlist &netlist, const QString &projectDirectory, const SpiceNetlistOptions &options = {});

} // namespace lvd::spice
