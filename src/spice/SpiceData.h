#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <complex>
#include <vector>

namespace lvd::spice {

// Quantity of a result vector, independent of the engine.
enum class VectorKind {
    Other,
    Time,
    Frequency,
    Voltage,
    Current,
    VoltageDensity,  // V/sqrt(Hz)
    CurrentDensity,  // A/sqrt(Hz)
    Pole,
    Zero,
    Temperature,
    Impedance,
    Admittance,
    Power,
};

QString vectorKindUnit(VectorKind kind); // "V", "A", "s", "Hz", "V/√Hz", ... or ""

// The engine-independent name of a vector as ngspice (or a raw file) calls
// it, for a plot of the given type ("tran", "tf", ...): lower case; node
// voltages "v(node)"; source and XSPICE currents "i(name)"; the sweep scale
// "v-sweep"; transfer-function results "transfer_function",
// "input_impedance", "output_impedance"; noise and pole-zero results bare.
QString normalisedVectorName(const QString &name, VectorKind kind, const QString &plotType);

// One result vector, such as "v(out)" or "time". Names are lower case as
// SPICE reports them; node voltages are always given as "v(<node>)" and
// source currents as "i(<source>)", whatever the engine calls them.
struct SpiceVector {
    QString name;
    VectorKind kind = VectorKind::Other;
    bool isComplex = false;
    std::vector<double> real;                 // when !isComplex
    std::vector<std::complex<double>> complex; // when isComplex

    std::size_t size() const { return isComplex ? complex.size() : real.size(); }
    std::complex<double> at(std::size_t i) const { return isComplex ? complex[i] : std::complex<double>(real[i], 0.0); }
};

// The vectors of one analysis run ("op", "tran", "ac", ...). The first vector
// is the scale (time, frequency, swept source) when the analysis has one.
struct SpicePlot {
    QString name;  // engine plot name, such as "tran1"
    QString type;  // analysis: op, dc, ac, tran, noise, tf, pz, sens, ...
    QString title;
    QString scaleName; // empty when the plot has no scale (op, tf)
    std::vector<SpiceVector> vectors;

    const SpiceVector *vector(const QString &name) const; // case-insensitive
    const SpiceVector *scale() const;
    QStringList vectorNames() const;
};

struct SpiceRunResult {
    bool ok = false;
    bool cancelled = false;
    QList<SpicePlot> plots; // in the order the engine created them
    QStringList errors;
    QStringList warnings;
    QStringList log; // everything the engine printed
    double seconds = 0;

    const SpicePlot *plot(const QString &type) const; // first plot of that analysis type
};

} // namespace lvd::spice
