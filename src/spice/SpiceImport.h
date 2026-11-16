#pragma once

#include "core/ComponentDefinition.h"
#include "spice/SpiceAnalysis.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace lvd {
class Document;
class Project;
} // namespace lvd

namespace lvd::spice {

// ---- Model files (requirement SP-7): .lib, .mod, .cir, .subckt

// One model or subcircuit found in a SPICE text.
struct SpiceModelEntry {
    enum class Kind { Model, Subcircuit };
    Kind kind = Kind::Model;
    QString name;    // as written
    QString type;    // models: D, NPN, PNP, NMOS, PMOS, NJF, PJF, VDMOS, SW, ... (upper case); subcircuits: empty
    QStringList pins; // subcircuits: the pin list in order
    QMap<QString, QString> parameters; // subcircuits: "params:" defaults
    QString section; // .lib section the entry is in, if any
    int line = 0;    // first line, 1-based
};

struct SpiceModelFile {
    QList<SpiceModelEntry> entries;
    QStringList sections; // .lib sections
    QStringList errors;   // encrypted content, unbalanced .subckt
    QStringList warnings;
};

// Joins continuation lines ("+") and drops comments ("*" lines, "; ..." and
// "$ ..." tails), keeping the first line number of each logical line.
QList<QPair<int, QString>> logicalSpiceLines(const QString &text);

SpiceModelFile parseSpiceModels(const QString &text);
std::optional<SpiceModelFile> readSpiceModelFile(const QString &path, QString *error = nullptr);

// A component proposed for a model or subcircuit: the symbol of the matching
// device type (from the given template definitions, e.g. the generic NPN of
// the default library) or, for subcircuits, a box with the pins (inputs on
// the left, outputs, supplies and the rest on the right), and a "spice"
// model whose template instantiates the entry and whose "include" names the
// file (with "section" for .lib files that need one).
//
// templates: device type ("D", "NPN", ...) -> a definition whose symbol and
// pins are reused (pin names are mapped by position: a/k, c/b/e, d/g/s).
ComponentDefinition proposeComponent(const SpiceModelEntry &entry, const QString &includePath,
                                     const QMap<QString, ComponentDefinition> &templates = {});

// ---- Netlists (requirement SP-10)

// An existing SPICE netlist imported as a simulation-only document: the
// circuit text without analysis and control cards, and the analyses found,
// as setups.
struct ImportedNetlist {
    QString title;
    QString circuit; // without the title line, analysis cards, .control blocks and .end
    QList<SpiceAnalysisSetup> analyses;
    QStringList nodes;    // node names used, lower case, without ground
    QStringList elements; // element names, as written
    QStringList warnings; // cards that were dropped
};

// Relative .include and .lib file names are made absolute against
// baseDirectory (the folder of the netlist file).
ImportedNetlist importSpiceNetlist(const QString &text, const QString &baseDirectory = {});

// A SPICE number ("10k", "1meg", "2.2u", "1e-3"; M is milli as in SPICE).
std::optional<double> parseSpiceNumber(const QString &text);

// A deck for an imported netlist's circuit (the "netlist" key of a
// document's simulation object), so analyses can be built and resolved as for
// a schematic: nodes map to themselves, elements to themselves.
SpiceDeck deckFromNetlist(const QString &circuit, const QString &title);

// Reads a netlist file into a new SPICE document of the project (name:
// the file's base name when empty): an empty drawing, the circuit in the
// simulation object's "netlist" and one analysis per analysis card.
Document *importNetlistDocument(Project &project, const QString &path, const QString &name = {}, QString *error = nullptr,
                               QStringList *warnings = nullptr);

} // namespace lvd::spice
