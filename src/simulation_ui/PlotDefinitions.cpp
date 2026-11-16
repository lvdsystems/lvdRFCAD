#include "simulation_ui/PlotDefinitions.h"

#include "core/Netlist.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace lvd::sim {

namespace {

const QString PlotsKey = QStringLiteral("plots");
const QString AnalysesKey = QStringLiteral("analyses");

const QStringList KnownKeys = {QStringLiteral("id"),       QStringLiteral("name"),   QStringLiteral("kind"),
                               QStringLiteral("analysis"), QStringLiteral("traces"), QStringLiteral("labels")};

} // namespace

QString PlotDefinition::label(const QString &expression) const
{
    const QString shown = labels.value(expression);
    return shown.isEmpty() ? expression : shown;
}

PlotDefinition PlotDefinition::fromJson(const QJsonObject &object)
{
    PlotDefinition plot;
    plot.id = object.value(QStringLiteral("id")).toString();
    plot.name = object.value(QStringLiteral("name")).toString();
    plot.kind = object.value(QStringLiteral("kind")).toString(plot.kind);
    plot.analysis = object.value(QStringLiteral("analysis")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("traces")).toArray()) {
        const QString expression = value.toString().trimmed();
        if (!expression.isEmpty())
            plot.traces.append(expression);
    }
    const QJsonObject labels = object.value(QStringLiteral("labels")).toObject();
    for (auto it = labels.begin(); it != labels.end(); ++it) {
        if (!it.value().toString().isEmpty())
            plot.labels.insert(it.key(), it.value().toString());
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!KnownKeys.contains(it.key()))
            plot.extra.insert(it.key(), it.value());
    }
    return plot;
}

QJsonObject PlotDefinition::toJson() const
{
    QJsonObject object = extra;
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("name"), name);
    object.insert(QStringLiteral("kind"), kind);
    object.insert(QStringLiteral("analysis"), analysis);
    object.insert(QStringLiteral("traces"), QJsonArray::fromStringList(traces));
    QJsonObject labelObject;
    for (auto it = labels.begin(); it != labels.end(); ++it) {
        if (traces.contains(it.key()) && !it.value().isEmpty() && it.value() != it.key())
            labelObject.insert(it.key(), it.value());
    }
    if (!labelObject.isEmpty())
        object.insert(QStringLiteral("labels"), labelObject);
    return object;
}

QList<PlotDefinition> plotDefinitions(const QJsonObject &simulation, const QString &analysisId)
{
    QList<PlotDefinition> result;
    for (const QJsonValue &value : simulation.value(PlotsKey).toArray()) {
        PlotDefinition plot = PlotDefinition::fromJson(value.toObject());
        if (analysisId.isEmpty() || plot.analysis == analysisId)
            result.append(plot);
    }
    return result;
}

QJsonObject withPlot(const QJsonObject &simulation, PlotDefinition plot, QString *assignedId)
{
    QJsonArray plots = simulation.value(PlotsKey).toArray();
    if (plot.id.isEmpty()) {
        int highest = 0;
        for (const QJsonValue &value : plots) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            if (id.startsWith(QLatin1Char('p')))
                highest = std::max(highest, id.mid(1).toInt());
        }
        plot.id = QStringLiteral("p%1").arg(highest + 1);
    }
    bool replaced = false;
    for (qsizetype i = 0; i < plots.size(); ++i) {
        if (plots.at(i).toObject().value(QStringLiteral("id")).toString() == plot.id) {
            // Keys written by newer versions inside the plot survive too.
            PlotDefinition merged = plot;
            const PlotDefinition old = PlotDefinition::fromJson(plots.at(i).toObject());
            for (auto it = old.extra.begin(); it != old.extra.end(); ++it) {
                if (!merged.extra.contains(it.key()))
                    merged.extra.insert(it.key(), it.value());
            }
            plots.replace(i, merged.toJson());
            replaced = true;
            break;
        }
    }
    if (!replaced)
        plots.append(plot.toJson());
    QJsonObject result = simulation;
    result.insert(PlotsKey, plots);
    if (assignedId)
        *assignedId = plot.id;
    return result;
}

QJsonObject withoutPlot(const QJsonObject &simulation, const QString &plotId)
{
    QJsonArray plots = simulation.value(PlotsKey).toArray();
    for (qsizetype i = plots.size() - 1; i >= 0; --i) {
        if (plots.at(i).toObject().value(QStringLiteral("id")).toString() == plotId)
            plots.removeAt(i);
    }
    QJsonObject result = simulation;
    result.insert(PlotsKey, plots);
    return result;
}

QJsonObject withoutAnalysis(const QJsonObject &simulation, const QString &analysisId)
{
    QJsonObject result = simulation;
    QJsonArray analyses = simulation.value(AnalysesKey).toArray();
    for (qsizetype i = analyses.size() - 1; i >= 0; --i) {
        if (analyses.at(i).toObject().value(QStringLiteral("id")).toString() == analysisId)
            analyses.removeAt(i);
    }
    result.insert(AnalysesKey, analyses);
    QJsonArray plots = simulation.value(PlotsKey).toArray();
    for (qsizetype i = plots.size() - 1; i >= 0; --i) {
        if (plots.at(i).toObject().value(QStringLiteral("analysis")).toString() == analysisId)
            plots.removeAt(i);
    }
    if (simulation.contains(PlotsKey))
        result.insert(PlotsKey, plots);
    return result;
}

std::vector<int> netlistPortNumbers(const Netlist &netlist)
{
    std::vector<int> ports;
    for (const FlatComponent &component : netlist.components) {
        if (component.definitionKey != QLatin1String("rf-system:port"))
            continue;
        const auto it = component.parameterValues.constFind(QStringLiteral("num"));
        if (it == component.parameterValues.cend() || !std::isfinite(*it))
            continue;
        ports.push_back(static_cast<int>(std::lround(*it)));
    }
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
}

QString sParameterName(int to, int from)
{
    if (to >= 0 && to < 10 && from >= 0 && from < 10)
        return QStringLiteral("S%1%2").arg(to).arg(from);
    return QStringLiteral("S(%1,%2)").arg(to).arg(from);
}

QList<PlotDefinition> defaultPlots(const QString &analysisId, const std::vector<int> &portList, bool noise)
{
    const std::vector<int> ports = portList.empty() ? std::vector<int>{1, 2} : portList;
    const size_t n = std::min<size_t>(ports.size(), 4);

    PlotDefinition response;
    response.name = QStringLiteral("Response");
    response.kind = QStringLiteral("rectangular");
    response.analysis = analysisId;
    // Transmission first, then reflection: the usual reading order.
    for (size_t c = 0; c < n; ++c) {
        for (size_t r = 0; r < n; ++r) {
            if (r != c)
                response.traces.append(QStringLiteral("dB(%1)").arg(sParameterName(ports[r], ports[c])));
        }
    }
    for (size_t i = 0; i < n; ++i)
        response.traces.append(QStringLiteral("dB(%1)").arg(sParameterName(ports[i], ports[i])));
    if (ports.size() == 2 && noise)
        response.traces.append(QStringLiteral("NF"));

    PlotDefinition smith;
    smith.name = QStringLiteral("Reflection");
    smith.kind = QStringLiteral("smith");
    smith.analysis = analysisId;
    smith.traces.append(sParameterName(ports[0], ports[0]));
    if (ports.size() == 2)
        smith.traces.append(sParameterName(ports[1], ports[1]));
    return {response, smith};
}

QStringList measurementCompletions(const std::vector<int> &portList, bool noise)
{
    const std::vector<int> ports = portList.empty() ? std::vector<int>{1, 2} : portList;
    QStringList list;
    QStringList sNames;
    for (int r : ports) {
        for (int c : ports)
            sNames.append(sParameterName(r, c));
    }
    for (const QString &s : sNames)
        list << QStringLiteral("dB(%1)").arg(s);
    list << sNames;
    for (int r : ports) {
        for (int c : ports) {
            const QString s = sParameterName(r, c);
            list << QStringLiteral("phase(%1)").arg(s) << QStringLiteral("mag(%1)").arg(s);
            if (r != c)
                list << QStringLiteral("GD(%1)").arg(s);
        }
    }
    for (int p : ports) {
        list << QStringLiteral("Zin(%1)").arg(p) << QStringLiteral("Yin(%1)").arg(p) << QStringLiteral("VSWR(%1)").arg(p)
             << QStringLiteral("RL(%1)").arg(p) << QStringLiteral("re(Zin(%1))").arg(p)
             << QStringLiteral("im(Zin(%1))").arg(p);
    }
    for (int r : ports) {
        for (int c : ports)
            list << QStringLiteral("Z(%1,%2)").arg(r).arg(c) << QStringLiteral("Y(%1,%2)").arg(r).arg(c);
    }
    if (ports.size() == 2) {
        list << QStringLiteral("K") << QStringLiteral("mu") << QStringLiteral("mup") << QStringLiteral("Delta")
             << QStringLiteral("mag(Delta)") << QStringLiteral("MAG");
        if (noise) {
            list << QStringLiteral("NF") << QStringLiteral("NFmin") << QStringLiteral("Rn") << QStringLiteral("GammaOpt");
        }
    } else if (noise && ports.size() > 2) {
        for (int r : ports) {
            for (int c : ports) {
                if (r != c)
                    list << QStringLiteral("NF(%1,%2)").arg(r).arg(c);
            }
        }
    }
    list.removeDuplicates();
    return list;
}

bool isNoiseMeasurement(const QString &expression)
{
    static const QRegularExpression noise(QStringLiteral("^\\s*(NF|NFmin|Rn)\\b"));
    return noise.match(expression).hasMatch();
}

} // namespace lvd::sim
