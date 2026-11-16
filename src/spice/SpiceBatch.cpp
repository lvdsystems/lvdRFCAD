#include "spice/SpiceBatch.h"

#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "core/Connectivity.h"
#include "spice/WaveformMath.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace lvd::spice {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("lvd::spice::batch", text);
}

QString numberText(double value)
{
    return QString::number(value, 'g', 12);
}

std::optional<double> evaluateText(const QString &text, const Expression::Lookup &scope, QString *error)
{
    if (text.trimmed().isEmpty())
        return std::nullopt;
    return Expression::parse(text).evaluate(scope, error);
}

// The plot an analysis' results are read from by default.
QString mainPlotType(AnalysisKind kind)
{
    switch (kind) {
    case AnalysisKind::OperatingPoint: return QStringLiteral("op");
    case AnalysisKind::DcSweep: return QStringLiteral("dc");
    case AnalysisKind::Ac: return QStringLiteral("ac");
    case AnalysisKind::Transient:
    case AnalysisKind::Fourier: return QStringLiteral("tran");
    case AnalysisKind::Noise: return QStringLiteral("noise");
    case AnalysisKind::TransferFunction: return QStringLiteral("tf");
    case AnalysisKind::PoleZero: return QStringLiteral("pz");
    case AnalysisKind::Sensitivity: return QStringLiteral("sens");
    }
    return {};
}

void defaultExecute(QList<SpiceSimulation *> &simulations, const std::atomic_bool *cancel,
                    const std::function<void(int)> &progress)
{
    int done = 0;
    for (SpiceSimulation *simulation : simulations) {
        if (cancel && cancel->load())
            break;
        if (simulation->errors.isEmpty())
            executeSpiceAnalysis(*simulation, defaultEngine());
        if (progress)
            progress(++done);
    }
}

Document *findDocument(Project &project, const QString &name)
{
    return project.document(name);
}

const PlacedComponent *componentByReference(const Schematic &schematic, const QString &reference)
{
    return schematic.componentByReference(reference);
}

// One batch: one prepared simulation per point, executed, then measured.
BatchResult runPoints(Project &snapshot, const QString &documentName, const SpiceAnalysisSetup &analysis,
                      const QList<SpiceTarget> &targets, const QList<std::vector<double>> &points, const QStringList &labels,
                      const QList<SpiceMeasure> &measures, const BatchOptions &options)
{
    BatchResult result;
    result.targets = targets;
    result.measures = measures;
    Document *document = findDocument(snapshot, documentName);
    if (!document || !document->schematic()) {
        result.errors << tr("Document %1 not found.").arg(documentName);
        return result;
    }

    // Netlisting reads the snapshot, so it happens here, point by point.
    std::vector<std::unique_ptr<SpiceSimulation>> simulations;
    for (qsizetype i = 0; i < points.size(); ++i) {
        if (options.cancel && options.cancel->load()) {
            result.cancelled = true;
            return result;
        }
        SpiceAnalysisSetup setup = analysis;
        QString error;
        auto simulation = std::make_unique<SpiceSimulation>();
        if (!applyTargets(snapshot, *document, setup, targets, points.at(i), &error)) {
            result.errors << error;
            return result;
        }
        *simulation = prepareSpiceAnalysis(snapshot, *document, setup);
        simulations.push_back(std::move(simulation));
    }
    if (!simulations.empty() && !simulations.front()->errors.isEmpty()) {
        // Netlist problems are the same for every point.
        result.errors = simulations.front()->errors;
        return result;
    }

    QList<SpiceSimulation *> list;
    for (const auto &simulation : simulations)
        list << simulation.get();
    const int total = int(list.size());
    const auto progress = [&](int done) {
        if (options.progress)
            options.progress(BatchProgress{done, total, tr("Simulating %1 of %2").arg(done).arg(total)});
    };
    if (options.execute)
        options.execute(list, options.cancel, progress);
    else
        defaultExecute(list, options.cancel, progress);
    result.cancelled = options.cancel && options.cancel->load();

    QStringList variableErrors;
    const Expression::Lookup scope = sheetVariables(*document, &variableErrors);
    for (qsizetype i = 0; i < points.size(); ++i) {
        BatchRun run;
        run.label = labels.value(i);
        run.values = points.at(i);
        SpiceSimulation &simulation = *simulations[std::size_t(i)];
        if (!simulation.ok() && !simulation.run.cancelled && simulation.run.plots.isEmpty() == false)
            result.warnings << QStringLiteral("%1: %2").arg(run.label, simulation.errors.join(QStringLiteral("; ")));
        else if (simulation.run.plots.isEmpty() && !result.cancelled)
            result.warnings << QStringLiteral("%1: %2").arg(run.label, simulation.errors.isEmpty()
                                                                         ? tr("no result")
                                                                         : simulation.errors.join(QStringLiteral("; ")));
        for (const SpiceMeasure &measure : measures) {
            QString error;
            const auto value = simulation.run.plots.isEmpty() ? std::nullopt
                                                              : evaluateMeasure(measure, simulation, scope, &error);
            run.measures.push_back(value ? *value : NAN);
            run.measureErrors << (value ? QString() : error);
        }
        run.simulation = std::make_shared<const SpiceSimulation>(std::move(simulation));
        result.runs << run;
    }
    return result;
}

QString targetLabel(const QList<SpiceTarget> &targets, const std::vector<double> &values)
{
    QStringList parts;
    for (qsizetype i = 0; i < targets.size() && std::size_t(i) < values.size(); ++i) {
        const SpiceTarget &target = targets.at(i);
        parts << QStringLiteral("%1 = %2").arg(target.text,
                                               formatEngineering(values[std::size_t(i)],
                                                                 target.isTemperature() ? QStringLiteral("°C") : QString()));
    }
    return parts.join(QStringLiteral(", "));
}

} // namespace

// ---------------------------------------------------------------- measures

QStringList measureFunctions()
{
    return {QStringLiteral("value"),     QStringLiteral("max"),      QStringLiteral("min"),     QStringLiteral("mean"),
            QStringLiteral("rms"),       QStringLiteral("pkpk"),     QStringLiteral("risetime"), QStringLiteral("falltime"),
            QStringLiteral("frequency"), QStringLiteral("period"),   QStringLiteral("duty"),    QStringLiteral("overshoot"),
            QStringLiteral("settling"),  QStringLiteral("crossing"), QStringLiteral("thd")};
}

QString SpiceMeasure::label() const
{
    if (!name.isEmpty())
        return name;
    if (function == QLatin1String("value"))
        return at.isEmpty() ? expression : QStringLiteral("%1 @ %2").arg(expression, at);
    return QStringLiteral("%1(%2)").arg(function, expression);
}

QJsonObject SpiceMeasure::toJson() const
{
    QJsonObject object{{QStringLiteral("expression"), expression}, {QStringLiteral("function"), function}};
    const auto put = [&](const char *key, const QString &value) {
        if (!value.isEmpty())
            object.insert(QLatin1String(key), value);
    };
    put("name", name);
    put("at", at);
    put("from", from);
    put("to", to);
    put("level", level);
    put("plot", plot);
    return object;
}

SpiceMeasure SpiceMeasure::fromJson(const QJsonObject &object)
{
    SpiceMeasure m;
    m.name = object.value(QStringLiteral("name")).toString();
    m.expression = object.value(QStringLiteral("expression")).toString();
    m.function = object.value(QStringLiteral("function")).toString(QStringLiteral("value"));
    m.at = object.value(QStringLiteral("at")).toString();
    m.from = object.value(QStringLiteral("from")).toString();
    m.to = object.value(QStringLiteral("to")).toString();
    m.level = object.value(QStringLiteral("level")).toString();
    m.plot = object.value(QStringLiteral("plot")).toString();
    return m;
}

std::optional<double> evaluateMeasure(const SpiceMeasure &measure, const SpiceSimulation &simulation,
                                      const Expression::Lookup &scope, QString *error)
{
    const auto fail = [&](const QString &message) -> std::optional<double> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    // The waveform: from the requested plot, else the analysis' main plot,
    // else the first plot that knows the expression (noise totals live in
    // the second noise plot).
    const QString wanted = measure.plot.isEmpty() ? mainPlotType(simulation.analysis.kind) : measure.plot;
    QList<const SpicePlot *> candidates;
    for (const SpicePlot &plot : simulation.run.plots)
        if (plot.type == wanted)
            candidates << &plot;
    if (measure.plot.isEmpty())
        for (const SpicePlot &plot : simulation.run.plots)
            if (!candidates.contains(&plot))
                candidates << &plot;
    if (candidates.isEmpty())
        return fail(tr("No result to measure."));
    std::optional<Waveform> waveform;
    QString firstError;
    for (const SpicePlot *plot : std::as_const(candidates)) {
        QString evaluationError;
        waveform = evaluateOnPlot(measure.expression, *plot, &simulation.deck, &evaluationError);
        if (waveform)
            break;
        if (firstError.isEmpty())
            firstError = evaluationError;
    }
    if (!waveform)
        return fail(firstError);
    if (waveform->y.empty())
        return fail(tr("%1 has no values.").arg(measure.expression));

    XWindow window;
    QString windowError;
    if (const auto from = evaluateText(measure.from, scope, &windowError))
        window.from = *from;
    if (const auto to = evaluateText(measure.to, scope, &windowError))
        window.to = *to;
    if (!windowError.isEmpty())
        return fail(windowError);

    const QString f = measure.function.toLower();
    QString e;
    std::optional<double> value;
    if (f == QLatin1String("value")) {
        if (measure.at.trimmed().isEmpty()) {
            value = waveform->y.front().real();
        } else {
            const auto x = evaluateText(measure.at, scope, &e);
            if (!x)
                return fail(e);
            value = valueAt(*waveform, *x, &e);
        }
    } else if (f == QLatin1String("max")) {
        if (const auto m = maximum(*waveform, window, &e))
            value = m->value;
    } else if (f == QLatin1String("min")) {
        if (const auto m = minimum(*waveform, window, &e))
            value = m->value;
    } else if (f == QLatin1String("mean")) {
        value = mean(*waveform, window, &e);
    } else if (f == QLatin1String("rms")) {
        value = rms(*waveform, window, &e);
    } else if (f == QLatin1String("pkpk")) {
        value = peakToPeak(*waveform, window, &e);
    } else if (f == QLatin1String("risetime")) {
        value = riseTime(*waveform, window, 0.1, 0.9, &e);
    } else if (f == QLatin1String("falltime")) {
        value = fallTime(*waveform, window, 0.1, 0.9, &e);
    } else if (f == QLatin1String("frequency")) {
        value = frequency(*waveform, window, &e);
    } else if (f == QLatin1String("period")) {
        value = period(*waveform, window, &e);
    } else if (f == QLatin1String("duty")) {
        value = dutyCycle(*waveform, window, &e);
    } else if (f == QLatin1String("overshoot")) {
        value = overshoot(*waveform, window, &e);
    } else if (f == QLatin1String("settling")) {
        value = settlingTime(*waveform, 2.0, window, &e);
    } else if (f == QLatin1String("crossing")) {
        const auto level = evaluateText(measure.level, scope, &e);
        if (!level)
            return fail(e.isEmpty() ? tr("crossing needs a level") : e);
        value = crossing(*waveform, *level, Edge::Either, 1, window, &e);
    } else if (f == QLatin1String("thd")) {
        value = thd(*waveform, 0, 9, 1, window, &e);
    } else {
        return fail(tr("Unknown measure function \"%1\".").arg(measure.function));
    }
    if (!value)
        return fail(e.isEmpty() ? tr("%1 failed").arg(measure.label()) : e);
    return value;
}

// ---------------------------------------------------------------- targets

bool applyTargets(Project &snapshot, Document &document, SpiceAnalysisSetup &analysis, const QList<SpiceTarget> &targets,
                  const std::vector<double> &values, QString *error)
{
    Schematic *schematic = document.schematic();
    if (!schematic)
        return false;
    QList<Variable> variables = schematic->variables();
    bool variablesChanged = false;
    for (qsizetype i = 0; i < targets.size() && std::size_t(i) < values.size(); ++i) {
        const SpiceTarget &target = targets.at(i);
        const QString text = numberText(values[std::size_t(i)]);
        if (target.isTemperature()) {
            analysis.set(QStringLiteral("temperature"), text);
        } else if (target.isParameter()) {
            const PlacedComponent *component = componentByReference(*schematic, target.component());
            if (!component) {
                if (error)
                    *error = tr("There is no component %1.").arg(target.component());
                return false;
            }
            schematic->setParameter(component->id, target.parameter(), text);
        } else {
            bool found = false;
            for (Variable &variable : variables)
                if (variable.name == target.text) {
                    variable.value = text;
                    found = true;
                }
            if (!found) {
                if (error)
                    *error = tr("There is no variable %1.").arg(target.text);
                return false;
            }
            variablesChanged = true;
        }
    }
    if (variablesChanged)
        schematic->setVariables(variables);
    if (snapshot.isSnapshot())
        schematic->undoStack()->clear();
    return true;
}

std::optional<double> targetValue(const Project &project, const Document &document, const SpiceAnalysisSetup &analysis,
                                  const SpiceTarget &target, QString *error)
{
    const Schematic *schematic = document.schematic();
    if (!schematic)
        return std::nullopt;
    const Expression::Lookup scope = sheetVariables(document);
    if (target.isTemperature()) {
        const QString text = analysis.text(QStringLiteral("temperature"));
        return text.trimmed().isEmpty() ? 27.0 : evaluateText(text, scope, error);
    }
    if (target.isParameter()) {
        const PlacedComponent *component = componentByReference(*schematic, target.component());
        if (!component) {
            if (error)
                *error = tr("There is no component %1.").arg(target.component());
            return std::nullopt;
        }
        const DefinitionPtr definition = project.resolve(*schematic, component->definition).definition;
        if (!definition || !definition->parameter(target.parameter())) {
            if (error)
                *error = tr("%1 has no parameter %2.").arg(target.component(), target.parameter());
            return std::nullopt;
        }
        return evaluateText(parameterText(*component, *definition, target.parameter()), scope, error);
    }
    const auto value = scope(target.text);
    if (!value && error)
        *error = tr("There is no variable %1.").arg(target.text);
    return value;
}

// ---------------------------------------------------------------- sweeps

QJsonObject SweepAxis::toJson() const
{
    QJsonObject object{{QStringLiteral("target"), target.text}, {QStringLiteral("mode"), mode}};
    if (mode == QLatin1String("list")) {
        object.insert(QStringLiteral("list"), QJsonArray::fromStringList(list));
    } else {
        object.insert(QStringLiteral("start"), start);
        object.insert(QStringLiteral("stop"), stop);
        object.insert(QStringLiteral("points"), points);
    }
    return object;
}

SweepAxis SweepAxis::fromJson(const QJsonObject &object)
{
    SweepAxis axis;
    axis.target.text = object.value(QStringLiteral("target")).toString();
    axis.mode = object.value(QStringLiteral("mode")).toString(QStringLiteral("lin"));
    axis.start = object.value(QStringLiteral("start")).toVariant().toString();
    axis.stop = object.value(QStringLiteral("stop")).toVariant().toString();
    axis.points = object.value(QStringLiteral("points")).toVariant().toString();
    for (const QJsonValue &value : object.value(QStringLiteral("list")).toArray())
        axis.list << value.toVariant().toString();
    return axis;
}

std::optional<std::vector<double>> sweepValues(const SweepAxis &axis, const Expression::Lookup &scope, QString *error)
{
    const auto fail = [&](const QString &message) -> std::optional<std::vector<double>> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    std::vector<double> values;
    if (axis.mode == QLatin1String("list")) {
        for (const QString &text : axis.list) {
            QString e;
            const auto value = evaluateText(text, scope, &e);
            if (!value)
                return fail(tr("%1: %2").arg(text, e));
            values.push_back(*value);
        }
        if (values.empty())
            return fail(tr("The list of values is empty."));
        return values;
    }
    QString e;
    const auto start = evaluateText(axis.start, scope, &e);
    const auto stop = evaluateText(axis.stop, scope, &e);
    const auto points = evaluateText(axis.points, scope, &e);
    if (!start || !stop || !points)
        return fail(e.isEmpty() ? tr("Start, stop and points are needed.") : e);
    const int n = int(std::lround(*points));
    if (n < 1 || n > 10000)
        return fail(tr("The number of points must be between 1 and 10000."));
    if (axis.mode == QLatin1String("log")) {
        if (*start <= 0 || *stop <= 0)
            return fail(tr("A logarithmic sweep needs positive limits."));
        const double decades = std::log10(*stop / *start);
        const int count = std::max(2, int(std::lround(std::abs(decades) * n)) + 1);
        for (int i = 0; i < count; ++i)
            values.push_back(*start * std::pow(10.0, decades * i / (count - 1)));
        return values;
    }
    if (n == 1)
        return std::vector<double>{*start};
    for (int i = 0; i < n; ++i)
        values.push_back(*start + (*stop - *start) * i / (n - 1));
    return values;
}

BatchResult runParameterSweep(Project &snapshot, const QString &documentName, const SpiceAnalysisSetup &analysis,
                              const QList<SweepAxis> &axes, const QList<SpiceMeasure> &measures, const BatchOptions &options)
{
    BatchResult result;
    Document *document = findDocument(snapshot, documentName);
    if (!document) {
        result.errors << tr("Document %1 not found.").arg(documentName);
        return result;
    }
    if (axes.isEmpty() || axes.size() > 3) {
        result.errors << tr("A sweep has one to three swept values.");
        return result;
    }
    const Expression::Lookup scope = sheetVariables(*document);
    QList<SpiceTarget> targets;
    std::vector<std::vector<double>> axisValues;
    for (const SweepAxis &axis : axes) {
        QString error;
        const auto values = sweepValues(axis, scope, &error);
        if (!values) {
            result.errors << QStringLiteral("%1: %2").arg(axis.target.text, error);
            return result;
        }
        if (!targetValue(snapshot, *document, analysis, axis.target, &error)) {
            result.errors << error;
            return result;
        }
        targets << axis.target;
        axisValues.push_back(*values);
    }
    // Cartesian product, first axis outermost.
    QList<std::vector<double>> points{{}};
    for (const std::vector<double> &values : axisValues) {
        QList<std::vector<double>> next;
        for (const std::vector<double> &prefix : std::as_const(points))
            for (double value : values) {
                std::vector<double> point = prefix;
                point.push_back(value);
                next << point;
            }
        points = next;
    }
    if (points.size() > 10000) {
        result.errors << tr("The sweep has %1 points; at most 10000 are allowed.").arg(points.size());
        return result;
    }
    QStringList labels;
    for (const std::vector<double> &point : std::as_const(points))
        labels << targetLabel(targets, point);
    return runPoints(snapshot, documentName, analysis, targets, points, labels, measures, options);
}

// ---------------------------------------------------------------- Monte Carlo

QJsonObject Tolerance::toJson() const
{
    return {{QStringLiteral("target"), target.text},
            {QStringLiteral("tolerance"), relative},
            {QStringLiteral("distribution"), distribution == Distribution::Uniform ? QStringLiteral("uniform")
                                                                                   : QStringLiteral("gaussian")}};
}

Tolerance Tolerance::fromJson(const QJsonObject &object)
{
    Tolerance t;
    t.target.text = object.value(QStringLiteral("target")).toString();
    t.relative = object.value(QStringLiteral("tolerance")).toDouble(0.05);
    t.distribution = object.value(QStringLiteral("distribution")).toString() == QLatin1String("uniform")
                         ? Distribution::Uniform
                         : Distribution::Gaussian;
    return t;
}

QJsonObject MonteCarloSetup::toJson() const
{
    QJsonArray toleranceArray, measureArray;
    for (const Tolerance &t : tolerances)
        toleranceArray.append(t.toJson());
    for (const SpiceMeasure &m : measures)
        measureArray.append(m.toJson());
    return {{QStringLiteral("tolerances"), toleranceArray},
            {QStringLiteral("runs"), runs},
            {QStringLiteral("seed"), qint64(seed)},
            {QStringLiteral("measures"), measureArray}};
}

MonteCarloSetup MonteCarloSetup::fromJson(const QJsonObject &object)
{
    MonteCarloSetup setup;
    for (const QJsonValue &value : object.value(QStringLiteral("tolerances")).toArray())
        setup.tolerances << Tolerance::fromJson(value.toObject());
    setup.runs = object.value(QStringLiteral("runs")).toInt(100);
    setup.seed = quint32(object.value(QStringLiteral("seed")).toInteger(1));
    for (const QJsonValue &value : object.value(QStringLiteral("measures")).toArray())
        setup.measures << SpiceMeasure::fromJson(value.toObject());
    return setup;
}

QList<Tolerance> expandTolerances(const Project &project, const Document &document, const QList<Tolerance> &tolerances,
                                  QStringList *warnings)
{
    QList<Tolerance> expanded;
    const Schematic *schematic = document.schematic();
    if (!schematic)
        return expanded;
    for (const Tolerance &tolerance : tolerances) {
        if (!tolerance.target.isParameter() || !tolerance.target.component().contains(QLatin1Char('*'))) {
            expanded << tolerance;
            continue;
        }
        const QRegularExpression pattern(QRegularExpression::wildcardToRegularExpression(tolerance.target.component()),
                                         QRegularExpression::CaseInsensitiveOption);
        int matched = 0;
        QList<const PlacedComponent *> components;
        for (const PlacedComponent &component : schematic->components())
            components << &component;
        std::sort(components.begin(), components.end(),
                  [](const PlacedComponent *a, const PlacedComponent *b) { return a->reference < b->reference; });
        for (const PlacedComponent *component : std::as_const(components)) {
            if (!pattern.match(component->reference).hasMatch())
                continue;
            const DefinitionPtr definition = project.resolve(*schematic, component->definition).definition;
            if (!definition || !definition->parameter(tolerance.target.parameter()))
                continue;
            Tolerance one = tolerance;
            one.target.text = component->reference + QLatin1Char('.') + tolerance.target.parameter();
            expanded << one;
            ++matched;
        }
        if (matched == 0 && warnings)
            *warnings << tr("%1 matches no component.").arg(tolerance.target.text);
    }
    return expanded;
}

BatchResult runMonteCarlo(Project &snapshot, const QString &documentName, const SpiceAnalysisSetup &analysis,
                          const MonteCarloSetup &setup, const BatchOptions &options)
{
    BatchResult result;
    Document *document = findDocument(snapshot, documentName);
    if (!document) {
        result.errors << tr("Document %1 not found.").arg(documentName);
        return result;
    }
    QStringList warnings;
    const QList<Tolerance> tolerances = expandTolerances(snapshot, *document, setup.tolerances, &warnings);
    if (tolerances.isEmpty()) {
        result.errors << tr("No toleranced values.");
        result.warnings = warnings;
        return result;
    }
    if (setup.runs < 1 || setup.runs > 100000) {
        result.errors << tr("The number of runs must be between 1 and 100000.");
        return result;
    }
    QList<SpiceTarget> targets;
    std::vector<double> nominal;
    for (const Tolerance &tolerance : tolerances) {
        QString error;
        const auto value = targetValue(snapshot, *document, analysis, tolerance.target, &error);
        if (!value) {
            result.errors << error;
            return result;
        }
        targets << tolerance.target;
        nominal.push_back(*value);
    }
    std::mt19937 random(setup.seed);
    std::normal_distribution<double> normal(0.0, 1.0);
    std::uniform_real_distribution<double> uniform(-1.0, 1.0);
    QList<std::vector<double>> points{nominal};
    QStringList labels{tr("nominal")};
    for (int run = 1; run <= setup.runs; ++run) {
        std::vector<double> point = nominal;
        for (qsizetype i = 0; i < tolerances.size(); ++i) {
            const Tolerance &tolerance = tolerances.at(i);
            const double deviation = tolerance.distribution == Tolerance::Distribution::Uniform
                                         ? uniform(random)
                                         : normal(random) / 3.0; // 3 sigma = the tolerance
            // A temperature tolerance is an absolute spread in °C.
            if (tolerance.target.isTemperature())
                point[std::size_t(i)] = nominal[std::size_t(i)] + deviation * tolerance.relative;
            else
                point[std::size_t(i)] = nominal[std::size_t(i)] * (1.0 + deviation * tolerance.relative);
        }
        points << point;
        labels << tr("run %1").arg(run);
    }
    result = runPoints(snapshot, documentName, analysis, targets, points, labels, setup.measures, options);
    result.warnings = warnings + result.warnings;
    return result;
}

QList<MeasureStatistics> statistics(const BatchResult &result)
{
    QList<MeasureStatistics> list;
    for (qsizetype m = 0; m < result.measures.size(); ++m) {
        MeasureStatistics s;
        s.name = result.measures.at(m).label();
        std::vector<double> values;
        for (qsizetype r = 0; r < result.runs.size(); ++r) {
            const double value = result.runs.at(r).measures.at(std::size_t(m));
            if (r == 0)
                s.nominal = value;
            if (std::isfinite(value))
                values.push_back(value);
        }
        s.count = int(values.size());
        if (!values.empty()) {
            s.mean = std::accumulate(values.begin(), values.end(), 0.0) / double(values.size());
            double sum = 0;
            for (double v : values)
                sum += (v - s.mean) * (v - s.mean);
            s.sigma = values.size() > 1 ? std::sqrt(sum / double(values.size() - 1)) : 0.0;
            s.minimum = *std::min_element(values.begin(), values.end());
            s.maximum = *std::max_element(values.begin(), values.end());
        }
        list << s;
    }
    return list;
}

// ---------------------------------------------------------------- worst case

BatchResult runWorstCase(Project &snapshot, const QString &documentName, const SpiceAnalysisSetup &analysis,
                         const QList<Tolerance> &tolerancesIn, const QList<SpiceMeasure> &measures,
                         QList<WorstCaseMeasure> *summary, const BatchOptions &options)
{
    BatchResult result;
    Document *document = findDocument(snapshot, documentName);
    if (!document) {
        result.errors << tr("Document %1 not found.").arg(documentName);
        return result;
    }
    QStringList warnings;
    const QList<Tolerance> tolerances = expandTolerances(snapshot, *document, tolerancesIn, &warnings);
    if (tolerances.isEmpty() || measures.isEmpty()) {
        result.errors << tr("Worst case needs toleranced values and measures.");
        return result;
    }
    QList<SpiceTarget> targets;
    std::vector<double> nominal;
    for (const Tolerance &tolerance : tolerances) {
        QString error;
        const auto value = targetValue(snapshot, *document, analysis, tolerance.target, &error);
        if (!value) {
            result.errors << error;
            return result;
        }
        targets << tolerance.target;
        nominal.push_back(*value);
    }
    const auto limit = [&](qsizetype i, int sign) {
        const Tolerance &tolerance = tolerances.at(i);
        return tolerance.target.isTemperature() ? nominal[std::size_t(i)] + sign * tolerance.relative
                                                : nominal[std::size_t(i)] * (1.0 + sign * tolerance.relative);
    };

    // Stage 1: nominal and one-at-a-time sensitivities.
    QList<std::vector<double>> points{nominal};
    QStringList labels{tr("nominal")};
    for (qsizetype i = 0; i < tolerances.size(); ++i) {
        std::vector<double> point = nominal;
        point[std::size_t(i)] = limit(i, +1);
        points << point;
        labels << tr("%1 high").arg(targets.at(i).text);
    }
    BatchResult sensitivity = runPoints(snapshot, documentName, analysis, targets, points, labels, measures, options);
    if (!sensitivity.ok() || sensitivity.cancelled)
        return sensitivity;

    // Stage 2: the corners per measure.
    QList<std::vector<double>> corners;
    QStringList cornerLabels;
    QList<WorstCaseMeasure> found;
    for (qsizetype m = 0; m < measures.size(); ++m) {
        WorstCaseMeasure w;
        w.name = measures.at(m).label();
        w.nominal = sensitivity.runs.at(0).measures.at(std::size_t(m));
        std::vector<double> low = nominal, high = nominal;
        for (qsizetype i = 0; i < tolerances.size(); ++i) {
            const double delta = sensitivity.runs.at(i + 1).measures.at(std::size_t(m)) - w.nominal;
            const int up = std::isfinite(delta) && delta < 0 ? -1 : 1;
            w.maximumCorner.push_back(up);
            w.minimumCorner.push_back(-up);
            high[std::size_t(i)] = limit(i, up);
            low[std::size_t(i)] = limit(i, -up);
        }
        corners << low << high;
        cornerLabels << tr("%1 min").arg(w.name) << tr("%1 max").arg(w.name);
        found << w;
    }
    BatchResult cornerRuns = runPoints(snapshot, documentName, analysis, targets, corners, cornerLabels, measures, options);
    result = sensitivity;
    result.runs << cornerRuns.runs;
    result.warnings = warnings + sensitivity.warnings + cornerRuns.warnings;
    result.errors << cornerRuns.errors;
    result.cancelled = cornerRuns.cancelled;
    for (qsizetype m = 0; m < found.size() && 2 * m + 1 < cornerRuns.runs.size(); ++m) {
        const double a = cornerRuns.runs.at(2 * m).measures.at(std::size_t(m));
        const double b = cornerRuns.runs.at(2 * m + 1).measures.at(std::size_t(m));
        // The sensitivity directions are linear estimates; the corners are
        // checked, and the nominal is kept if a corner does not go past it.
        found[m].minimum = std::min({a, b, found[m].nominal});
        found[m].maximum = std::max({a, b, found[m].nominal});
    }
    if (summary)
        *summary = found;
    return result;
}

// ---------------------------------------------------------------- optimisation

QString SpiceGoal::label() const
{
    const QString m = measure.label();
    switch (type) {
    case Type::Below: return QStringLiteral("%1 < %2").arg(m).arg(target);
    case Type::Above: return QStringLiteral("%1 > %2").arg(m).arg(target);
    case Type::Equal: return QStringLiteral("%1 = %2").arg(m).arg(target);
    case Type::Minimize: return tr("minimise %1").arg(m);
    case Type::Maximize: return tr("maximise %1").arg(m);
    }
    return m;
}

QJsonObject SpiceGoal::toJson() const
{
    static const char *names[] = {"below", "above", "equal", "minimize", "maximize"};
    return {{QStringLiteral("measure"), measure.toJson()},
            {QStringLiteral("type"), QLatin1String(names[int(type)])},
            {QStringLiteral("target"), target},
            {QStringLiteral("weight"), weight}};
}

SpiceGoal SpiceGoal::fromJson(const QJsonObject &object)
{
    SpiceGoal goal;
    goal.measure = SpiceMeasure::fromJson(object.value(QStringLiteral("measure")).toObject());
    const QString type = object.value(QStringLiteral("type")).toString();
    goal.type = type == QLatin1String("above")      ? Type::Above
                : type == QLatin1String("equal")    ? Type::Equal
                : type == QLatin1String("minimize") ? Type::Minimize
                : type == QLatin1String("maximize") ? Type::Maximize
                                                    : Type::Below;
    goal.target = object.value(QStringLiteral("target")).toDouble();
    goal.weight = object.value(QStringLiteral("weight")).toDouble(1.0);
    return goal;
}

QJsonObject SpiceOptimizationVariable::toJson() const
{
    QJsonObject object{{QStringLiteral("target"), target.text},
                       {QStringLiteral("min"), minimum},
                       {QStringLiteral("max"), maximum},
                       {QStringLiteral("log"), logarithmic}};
    if (series)
        object.insert(QStringLiteral("series"), seriesName(*series));
    return object;
}

SpiceOptimizationVariable SpiceOptimizationVariable::fromJson(const QJsonObject &object)
{
    SpiceOptimizationVariable v;
    v.target.text = object.value(QStringLiteral("target")).toString();
    v.minimum = object.value(QStringLiteral("min")).toDouble();
    v.maximum = object.value(QStringLiteral("max")).toDouble();
    v.logarithmic = object.value(QStringLiteral("log")).toBool(true);
    v.series = seriesFromName(object.value(QStringLiteral("series")).toString());
    return v;
}

QJsonObject SpiceOptimizationSetup::toJson() const
{
    QJsonArray variableArray, goalArray;
    for (const SpiceOptimizationVariable &v : variables)
        variableArray.append(v.toJson());
    for (const SpiceGoal &g : goals)
        goalArray.append(g.toJson());
    return {{QStringLiteral("variables"), variableArray},
            {QStringLiteral("goals"), goalArray},
            {QStringLiteral("maxEvaluations"), maxEvaluations},
            {QStringLiteral("tolerance"), tolerance}};
}

SpiceOptimizationSetup SpiceOptimizationSetup::fromJson(const QJsonObject &object)
{
    SpiceOptimizationSetup setup;
    for (const QJsonValue &value : object.value(QStringLiteral("variables")).toArray())
        setup.variables << SpiceOptimizationVariable::fromJson(value.toObject());
    for (const QJsonValue &value : object.value(QStringLiteral("goals")).toArray())
        setup.goals << SpiceGoal::fromJson(value.toObject());
    setup.maxEvaluations = object.value(QStringLiteral("maxEvaluations")).toInt(200);
    setup.tolerance = object.value(QStringLiteral("tolerance")).toDouble(1e-4);
    return setup;
}

double goalCost(const QList<SpiceGoal> &goals, const std::vector<double> &values, std::vector<bool> *met)
{
    double cost = 0;
    if (met)
        met->assign(std::size_t(goals.size()), false);
    for (qsizetype i = 0; i < goals.size(); ++i) {
        const SpiceGoal &goal = goals.at(i);
        const double value = std::size_t(i) < values.size() ? values[std::size_t(i)] : NAN;
        if (!std::isfinite(value)) {
            cost += 1e6 * goal.weight; // a failed measure: far from any goal
            continue;
        }
        const double scale = std::abs(goal.target) > 0 ? std::abs(goal.target) : 1.0;
        double violation = 0;
        bool ok = true;
        switch (goal.type) {
        case SpiceGoal::Type::Below:
            violation = std::max(0.0, value - goal.target) / scale;
            ok = value <= goal.target;
            break;
        case SpiceGoal::Type::Above:
            violation = std::max(0.0, goal.target - value) / scale;
            ok = value >= goal.target;
            break;
        case SpiceGoal::Type::Equal:
            violation = (value - goal.target) / scale;
            ok = std::abs(violation) <= 0.01;
            break;
        case SpiceGoal::Type::Minimize:
            cost += goal.weight * value;
            break;
        case SpiceGoal::Type::Maximize:
            cost -= goal.weight * value;
            break;
        }
        cost += goal.weight * violation * violation;
        if (met)
            (*met)[std::size_t(i)] = ok;
    }
    return cost;
}

SpiceOptimizationResult optimizeSpice(Project &snapshot, const QString &documentName, const SpiceAnalysisSetup &analysis,
                                      const SpiceOptimizationSetup &setup, const BatchOptions &options)
{
    SpiceOptimizationResult result;
    Document *document = findDocument(snapshot, documentName);
    if (!document) {
        result.errors << tr("Document %1 not found.").arg(documentName);
        return result;
    }
    if (setup.variables.isEmpty())
        result.errors << tr("The optimisation has no variables.");
    if (setup.goals.isEmpty())
        result.errors << tr("The optimisation has no goals.");
    const std::size_t n = std::size_t(setup.variables.size());
    QList<SpiceTarget> targets;
    std::vector<bool> logarithmic;
    for (const SpiceOptimizationVariable &variable : setup.variables) {
        targets << variable.target;
        if (!(variable.maximum > variable.minimum))
            result.errors << tr("%1: the maximum must exceed the minimum.").arg(variable.target.text);
        logarithmic.push_back(variable.logarithmic && variable.minimum > 0);
        QString error;
        const auto value = targetValue(snapshot, *document, analysis, variable.target, &error);
        if (!value)
            result.errors << error;
        else
            result.initial.push_back(std::clamp(*value, variable.minimum, variable.maximum));
    }
    if (!result.errors.isEmpty())
        return result;

    // Unit cube <-> values.
    const auto toValues = [&](const std::vector<double> &u) {
        std::vector<double> v(n);
        for (std::size_t i = 0; i < n; ++i) {
            const SpiceOptimizationVariable &variable = setup.variables.at(qsizetype(i));
            const double t = std::clamp(u[i], 0.0, 1.0);
            v[i] = logarithmic[i] ? variable.minimum * std::pow(variable.maximum / variable.minimum, t)
                                  : variable.minimum + (variable.maximum - variable.minimum) * t;
        }
        return v;
    };
    const auto toUnit = [&](const std::vector<double> &v) {
        std::vector<double> u(n);
        for (std::size_t i = 0; i < n; ++i) {
            const SpiceOptimizationVariable &variable = setup.variables.at(qsizetype(i));
            u[i] = logarithmic[i] ? std::log(v[i] / variable.minimum) / std::log(variable.maximum / variable.minimum)
                                  : (v[i] - variable.minimum) / (variable.maximum - variable.minimum);
        }
        return u;
    };
    QList<SpiceMeasure> measures;
    for (const SpiceGoal &goal : setup.goals)
        measures << goal.measure;

    bool stop = false;
    QStringList firstErrors;
    const auto evaluateValues = [&](const std::vector<double> &values, std::vector<double> *measured) -> double {
        BatchOptions single = options;
        single.progress = {};
        const BatchResult run = runPoints(snapshot, documentName, analysis, targets, {values}, {QString()}, measures, single);
        ++result.evaluations;
        if (!run.ok()) {
            firstErrors = run.errors;
            stop = true;
            return INFINITY;
        }
        if (options.cancel && options.cancel->load()) {
            result.cancelled = true;
            stop = true;
        }
        const std::vector<double> m = run.runs.isEmpty() ? std::vector<double>(measures.size(), NAN) : run.runs.first().measures;
        if (measured)
            *measured = m;
        if (options.progress)
            options.progress(BatchProgress{result.evaluations, setup.maxEvaluations,
                                           tr("Evaluation %1").arg(result.evaluations)});
        if (result.evaluations >= setup.maxEvaluations)
            stop = true;
        return goalCost(setup.goals, m);
    };
    const auto cost = [&](const std::vector<double> &u) { return evaluateValues(toValues(u), nullptr); };

    // Nelder-Mead on the unit cube.
    std::vector<std::vector<double>> simplex{toUnit(result.initial)};
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<double> vertex = simplex.front();
        vertex[i] = vertex[i] + (vertex[i] < 0.75 ? 0.15 : -0.15);
        simplex.push_back(vertex);
    }
    std::vector<double> costs;
    for (const auto &vertex : simplex) {
        costs.push_back(cost(vertex));
        if (stop)
            break;
    }
    result.initialCost = costs.front();
    if (!firstErrors.isEmpty()) {
        result.errors = firstErrors;
        return result;
    }
    const auto clampUnit = [](std::vector<double> u) {
        for (double &x : u)
            x = std::clamp(x, 0.0, 1.0);
        return u;
    };
    while (!stop && costs.size() == simplex.size()) {
        std::vector<std::size_t> order(simplex.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return costs[a] < costs[b]; });
        std::vector<std::vector<double>> s;
        std::vector<double> c;
        for (std::size_t index : order) {
            s.push_back(simplex[index]);
            c.push_back(costs[index]);
        }
        simplex = s;
        costs = c;
        const double spread = std::abs(costs.back() - costs.front());
        double size = 0;
        for (std::size_t k = 1; k < simplex.size(); ++k)
            for (std::size_t i = 0; i < n; ++i)
                size = std::max(size, std::abs(simplex[k][i] - simplex[0][i]));
        if (spread <= setup.tolerance * (std::abs(costs.front()) + 1e-12) || size < 1e-6 || costs.front() == 0.0) {
            result.converged = true;
            break;
        }
        std::vector<double> centroid(n, 0.0);
        for (std::size_t k = 0; k + 1 < simplex.size(); ++k)
            for (std::size_t i = 0; i < n; ++i)
                centroid[i] += simplex[k][i] / double(n);
        const auto along = [&](double factor) {
            std::vector<double> p(n);
            for (std::size_t i = 0; i < n; ++i)
                p[i] = centroid[i] + factor * (simplex.back()[i] - centroid[i]);
            return clampUnit(p);
        };
        const std::vector<double> reflected = along(-1.0);
        const double reflectedCost = cost(reflected);
        if (reflectedCost < costs.front()) {
            const std::vector<double> expanded = along(-2.0);
            const double expandedCost = stop ? INFINITY : cost(expanded);
            if (expandedCost < reflectedCost) {
                simplex.back() = expanded;
                costs.back() = expandedCost;
            } else {
                simplex.back() = reflected;
                costs.back() = reflectedCost;
            }
        } else if (reflectedCost < costs[costs.size() - 2]) {
            simplex.back() = reflected;
            costs.back() = reflectedCost;
        } else {
            const std::vector<double> contracted = along(reflectedCost < costs.back() ? -0.5 : 0.5);
            const double contractedCost = stop ? INFINITY : cost(contracted);
            if (contractedCost < std::min(reflectedCost, costs.back())) {
                simplex.back() = contracted;
                costs.back() = contractedCost;
            } else {
                for (std::size_t k = 1; k < simplex.size() && !stop; ++k) {
                    for (std::size_t i = 0; i < n; ++i)
                        simplex[k][i] = simplex[0][i] + 0.5 * (simplex[k][i] - simplex[0][i]);
                    costs[k] = cost(simplex[k]);
                }
            }
        }
    }
    const std::size_t best = std::size_t(std::min_element(costs.begin(), costs.end()) - costs.begin());
    result.best = toValues(simplex[best]);
    result.bestCost = costs[best];

    // Standard values, then the final evaluation (also leaves the snapshot
    // at the reported point).
    result.snapped = result.best;
    for (std::size_t i = 0; i < n; ++i)
        if (const auto &series = setup.variables.at(qsizetype(i)).series)
            result.snapped[i] = snapToSeries(result.best[i], *series);
    stop = false;
    std::vector<double> measured;
    const int evaluations = result.evaluations;
    result.snappedCost = evaluateValues(result.snapped, &measured);
    result.evaluations = evaluations + 1;
    result.bestMeasures = measured;
    goalCost(setup.goals, measured, &result.goalsMet);
    if (!result.converged && !result.cancelled)
        result.warnings << tr("Stopped after %1 evaluations without converging.").arg(result.evaluations);
    return result;
}

} // namespace lvd::spice
