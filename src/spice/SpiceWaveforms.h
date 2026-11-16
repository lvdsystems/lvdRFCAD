#pragma once

#include "spice/Fourier.h"
#include "spice/SpiceData.h"
#include "spice/VectorExpression.h"

#include <QString>

namespace lvd::spice {

struct SpiceDeck;

// A result vector as a waveform over its plot's scale (the scale is the
// sample index when the plot has none).
Waveform waveformOf(const SpicePlot &plot, const SpiceVector &vector);

// Resolves trace-expression names against a plot. With a deck, names may
// also be written as on the schematic: v(<net label>) for the node, and
// i(<component reference>) for the current of the part's source, e.g.
// "i(V1)" for the vector "i(vv1)".
VectorExpression::Resolver plotResolver(const SpicePlot &plot, const SpiceDeck *deck = nullptr);

// Evaluates a trace expression on a plot.
std::optional<Waveform> evaluateOnPlot(const QString &expression, const SpicePlot &plot, const SpiceDeck *deck = nullptr,
                                       QString *error = nullptr);

// One signal of a Fourier analysis (requirement SP-2, like SPICE's .four).
struct FourierSignal {
    QString expression;
    std::optional<FourierResult> result;
    QString error;
};

} // namespace lvd::spice
