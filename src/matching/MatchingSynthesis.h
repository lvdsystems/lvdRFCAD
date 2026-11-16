#pragma once

#include "core/StandardValues.h"

#include <QString>
#include <QStringList>

#include <complex>
#include <optional>
#include <vector>

namespace lvd::rf {

// Impedance-matching network synthesis (requirements RF-10 and RF-13).
//
// A network is a ladder of ideal lossless elements between a source of
// impedance Zs (left) and a load ZL (right). It is a conjugate match at the
// design frequency f0 when the impedance looking from the source into the
// network, with the load attached, equals Zs*.

using MatchComplex = std::complex<double>;

enum class MatchPlacement { Series, Shunt };

enum class MatchElementKind {
    Capacitor, // value in F
    Inductor,  // value in H
    Line,      // series transmission line: z0 (ohm), electrical length (deg at f0)
    OpenStub,  // shunt open-circuited stub: z0, electrical length
    ShortStub, // shunt short-circuited stub: z0, electrical length
};

struct MatchElement {
    MatchPlacement placement = MatchPlacement::Series;
    MatchElementKind kind = MatchElementKind::Capacitor;
    double value = 0;      // F or H (lumped elements), after snapping
    double idealValue = 0; // F or H before snapping (equals value when not snapped)
    double z0 = 50;        // ohm (lines and stubs)
    double lengthDeg = 0;  // electrical length at f0, degrees (lines and stubs)

    bool isLumped() const { return kind == MatchElementKind::Capacitor || kind == MatchElementKind::Inductor; }
    bool isDistributed() const { return !isLumped(); }
};

enum class MatchTopology { LNetwork, PiNetwork, TNetwork, MultiSection, QuarterWave, SingleStub, DoubleStub };

QString matchTopologyName(MatchTopology topology);

struct MatchSolution {
    MatchTopology topology = MatchTopology::LNetwork;
    QString description;               // "L-network: series L, shunt C at load"
    std::vector<MatchElement> elements; // from source (index 0) to load
    MatchComplex zs;
    MatchComplex zl;
    double frequency = 0; // f0, Hz

    double loadedQ = 0;           // largest nodal Q: |X|/R between elements, looking toward the load
    double bandwidthEstimate = 0; // f0 / loadedQ, Hz (infinity for Q = 0)
    double bandwidth3dB = 0;      // computed band around f0 with |Gamma|^2 <= 1/2, Hz; NaN if not bounded within (0, 3 f0)

    bool snapped = false;                         // lumped values snapped to a series
    std::optional<ESeries> series;                // the series, when snapped
    double mismatch = 0;                          // |Gamma| at f0 with the element values used
};

struct MatchRequest {
    MatchComplex zs{50, 0};
    MatchComplex zl{50, 0};
    double frequency = 1e9; // Hz
    MatchTopology topology = MatchTopology::LNetwork;
    double q = 3;             // Pi and T: loaded (nodal) Q
    int sections = 2;         // multi-section ladder
    double lineZ0 = 0;        // lines and stubs; 0 means Re(Zs)
    double stubSpacingDeg = 45; // double stub: line between the stubs
    std::optional<ESeries> series; // snap lumped values (RF-13)
};

struct MatchResult {
    std::vector<MatchSolution> solutions;
    QString error; // set when no solution exists or the request is invalid
    QStringList warnings;

    bool ok() const { return error.isEmpty(); }
};

MatchResult synthesizeMatch(const MatchRequest &request);

// Minimum loaded Q of a Pi (shunt-series-shunt) or T (series-shunt-series)
// network for these terminations: the Q of the corresponding L-network.
double minimumPiQ(MatchComplex zs, MatchComplex zl);
double minimumTQ(MatchComplex zs, MatchComplex zl);

// ---- Evaluation of a solution (ideal lossless elements)

// The impedance at the source side of an element, given the impedance at its
// load side, at `frequency` (line lengths are specified at f0).
MatchComplex transformImpedance(const MatchElement &element, MatchComplex loadSide, double frequency, double f0);

// Impedance seen from the source side of the network with the load attached.
MatchComplex inputImpedance(const MatchSolution &solution, double frequency);
// Power-wave reflection at the source: (Zin - Zs*) / (Zin + Zs). Equals S11
// for a real source impedance used as reference.
MatchComplex inputReflection(const MatchSolution &solution, double frequency);
std::vector<double> reflectionMagnitude(const MatchSolution &solution, const std::vector<double> &frequencies);

// The impedance path on the Smith chart at f0, from the load toward the
// source: one segment per element, starting with the element next to the
// load, as the element grows from nothing to its final value (reactance or
// susceptance from zero for lumped elements and stubs, length from zero for
// lines). Each segment holds pointsPerElement + 1 impedances.
struct TrajectorySegment {
    int elementIndex = -1; // into MatchSolution::elements
    std::vector<MatchComplex> impedances;
};
std::vector<TrajectorySegment> impedanceTrajectory(const MatchSolution &solution, int pointsPerElement = 64);

// A copy with all lumped values snapped to the series; mismatch updated.
MatchSolution snapSolution(const MatchSolution &solution, ESeries series);

// Recomputes loadedQ, bandwidthEstimate, bandwidth3dB and mismatch.
void updateFigures(MatchSolution &solution);

// "series L 3.3 nH", "shunt open stub 50 ohm 41.2 deg", ...
QString describeElement(const MatchElement &element);

} // namespace lvd::rf
