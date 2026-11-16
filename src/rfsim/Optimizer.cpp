#include "rfsim/Optimizer.h"

#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "rfsim/Measurement.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lvd::rf {

namespace {

constexpr double FailurePenalty = 1e9;

const QString OptimizationsKey = QStringLiteral("optimizations");

// Engineering notation that reads well in the schematic ("3.28537n") and
// keeps 12 significant digits, enough for the optimiser's results.
QString numberText(double value)
{
    const QString text = formatEngineering(value, QString(), 12);
    // Values outside the prefix range come back in scientific notation, which the parser also reads.
    return parseNumber(text) ? text : QString::number(value, 'g', 15);
}

const char *goalTypeName(Goal::Type type)
{
    switch (type) {
    case Goal::Type::Below: return "below";
    case Goal::Type::Above: return "above";
    case Goal::Type::Equal: return "equal";
    case Goal::Type::Minimize: return "minimize";
    case Goal::Type::Maximize: return "maximize";
    }
    return "below";
}

Goal::Type goalTypeFromName(const QString &name)
{
    if (name == QLatin1String("above"))
        return Goal::Type::Above;
    if (name == QLatin1String("equal"))
        return Goal::Type::Equal;
    if (name == QLatin1String("minimize"))
        return Goal::Type::Minimize;
    if (name == QLatin1String("maximize"))
        return Goal::Type::Maximize;
    return Goal::Type::Below;
}

QJsonValue optionalNumber(double value)
{
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue();
}

double readOptionalNumber(const QJsonValue &value)
{
    return value.isDouble() ? value.toDouble() : NotSet;
}

Document *findDocument(Project &project, const QString &name)
{
    return project.document(name);
}

const Document *findDocument(const Project &project, const QString &name)
{
    return project.document(name);
}

// Maps a variable between its value and the unit interval used by the search.
struct Mapping {
    double minimum, maximum;
    bool logarithmic;

    double toUnit(double value) const
    {
        double t = logarithmic ? std::log(value / minimum) / std::log(maximum / minimum)
                               : (value - minimum) / (maximum - minimum);
        return std::clamp(t, 0.0, 1.0);
    }
    double fromUnit(double t) const
    {
        t = std::clamp(t, 0.0, 1.0);
        return logarithmic ? minimum * std::pow(maximum / minimum, t) : minimum + (maximum - minimum) * t;
    }
};

struct Evaluation {
    double cost = FailurePenalty;
    std::vector<double> residuals;
    QList<GoalEvaluation> goals;
    bool ok = false;
};

} // namespace

// ---------------------------------------------------------------- variables

QString DesignVariable::label() const
{
    return kind == Kind::Variable ? name : QStringLiteral("%1.%2").arg(component, name);
}

QJsonObject DesignVariable::toJson() const
{
    QJsonObject object{{QStringLiteral("kind"), kind == Kind::Variable ? QStringLiteral("variable") : QStringLiteral("parameter")},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("min"), minimum},
                       {QStringLiteral("max"), maximum},
                       {QStringLiteral("log"), logarithmic}};
    if (!document.isEmpty())
        object.insert(QStringLiteral("document"), document);
    if (!component.isEmpty())
        object.insert(QStringLiteral("component"), component);
    if (series)
        object.insert(QStringLiteral("series"), seriesName(*series));
    if (!unit.isEmpty())
        object.insert(QStringLiteral("unit"), unit);
    return object;
}

DesignVariable DesignVariable::fromJson(const QJsonObject &object)
{
    DesignVariable variable;
    variable.kind = object.value(QStringLiteral("kind")).toString() == QLatin1String("parameter") ? Kind::Parameter
                                                                                                 : Kind::Variable;
    variable.name = object.value(QStringLiteral("name")).toString();
    variable.document = object.value(QStringLiteral("document")).toString();
    variable.component = object.value(QStringLiteral("component")).toString();
    variable.minimum = object.value(QStringLiteral("min")).toDouble();
    variable.maximum = object.value(QStringLiteral("max")).toDouble();
    variable.logarithmic = object.value(QStringLiteral("log")).toBool(true);
    variable.series = seriesFromName(object.value(QStringLiteral("series")).toString());
    variable.unit = object.value(QStringLiteral("unit")).toString();
    return variable;
}

std::optional<double> currentValue(const Project &project, const Document &document, const DesignVariable &variable,
                                   QString *error)
{
    const auto fail = [error](const QString &message) -> std::optional<double> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (variable.kind == DesignVariable::Kind::Variable) {
        const auto value = sheetScope(document)(variable.name);
        if (!value)
            return fail(QStringLiteral("variable %1 is not defined or has an error").arg(variable.name));
        return value;
    }
    const Document *sheet = variable.document.isEmpty() ? &document : findDocument(project, variable.document);
    if (!sheet || !sheet->schematic())
        return fail(QStringLiteral("sheet %1 not found").arg(variable.document));
    const PlacedComponent *component = sheet->schematic()->componentByReference(variable.component);
    if (!component)
        return fail(QStringLiteral("component %1 not found").arg(variable.component));
    const DefinitionPtr definition = project.resolve(*sheet->schematic(), component->definition).definition;
    if (!definition || !definition->parameter(variable.name))
        return fail(QStringLiteral("%1 has no parameter %2").arg(variable.component, variable.name));
    QString evaluationError;
    const auto value = Expression::parse(parameterText(*component, *definition, variable.name))
                           .evaluate(sheetScope(*sheet), &evaluationError);
    if (!value)
        return fail(QStringLiteral("%1: %2").arg(variable.label(), evaluationError));
    return value;
}

bool applyDesignValues(Project &project, Document &document, const QList<DesignVariable> &variables,
                       const std::vector<double> &values, QString *error)
{
    if (static_cast<size_t>(variables.size()) != values.size()) {
        if (error)
            *error = QStringLiteral("%1 variables but %2 values").arg(variables.size()).arg(values.size());
        return false;
    }
    QList<Schematic *> touched;
    for (qsizetype i = 0; i < variables.size(); ++i) {
        const DesignVariable &variable = variables.at(i);
        const QString text = numberText(values[static_cast<size_t>(i)]);
        if (variable.kind == DesignVariable::Kind::Variable) {
            Schematic *schematic = document.schematic();
            QList<Variable> list = schematic->variables();
            bool found = false;
            for (Variable &item : list) {
                if (item.name == variable.name) {
                    item.value = text;
                    found = true;
                }
            }
            if (!found)
                list.append(Variable{variable.name, text});
            schematic->setVariables(list);
            touched.append(schematic);
            continue;
        }
        Document *sheet = variable.document.isEmpty() ? &document : findDocument(project, variable.document);
        if (!sheet || !sheet->schematic()) {
            if (error)
                *error = QStringLiteral("sheet %1 not found").arg(variable.document);
            return false;
        }
        Schematic *schematic = sheet->schematic();
        const PlacedComponent *component = schematic->componentByReference(variable.component);
        if (!component) {
            if (error)
                *error = QStringLiteral("component %1 not found").arg(variable.component);
            return false;
        }
        schematic->setParameter(component->id, variable.name, text);
        touched.append(schematic);
    }
    if (project.isSnapshot()) {
        for (Schematic *schematic : std::as_const(touched))
            schematic->undoStack()->clear();
    }
    return true;
}

// ---------------------------------------------------------------- goals

QString Goal::label() const
{
    QString text = measurement;
    switch (type) {
    case Type::Below: text += QStringLiteral(" < %1").arg(target); break;
    case Type::Above: text += QStringLiteral(" > %1").arg(target); break;
    case Type::Equal: text += QStringLiteral(" = %1").arg(target); break;
    case Type::Minimize: text = QStringLiteral("minimise %1").arg(measurement); break;
    case Type::Maximize: text = QStringLiteral("maximise %1").arg(measurement); break;
    }
    if (std::isfinite(bandStart) && std::isfinite(bandStop))
        text += QStringLiteral(" from %1 to %2").arg(formatEngineering(bandStart, QStringLiteral("Hz")),
                                                     formatEngineering(bandStop, QStringLiteral("Hz")));
    return text;
}

QJsonObject Goal::toJson() const
{
    QJsonObject object{{QStringLiteral("measurement"), measurement},
                       {QStringLiteral("type"), QString::fromLatin1(goalTypeName(type))},
                       {QStringLiteral("target"), target},
                       {QStringLiteral("weight"), weight}};
    if (std::isfinite(bandStart))
        object.insert(QStringLiteral("bandStart"), bandStart);
    if (std::isfinite(bandStop))
        object.insert(QStringLiteral("bandStop"), bandStop);
    return object;
}

Goal Goal::fromJson(const QJsonObject &object)
{
    Goal goal;
    goal.measurement = object.value(QStringLiteral("measurement")).toString();
    goal.type = goalTypeFromName(object.value(QStringLiteral("type")).toString());
    goal.target = object.value(QStringLiteral("target")).toDouble();
    goal.weight = object.value(QStringLiteral("weight")).toDouble(1);
    goal.bandStart = readOptionalNumber(object.value(QStringLiteral("bandStart")));
    goal.bandStop = readOptionalNumber(object.value(QStringLiteral("bandStop")));
    return goal;
}

namespace {

// Goal costs and least-squares residuals of one result.
Evaluation evaluateResult(const LinearResult &result, const QList<Goal> &goals)
{
    Evaluation evaluation;
    evaluation.cost = 0;
    evaluation.ok = result.ok();
    for (const Goal &goal : goals) {
        GoalEvaluation item;
        item.goal = goal;
        if (!result.ok()) {
            item.error = result.errors.value(0, QStringLiteral("the simulation failed"));
            item.cost = FailurePenalty;
            evaluation.goals.append(item);
            evaluation.cost += item.cost;
            evaluation.residuals.push_back(std::sqrt(FailurePenalty));
            continue;
        }
        const Measurement m = evaluateMeasurement(result, goal.measurement);
        if (!m.ok()) {
            item.error = m.error;
            item.cost = FailurePenalty;
            evaluation.goals.append(item);
            evaluation.cost += item.cost;
            evaluation.residuals.push_back(std::sqrt(FailurePenalty));
            continue;
        }
        std::vector<double> values;
        for (size_t i = 0; i < m.frequencies.size(); ++i) {
            const double f = m.frequencies[i];
            if ((std::isfinite(goal.bandStart) && f < goal.bandStart) || (std::isfinite(goal.bandStop) && f > goal.bandStop))
                continue;
            values.push_back(m.values[i].real());
        }
        if (values.empty()) {
            item.error = QStringLiteral("no sweep frequency lies in the goal's band");
            item.cost = FailurePenalty;
            evaluation.goals.append(item);
            evaluation.cost += item.cost;
            evaluation.residuals.push_back(std::sqrt(FailurePenalty));
            continue;
        }
        const double n = static_cast<double>(values.size());
        const double scale = std::sqrt(goal.weight / n);
        double sum = 0;
        double worst = NotSet;
        bool met = true;
        for (double v : values) {
            if (!std::isfinite(v))
                v = goal.type == Goal::Type::Above || goal.type == Goal::Type::Maximize ? -1e3 : 1e3;
            double violation = 0;
            switch (goal.type) {
            case Goal::Type::Below:
                violation = std::max(0.0, v - goal.target);
                worst = std::isfinite(worst) ? std::max(worst, v) : v;
                break;
            case Goal::Type::Above:
                violation = std::max(0.0, goal.target - v);
                worst = std::isfinite(worst) ? std::min(worst, v) : v;
                break;
            case Goal::Type::Equal:
                violation = v - goal.target;
                if (!std::isfinite(worst) || std::abs(v - goal.target) > std::abs(worst - goal.target))
                    worst = v;
                break;
            case Goal::Type::Minimize:
                worst = std::isfinite(worst) ? std::max(worst, v) : v;
                break;
            case Goal::Type::Maximize:
                worst = std::isfinite(worst) ? std::min(worst, v) : v;
                break;
            }
            if (goal.type == Goal::Type::Minimize || goal.type == Goal::Type::Maximize) {
                sum += (goal.type == Goal::Type::Minimize ? v : -v) * 1e-3;
                continue;
            }
            sum += violation * violation;
            evaluation.residuals.push_back(scale * violation);
            if (goal.type == Goal::Type::Equal ? std::abs(violation) > 0.01 : violation > 1e-9)
                met = false;
        }
        item.worst = worst;
        item.cost = goal.weight * sum / n;
        item.met = met;
        evaluation.goals.append(item);
        evaluation.cost += item.cost;
    }
    return evaluation;
}

} // namespace

QList<GoalEvaluation> evaluateGoals(const LinearResult &result, const QList<Goal> &goals, double *totalCost)
{
    const Evaluation evaluation = evaluateResult(result, goals);
    if (totalCost)
        *totalCost = evaluation.cost;
    return evaluation.goals;
}

// ---------------------------------------------------------------- setups

QJsonObject OptimizationSetup::toJson() const
{
    QJsonArray variableArray, goalArray;
    for (const DesignVariable &variable : variables)
        variableArray.append(variable.toJson());
    for (const Goal &goal : goals)
        goalArray.append(goal.toJson());
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("analysis"), analysis},
                       {QStringLiteral("method"), method == Method::NelderMead ? QStringLiteral("nelder-mead")
                                                                               : QStringLiteral("levenberg-marquardt")},
                       {QStringLiteral("maxEvaluations"), maxEvaluations},
                       {QStringLiteral("tolerance"), tolerance},
                       {QStringLiteral("variables"), variableArray},
                       {QStringLiteral("goals"), goalArray}};
}

OptimizationSetup OptimizationSetup::fromJson(const QJsonObject &object)
{
    OptimizationSetup setup;
    setup.id = object.value(QStringLiteral("id")).toString();
    setup.name = object.value(QStringLiteral("name")).toString();
    setup.analysis = object.value(QStringLiteral("analysis")).toString();
    setup.method = object.value(QStringLiteral("method")).toString() == QLatin1String("levenberg-marquardt")
                       ? Method::LevenbergMarquardt
                       : Method::NelderMead;
    setup.maxEvaluations = object.value(QStringLiteral("maxEvaluations")).toInt(500);
    setup.tolerance = object.value(QStringLiteral("tolerance")).toDouble(1e-6);
    for (const QJsonValue &value : object.value(QStringLiteral("variables")).toArray())
        setup.variables.append(DesignVariable::fromJson(value.toObject()));
    for (const QJsonValue &value : object.value(QStringLiteral("goals")).toArray())
        setup.goals.append(Goal::fromJson(value.toObject()));
    return setup;
}

QList<OptimizationSetup> optimizationSetups(const QJsonObject &simulation)
{
    QList<OptimizationSetup> result;
    for (const QJsonValue &value : simulation.value(OptimizationsKey).toArray())
        result.append(OptimizationSetup::fromJson(value.toObject()));
    return result;
}

QJsonObject withOptimizationSetup(const QJsonObject &simulation, OptimizationSetup setup, QString *assignedId)
{
    QJsonArray setups = simulation.value(OptimizationsKey).toArray();
    if (setup.id.isEmpty()) {
        int highest = 0;
        for (const QJsonValue &value : setups) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            if (id.startsWith(QLatin1Char('o')))
                highest = std::max(highest, id.mid(1).toInt());
        }
        setup.id = QStringLiteral("o%1").arg(highest + 1);
    }
    if (setup.name.isEmpty())
        setup.name = QStringLiteral("Optimisation");
    const QJsonObject fresh = setup.toJson();
    bool replaced = false;
    for (qsizetype i = 0; i < setups.size(); ++i) {
        QJsonObject existing = setups.at(i).toObject();
        if (existing.value(QStringLiteral("id")).toString() == setup.id) {
            for (auto it = fresh.begin(); it != fresh.end(); ++it)
                existing.insert(it.key(), it.value());
            setups.replace(i, existing);
            replaced = true;
            break;
        }
    }
    if (!replaced)
        setups.append(fresh);
    QJsonObject result = simulation;
    result.insert(OptimizationsKey, setups);
    if (assignedId)
        *assignedId = setup.id;
    return result;
}

// ---------------------------------------------------------------- running

LinearResult evaluateDesign(Project &snapshot, const QString &documentName, const LinearAnalysisSetup &analysis,
                            const QList<DesignVariable> &variables, const std::vector<double> &values,
                            TouchstoneCache *touchstones)
{
    Document *document = findDocument(snapshot, documentName);
    LinearResult result;
    if (!document) {
        result.errors.append(QStringLiteral("document %1 not found").arg(documentName));
        return result;
    }
    QString error;
    if (!applyDesignValues(snapshot, *document, variables, values, &error)) {
        result.errors.append(error);
        return result;
    }
    return runLinearAnalysis(snapshot, *document, analysis, touchstones);
}

OptimizationResult optimize(Project &snapshot, const QString &documentName, const OptimizationSetup &setup,
                            const std::function<void(const OptimizationProgress &)> &progress,
                            const std::atomic_bool *cancel)
{
    OptimizationResult result;
    Document *document = findDocument(snapshot, documentName);
    if (!document || !document->schematic()) {
        result.errors.append(QStringLiteral("document %1 not found").arg(documentName));
        return result;
    }
    LinearAnalysisSetup analysis;
    bool foundAnalysis = false;
    for (const LinearAnalysisSetup &candidate : linearAnalyses(document->schematic()->simulation())) {
        if (candidate.id == setup.analysis || (setup.analysis.isEmpty() && !foundAnalysis)) {
            analysis = candidate;
            foundAnalysis = true;
        }
    }
    if (!foundAnalysis)
        result.errors.append(QStringLiteral("the optimisation needs an S-parameter analysis"));
    if (setup.variables.isEmpty())
        result.errors.append(QStringLiteral("the optimisation has no variables"));
    if (setup.goals.isEmpty())
        result.errors.append(QStringLiteral("the optimisation has no goals"));

    std::vector<Mapping> mappings;
    for (const DesignVariable &variable : setup.variables) {
        if (!(variable.maximum > variable.minimum)) {
            result.errors.append(QStringLiteral("%1: the maximum must exceed the minimum").arg(variable.label()));
            continue;
        }
        const bool logarithmic = variable.logarithmic && variable.minimum > 0;
        if (variable.logarithmic && !logarithmic)
            result.warnings.append(QStringLiteral("%1: a logarithmic range needs a positive minimum; searching linearly")
                                       .arg(variable.label()));
        mappings.push_back(Mapping{variable.minimum, variable.maximum, logarithmic});
        QString error;
        const auto value = currentValue(snapshot, *document, variable, &error);
        if (!value) {
            result.errors.append(error);
            continue;
        }
        double start = *value;
        if (start < variable.minimum || start > variable.maximum) {
            result.warnings.append(QStringLiteral("%1 starts outside its range and is moved inside").arg(variable.label()));
            start = std::clamp(start, variable.minimum, variable.maximum);
        }
        result.initial.push_back(start);
    }
    if (!result.errors.isEmpty())
        return result;

    OptimizationSetup::Method method = setup.method;
    if (method == OptimizationSetup::Method::LevenbergMarquardt) {
        for (const Goal &goal : setup.goals) {
            if (goal.type == Goal::Type::Minimize || goal.type == Goal::Type::Maximize) {
                result.warnings.append(QStringLiteral("minimise/maximise goals need Nelder-Mead; it is used instead"));
                method = OptimizationSetup::Method::NelderMead;
                break;
            }
        }
    }

    TouchstoneCache cache;
    const size_t n = mappings.size();
    const auto values = [&](const std::vector<double> &t) {
        std::vector<double> v(n);
        for (size_t i = 0; i < n; ++i)
            v[i] = mappings[i].fromUnit(t[i]);
        return v;
    };
    std::vector<double> bestT;
    double bestCost = INFINITY;
    bool stop = false;
    const auto evaluate = [&](const std::vector<double> &t) -> Evaluation {
        const LinearResult linear = evaluateDesign(snapshot, documentName, analysis, setup.variables, values(t), &cache);
        Evaluation e = evaluateResult(linear, setup.goals);
        ++result.evaluations;
        if (e.cost < bestCost) {
            bestCost = e.cost;
            bestT = t;
        }
        if (progress)
            progress(OptimizationProgress{result.evaluations, e.cost, bestCost, values(bestT)});
        if ((cancel && cancel->load()) || result.evaluations >= setup.maxEvaluations)
            stop = true;
        return e;
    };

    std::vector<double> t0(n);
    for (size_t i = 0; i < n; ++i)
        t0[i] = mappings[i].toUnit(result.initial[i]);
    const Evaluation initial = evaluate(t0);
    result.initialCost = initial.cost;
    result.goalsInitial = initial.goals;
    if (!initial.ok) {
        result.errors.append(initial.goals.value(0).error.isEmpty() ? QStringLiteral("the initial simulation failed")
                                                                    : initial.goals.value(0).error);
        return result;
    }

    if (method == OptimizationSetup::Method::NelderMead) {
        // Simplex in the unit cube; points are clamped into it.
        std::vector<std::vector<double>> simplex{t0};
        std::vector<double> costs{initial.cost};
        for (size_t i = 0; i < n && !stop; ++i) {
            std::vector<double> point = t0;
            point[i] += point[i] < 0.9 ? 0.1 : -0.1;
            simplex.push_back(point);
            costs.push_back(evaluate(point).cost);
        }
        const auto clampPoint = [](std::vector<double> p) {
            for (double &x : p)
                x = std::clamp(x, 0.0, 1.0);
            return p;
        };
        while (!stop) {
            std::vector<size_t> order(simplex.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return costs[a] < costs[b]; });
            const size_t best = order.front(), worst = order.back(), second = order[order.size() - 2];
            if (costs[best] == 0.0
                || std::abs(costs[worst] - costs[best]) <= setup.tolerance * (std::abs(costs[best]) + 1e-12)) {
                double size = 0;
                for (const auto &p : simplex) {
                    for (size_t i = 0; i < n; ++i)
                        size = std::max(size, std::abs(p[i] - simplex[best][i]));
                }
                if (costs[best] == 0.0 || size < 1e-4) {
                    result.converged = true;
                    break;
                }
            }
            std::vector<double> centroid(n, 0.0);
            for (size_t k : order) {
                if (k == worst)
                    continue;
                for (size_t i = 0; i < n; ++i)
                    centroid[i] += simplex[k][i] / static_cast<double>(n);
            }
            const auto along = [&](double factor) {
                std::vector<double> p(n);
                for (size_t i = 0; i < n; ++i)
                    p[i] = centroid[i] + factor * (simplex[worst][i] - centroid[i]);
                return clampPoint(p);
            };
            const std::vector<double> reflected = along(-1.0);
            const double reflectedCost = evaluate(reflected).cost;
            if (stop)
                break;
            if (reflectedCost < costs[best]) {
                const std::vector<double> expanded = along(-2.0);
                const double expandedCost = evaluate(expanded).cost;
                if (expandedCost < reflectedCost) {
                    simplex[worst] = expanded;
                    costs[worst] = expandedCost;
                } else {
                    simplex[worst] = reflected;
                    costs[worst] = reflectedCost;
                }
            } else if (reflectedCost < costs[second]) {
                simplex[worst] = reflected;
                costs[worst] = reflectedCost;
            } else {
                const bool outside = reflectedCost < costs[worst];
                const std::vector<double> contracted = along(outside ? -0.5 : 0.5);
                const double contractedCost = evaluate(contracted).cost;
                if (contractedCost < std::min(reflectedCost, costs[worst])) {
                    simplex[worst] = contracted;
                    costs[worst] = contractedCost;
                } else {
                    // Shrink towards the best point.
                    for (size_t k = 0; k < simplex.size() && !stop; ++k) {
                        if (k == best)
                            continue;
                        for (size_t i = 0; i < n; ++i)
                            simplex[k][i] = simplex[best][i] + 0.5 * (simplex[k][i] - simplex[best][i]);
                        costs[k] = evaluate(simplex[k]).cost;
                    }
                }
            }
        }
    } else {
        // Levenberg-Marquardt on the residuals, forward-difference Jacobian.
        std::vector<double> t = t0;
        Evaluation current = initial;
        double lambda = 1e-2;
        const double h = 1e-4;
        int smallSteps = 0; // consecutive accepted steps with a tiny relative improvement
        int restarts = 0;
        while (!stop) {
            const Eigen::Index m = static_cast<Eigen::Index>(current.residuals.size());
            if (m == 0 || current.cost == 0.0) {
                result.converged = true;
                break;
            }
            Eigen::VectorXd r(m);
            for (Eigen::Index i = 0; i < m; ++i)
                r(i) = current.residuals[static_cast<size_t>(i)];
            // Central differences inside the box, one-sided at its faces: dB goals
            // around a deep match notch are steep, and forward differences mislead.
            Eigen::MatrixXd jacobian(m, static_cast<Eigen::Index>(n));
            const auto residualsAt = [&](const std::vector<double> &point) {
                const Evaluation e = evaluate(point);
                Eigen::VectorXd values(m);
                for (Eigen::Index i = 0; i < m; ++i)
                    values(i) = static_cast<size_t>(i) < e.residuals.size() ? e.residuals[static_cast<size_t>(i)] : r(i);
                return values;
            };
            for (size_t j = 0; j < n && !stop; ++j) {
                std::vector<double> plus = t, minus = t;
                plus[j] = std::min(1.0, t[j] + h);
                minus[j] = std::max(0.0, t[j] - h);
                const Eigen::VectorXd rPlus = plus[j] != t[j] ? residualsAt(plus) : r;
                const Eigen::VectorXd rMinus = minus[j] != t[j] ? residualsAt(minus) : r;
                jacobian.col(static_cast<Eigen::Index>(j)) = (rPlus - rMinus) / (plus[j] - minus[j]);
            }
            if (stop)
                break;
            // Variables held at a bound by the descent direction take no part in the step.
            {
                const Eigen::VectorXd g = jacobian.transpose() * r;
                for (size_t j = 0; j < n; ++j) {
                    const double gj = g(static_cast<Eigen::Index>(j));
                    if ((t[j] <= 0.0 && gj > 0) || (t[j] >= 1.0 && gj < 0))
                        jacobian.col(static_cast<Eigen::Index>(j)).setZero();
                }
            }
            const Eigen::MatrixXd jtj = jacobian.transpose() * jacobian;
            const Eigen::VectorXd gradient = jacobian.transpose() * r;
            if (gradient.norm() < 1e-12 * (1.0 + current.cost)) {
                result.converged = true; // a stationary point
                break;
            }
            bool improved = false;
            for (int attempt = 0; attempt < 30 && !stop; ++attempt) {
                Eigen::MatrixXd a = jtj;
                for (Eigen::Index i = 0; i < a.rows(); ++i)
                    a(i, i) += lambda * std::max(jtj(i, i), 1e-12);
                const Eigen::VectorXd delta = a.ldlt().solve(-gradient);
                std::vector<double> candidate = t;
                for (size_t i = 0; i < n; ++i)
                    candidate[i] = std::clamp(candidate[i] + delta(static_cast<Eigen::Index>(i)), 0.0, 1.0);
                const Evaluation e = evaluate(candidate);
                if (e.cost < current.cost) {
                    const double change = (current.cost - e.cost) / (std::abs(current.cost) + 1e-12);
                    t = candidate;
                    current = e;
                    lambda = std::max(lambda / 3.0, 1e-9);
                    improved = true;
                    smallSteps = change < setup.tolerance ? smallSteps + 1 : 0;
                    break;
                }
                lambda *= 4.0;
            }
            if (!improved && bestCost < current.cost * (1 - 1e-9) && restarts < 5) {
                // A probe found a better point than the step: continue from there.
                ++restarts;
                t = bestT;
                current = evaluate(t);
                lambda = 1e-2;
                smallSteps = 0;
                continue;
            }
            if (!improved) {
                // No descent even with heavy damping: a minimum of the residuals, or the
                // search is stuck at the edge of the variable ranges.
                result.converged = true;
                if (current.cost > 0)
                    result.warnings.append(QStringLiteral("Levenberg-Marquardt stopped at cost %1; the goals may be "
                                                          "out of reach, or try Nelder-Mead")
                                               .arg(current.cost, 0, 'g', 4));
                break;
            }
            if (smallSteps >= 3) {
                result.converged = true;
                break;
            }
        }
    }
    result.cancelled = cancel && cancel->load();

    // Best point, then snapped to standard values where requested.
    result.best = values(bestT);
    result.bestCost = bestCost;
    {
        const LinearResult linear = evaluateDesign(snapshot, documentName, analysis, setup.variables, result.best, &cache);
        result.goalsBest = evaluateResult(linear, setup.goals).goals;
    }
    result.snapped = result.best;
    bool anySeries = false;
    for (size_t i = 0; i < n; ++i) {
        if (const auto &series = setup.variables.at(static_cast<qsizetype>(i)).series) {
            result.snapped[i] = snapToSeries(result.best[i], *series);
            anySeries = true;
        }
    }
    const LinearResult snappedResult =
        evaluateDesign(snapshot, documentName, analysis, setup.variables, result.snapped, &cache);
    const Evaluation snapped = evaluateResult(snappedResult, setup.goals);
    result.snappedCost = snapped.cost;
    result.goalsSnapped = snapped.goals;
    if (anySeries && snapped.cost > result.bestCost * 1.5 + 1e-9)
        result.warnings.append(QStringLiteral("snapping to standard values raised the cost from %1 to %2")
                                   .arg(result.bestCost, 0, 'g', 4)
                                   .arg(snapped.cost, 0, 'g', 4));
    return result;
}

} // namespace lvd::rf
