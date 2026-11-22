#include "spice/SpiceAnalysis.h"

#include "spice/SpiceImport.h"

#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QRegularExpression>

namespace lvd::spice {

namespace {

const QString AnalysesKey = QStringLiteral("analyses");

struct KindInfo {
    AnalysisKind kind;
    const char *type;
    const char *display;
};

constexpr KindInfo Kinds[] = {
    {AnalysisKind::OperatingPoint, "spice-op", QT_TRANSLATE_NOOP("lvd::spice", "Operating point")},
    {AnalysisKind::DcSweep, "spice-dc", QT_TRANSLATE_NOOP("lvd::spice", "DC sweep")},
    {AnalysisKind::Ac, "spice-ac", QT_TRANSLATE_NOOP("lvd::spice", "AC")},
    {AnalysisKind::Transient, "spice-tran", QT_TRANSLATE_NOOP("lvd::spice", "Transient")},
    {AnalysisKind::Noise, "spice-noise", QT_TRANSLATE_NOOP("lvd::spice", "Noise")},
    {AnalysisKind::Fourier, "spice-fourier", QT_TRANSLATE_NOOP("lvd::spice", "Fourier")},
    {AnalysisKind::TransferFunction, "spice-tf", QT_TRANSLATE_NOOP("lvd::spice", "Transfer function")},
    {AnalysisKind::PoleZero, "spice-pz", QT_TRANSLATE_NOOP("lvd::spice", "Pole-zero")},
    {AnalysisKind::Sensitivity, "spice-sens", QT_TRANSLATE_NOOP("lvd::spice", "Sensitivity")},
};

QString tr(const char *text)
{
    return QCoreApplication::translate("lvd::spice", text);
}

// Collects errors while turning setup fields into card text.
class CardBuilder
{
public:
    CardBuilder(const SpiceAnalysisSetup &analysis, const SpiceDeck &deck, const Expression::Lookup &scope)
        : m_analysis(analysis), m_deck(deck), m_scope(scope)
    {}

    QStringList errors;

    // An expression field as a SPICE number.
    QString number(const QString &key, const QString &label, bool required = true)
    {
        const QString text = m_analysis.text(key).trimmed();
        if (text.isEmpty()) {
            if (required)
                errors << tr("%1 is missing.").arg(label);
            return {};
        }
        QString error;
        const auto value = Expression::parse(text).evaluate(m_scope, &error);
        if (!value) {
            errors << QStringLiteral("%1: %2").arg(label, error);
            return {};
        }
        return formatSpiceNumber(*value);
    }

    QString integer(const QString &key, const QString &label)
    {
        const QString text = number(key, label);
        if (text.isEmpty())
            return {};
        const double value = text.toDouble();
        if (value < 1 || value != std::floor(value)) {
            errors << tr("%1 must be a whole number of at least 1.").arg(label);
            return {};
        }
        return text;
    }

    // A net as typed (label or flat name) to its node.
    QString node(const QString &net, const QString &label)
    {
        const QString trimmed = net.trimmed();
        if (trimmed.isEmpty()) {
            errors << tr("%1 is missing.").arg(label);
            return {};
        }
        const QString found = m_deck.node(trimmed);
        if (found.isEmpty())
            errors << tr("%1: there is no net \"%2\".").arg(label, trimmed);
        return found;
    }

    // A component reference to its first element's SPICE name; it must be
    // an independent source when sourceOnly.
    QString source(const QString &reference, const QString &label)
    {
        const QString trimmed = reference.trimmed();
        if (trimmed.isEmpty()) {
            errors << tr("%1 is missing.").arg(label);
            return {};
        }
        const SpiceElement *element = m_deck.element(trimmed);
        if (!element)
            element = m_deck.elementByName(trimmed);
        if (!element || element->name.isEmpty()) {
            errors << tr("%1: there is no component \"%2\".").arg(label, trimmed);
            return {};
        }
        // The source may be one of several elements of the part; take the
        // first independent source.
        for (const QString &name : element->elementNames)
            if (name.startsWith(QLatin1Char('v')) || name.startsWith(QLatin1Char('i')))
                return name;
        errors << tr("%1: %2 is not a voltage or current source.").arg(label, trimmed);
        return {};
    }

    // "out", "v(out)", "v(out,ref)" -> "v(out)" / "v(out,ref)" with nodes;
    // "i(V1)" -> "i(vv1)" when currents are allowed.
    QString output(const QString &text, const QString &label, bool allowCurrent)
    {
        const QString trimmed = text.trimmed();
        static const QRegularExpression call(QStringLiteral("^([vViI])\\s*\\(([^)]*)\\)$"));
        const QRegularExpressionMatch match = call.match(trimmed);
        if (match.hasMatch() && match.captured(1).toLower() == QLatin1String("i")) {
            if (!allowCurrent) {
                errors << tr("%1 must be a voltage.").arg(label);
                return {};
            }
            const QString name = source(match.captured(2), label);
            return name.isEmpty() ? QString() : QStringLiteral("i(%1)").arg(name);
        }
        QStringList nets;
        if (match.hasMatch())
            nets = match.captured(2).split(QLatin1Char(','));
        else
            nets << trimmed;
        if (nets.size() > 2) {
            errors << tr("%1: v() takes one or two nets.").arg(label);
            return {};
        }
        QStringList nodes;
        for (const QString &net : nets)
            nodes << node(net, label);
        if (nodes.contains(QString()))
            return {};
        return QStringLiteral("v(%1)").arg(nodes.join(QLatin1Char(',')));
    }

    QString sweep(const QString &label)
    {
        const QString type = m_analysis.text(QStringLiteral("sweep"), QStringLiteral("dec")).trimmed().toLower();
        if (type != QLatin1String("dec") && type != QLatin1String("oct") && type != QLatin1String("lin")) {
            errors << tr("%1: the sweep is dec, oct or lin, not \"%2\".").arg(label, type);
            return {};
        }
        const QString points = integer(QStringLiteral("points"), tr("Points"));
        const QString start = number(QStringLiteral("start"), tr("Start frequency"));
        const QString stop = number(QStringLiteral("stop"), tr("Stop frequency"));
        if (points.isEmpty() || start.isEmpty() || stop.isEmpty())
            return {};
        if (start.toDouble() <= 0 && type != QLatin1String("lin"))
            errors << tr("A logarithmic sweep needs a start frequency above 0.");
        if (stop.toDouble() < start.toDouble())
            errors << tr("The stop frequency is below the start frequency.");
        return QStringLiteral("%1 %2 %3 %4").arg(type, points, start, stop);
    }

    QString transient()
    {
        const QString step = number(QStringLiteral("step"), tr("Time step"));
        const QString stop = number(QStringLiteral("stop"), tr("Stop time"));
        const QString start = number(QStringLiteral("start"), tr("Start time"), false);
        const QString maxStep = number(QStringLiteral("maxStep"), tr("Maximum step"), false);
        if (step.isEmpty() || stop.isEmpty())
            return {};
        if (stop.toDouble() <= 0)
            errors << tr("The stop time must be above 0.");
        QString card = QStringLiteral(".tran %1 %2").arg(step, stop);
        if (!start.isEmpty() || !maxStep.isEmpty())
            card += QLatin1Char(' ') + (start.isEmpty() ? QStringLiteral("0") : start);
        if (!maxStep.isEmpty())
            card += QLatin1Char(' ') + maxStep;
        if (m_analysis.flag(QStringLiteral("uic")))
            card += QStringLiteral(" uic");
        return card;
    }

private:
    const SpiceAnalysisSetup &m_analysis;
    const SpiceDeck &m_deck;
    const Expression::Lookup &m_scope;
};

} // namespace

QString analysisTypeName(AnalysisKind kind)
{
    for (const KindInfo &info : Kinds)
        if (info.kind == kind)
            return QString::fromLatin1(info.type);
    return {};
}

QString analysisDisplayName(AnalysisKind kind)
{
    for (const KindInfo &info : Kinds)
        if (info.kind == kind)
            return tr(info.display);
    return {};
}

std::optional<AnalysisKind> analysisKindFromType(const QString &type)
{
    for (const KindInfo &info : Kinds)
        if (type == QLatin1String(info.type))
            return info.kind;
    return std::nullopt;
}

bool isSpiceAnalysisType(const QString &type)
{
    return analysisKindFromType(type).has_value();
}

QList<AnalysisKind> allAnalysisKinds()
{
    QList<AnalysisKind> kinds;
    for (const KindInfo &info : Kinds)
        kinds << info.kind;
    return kinds;
}

QString SpiceAnalysisSetup::text(const QString &key, const QString &fallback) const
{
    const QJsonValue value = data.value(key);
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toDouble(), 'g', 15);
    return fallback;
}

bool SpiceAnalysisSetup::flag(const QString &key, bool fallback) const
{
    const QJsonValue value = data.value(key);
    return value.isBool() ? value.toBool() : fallback;
}

QStringList SpiceAnalysisSetup::list(const QString &key) const
{
    QStringList out;
    for (const QJsonValue &value : data.value(key).toArray())
        if (value.isString())
            out << value.toString();
    return out;
}

SpiceAnalysisSetup SpiceAnalysisSetup::create(AnalysisKind kind, const QString &name)
{
    SpiceAnalysisSetup setup;
    setup.kind = kind;
    setup.name = name.isEmpty() ? analysisDisplayName(kind) : name;
    const auto put = [&](const char *key, const QJsonValue &value) { setup.data.insert(QLatin1String(key), value); };
    switch (kind) {
    case AnalysisKind::OperatingPoint:
        break;
    case AnalysisKind::DcSweep:
        put("source", QStringLiteral("V1"));
        put("start", QStringLiteral("0"));
        put("stop", QStringLiteral("5"));
        put("step", QStringLiteral("0.1"));
        break;
    case AnalysisKind::Ac:
    case AnalysisKind::Noise:
        put("sweep", QStringLiteral("dec"));
        put("points", QStringLiteral("20"));
        put("start", QStringLiteral("1Hz"));
        put("stop", QStringLiteral("10MHz"));
        if (kind == AnalysisKind::Noise) {
            put("output", QStringLiteral("out"));
            put("source", QStringLiteral("V1"));
        }
        break;
    case AnalysisKind::Transient:
    case AnalysisKind::Fourier:
        put("step", QStringLiteral("1us"));
        put("stop", QStringLiteral("1ms"));
        if (kind == AnalysisKind::Fourier) {
            put("fundamental", QStringLiteral("1kHz"));
            put("harmonics", QStringLiteral("9"));
            put("signals", QJsonArray{QStringLiteral("v(out)")});
        }
        break;
    case AnalysisKind::TransferFunction:
        put("output", QStringLiteral("out"));
        put("source", QStringLiteral("V1"));
        break;
    case AnalysisKind::PoleZero:
        put("inputPositive", QStringLiteral("in"));
        put("inputNegative", QStringLiteral("0"));
        put("outputPositive", QStringLiteral("out"));
        put("outputNegative", QStringLiteral("0"));
        put("transfer", QStringLiteral("voltage"));
        put("find", QStringLiteral("both"));
        break;
    case AnalysisKind::Sensitivity:
        put("output", QStringLiteral("out"));
        put("ac", false);
        break;
    }
    return setup;
}

std::optional<SpiceAnalysisSetup> SpiceAnalysisSetup::fromJson(const QJsonObject &object)
{
    const auto kind = analysisKindFromType(object.value(QStringLiteral("type")).toString());
    if (!kind)
        return std::nullopt;
    SpiceAnalysisSetup setup;
    setup.kind = *kind;
    setup.id = object.value(QStringLiteral("id")).toString();
    setup.name = object.value(QStringLiteral("name")).toString();
    if (setup.name.isEmpty())
        setup.name = analysisDisplayName(*kind);
    setup.data = object;
    return setup;
}

QJsonObject SpiceAnalysisSetup::toJson() const
{
    QJsonObject object = data;
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("type"), analysisTypeName(kind));
    object.insert(QStringLiteral("name"), name);
    return object;
}

QList<SpiceAnalysisSetup> spiceAnalyses(const QJsonObject &simulation)
{
    QList<SpiceAnalysisSetup> analyses;
    for (const QJsonValue &value : simulation.value(AnalysesKey).toArray())
        if (auto setup = SpiceAnalysisSetup::fromJson(value.toObject()))
            analyses << *setup;
    return analyses;
}

QJsonObject withSpiceAnalysis(const QJsonObject &simulation, SpiceAnalysisSetup analysis, QString *assignedId)
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
    bool replaced = false;
    for (qsizetype i = 0; i < analyses.size(); ++i) {
        if (analyses.at(i).toObject().value(QStringLiteral("id")).toString() == analysis.id) {
            analyses.replace(i, analysis.toJson());
            replaced = true;
            break;
        }
    }
    if (!replaced)
        analyses.append(analysis.toJson());
    if (assignedId)
        *assignedId = analysis.id;
    QJsonObject result = simulation;
    result.insert(AnalysesKey, analyses);
    return result;
}

AnalysisCards analysisCards(const SpiceAnalysisSetup &analysis, const SpiceDeck &deck, const Expression::Lookup &scope)
{
    AnalysisCards result;
    CardBuilder b(analysis, deck, scope);
    QStringList &cards = result.cards;

    const QString options = analysis.text(QStringLiteral("options")).trimmed();
    if (!options.isEmpty())
        cards << QStringLiteral(".options ") + options;
    if (!analysis.text(QStringLiteral("temperature")).trimmed().isEmpty()) {
        const QString temperature = b.number(QStringLiteral("temperature"), tr("Temperature"));
        if (!temperature.isEmpty())
            cards << QStringLiteral(".temp ") + temperature;
    }
    const QString extra = analysis.text(QStringLiteral("cards")).trimmed();
    if (!extra.isEmpty())
        cards << extra.split(QLatin1Char('\n'));

    switch (analysis.kind) {
    case AnalysisKind::OperatingPoint:
        cards << QStringLiteral(".op");
        break;
    case AnalysisKind::DcSweep: {
        const auto sweepOf = [&](const QString &suffix, const QString &label) -> QString {
            const QString sourceText = analysis.text(QStringLiteral("source") + suffix).trimmed();
            const QString name = sourceText.compare(QStringLiteral("temp"), Qt::CaseInsensitive) == 0
                                     ? QStringLiteral("temp")
                                     : b.source(sourceText, label);
            const QString start = b.number(QStringLiteral("start") + suffix, tr("Start"));
            const QString stop = b.number(QStringLiteral("stop") + suffix, tr("Stop"));
            const QString step = b.number(QStringLiteral("step") + suffix, tr("Step"));
            if (name.isEmpty() || start.isEmpty() || stop.isEmpty() || step.isEmpty())
                return {};
            if (step.toDouble() == 0)
                b.errors << tr("The step must not be 0.");
            return QStringLiteral("%1 %2 %3 %4").arg(name, start, stop, step);
        };
        const QString first = sweepOf(QString(), tr("Swept source"));
        QString card = QStringLiteral(".dc ") + first;
        if (!analysis.text(QStringLiteral("source2")).trimmed().isEmpty())
            card += QLatin1Char(' ') + sweepOf(QStringLiteral("2"), tr("Second swept source"));
        cards << card;
        break;
    }
    case AnalysisKind::Ac:
        cards << QStringLiteral(".ac ") + b.sweep(tr("AC sweep"));
        break;
    case AnalysisKind::Transient:
    case AnalysisKind::Fourier:
        cards << b.transient();
        break;
    case AnalysisKind::Noise: {
        const QString output = b.output(analysis.text(QStringLiteral("output")), tr("Output"), false);
        const QString source = b.source(analysis.text(QStringLiteral("source")), tr("Input source"));
        cards << QStringLiteral(".noise %1 %2 %3").arg(output, source, b.sweep(tr("Noise sweep")));
        break;
    }
    case AnalysisKind::TransferFunction: {
        const QString output = b.output(analysis.text(QStringLiteral("output")), tr("Output"), true);
        const QString source = b.source(analysis.text(QStringLiteral("source")), tr("Input source"));
        cards << QStringLiteral(".tf %1 %2").arg(output, source);
        break;
    }
    case AnalysisKind::PoleZero: {
        const QString inP = b.node(analysis.text(QStringLiteral("inputPositive")), tr("Input +"));
        const QString inN = b.node(analysis.text(QStringLiteral("inputNegative"), QStringLiteral("0")), tr("Input −"));
        const QString outP = b.node(analysis.text(QStringLiteral("outputPositive")), tr("Output +"));
        const QString outN = b.node(analysis.text(QStringLiteral("outputNegative"), QStringLiteral("0")), tr("Output −"));
        const QString transfer = analysis.text(QStringLiteral("transfer"), QStringLiteral("voltage")) == QLatin1String("current")
                                     ? QStringLiteral("cur")
                                     : QStringLiteral("vol");
        const QString findText = analysis.text(QStringLiteral("find"), QStringLiteral("both"));
        const QString find = findText == QLatin1String("poles")   ? QStringLiteral("pol")
                             : findText == QLatin1String("zeros") ? QStringLiteral("zer")
                                                                  : QStringLiteral("pz");
        cards << QStringLiteral(".pz %1 %2 %3 %4 %5 %6").arg(inP, inN, outP, outN, transfer, find);
        break;
    }
    case AnalysisKind::Sensitivity: {
        const QString output = b.output(analysis.text(QStringLiteral("output")), tr("Output"), true);
        QString card = QStringLiteral(".sens ") + output;
        if (analysis.flag(QStringLiteral("ac")))
            card += QStringLiteral(" ac ") + b.sweep(tr("AC sweep"));
        cards << card;
        break;
    }
    }
    result.errors = b.errors;
    return result;
}

QString deckWithCards(const SpiceDeck &deck, const QStringList &cards)
{
    QString text = deck.text;
    const qsizetype end = text.lastIndexOf(QStringLiteral(".end"));
    if (end < 0)
        return text + cards.join(QLatin1Char('\n')) + QStringLiteral("\n.end\n");
    return text.left(end) + (cards.isEmpty() ? QString() : cards.join(QLatin1Char('\n')) + QLatin1Char('\n')) + text.mid(end);
}

Expression::Lookup sheetVariables(const Document &document, QStringList *errors)
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

SpiceSimulation runSpiceAnalysis(const Project &project, const Document &document, const SpiceAnalysisSetup &analysis,
                                 SpiceEngine &engine, const SpiceRunOptions &options)
{
    SpiceSimulation simulation = prepareSpiceAnalysis(project, document, analysis);
    if (simulation.errors.isEmpty())
        executeSpiceAnalysis(simulation, engine, options);
    return simulation;
}

void executeSpiceAnalysis(SpiceSimulation &simulation, SpiceEngine &engine, const SpiceRunOptions &options)
{
    simulation.run = engine.run(simulation.netlist, options);
    finishSpiceAnalysis(simulation);
}

void finishSpiceAnalysis(SpiceSimulation &simulation)
{
    simulation.errors << simulation.run.errors;
    simulation.warnings << simulation.run.warnings;

    if (simulation.analysis.kind == AnalysisKind::Fourier && simulation.run.ok) {
        const SpicePlot *tran = simulation.run.plot(QStringLiteral("tran"));
        for (const QString &expression : simulation.analysis.list(QStringLiteral("signals"))) {
            FourierSignal signal;
            signal.expression = expression;
            if (!tran) {
                signal.error = tr("No transient result.");
            } else if (const auto waveform = evaluateOnPlot(expression, *tran, &simulation.deck, &signal.error)) {
                signal.result = lvd::spice::fourier(*waveform, simulation.fourierFundamental, simulation.fourierHarmonics,
                                                    simulation.fourierPeriods, {}, &signal.error);
            }
            if (!signal.error.isEmpty())
                simulation.warnings << QStringLiteral("Fourier %1: %2").arg(expression, signal.error);
            simulation.fourier << signal;
        }
    }
}

SpiceSimulation prepareSpiceAnalysis(const Project &project, const Document &document, const SpiceAnalysisSetup &analysis)
{
    SpiceSimulation simulation;
    simulation.analysis = analysis;
    if (!document.schematic()) {
        simulation.errors << tr("%1 is not a schematic.").arg(document.name());
        return simulation;
    }

    SpiceNetlistOptions netlistOptions;
    netlistOptions.title = QStringLiteral("%1: %2").arg(document.name(), analysis.name);
    netlistOptions.saveDeviceCurrents = analysis.kind == AnalysisKind::OperatingPoint || analysis.flag(QStringLiteral("saveCurrents"));
    // An imported netlist (requirement SP-10) replaces the drawing.
    const QString importedNetlist = document.schematic()->simulation().value(QStringLiteral("netlist")).toString();
    if (!importedNetlist.trimmed().isEmpty())
        simulation.deck = deckFromNetlist(importedNetlist, netlistOptions.title, project.directory());
    else
        simulation.deck = buildSpiceDeck(project, document, netlistOptions);
    simulation.errors = simulation.deck.errors;
    simulation.warnings = simulation.deck.warnings;

    QStringList variableErrors;
    const Expression::Lookup scope = sheetVariables(document, &variableErrors);
    simulation.errors << variableErrors;
    const AnalysisCards cards = analysisCards(analysis, simulation.deck, scope);
    for (const QString &error : cards.errors)
        simulation.errors << QStringLiteral("%1: %2").arg(analysis.name, error);
    simulation.netlist = deckWithCards(simulation.deck, cards.cards);

    if (analysis.kind == AnalysisKind::Fourier) {
        CardBuilder b(analysis, simulation.deck, scope);
        simulation.fourierFundamental = b.number(QStringLiteral("fundamental"), tr("Fundamental"), false).toDouble();
        const QString harmonics = analysis.text(QStringLiteral("harmonics")).trimmed();
        if (!harmonics.isEmpty())
            simulation.fourierHarmonics = std::max(1, b.integer(QStringLiteral("harmonics"), tr("Harmonics")).toInt());
        if (!analysis.text(QStringLiteral("periods")).trimmed().isEmpty())
            simulation.fourierPeriods = std::max(1, b.integer(QStringLiteral("periods"), tr("Periods")).toInt());
        if (analysis.list(QStringLiteral("signals")).isEmpty())
            b.errors << tr("No signals to analyse.");
        for (const QString &error : std::as_const(b.errors))
            simulation.errors << QStringLiteral("%1: %2").arg(analysis.name, error);
    }
    return simulation;
}

} // namespace lvd::spice
