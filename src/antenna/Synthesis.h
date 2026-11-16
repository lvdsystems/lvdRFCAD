#pragma once

#include "antenna/ArrayGeometry.h"

#include <QJsonObject>
#include <QStringList>

#include <atomic>
#include <functional>
#include <vector>

namespace lvd::ant {

// Pattern synthesis against a side-lobe mask (decision 9.6 of the
// requirements). The mask is a function of the angle from the beam; the
// array factor is synthesised for a broadside beam in the UV plane, so the
// result keeps its shape when the beam is steered (phase steering).
struct MaskSegment {
    double fromDeg = 0;  // the level applies from this angle from the beam outwards
    double levelDb = -30;
};

struct SynthesisSpec {
    double mainBeamDeg = 0;   // half-width of the excluded main-beam region; 0: automatic
    double sllDb = -30;       // level outside the main beam (relative to the peak)
    std::vector<MaskSegment> segments; // optional steps further out, e.g. {20 deg, -40 dB}
    bool phaseOnly = false;
    int iterations = 300;

    double levelAt(double angleDeg) const;
    QJsonObject toJson() const;
    static SynthesisSpec fromJson(const QJsonObject &object);
};

struct SynthesisResult {
    std::vector<cplx> weights;   // broadside weights, peak amplitude 1
    double achievedSllDb = NAN;  // highest side lobe relative to the peak, on the sample grid
    double worstExcessDb = NAN;  // largest mask violation (<= 0 when the mask is met)
    double mainBeamDeg = 0;      // the main-beam region used
    int iterations = 0;
    bool converged = false;
    bool cancelled = false;
    std::vector<double> history; // worst excess per iteration
    QStringList warnings;
};

// Planar (xy) and linear arrays; positions are projected on the xy-plane.
// `initial` gives the starting weights (and the amplitudes kept by
// phase-only synthesis); empty means uniform.
SynthesisResult synthesise(const ArrayLayout &layout, const std::vector<cplx> &initial, double frequency,
                           const SynthesisSpec &spec, const std::atomic_bool *cancel = nullptr,
                           const std::function<void(int)> &progress = {});

} // namespace lvd::ant
