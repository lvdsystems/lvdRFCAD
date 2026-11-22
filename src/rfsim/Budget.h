#pragma once

#include "core/Expression.h"
#include "rfsim/SignalChain.h"
#include "rfsim/Spurs.h"

#include <QJsonObject>
#include <QList>

#include <atomic>

namespace lvd {
class Document;
class Project;
} // namespace lvd

namespace lvd::rf {

// System budget cascade (requirements RF-6, RF-7, RF-9).
//
// Every quantity is in dB, dBm, Hz or K. "Cumulative" values describe the
// cascade from the chain input to the output of that stage; input-referred
// figures (IIP3, IP1dB, IIP2) refer to the chain input. Values that do not
// apply (IP3 of a linear passive stage) are +infinity.
//
// Noise in the bandwidth B has three parts, added in power for the SNR:
// - thermal: k T B of the source through the cascade noise figure;
// - LO phase noise: every mixer adds the integrated phase noise of its LO
//   (both sidebands, over the integration range, raised by 20 log10 N for
//   multipliers between the LO block and the mixer) as noise proportional
//   to the signal; a multiplier on the signal path raises the accumulated
//   ratio by 20 log10 N;
// - reciprocal mixing: a blocker at the chain input (offset above the wanted
//   frequency) reaches each mixer through the stages before it and mixes
//   with the LO phase noise at its offset into the channel:
//   blocker at the mixer input + L(offset) + 10 log10(B), carried to later
//   stages with the gain of the wanted signal.

struct BudgetStage {
    QString path;
    QString name;
    QString kind;
    double inputFrequency = 0;
    double outputFrequency = 0;

    // The stage alone
    double gainDb = 0;          // transducer gain inside the cascade (actual mode) or |S21|^2 (ideal mode)
    double nfDb = 0;
    double oip3Dbm = INFINITY;
    double op1dbDbm = INFINITY;
    double oip2Dbm = INFINITY;
    double inputReturnLossDb = INFINITY; // -20 log |S11| of the stage

    // Cumulative, at the stage output
    double cumulativeGainDb = 0;
    double cumulativeNfDb = 0;
    double iip3Dbm = INFINITY;
    double oip3CumulativeDbm = INFINITY;
    double ip1dbDbm = INFINITY;
    double op1dbCumulativeDbm = INFINITY;
    double iip2Dbm = INFINITY;
    double signalDbm = 0;       // wanted signal power
    double noiseDbm = 0;        // thermal noise power in the analysis bandwidth
    double snrDb = 0;           // signal over the total noise (thermal, phase noise, reciprocal mixing)
    double thermalSnrDb = 0;    // signal over thermal noise only
    double phaseNoiseDbm = -INFINITY;       // in-channel noise from LO phase noise on the signal
    double reciprocalMixingDbm = -INFINITY; // in-channel noise from the blocker and LO phase noise
    double totalNoiseDbm = 0;
    double phaseNoiseSnrDb = INFINITY;
    double reciprocalMixingSnrDb = INFINITY;
    double dacImageDbm = NotSet; // chain starting at a DAC: its first image at the stage output

    // Mixers: the LO at the lo pin (NotSet without phase noise or blocker)
    double loPhaseNoiseDbc = NotSet;  // integrated, both sidebands
    double loRmsPhaseDeg = NotSet;
    double loJitterS = NotSet;
    double loPhaseNoiseAtOffsetDbcHz = NotSet; // L at the blocker's offset
    double blockerDbm = NotSet;       // blocker power at the mixer input

    double sfdrDb = INFINITY;   // input-referred third-order spurious-free dynamic range
    double headroomDb = INFINITY; // cumulative OP1dB minus signal power
    bool compressed = false;      // headroom below 0 dB
    QStringList notes;
};

struct BudgetResult {
    QString start;
    QString end;
    double inputFrequency = 0;
    double inputPowerDbm = 0;
    double bandwidth = 0;
    double sourceTemperature = T0;
    double inputNoiseDbm = 0;   // k (T_source) B
    bool mismatch = true;       // true: actual S-parameters (RF-7); false: ideal matched cascade
    std::vector<BudgetStage> stages;

    // Analog-to-digital converter at the end of the chain
    bool hasAdc = false;
    double adcFullScaleDbm = NotSet;
    double adcHeadroomDb = NotSet;       // full scale minus signal
    double adcQuantisationSnrDb = NotSet; // 6.02 bits + 1.76
    double adcSampleRate = NotSet;
    double adcAliasFrequency = NotSet;   // the output frequency folded into 0 .. fs/2

    // Phase noise and blocker (see the top of this file)
    bool hasPhaseNoise = false;          // some mixer's LO has a phase-noise profile
    double phaseNoiseStart = NotSet;     // Hz, integration range used
    double phaseNoiseStop = NotSet;
    bool hasBlocker = false;
    double blockerOffset = NotSet;       // Hz above the wanted input frequency
    double blockerPowerDbm = NotSet;     // at the chain input

    // Digital-to-analogue converter at the start of the chain
    bool hasDac = false;
    double dacSincDb = NotSet;           // NRZ roll-off at the output frequency (included in inputPowerDbm)
    double dacNsdDbmHz = NotSet;         // noise density (inputNoiseDbm = NSD + 10 log10 B)
    bool dacNsdFromQuantisation = false;
    double dacImageFrequency = NotSet;   // first image, |fs - f|
    double dacImageDbm = NotSet;         // at the DAC output

    // Detector at the end of the chain
    bool hasDetector = false;
    double detectorSensitivityDbm = NotSet;
    double detectorMaxDbm = NotSet;      // sensitivity + dynamic range
    double detectorMarginDb = NotSet;    // signal minus sensitivity
    double detectorHeadroomDb = NotSet;  // top of the dynamic range minus signal

    QStringList warnings;
    QStringList errors;
    bool ok() const { return errors.isEmpty(); }
};

struct BudgetOptions {
    double bandwidth = 1e6;
    bool mismatch = true;
    // Phase-noise integration range; NotSet: bandwidth / 1000 to bandwidth / 2.
    double phaseNoiseStart = NotSet;
    double phaseNoiseStop = NotSet;
    // Blocker at the chain input, `blockerOffset` Hz above the wanted signal; NotSet: none.
    double blockerOffset = NotSet;
    double blockerPowerDbm = NotSet;
};

// The budget of a traced chain at its input frequency, or at `inputFrequency` when given.
BudgetResult computeBudget(ChainEvaluator &evaluator, const BudgetOptions &options, double inputFrequency = NotSet);

// "sum" or "difference" for a mixer stage, empty for other stages.
QString mixerProductName(const ChainStage &stage);

// Warns for every mixer whose following stages (up to the next frequency
// translation) pass the other product at least 20 dB better than the
// selected one: a likely wrong `product` setting.
QStringList checkMixerProducts(ChainEvaluator &evaluator);

// ---- Analysis setup (docs/file-format.md, section 5.1, type "rf-budget")

struct BudgetAnalysisSetup {
    QString id;
    QString name;
    QStringList path;                  // explicit path, see ChainOptions
    QString start;                     // start component reference
    QString frequency;                 // expression; empty: from the start component
    QString power;                     // expression (dBm); empty: from the start component
    QString bandwidth = QStringLiteral("1MHz");
    bool mismatch = true;
    // Swept budget: input frequencies (empty start/stop: no sweep)
    QString sweepStart;
    QString sweepStop;
    QString sweepPoints = QStringLiteral("101");
    // Spur analysis
    bool spurs = true;
    QString spurOrder = QStringLiteral("5");
    // Phase-noise integration range (empty: bandwidth/1000 to bandwidth/2)
    QString phaseNoiseStart;
    QString phaseNoiseStop;
    // Blocker for reciprocal mixing (empty power: no blocker)
    QString blockerOffset;
    QString blockerPower;

    static QString type() { return QStringLiteral("rf-budget"); }
    static BudgetAnalysisSetup fromJson(const QJsonObject &object);
    QJsonObject toJson() const;
};

QList<BudgetAnalysisSetup> budgetAnalyses(const QJsonObject &simulation);
QJsonObject withBudgetAnalysis(const QJsonObject &simulation, BudgetAnalysisSetup analysis, QString *assignedId = nullptr);

struct BudgetRun {
    SignalChain chain;
    BudgetResult budget;                 // at the wanted frequency
    std::vector<double> sweepFrequencies; // input frequencies of the swept budget (may be empty)
    std::vector<BudgetResult> sweep;     // one budget per sweep frequency
    SpurResult spurs;
    QStringList errors;                  // setup errors (expressions, tracing); see also the parts
    QStringList warnings;
    bool cancelled = false;

    bool ok() const { return errors.isEmpty() && chain.ok() && budget.ok() && !cancelled; }
};

// Flattens the document, traces the chain and computes the budget, sweep and
// spurs. The netlist is built from the document, so call it on the thread
// that owns the document, or use runBudget(netlist, ...) on a worker.
BudgetRun runBudgetAnalysis(const Project &project, const Document &document, const BudgetAnalysisSetup &setup,
                            TouchstoneCache *touchstones = nullptr, const std::atomic_bool *cancel = nullptr);
// The same on an already built netlist; safe on a worker thread.
// `scope` evaluates the setup's expressions (the top sheet's variables).
BudgetRun runBudget(const Netlist &netlist, const BudgetAnalysisSetup &setup, const Expression::Lookup &scope,
                    const QString &projectDirectory, TouchstoneCache *touchstones = nullptr,
                    const std::atomic_bool *cancel = nullptr);

} // namespace lvd::rf
