#pragma once

#include "core/Expression.h"
#include "rfsim/LinearSolver.h"

#include <QJsonObject>
#include <QList>

namespace lvd {
class Document;
class Project;
} // namespace lvd

namespace lvd::rf {

// A linear (S-parameter and noise) analysis as stored in a schematic's
// "simulation" object (docs/file-format.md, section 5.1). Values are
// expression text evaluated with the schematic's variables.
struct LinearAnalysisSetup {
    QString id;
    QString name;
    QString sweep = QStringLiteral("linear"); // linear, log or list
    QString start = QStringLiteral("100MHz");
    QString stop = QStringLiteral("3GHz");
    QString points = QStringLiteral("201");
    QStringList list;                         // frequencies for sweep = list
    bool noise = true;

    static QString type() { return QStringLiteral("rf-linear"); }
    static LinearAnalysisSetup fromJson(const QJsonObject &object);
    QJsonObject toJson() const;
};

// Analyses of type rf-linear in a simulation object, in order.
QList<LinearAnalysisSetup> linearAnalyses(const QJsonObject &simulation);
// Adds or replaces (by id) an analysis; assigns an id if it has none.
QJsonObject withLinearAnalysis(const QJsonObject &simulation, LinearAnalysisSetup analysis, QString *assignedId = nullptr);

// Evaluates the sweep. Limit: 100,000 points.
std::optional<std::vector<double>> sweepFrequencies(const LinearAnalysisSetup &analysis,
                                                    const Expression::Lookup &scope, QString *error = nullptr);

// Flattens the document's hierarchy, evaluates the sweep with the top
// sheet's variables and runs the linear solver.
LinearResult runLinearAnalysis(const Project &project, const Document &document, const LinearAnalysisSetup &analysis,
                               TouchstoneCache *touchstones = nullptr, const std::atomic_bool *cancel = nullptr);

// The top sheet's variables as a lookup, with errors in *errors.
Expression::Lookup sheetScope(const Document &document, QStringList *errors = nullptr);

} // namespace lvd::rf
