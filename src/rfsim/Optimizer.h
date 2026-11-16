#pragma once

#include "core/StandardValues.h"
#include "rfsim/Analysis.h"
#include "rfsim/LinearSolver.h"
#include "rfsim/SignalChain.h" // NotSet

#include <QJsonObject>
#include <QList>
#include <QString>

#include <atomic>
#include <functional>
#include <optional>

namespace lvd {
class Document;
class Project;
} // namespace lvd

namespace lvd::rf {

// Tuning and optimisation (requirements RF-11, RF-12).
//
// Both work on a project snapshot (Project::snapshot()), so the document is
// never touched until the user applies the result, and the work can run on
// a worker thread.

// A value the tuner or optimiser may change: a variable of the top sheet,
// or a parameter of a component in any sheet (changing a component inside
// a sub-sheet changes it in every instance of that sheet).
struct DesignVariable {
    enum class Kind { Variable, Parameter };

    Kind kind = Kind::Variable;
    QString name;      // variable name, or parameter name
    QString document;  // parameters: sheet document; empty = the analysed document
    QString component; // parameters: component reference in that sheet
    double minimum = 0;
    double maximum = 0;
    bool logarithmic = true;          // search in log space (needs minimum > 0)
    std::optional<ESeries> series;    // snap to standard values after optimising
    QString unit;                     // for display

    QString label() const;            // "C1.c" or "f0"
    QJsonObject toJson() const;
    static DesignVariable fromJson(const QJsonObject &object);
};

// Current numeric value of a variable in a project (evaluated expression).
std::optional<double> currentValue(const Project &project, const Document &document, const DesignVariable &variable,
                                   QString *error = nullptr);

// Writes numeric values (as exact number text) into a project. In a
// snapshot the undo history is cleared to keep memory flat. For the real
// project, call it inside Schematic edits to make it undoable.
bool applyDesignValues(Project &project, Document &document, const QList<DesignVariable> &variables,
                       const std::vector<double> &values, QString *error = nullptr);

struct Goal {
    enum class Type { Below, Above, Equal, Minimize, Maximize };

    QString measurement;              // e.g. "dB(S11)", evaluated with evaluateMeasurement
    Type type = Type::Below;
    double target = 0;                // ignored for Minimize / Maximize
    double weight = 1;
    double bandStart = NotSet;        // Hz; NotSet: the whole sweep
    double bandStop = NotSet;

    QString label() const;            // "dB(S11) < -15 from 2.4 GHz to 2.5 GHz"
    QJsonObject toJson() const;
    static Goal fromJson(const QJsonObject &object);
};

struct GoalEvaluation {
    Goal goal;
    double worst = NotSet;   // the worst value in the band (largest for Below/Equal-above, ...)
    double cost = 0;         // contribution to the total cost
    bool met = false;        // Below/Above/Equal (within 0.01): satisfied at every point; Minimize/Maximize: always true
    QString error;
};

// Evaluates goals on a result; the total cost is the sum of the costs. A
// goal's cost is weight * mean over its band of the squared violation
// (Minimize/Maximize: weight * mean of -/+ value scaled by 1e-3).
QList<GoalEvaluation> evaluateGoals(const LinearResult &result, const QList<Goal> &goals, double *totalCost = nullptr);

struct OptimizationSetup {
    enum class Method { NelderMead, LevenbergMarquardt };

    QString id;
    QString name;
    QString analysis;                 // id of the rf-linear analysis whose sweep is used
    QList<DesignVariable> variables;
    QList<Goal> goals;
    Method method = Method::NelderMead;
    int maxEvaluations = 500;
    double tolerance = 1e-6;          // relative cost change for convergence

    QJsonObject toJson() const;
    static OptimizationSetup fromJson(const QJsonObject &object);
};

// Optimisation setups in a simulation object ("optimizations" array; see
// docs/file-format.md, section 5.1). Unknown keys are preserved by the
// with...() helper.
QList<OptimizationSetup> optimizationSetups(const QJsonObject &simulation);
QJsonObject withOptimizationSetup(const QJsonObject &simulation, OptimizationSetup setup, QString *assignedId = nullptr);

struct OptimizationProgress {
    int evaluation = 0;
    double cost = 0;          // cost of the last evaluation
    double bestCost = 0;
    std::vector<double> best; // values of the best point so far
};

struct OptimizationResult {
    std::vector<double> initial;
    std::vector<double> best;
    std::vector<double> snapped;      // best snapped to the variables' series (equals best without series)
    double initialCost = NotSet;
    double bestCost = NotSet;
    double snappedCost = NotSet;
    QList<GoalEvaluation> goalsInitial;
    QList<GoalEvaluation> goalsBest;
    QList<GoalEvaluation> goalsSnapped;
    int evaluations = 0;
    bool converged = false;
    bool cancelled = false;
    QStringList errors;
    QStringList warnings;

    bool ok() const { return errors.isEmpty(); }
};

// Runs the optimisation on a snapshot (it is modified; the final, snapped
// values are left applied). `documentName` is the analysed schematic in the snapshot.
OptimizationResult optimize(Project &snapshot, const QString &documentName, const OptimizationSetup &setup,
                            const std::function<void(const OptimizationProgress &)> &progress = {},
                            const std::atomic_bool *cancel = nullptr);

// One evaluation: apply values to the snapshot and simulate with the
// analysis. Used by tuning.
LinearResult evaluateDesign(Project &snapshot, const QString &documentName, const LinearAnalysisSetup &analysis,
                            const QList<DesignVariable> &variables, const std::vector<double> &values,
                            TouchstoneCache *touchstones = nullptr);

} // namespace lvd::rf
