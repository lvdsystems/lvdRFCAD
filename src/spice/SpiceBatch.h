#pragma once

#include "core/StandardValues.h"
#include "spice/SpiceAnalysis.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace lvd {
class Document;
class Project;
} // namespace lvd

namespace lvd::spice {

// Sweeps and statistics on SPICE analyses (requirement SP-3): parameter and
// temperature stepping, Monte Carlo, worst case and optimisation.
//
// Every variant is a batch: the GUI thread (or a script) takes a project
// snapshot, changes values in it and netlists one simulation per point;
// the prepared simulations then run on one engine in turn or, since
// ngspice allows one simulation per process, in parallel worker processes
// (SpiceProcessPool). Scalar measures reduce each run to numbers.

// ---- Measures

// A number taken from one run: a trace expression reduced by a function.
struct SpiceMeasure {
    QString name;              // label; default: function(expression)
    QString expression;        // trace expression, e.g. "v(out)"
    QString function = QStringLiteral("value");
    // value (at "at", or the first point), max, min, mean, rms, pkpk,
    // risetime, falltime, frequency, period, duty, overshoot, settling,
    // crossing (first time the trace crosses "level"), thd (percent)
    QString at;                // value: x position (expression text)
    QString from;              // window start (expression text, optional)
    QString to;                // window end
    QString level;             // crossing: the level
    QString plot;              // plot type to use; default: the analysis' main result

    QString label() const;
    QJsonObject toJson() const;
    static SpiceMeasure fromJson(const QJsonObject &object);
};

QStringList measureFunctions();

std::optional<double> evaluateMeasure(const SpiceMeasure &measure, const SpiceSimulation &simulation,
                                      const Expression::Lookup &scope = {}, QString *error = nullptr);

// ---- What is changed

// A schematic variable of the analysed sheet ("f0"), a component parameter
// ("R1.r", a part of the analysed sheet), or the circuit temperature
// ("temperature").
struct SpiceTarget {
    QString text; // "f0", "R1.r" or "temperature"

    bool isTemperature() const { return text.compare(QStringLiteral("temperature"), Qt::CaseInsensitive) == 0; }
    bool isParameter() const { return text.contains(QLatin1Char('.')); }
    QString component() const { return text.section(QLatin1Char('.'), 0, 0); }
    QString parameter() const { return text.section(QLatin1Char('.'), 1); }
};

// Applies numeric values to a snapshot's analysed document; temperatures
// go into the analysis. False with *error for an unknown target.
bool applyTargets(Project &snapshot, Document &document, SpiceAnalysisSetup &analysis, const QList<SpiceTarget> &targets,
                  const std::vector<double> &values, QString *error = nullptr);
std::optional<double> targetValue(const Project &project, const Document &document, const SpiceAnalysisSetup &analysis,
                                  const SpiceTarget &target, QString *error = nullptr);

// ---- Batches

struct BatchRun {
    QString label;              // "R1.r = 1k", "run 17", "nominal", ...
    std::vector<double> values; // of the batch's targets
    std::shared_ptr<const SpiceSimulation> simulation;
    std::vector<double> measures; // NaN where a measure failed
    QStringList measureErrors;
};

struct BatchResult {
    QList<SpiceTarget> targets;
    QList<SpiceMeasure> measures;
    QList<BatchRun> runs;
    QStringList errors;   // problems that stopped the batch
    QStringList warnings; // failed runs and measures
    bool cancelled = false;

    bool ok() const { return errors.isEmpty(); }
};

struct BatchProgress {
    int done = 0;
    int total = 0;
    QString message;
};

struct BatchOptions {
    // Simulates prepared runs. Default: one after another on the default
    // engine. A SpiceProcessPool runs them in parallel.
    std::function<void(QList<SpiceSimulation *> &simulations, const std::atomic_bool *cancel,
                       const std::function<void(int done)> &progress)>
        execute;
    std::function<void(const BatchProgress &)> progress;
    const std::atomic_bool *cancel = nullptr;
};

// Parameter stepping (nested: the first axis is the outer loop).
struct SweepAxis {
    SpiceTarget target;
    QString mode = QStringLiteral("lin"); // lin, log (points per decade), list
    QString start, stop, points;           // expression text
    QStringList list;

    QJsonObject toJson() const;
    static SweepAxis fromJson(const QJsonObject &object);
};
std::optional<std::vector<double>> sweepValues(const SweepAxis &axis, const Expression::Lookup &scope, QString *error = nullptr);

BatchResult runParameterSweep(Project &snapshot, const QString &documentName, const SpiceAnalysisSetup &analysis,
                              const QList<SweepAxis> &axes, const QList<SpiceMeasure> &measures, const BatchOptions &options = {});

// Monte Carlo: each run draws every toleranced value independently.
struct Tolerance {
    SpiceTarget target;       // "R1.r", "R*.r" (glob on references of the analysed sheet) or a variable
    double relative = 0.05;   // ±5 %
    enum class Distribution { Uniform, Gaussian } distribution = Distribution::Gaussian; // Gaussian: 3 sigma = tolerance

    QJsonObject toJson() const;
    static Tolerance fromJson(const QJsonObject &object);
};

struct MonteCarloSetup {
    QList<Tolerance> tolerances;
    int runs = 100;
    quint32 seed = 1;
    QList<SpiceMeasure> measures;

    QJsonObject toJson() const;
    static MonteCarloSetup fromJson(const QJsonObject &object);
};

// Expands globs into one tolerance per matched target.
QList<Tolerance> expandTolerances(const Project &project, const Document &document, const QList<Tolerance> &tolerances,
                                  QStringList *warnings = nullptr);

// Run 0 is the nominal circuit; runs 1..N are random.
BatchResult runMonteCarlo(Project &snapshot, const QString &documentName, const SpiceAnalysisSetup &analysis,
                          const MonteCarloSetup &setup, const BatchOptions &options = {});

struct MeasureStatistics {
    QString name;
    int count = 0; // runs with a value
    double nominal = NAN;
    double mean = NAN;
    double sigma = NAN;
    double minimum = NAN;
    double maximum = NAN;
};
QList<MeasureStatistics> statistics(const BatchResult &result); // Monte Carlo: run 0 is the nominal

// Worst case: the sensitivity of every measure to every toleranced value
// (one run per value at its upper limit), then for each measure the two
// corners that push it lowest and highest. Runs: nominal, the sensitivity
// runs, then "<measure> min" and "<measure> max" per measure.
struct WorstCaseMeasure {
    QString name;
    double nominal = NAN;
    double minimum = NAN;
    double maximum = NAN;
    std::vector<int> minimumCorner; // per tolerance: -1 lower limit, +1 upper
    std::vector<int> maximumCorner;
};
BatchResult runWorstCase(Project &snapshot, const QString &documentName, const SpiceAnalysisSetup &analysis,
                         const QList<Tolerance> &tolerances, const QList<SpiceMeasure> &measures,
                         QList<WorstCaseMeasure> *summary, const BatchOptions &options = {});

// Optimisation toward goals on measures (Nelder-Mead in a bounded, optionally
// logarithmic, space; values may be snapped to a standard series at the end).
struct SpiceGoal {
    SpiceMeasure measure;
    enum class Type { Below, Above, Equal, Minimize, Maximize } type = Type::Below;
    double target = 0;
    double weight = 1;

    QString label() const;
    QJsonObject toJson() const;
    static SpiceGoal fromJson(const QJsonObject &object);
};

struct SpiceOptimizationVariable {
    SpiceTarget target;
    double minimum = 0;
    double maximum = 0;
    bool logarithmic = true;
    std::optional<ESeries> series;

    QJsonObject toJson() const;
    static SpiceOptimizationVariable fromJson(const QJsonObject &object);
};

struct SpiceOptimizationSetup {
    QList<SpiceOptimizationVariable> variables;
    QList<SpiceGoal> goals;
    int maxEvaluations = 200;
    double tolerance = 1e-4;

    QJsonObject toJson() const;
    static SpiceOptimizationSetup fromJson(const QJsonObject &object);
};

struct SpiceOptimizationResult {
    std::vector<double> initial;
    std::vector<double> best;
    std::vector<double> snapped;
    double initialCost = NAN;
    double bestCost = NAN;
    double snappedCost = NAN;
    std::vector<double> bestMeasures; // per goal, at the snapped point
    std::vector<bool> goalsMet;
    int evaluations = 0;
    bool converged = false;
    bool cancelled = false;
    QStringList errors;
    QStringList warnings;

    bool ok() const { return errors.isEmpty(); }
};

// Runs on the snapshot (left with the snapped values applied).
SpiceOptimizationResult optimizeSpice(Project &snapshot, const QString &documentName, const SpiceAnalysisSetup &analysis,
                                      const SpiceOptimizationSetup &setup, const BatchOptions &options = {});

// Cost of goal values: sum of weight * squared violation, relative to
// max(|target|, 1e-12) (Minimize/Maximize: weight * value * 1e-3).
double goalCost(const QList<SpiceGoal> &goals, const std::vector<double> &values, std::vector<bool> *met = nullptr);

} // namespace lvd::spice
