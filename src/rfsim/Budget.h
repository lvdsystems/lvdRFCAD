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
    double noiseDbm = 0;        // noise power in the analysis bandwidth
    double snrDb = 0;
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

    QStringList warnings;
    QStringList errors;
    bool ok() const { return errors.isEmpty(); }
};

struct BudgetOptions {
    double bandwidth = 1e6;
    bool mismatch = true;
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
