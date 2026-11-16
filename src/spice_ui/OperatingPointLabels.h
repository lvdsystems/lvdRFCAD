#pragma once

#include "spice/SpiceAnalysis.h"

#include <QList>
#include <QPointF>
#include <QString>

namespace lvd {
class Document;
class Project;
} // namespace lvd

namespace lvd::spiceui {

// A value of an operating point placed on the schematic (requirement SP-6).
struct OperatingPointLabel {
    enum class Kind { Voltage, Current, Power };
    QPointF at; // sheet coordinates of the shown document
    QString text;
    Kind kind = Kind::Voltage;
    QString net;         // voltages: the flat net
    QString componentId; // currents and powers: the part
};

// Labels for the document's own sheet: one node voltage per net (ground
// left out), drawn at the middle of the net's longest wire segment, and the
// current and power of every primitive part, next to the part.
QList<OperatingPointLabel> operatingPointLabels(const Project &project, const Document &document,
                                                const spice::SpiceSimulation &simulation);

} // namespace lvd::spiceui
