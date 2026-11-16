#pragma once

#include "core/Netlist.h"
#include "rfdata/RfTypes.h"

#include <QDateTime>
#include <QHash>
#include <QStringList>

namespace lvd::rf {

// Linear models of the components in docs/libraries.md. Every element is an
// N-port referenced to ground (the global net 0), with S-matrices referred to
// the solver's internal reference impedance (SolverZ0) on every port.

constexpr double SolverZ0 = 50.0;

// Touchstone files loaded during a simulation, reused while unchanged on disk.
class TouchstoneCache
{
public:
    // Returns the file's network or nullopt with *error set.
    std::optional<NetworkData> load(const QString &path, QString *error, QStringList *warnings);

private:
    struct Entry {
        QDateTime modified;
        qint64 size = 0;
        NetworkData network;
        QStringList warnings;
    };
    QHash<QString, Entry> m_entries;
};

struct ElementContext {
    QString projectDirectory; // relative file parameters resolve against it
    TouchstoneCache *touchstones = nullptr;
    bool noise = true;        // compute noise correlation matrices
};

struct Element {
    QString path;   // hierarchical reference, for messages
    int ports = 0;

    // Simulation ports (rf-system:port) are not elements of the circuit:
    // they mark where the external waves enter.
    bool isPort = false;
    int portNumber = 0;
    double portZ0 = SolverZ0;

    std::vector<CMatrix> s;     // per frequency, ports x ports, reference SolverZ0
    std::vector<CMatrix> noise; // per frequency, kelvin; empty when noiseless
};

// Builds the linear model of a flattened component over the frequencies.
// Returns nullopt with *error set when the component cannot be modelled;
// approximations (such as a mixer's conversion path) go to *warnings.
std::optional<Element> buildElement(const FlatComponent &component, const std::vector<double> &frequencies,
                                    const ElementContext &context, QStringList *warnings, QString *error);

// Normalised low-pass prototype transfer function H(s), with s = j Omega.
enum class FilterResponse { Ideal, Butterworth, Chebyshev };
Complex prototypeResponse(FilterResponse response, int order, double rippleDb, double omega);

// Noise wave correlation (kelvin) of a passive network at temperature T: T (I - S S^H).
CMatrix passiveNoise(const CMatrix &s, double temperature);

} // namespace lvd::rf
