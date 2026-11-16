#pragma once

#include "rfsim/Optimizer.h"

#include <QList>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

class QMenu;

namespace lvd {
class Document;
class Project;
class Schematic;
} // namespace lvd

namespace lvd::sim {

// Values the tuner and the optimiser may change (requirements RF-11, RF-12):
// the variables of the analysed sheet and the numeric expression parameters
// of its components, with their current values.
struct DesignCandidate {
    rf::DesignVariable variable; // with the default range around the value
    double value = 0;
    QString title;               // "C1.c", "f0"
    QString description;         // "c = 6.37 pF" or "f0 = 2.4 GHz"
};

// Candidates of a schematic document: its variables first, then the
// parameters of its components by reference. Parameters that do not
// evaluate to a finite number are left out.
QList<DesignCandidate> designCandidates(const Project &project, const Document &document);

// The default range of a value: value/2 .. value*2 on a logarithmic scale
// for positive values, value*2 .. value/2 (linear) for negative ones, and
// 0 .. 1 (linear) for zero.
void setDefaultRange(rf::DesignVariable &variable, double value);

// True when both refer to the same variable or parameter.
bool sameDesignVariable(const rf::DesignVariable &a, const rf::DesignVariable &b);

// The schematics rf::applyDesignValues changes for these variables.
QList<Schematic *> touchedSchematics(Project &project, Document &document, const QList<rf::DesignVariable> &variables);

// Writes values into the real project with rf::applyDesignValues, inside one
// Schematic edit on every touched schematic: one undo step per schematic
// (each schematic has its own undo history).
bool applyDesignValuesUndoable(Project &project, Document &document, const QList<rf::DesignVariable> &variables,
                               const std::vector<double> &values, const QString &description, QString *error = nullptr);

// Fills `menu` with the candidates of a document: a "Variables" section and
// one submenu per component. Entries already in `existing` are disabled.
// Choosing an entry calls `chosen` with the variable (default range set).
void fillDesignVariableMenu(QMenu *menu, const Project *project, const Document *document,
                            const QList<rf::DesignVariable> &existing,
                            const std::function<void(const rf::DesignVariable &)> &chosen);

// A number with optional SI prefix and unit ("4.7pF", "1e-9"), or a constant
// expression ("2*2.2p"); nullopt otherwise.
std::optional<double> parseValue(const QString &text);
// Engineering notation with the unit: "4.7 pF".
QString formatValue(double value, const QString &unit, int significantDigits = 4);

// Display name of a series choice: "E24", or "None" for no series.
QString seriesLabel(const std::optional<ESeries> &series);
// The series offered in the user interface: E12, E24, E48, E96.
std::vector<ESeries> offeredSeries();

} // namespace lvd::sim
