#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

namespace lvd {
struct Netlist;
}

namespace lvd::rf {
struct LinearResult;
}

namespace lvd::sim {

// A plot definition in a schematic's "simulation" object
// (docs/file-format.md, section 5.1). Keys this module does not know are kept
// in `extra` and written back unchanged.
//
// Extension used by the simulation view: an optional "labels" object maps a
// trace expression to the name shown in the legend.
struct PlotDefinition {
    QString id;
    QString name;
    QString kind = QStringLiteral("rectangular"); // rectangular, smith or polar
    QString analysis;
    QStringList traces;          // measurement expressions
    QMap<QString, QString> labels; // expression -> legend name (optional)
    QJsonObject extra;

    QString label(const QString &expression) const;
    static PlotDefinition fromJson(const QJsonObject &object);
    QJsonObject toJson() const;
    bool operator==(const PlotDefinition &) const = default;
};

// Plots of a simulation object, in order; all of them or those of one analysis.
QList<PlotDefinition> plotDefinitions(const QJsonObject &simulation, const QString &analysisId = {});
// Adds or replaces (by id) a plot; assigns an id ("p1", "p2", ...) if it has none.
QJsonObject withPlot(const QJsonObject &simulation, PlotDefinition plot, QString *assignedId = nullptr);
QJsonObject withoutPlot(const QJsonObject &simulation, const QString &plotId);
// Removes an analysis and the plots bound to it.
QJsonObject withoutAnalysis(const QJsonObject &simulation, const QString &analysisId);

// Port numbers of the simulation ports (rf-system:port) of a netlist, ascending.
std::vector<int> netlistPortNumbers(const Netlist &netlist);

// Measurement name of an S-parameter: "S21", or "S(12,1)" when a port number
// has more than one digit.
QString sParameterName(int to, int from);

// Default plots of an analysis: a rectangular plot of dB of all S-parameters
// (up to 4 ports) and, for two-ports with noise, NF; and a Smith chart of S11
// (and S22 for two-ports). Without ports, a two-port is assumed.
QList<PlotDefinition> defaultPlots(const QString &analysisId, const std::vector<int> &ports, bool noise);

// Completion candidates for trace expressions, built from the port numbers.
QStringList measurementCompletions(const std::vector<int> &ports, bool noise);

// True for noise measurements (NF, NFmin, Rn, ...), drawn on the right axis
// of a rectangular plot next to gains.
bool isNoiseMeasurement(const QString &expression);

} // namespace lvd::sim
