#include "rfsim/Analysis.h"

#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QJsonArray>

#include <cmath>

namespace lvd::rf {

namespace {

constexpr int MaxPoints = 100000;

const QString AnalysesKey = QStringLiteral("analyses");

} // namespace

LinearAnalysisSetup LinearAnalysisSetup::fromJson(const QJsonObject &object)
{
    LinearAnalysisSetup setup;
    setup.id = object.value(QStringLiteral("id")).toString();
    setup.name = object.value(QStringLiteral("name")).toString();
    setup.sweep = object.value(QStringLiteral("sweep")).toString(setup.sweep);
    setup.start = object.value(QStringLiteral("start")).toString(setup.start);
    setup.stop = object.value(QStringLiteral("stop")).toString(setup.stop);
    setup.points = object.value(QStringLiteral("points")).toString(setup.points);
    for (const QJsonValue &value : object.value(QStringLiteral("list")).toArray())
        setup.list.append(value.toString());
    setup.noise = object.value(QStringLiteral("noise")).toBool(true);
    return setup;
}

QJsonObject LinearAnalysisSetup::toJson() const
{
    QJsonObject object{{QStringLiteral("id"), id},
                       {QStringLiteral("type"), type()},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("sweep"), sweep},
                       {QStringLiteral("noise"), noise}};
    if (sweep == QLatin1String("list")) {
        object.insert(QStringLiteral("list"), QJsonArray::fromStringList(list));
    } else {
        object.insert(QStringLiteral("start"), start);
        object.insert(QStringLiteral("stop"), stop);
        object.insert(QStringLiteral("points"), points);
    }
    return object;
}

QList<LinearAnalysisSetup> linearAnalyses(const QJsonObject &simulation)
{
    QList<LinearAnalysisSetup> result;
    for (const QJsonValue &value : simulation.value(AnalysesKey).toArray()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("type")).toString() == LinearAnalysisSetup::type())
            result.append(LinearAnalysisSetup::fromJson(object));
    }
    return result;
}

QJsonObject withLinearAnalysis(const QJsonObject &simulation, LinearAnalysisSetup analysis, QString *assignedId)
{
    QJsonArray analyses = simulation.value(AnalysesKey).toArray();
    if (analysis.id.isEmpty()) {
        int highest = 0;
        for (const QJsonValue &value : analyses) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            if (id.startsWith(QLatin1Char('a')))
                highest = std::max(highest, id.mid(1).toInt());
        }
        analysis.id = QStringLiteral("a%1").arg(highest + 1);
    }
    if (analysis.name.isEmpty())
        analysis.name = QStringLiteral("S-parameters");
    // Keys this version does not know are kept; known keys are rewritten.
    static const QStringList known = {QStringLiteral("id"),    QStringLiteral("type"),  QStringLiteral("name"),
                                      QStringLiteral("sweep"), QStringLiteral("start"), QStringLiteral("stop"),
                                      QStringLiteral("points"), QStringLiteral("list"), QStringLiteral("noise")};
    bool replaced = false;
    for (qsizetype i = 0; i < analyses.size(); ++i) {
        QJsonObject existing = analyses.at(i).toObject();
        if (existing.value(QStringLiteral("id")).toString() == analysis.id) {
            for (const QString &key : known)
                existing.remove(key);
            const QJsonObject fresh = analysis.toJson();
            for (auto it = fresh.begin(); it != fresh.end(); ++it)
                existing.insert(it.key(), it.value());
            analyses.replace(i, existing);
            replaced = true;
            break;
        }
    }
    if (!replaced)
        analyses.append(analysis.toJson());
    QJsonObject result = simulation;
    result.insert(AnalysesKey, analyses);
    if (assignedId)
        *assignedId = analysis.id;
    return result;
}

std::optional<std::vector<double>> sweepFrequencies(const LinearAnalysisSetup &analysis,
                                                    const Expression::Lookup &scope, QString *error)
{
    const auto fail = [error](const QString &message) -> std::optional<std::vector<double>> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const auto value = [&](const QString &text, const QString &what) -> std::optional<double> {
        QString evaluationError;
        const auto result = Expression::parse(text).evaluate(scope, &evaluationError);
        if (!result && error)
            *error = QStringLiteral("%1: %2").arg(what, evaluationError);
        return result;
    };

    std::vector<double> frequencies;
    if (analysis.sweep == QLatin1String("list")) {
        for (const QString &item : analysis.list) {
            const auto f = value(item, QStringLiteral("frequency list"));
            if (!f)
                return std::nullopt;
            frequencies.push_back(*f);
        }
        std::sort(frequencies.begin(), frequencies.end());
        frequencies.erase(std::unique(frequencies.begin(), frequencies.end()), frequencies.end());
        if (frequencies.empty())
            return fail(QStringLiteral("the frequency list is empty"));
        if (frequencies.front() < 0)
            return fail(QStringLiteral("frequencies must not be negative"));
        return frequencies;
    }

    const auto start = value(analysis.start, QStringLiteral("start"));
    const auto stop = value(analysis.stop, QStringLiteral("stop"));
    const auto points = value(analysis.points, QStringLiteral("points"));
    if (!start || !stop || !points)
        return std::nullopt;
    const int count = static_cast<int>(std::lround(*points));
    if (count < 1 || count > MaxPoints || std::abs(*points - count) > 1e-9)
        return fail(QStringLiteral("points must be an integer from 1 to %1").arg(MaxPoints));
    if (*start < 0 || *stop < *start)
        return fail(QStringLiteral("the sweep needs 0 <= start <= stop"));
    if (count > 1 && *stop == *start)
        return fail(QStringLiteral("start and stop are equal; use 1 point"));

    if (analysis.sweep == QLatin1String("log")) {
        if (*start <= 0)
            return fail(QStringLiteral("a logarithmic sweep needs start > 0"));
        for (int i = 0; i < count; ++i) {
            const double t = count == 1 ? 0.0 : static_cast<double>(i) / (count - 1);
            frequencies.push_back(*start * std::pow(*stop / *start, t));
        }
    } else if (analysis.sweep == QLatin1String("linear")) {
        for (int i = 0; i < count; ++i) {
            const double t = count == 1 ? 0.0 : static_cast<double>(i) / (count - 1);
            frequencies.push_back(*start + (*stop - *start) * t);
        }
    } else {
        return fail(QStringLiteral("unknown sweep type \"%1\"").arg(analysis.sweep));
    }
    frequencies.back() = count == 1 ? *start : *stop; // no rounding drift at the end
    return frequencies;
}

Expression::Lookup sheetScope(const Document &document, QStringList *errors)
{
    QList<QPair<QString, QString>> definitions;
    if (const Schematic *schematic = document.schematic()) {
        for (const Variable &variable : schematic->variables())
            definitions.append({variable.name, variable.value});
    }
    const VariableResults results = evaluateVariables(definitions);
    if (errors) {
        for (auto it = results.errors.begin(); it != results.errors.end(); ++it)
            errors->append(QStringLiteral("variable %1: %2").arg(it.key(), it.value()));
    }
    const QHash<QString, double> values = results.values;
    return [values](const QString &name) -> std::optional<double> {
        if (const auto it = values.constFind(name); it != values.cend())
            return *it;
        return std::nullopt;
    };
}

LinearResult runLinearAnalysis(const Project &project, const Document &document, const LinearAnalysisSetup &analysis,
                               TouchstoneCache *touchstones, const std::atomic_bool *cancel)
{
    LinearResult result;
    if (!document.schematic()) {
        result.errors.append(QStringLiteral("%1 is not a schematic").arg(document.name()));
        return result;
    }
    if (document.type() != DocumentType::RfSchematic)
        result.warnings.append(QStringLiteral("%1 is not an RF schematic").arg(document.name()));

    QString sweepError;
    const auto frequencies = sweepFrequencies(analysis, sheetScope(document), &sweepError);
    if (!frequencies) {
        result.errors.append(QStringLiteral("%1: %2").arg(analysis.name, sweepError));
        return result;
    }
    const Netlist netlist = buildNetlist(project, document);
    LinearOptions options;
    options.noise = analysis.noise;
    options.projectDirectory = project.directory();
    options.touchstones = touchstones;
    options.cancel = cancel;
    LinearResult simulated = simulateLinear(netlist, *frequencies, options);
    simulated.warnings = result.warnings + simulated.warnings;
    return simulated;
}

} // namespace lvd::rf
