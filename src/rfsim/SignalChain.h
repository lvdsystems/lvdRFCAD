#pragma once

#include "core/Netlist.h"
#include "rfdata/RfTypes.h"
#include "rfsim/ElementModels.h"

#include <QStringList>

#include <limits>
#include <memory>

namespace lvd::rf {

// The signal path of a system budget (requirements RF-6 to RF-9): an ordered
// list of two-port stages from a start component (source, port 1 or
// antenna) to an end component (termination, ADC, port, antenna), with the
// frequency of the wanted signal at every stage.

constexpr double NotSet = std::numeric_limits<double>::quiet_NaN();

struct ChainStage {
    const FlatComponent *component = nullptr; // owned by the SignalChain's netlist
    QString path;        // hierarchical reference, e.g. "X1/A1"
    QString name;        // definition name, e.g. "Amplifier"
    QString kind;        // block or element type ("amplifier", "filter", "tline", "sparameters", ...)
    int inputPin = 0;    // pin indices of the component used as input and output
    int outputPin = 1;
    double inputFrequency = 0;  // Hz, wanted signal entering the stage
    double outputFrequency = 0; // Hz, wanted signal leaving it (differs for mixers and multipliers)

    // Frequency-translating stages
    enum class Translation { None, Mixer, Multiplier };
    Translation translation = Translation::None;
    double loFrequency = NotSet;   // mixers: frequency at the LO pin
    double loPowerDbm = NotSet;    // mixers: LO level at the LO pin (source power plus the LO path gain)
    QString loSource;              // path of the LO block feeding the mixer
    bool upConversion = false;     // mixers: entered at `if`, left at `rf`
    // Mixers: f_out = f_in + f_LO, else |f_in - f_LO|. From the mixer's `product`
    // parameter; with "auto": the difference when entered at `rf`, and by `side`
    // when entered at `if` (low: sum).
    bool sumProduct = false;
    int factor = 1;                // multipliers
};

struct SignalChain {
    Netlist netlist;                // the flattened design the stages point into
    QString start;                  // path of the start component
    QString startKind;              // "source", "port", "antenna"
    QString end;                    // path of the end component, empty if the path is open
    QString endKind;                // "termination", "adc", "port", "antenna", ...
    double inputFrequency = NotSet; // Hz
    double inputPowerDbm = NotSet;  // available input power
    double sourceTemperature = T0;  // K: an antenna's `temp`, otherwise T0
    double sourceZ0 = SolverZ0;
    std::vector<ChainStage> stages;
    QStringList warnings;
    QStringList errors;

    bool ok() const { return errors.isEmpty(); }
    // Index of the stage with this path, or -1.
    int stageIndex(const QString &path) const;
};

struct ChainOptions {
    // Explicit path: component references in order, from start to end. A
    // branch is chosen with "REF.pin", e.g. "SPL1.out2" or "SW1.t2". Empty:
    // traced automatically from the start component. The first entry is the
    // start component (when `start` is also set, it may be repeated or omitted).
    QStringList path;
    QString start;                    // start component; empty: source, else port 1, else antenna
    double frequency = NotSet;        // overrides the start component's frequency
    double powerDbm = NotSet;         // overrides the start component's power
};

// Traces the signal path and the frequency plan. Problems go to the chain's
// errors (no start, a dead end, a mixer without LO) and warnings (a branch
// taken by default, a shunt part on the path that the budget ignores).
SignalChain traceChain(const Netlist &netlist, const ChainOptions &options = {});

// Evaluates stages of a chain at any frequency, for budgets, swept budgets
// and spur analysis. Models are built on demand and cached per frequency.
class ChainEvaluator
{
public:
    ChainEvaluator(const SignalChain &chain, const QString &projectDirectory, TouchstoneCache *touchstones = nullptr);
    ~ChainEvaluator();

    const SignalChain &chain() const { return m_chain; }

    // Two-port S (port 1 = stage input, 2 = output), reference SolverZ0, with
    // the stage's input at `inputFrequency`. Mixers and multipliers give a
    // matched two-port with the conversion gain (other ports matched); the
    // output is at the translated frequency. nullopt with *error on failure.
    std::optional<CMatrix> stageS(int stage, double inputFrequency, QString *error = nullptr);
    // Noise wave correlation (kelvin) of the same two-port.
    std::optional<CMatrix> stageNoise(int stage, double inputFrequency, QString *error = nullptr);

    // Frequency at the output of a stage for a signal entering it at f:
    // translated by mixers (with the same product as the wanted signal) and
    // multipliers; unchanged otherwise.
    double outputFrequency(int stage, double inputFrequency) const;

    // Forward transmission in dB of stages [from, to) for a signal entering
    // stage `from` at f, following frequency translation; *finalFrequency
    // receives the frequency after the last stage. Uses |S21| of each stage
    // (matched cascade).
    double forwardGainDb(int from, int to, double inputFrequency, double *finalFrequency = nullptr,
                         QString *error = nullptr);
    // Reverse transmission in dB (|S12|) through stages [from, to) for a
    // signal entering stage to-1 at its output at f, travelling to the input
    // of stage `from`. No frequency translation (used for LO leakage).
    double reverseGainDb(int from, int to, double frequency, QString *error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    const SignalChain &m_chain;
};

} // namespace lvd::rf
