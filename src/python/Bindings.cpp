#include "python/Bindings.h"
#include "python/AntennaBindings.h"

#include <pybind11/stl.h>

#include "core/Connectivity.h"
#include "core/Expression.h"
#include "core/Netlist.h"
#include "core/RuleCheck.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "core/Substrates.h"
#include "rfdata/Touchstone.h"
#include "rfsim/Analysis.h"
#include "rfsim/Budget.h"
#include "rfsim/Optimizer.h"
#include "rfsim/Measurement.h"
#include "spice/SpiceAnalysis.h"
#include "spice/SpiceBatch.h"
#include "spice/SpiceImport.h"
#include "spice/SpiceProcessPool.h"
#include "spice/SpiceWaveforms.h"
#include <QJsonDocument>

#include <pybind11/complex.h>

#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>

#include <algorithm>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace lvd {
namespace {

std::string toStd(const QString &text)
{
    return text.toStdString();
}

QString toQt(const std::string &text)
{
    return QString::fromStdString(text);
}

// Accepts str and os.PathLike.
QString pathArgument(const py::object &path)
{
    return toQt(py::str(py::module_::import("os").attr("fspath")(path)));
}

// Python objects hold guarded pointers, so using an object after its
// document or project has gone raises an exception instead of crashing.
struct DocumentHandle {
    QPointer<Document> pointer;

    Document &get() const
    {
        if (!pointer)
            throw std::runtime_error("this document no longer exists");
        return *pointer;
    }
};

struct ProjectHandle {
    QPointer<Project> pointer;

    Project &get() const
    {
        if (!pointer)
            throw std::runtime_error("this project is no longer open");
        return *pointer;
    }
};

struct OutputStream {
    bool isError;
};

// The schematic of a document; valid while the document exists.
struct SchematicHandle {
    QPointer<Document> document;

    Document &doc() const
    {
        if (!document)
            throw std::runtime_error("this document no longer exists");
        return *document;
    }
    Schematic &get() const { return *doc().schematic(); }
    Project &project() const
    {
        Project *owner = doc().project();
        if (!owner)
            throw std::runtime_error("the document does not belong to a project");
        return *owner;
    }
};

// A simulation result, shared so Python can keep it after the schematic changes.
struct ResultHandle {
    std::shared_ptr<const rf::LinearResult> result;
};

// Port numbers of the simulation ports in a document (flattened), ascending.
std::vector<int> simulationPorts(const Project &project, const Document &document)
{
    std::vector<int> ports;
    for (const FlatComponent &component : buildNetlist(project, document).components) {
        const ModelDefinition *model = component.definition->model(QStringLiteral("rf-behavioural"));
        if (model && model->data.value(QStringLiteral("block")).toString() == QLatin1String("port")) {
            const double number = component.parameterValues.value(QStringLiteral("num"), 0);
            if (number >= 1)
                ports.push_back(static_cast<int>(number));
        }
    }
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
}

// The default plots the simulation view would create: dB of all S-parameters
// (up to four ports) and NF for a two-port with noise; a Smith chart of the reflections.
QJsonObject withDefaultPlots(QJsonObject simulation, const QString &analysisId, const std::vector<int> &ports, bool noise)
{
    if (ports.empty())
        return simulation;
    QJsonArray plots = simulation.value(QStringLiteral("plots")).toArray();
    int highest = 0;
    for (const QJsonValue &value : plots) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (id.startsWith(QLatin1Char('p')))
            highest = std::max(highest, id.mid(1).toInt());
    }
    const size_t n = std::min<size_t>(ports.size(), 4);
    QJsonArray response, reflection;
    for (size_t r = 0; r < n; ++r) {
        for (size_t c = 0; c < n; ++c)
            response.append(QStringLiteral("dB(S(%1,%2))").arg(ports[r]).arg(ports[c]));
        reflection.append(QStringLiteral("S(%1,%1)").arg(ports[r]));
    }
    if (n == 2 && noise)
        response.append(QStringLiteral("NF"));
    plots.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("p%1").arg(highest + 1)},
                             {QStringLiteral("name"), QStringLiteral("Response")},
                             {QStringLiteral("kind"), QStringLiteral("rectangular")},
                             {QStringLiteral("analysis"), analysisId},
                             {QStringLiteral("traces"), response}});
    plots.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("p%1").arg(highest + 2)},
                             {QStringLiteral("name"), QStringLiteral("Reflection")},
                             {QStringLiteral("kind"), QStringLiteral("smith")},
                             {QStringLiteral("analysis"), analysisId},
                             {QStringLiteral("traces"), reflection}});
    simulation.insert(QStringLiteral("plots"), plots);
    return simulation;
}

py::dict analysisDict(const rf::LinearAnalysisSetup &analysis)
{
    py::dict item;
    item["id"] = analysis.id.toStdString();
    item["name"] = analysis.name.toStdString();
    item["sweep"] = analysis.sweep.toStdString();
    item["start"] = analysis.start.toStdString();
    item["stop"] = analysis.stop.toStdString();
    item["points"] = analysis.points.toStdString();
    std::vector<std::string> list;
    for (const QString &value : analysis.list)
        list.push_back(value.toStdString());
    item["list"] = list;
    item["noise"] = analysis.noise;
    return item;
}

py::dict budgetStageDict(const rf::BudgetStage &s)
{
    py::dict d;
    d["component"] = s.path.toStdString();
    d["name"] = s.name.toStdString();
    d["kind"] = s.kind.toStdString();
    d["input_frequency"] = s.inputFrequency;
    d["output_frequency"] = s.outputFrequency;
    d["gain"] = s.gainDb;
    d["nf"] = s.nfDb;
    d["oip3"] = s.oip3Dbm;
    d["op1db"] = s.op1dbDbm;
    d["oip2"] = s.oip2Dbm;
    d["input_return_loss"] = s.inputReturnLossDb;
    d["cumulative_gain"] = s.cumulativeGainDb;
    d["cumulative_nf"] = s.cumulativeNfDb;
    d["iip3"] = s.iip3Dbm;
    d["cumulative_oip3"] = s.oip3CumulativeDbm;
    d["ip1db"] = s.ip1dbDbm;
    d["cumulative_op1db"] = s.op1dbCumulativeDbm;
    d["iip2"] = s.iip2Dbm;
    d["signal"] = s.signalDbm;
    d["noise"] = s.noiseDbm;
    d["snr"] = s.snrDb;
    d["thermal_snr"] = s.thermalSnrDb;
    d["phase_noise"] = s.phaseNoiseDbm;
    d["reciprocal_mixing"] = s.reciprocalMixingDbm;
    d["total_noise"] = s.totalNoiseDbm;
    d["phase_noise_snr"] = s.phaseNoiseSnrDb;
    d["reciprocal_mixing_snr"] = s.reciprocalMixingSnrDb;
    d["lo_phase_noise"] = s.loPhaseNoiseDbc;
    d["lo_rms_phase"] = s.loRmsPhaseDeg;
    d["lo_jitter"] = s.loJitterS;
    d["lo_phase_noise_at_offset"] = s.loPhaseNoiseAtOffsetDbcHz;
    d["blocker"] = s.blockerDbm;
    d["dac_image"] = s.dacImageDbm;
    d["sfdr"] = s.sfdrDb;
    d["headroom"] = s.headroomDb;
    d["compressed"] = s.compressed;
    std::vector<std::string> notes;
    for (const QString &note : s.notes)
        notes.push_back(note.toStdString());
    d["notes"] = notes;
    return d;
}

py::dict budgetResultDict(const rf::BudgetResult &b)
{
    py::dict d;
    d["start"] = b.start.toStdString();
    d["end"] = b.end.toStdString();
    d["input_frequency"] = b.inputFrequency;
    d["input_power"] = b.inputPowerDbm;
    d["bandwidth"] = b.bandwidth;
    d["input_noise"] = b.inputNoiseDbm;
    d["mismatch"] = b.mismatch;
    py::list stages;
    for (const rf::BudgetStage &stage : b.stages)
        stages.append(budgetStageDict(stage));
    d["stages"] = stages;
    if (b.hasAdc) {
        py::dict adc;
        adc["full_scale"] = b.adcFullScaleDbm;
        adc["headroom"] = b.adcHeadroomDb;
        adc["quantisation_snr"] = b.adcQuantisationSnrDb;
        adc["sample_rate"] = b.adcSampleRate;
        adc["alias_frequency"] = b.adcAliasFrequency;
        d["adc"] = adc;
    } else {
        d["adc"] = py::none();
    }
    if (b.hasPhaseNoise || b.hasBlocker) {
        py::dict phaseNoise;
        phaseNoise["start"] = b.phaseNoiseStart;
        phaseNoise["stop"] = b.phaseNoiseStop;
        phaseNoise["has_lo_phase_noise"] = b.hasPhaseNoise;
        phaseNoise["blocker_offset"] = b.blockerOffset;
        phaseNoise["blocker_power"] = b.blockerPowerDbm;
        d["phase_noise"] = phaseNoise;
    } else {
        d["phase_noise"] = py::none();
    }
    if (b.hasDac) {
        py::dict dac;
        dac["sinc"] = b.dacSincDb;
        dac["nsd"] = b.dacNsdDbmHz;
        dac["nsd_from_quantisation"] = b.dacNsdFromQuantisation;
        dac["image_frequency"] = b.dacImageFrequency;
        dac["image_level"] = b.dacImageDbm;
        d["dac"] = dac;
    } else {
        d["dac"] = py::none();
    }
    if (b.hasDetector) {
        py::dict detector;
        detector["sensitivity"] = b.detectorSensitivityDbm;
        detector["max_level"] = b.detectorMaxDbm;
        detector["margin"] = b.detectorMarginDb;
        detector["headroom"] = b.detectorHeadroomDb;
        d["detector"] = detector;
    } else {
        d["detector"] = py::none();
    }
    return d;
}

py::list spurList(const rf::SpurResult &result)
{
    static const char *kinds[] = {"product", "spurious_response", "image", "half_if", "lo_leakage"};
    py::list spurs;
    for (const rf::Spur &spur : result.spurs) {
        py::dict d;
        d["kind"] = kinds[static_cast<int>(spur.kind)];
        d["mixer"] = spur.mixer.toStdString();
        d["m"] = spur.m;
        d["n"] = spur.n;
        d["frequency"] = spur.frequency;
        d["input_frequency"] = spur.inputFrequency;
        d["level"] = spur.levelDbm;
        d["dbc"] = spur.levelDbc;
        d["rejection"] = spur.rejectionDb;
        d["in_band"] = spur.inBand;
        d["estimated"] = spur.estimated;
        d["note"] = spur.note.toStdString();
        spurs.append(d);
    }
    return spurs;
}

py::dict budgetRunDict(const rf::BudgetRun &run)
{
    py::dict d = budgetResultDict(run.budget);
    py::list plan;
    for (const rf::ChainStage &stage : run.chain.stages) {
        py::dict item;
        item["component"] = stage.path.toStdString();
        item["input_frequency"] = stage.inputFrequency;
        item["output_frequency"] = stage.outputFrequency;
        if (stage.translation == rf::ChainStage::Translation::Mixer) {
            item["product"] = rf::mixerProductName(stage).toStdString();
            item["lo_source"] = stage.loSource.toStdString();
            item["lo_frequency"] = stage.loFrequency;
            item["lo_power"] = stage.loPowerDbm;
            item["lo_multiplication"] = stage.loMultiplication;
        }
        plan.append(item);
    }
    d["frequency_plan"] = plan;
    d["end_kind"] = run.chain.endKind.toStdString();
    if (!run.sweep.empty()) {
        py::list sweep;
        for (const rf::BudgetResult &point : run.sweep)
            sweep.append(budgetResultDict(point));
        d["sweep_frequencies"] = run.sweepFrequencies;
        d["sweep"] = sweep;
    }
    d["spurs"] = spurList(run.spurs);
    std::vector<std::string> warnings;
    for (const QString &warning : run.warnings)
        warnings.push_back(warning.toStdString());
    d["warnings"] = warnings;
    return d;
}

// Design variables from Python: {"variable": "Lm", "min": .., "max": ..} or
// {"component": "C1", "parameter": "c", "document": "", "min": .., "max": .., "log": True, "series": "E24"}.
rf::DesignVariable designVariableArgument(const py::handle &item)
{
    const py::dict d = item.cast<py::dict>();
    rf::DesignVariable v;
    const auto text = [&](const char *key) {
        return d.contains(key) ? QString::fromStdString(py::str(d[key])) : QString();
    };
    if (d.contains("variable")) {
        v.kind = rf::DesignVariable::Kind::Variable;
        v.name = text("variable");
    } else if (d.contains("component") && d.contains("parameter")) {
        v.kind = rf::DesignVariable::Kind::Parameter;
        v.component = text("component");
        v.name = text("parameter");
        v.document = text("document");
    } else {
        throw py::value_error("a design variable needs 'variable', or 'component' and 'parameter'");
    }
    if (!d.contains("min") || !d.contains("max"))
        throw py::value_error("a design variable needs 'min' and 'max'");
    const auto number = [&](const char *key) {
        const py::object value = d[key];
        if (py::isinstance<py::str>(value)) {
            QString error;
            const auto parsed = Expression::parse(QString::fromStdString(value.cast<std::string>())).evaluate({}, &error);
            if (!parsed)
                throw py::value_error(error.toStdString());
            return *parsed;
        }
        return value.cast<double>();
    };
    v.minimum = number("min");
    v.maximum = number("max");
    v.logarithmic = d.contains("log") ? d["log"].cast<bool>() : v.minimum > 0;
    if (d.contains("series") && !d["series"].is_none()) {
        v.series = seriesFromName(text("series"));
        if (!v.series)
            throw py::value_error("unknown series " + text("series").toStdString());
    }
    return v;
}

// Goals from Python: {"measurement": "dB(S11)", "below": -20, "band": ("2.4GHz", "2.5GHz"), "weight": 1}
// with exactly one of below / above / equal / minimize=True / maximize=True.
rf::Goal goalArgument(const py::handle &item)
{
    const py::dict d = item.cast<py::dict>();
    rf::Goal g;
    if (!d.contains("measurement"))
        throw py::value_error("a goal needs 'measurement'");
    g.measurement = QString::fromStdString(py::str(d["measurement"]));
    int kinds = 0;
    if (d.contains("below")) { g.type = rf::Goal::Type::Below; g.target = d["below"].cast<double>(); ++kinds; }
    if (d.contains("above")) { g.type = rf::Goal::Type::Above; g.target = d["above"].cast<double>(); ++kinds; }
    if (d.contains("equal")) { g.type = rf::Goal::Type::Equal; g.target = d["equal"].cast<double>(); ++kinds; }
    if (d.contains("minimize")) { g.type = rf::Goal::Type::Minimize; ++kinds; }
    if (d.contains("maximize")) { g.type = rf::Goal::Type::Maximize; ++kinds; }
    if (kinds != 1)
        throw py::value_error("a goal needs exactly one of below, above, equal, minimize, maximize");
    if (d.contains("weight"))
        g.weight = d["weight"].cast<double>();
    if (d.contains("band")) {
        const auto band = d["band"].cast<py::sequence>();
        const auto frequency = [](const py::handle &value) {
            if (py::isinstance<py::str>(value)) {
                const auto parsed = parseNumber(QString::fromStdString(value.cast<std::string>()));
                if (!parsed)
                    throw py::value_error("invalid frequency in band");
                return *parsed;
            }
            return value.cast<double>();
        };
        g.bandStart = frequency(band[0]);
        g.bandStop = frequency(band[1]);
    }
    return g;
}

py::list goalEvaluations(const QList<rf::GoalEvaluation> &evaluations)
{
    py::list list;
    for (const rf::GoalEvaluation &e : evaluations) {
        py::dict d;
        d["goal"] = e.goal.label().toStdString();
        d["worst"] = e.worst;
        d["met"] = e.met;
        d["cost"] = e.cost;
        d["error"] = e.error.toStdString();
        list.append(d);
    }
    return list;
}

// Groups the calls made inside a "with" block into one undo step.
struct EditScope {
    SchematicHandle schematic;
    QString description;
};

py::tuple pointTuple(QPoint point)
{
    return py::make_tuple(point.x(), point.y());
}

QPoint pointArgument(const py::handle &value)
{
    const auto sequence = value.cast<py::sequence>();
    if (sequence.size() != 2)
        throw py::value_error("a point is a pair (x, y)");
    return QPoint(sequence[0].cast<int>(), sequence[1].cast<int>());
}

QStringList idList(const py::object &ids)
{
    QStringList result;
    if (py::isinstance<py::str>(ids)) {
        result.append(QString::fromStdString(ids.cast<std::string>()));
        return result;
    }
    for (const py::handle &id : ids)
        result.append(QString::fromStdString(id.cast<std::string>()));
    return result;
}

py::dict componentDict(const PlacedComponent &component)
{
    py::dict parameters;
    for (auto it = component.parameters.begin(); it != component.parameters.end(); ++it)
        parameters[py::str(it.key().toStdString())] = it.value().toStdString();
    py::dict result;
    result["id"] = component.id.toStdString();
    result["reference"] = component.reference.toStdString();
    result["definition"] = component.definition.toStdString();
    result["at"] = pointTuple(component.at);
    result["rotation"] = component.rotation;
    result["mirror"] = component.mirror;
    result["parameters"] = parameters;
    return result;
}

py::dict definitionDict(const ComponentDefinition &definition)
{
    py::list parameters;
    for (const ParameterDefinition &parameter : definition.parameters) {
        py::dict item;
        item["name"] = parameter.name.toStdString();
        item["label"] = parameter.label.toStdString();
        item["unit"] = parameter.unit.toStdString();
        item["default"] = parameter.defaultValue.toStdString();
        item["shown"] = parameter.shown;
        std::vector<std::string> choices;
        for (const QString &choice : parameter.choices)
            choices.push_back(choice.toStdString());
        item["choices"] = choices;
        item["file"] = parameter.isFile;
        item["text"] = parameter.isText;
        parameters.append(item);
    }
    std::vector<std::string> pins, models;
    for (const QString &pin : definition.pins)
        pins.push_back(pin.toStdString());
    for (const ModelDefinition &model : definition.models)
        models.push_back(model.kind.toStdString());
    py::dict result;
    result["id"] = definition.id.toStdString();
    result["name"] = definition.name.toStdString();
    result["category"] = definition.category.toStdString();
    result["description"] = definition.description.toStdString();
    result["reference_prefix"] = definition.referencePrefix.toStdString();
    result["pins"] = pins;
    result["parameters"] = parameters;
    result["models"] = models;
    result["net_name"] = definition.netName.toStdString();
    result["role"] = definition.role.toStdString();
    return result;
}

// Accepts an item id ("c3") or a component reference ("A1").
QString componentId(const Schematic &schematic, const std::string &idOrReference)
{
    const QString text = QString::fromStdString(idOrReference);
    if (schematic.component(text))
        return text;
    if (const PlacedComponent *component = schematic.componentByReference(text))
        return component->id;
    throw py::key_error("no component '" + idOrReference + "'");
}

QPoint pinPoint(const SchematicHandle &h, const std::string &idOrReference, const std::string &pin)
{
    Schematic &schematic = h.get();
    const PlacedComponent &component = *schematic.component(componentId(schematic, idOrReference));
    const DefinitionPtr definition = h.project().resolve(schematic, component.definition).definition;
    if (!definition)
        throw std::runtime_error("the definition of " + component.reference.toStdString() + " is missing");
    const SymbolPin *symbolPin = definition->symbol.pin(QString::fromStdString(pin));
    if (!symbolPin)
        throw py::key_error(component.reference.toStdString() + " has no pin '" + pin + "'");
    return pinPosition(component, *symbolPin);
}

Session &session()
{
    Session *current = pythonSession();
    if (!current)
        throw std::runtime_error("no application session");
    return *current;
}

DocumentType typeArgument(const std::string &id)
{
    const auto type = documentTypeFromId(toQt(id));
    if (!type)
        throw py::value_error("unknown document type '" + id + "'; see lvdrfcad.document_types()");
    return *type;
}

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(toStd(message));
}

// ---- SPICE (requirements SP-1 to SP-10)

struct SpiceResultHandle {
    std::shared_ptr<const spice::SpiceSimulation> simulation;

    const spice::SpicePlot &plot(const py::object &which) const
    {
        const QList<spice::SpicePlot> &plots = simulation->run.plots;
        if (plots.isEmpty())
            throw py::value_error("the simulation has no results");
        if (which.is_none())
            return plots.first();
        const QString wanted = toQt(py::str(which));
        for (const spice::SpicePlot &p : plots)
            if (p.name == wanted || p.type == wanted)
                return p;
        throw py::key_error("no plot '" + wanted.toStdString() + "'");
    }
};

py::object jsonToPython(const QJsonObject &object)
{
    return py::module_::import("json").attr("loads")(QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString());
}

QJsonObject pythonToJson(const py::object &object)
{
    const std::string text = py::str(py::module_::import("json").attr("dumps")(object));
    return QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
}

py::object waveformValues(const spice::Waveform &w)
{
    if (w.isComplex)
        return py::cast(w.y);
    return py::cast(w.real());
}

spice::SpiceAnalysisSetup findSpiceAnalysis(const Schematic &schematic, const py::object &analysis)
{
    const QList<spice::SpiceAnalysisSetup> analyses = spice::spiceAnalyses(schematic.simulation());
    if (analysis.is_none()) {
        if (analyses.isEmpty())
            throw py::value_error("the schematic has no SPICE analysis; add one with add_spice_analysis()");
        return analyses.first();
    }
    const QString wanted = toQt(py::str(analysis));
    for (const spice::SpiceAnalysisSetup &setup : analyses)
        if (setup.id == wanted || setup.name == wanted)
            return setup;
    throw py::key_error("no SPICE analysis '" + wanted.toStdString() + "'");
}

py::dict batchToPython(const spice::BatchResult &result)
{
    py::dict out;
    std::vector<std::string> targets, measures;
    for (const spice::SpiceTarget &t : result.targets)
        targets.push_back(t.text.toStdString());
    for (const spice::SpiceMeasure &m : result.measures)
        measures.push_back(m.label().toStdString());
    out["targets"] = targets;
    out["measures"] = measures;
    py::list runs;
    for (const spice::BatchRun &run : result.runs) {
        py::dict item;
        item["label"] = run.label.toStdString();
        item["values"] = run.values;
        item["measures"] = run.measures;
        runs.append(item);
    }
    out["runs"] = runs;
    std::vector<std::string> warnings;
    for (const QString &w : result.warnings)
        warnings.push_back(w.toStdString());
    out["warnings"] = warnings;
    return out;
}

QList<spice::SpiceMeasure> measuresFromPython(const py::object &list)
{
    QList<spice::SpiceMeasure> measures;
    for (const py::handle &item : list) {
        if (py::isinstance<py::str>(item)) {
            spice::SpiceMeasure m;
            m.expression = toQt(py::str(item));
            measures << m;
        } else {
            measures << spice::SpiceMeasure::fromJson(pythonToJson(py::reinterpret_borrow<py::object>(item)));
        }
    }
    return measures;
}

spice::BatchOptions batchOptions(int workers)
{
    spice::BatchOptions options;
    if (workers != 1) {
        auto pool = std::make_shared<spice::SpiceProcessPool>(workers);
        if (QFileInfo::exists(pool->program()))
            options.execute = [pool](QList<spice::SpiceSimulation *> &simulations, const std::atomic_bool *cancel,
                                     const std::function<void(int)> &progress) { pool->execute(simulations, cancel, progress); };
    }
    return options;
}

} // namespace

void registerBindings(py::module_ &m)
{
    m.doc() = "lvd RF cad application API";
    m.attr("__version__") = LVD_VERSION;

    py::class_<OutputStream>(m, "_OutputStream")
        .def(py::init<bool>())
        .def("write",
             [](const OutputStream &stream, const std::string &text) {
                 writePythonOutput(toQt(text), stream.isError);
                 return text.size();
             })
        .def("flush", [](const OutputStream &) {})
        .def("isatty", [](const OutputStream &) { return false; });

    registerAntennaBindings(m);

    py::class_<DocumentHandle>(m, "Document", "A design document of the open project.")
        .def_property_readonly("name", [](const DocumentHandle &h) { return toStd(h.get().name()); })
        .def_property_readonly("type", [](const DocumentHandle &h) { return toStd(documentTypeInfo(h.get().type()).id); },
                               "Document type identifier: 'rf', 'spice', 'antenna' (array) or 'antenna-element'.")
        .def_property_readonly("path", [](const DocumentHandle &h) { return toStd(h.get().filePath()); })
        .def_property_readonly("modified", [](const DocumentHandle &h) { return h.get().isModified(); })
        .def_property_readonly("schematic",
                               [](const DocumentHandle &h) -> py::object {
                                   if (!h.get().schematic())
                                       return py::none();
                                   return py::cast(SchematicHandle{h.pointer});
                               },
                               "The schematic of an 'rf' or 'spice' document; None for other types.")
        .def_property_readonly("array", [](const DocumentHandle &h) { return antennaArrayObject(&h.get()); },
                               "The AntennaArray of an 'antenna' document; None for other types.")
        .def_property_readonly("element", [](const DocumentHandle &h) { return antennaElementObject(&h.get()); },
                               "The AntennaElement of an 'antenna-element' document; None for other types.")
        .def("save",
             [](const DocumentHandle &h) {
                 QString error;
                 if (!h.get().save(&error))
                     fail(error);
             })
        .def("__eq__", [](const DocumentHandle &a, const DocumentHandle &b) { return a.pointer == b.pointer; })
        .def("__repr__", [](const DocumentHandle &h) {
            if (!h.pointer)
                return std::string("<Document (removed)>");
            return "<Document '" + toStd(h.pointer->name()) + "' type='"
                   + toStd(documentTypeInfo(h.pointer->type()).id) + "'>";
        });

    py::class_<ProjectHandle>(m, "Project", "The open project.")
        .def("import_spice_netlist",
             [](const ProjectHandle &h, const py::object &path, const std::string &name) {
                 QString error;
                 QStringList warnings;
                 Document *document = spice::importNetlistDocument(h.get(), pathArgument(path), QString::fromStdString(name),
                                                                   &error, &warnings);
                 if (!document)
                     throw std::runtime_error(error.toStdString());
                 for (const QString &warning : warnings)
                     writePythonOutput(QStringLiteral("warning: %1\n").arg(warning), true);
                 return DocumentHandle{document};
             },
             py::arg("path"), py::arg("name") = "",
             "Imports a SPICE netlist file as a simulation-only SPICE document (the analysis cards become\n"
             "analyses); returns the document.")
        .def_property_readonly("name", [](const ProjectHandle &h) { return toStd(h.get().name()); })
        .def_property_readonly("directory", [](const ProjectHandle &h) { return toStd(h.get().directory()); })
        .def_property_readonly("path", [](const ProjectHandle &h) { return toStd(h.get().filePath()); })
        .def_property_readonly("documents",
                               [](const ProjectHandle &h) {
                                   std::vector<DocumentHandle> result;
                                   for (Document *document : h.get().documents())
                                       result.push_back(DocumentHandle{document});
                                   return result;
                               })
        .def("document",
             [](const ProjectHandle &h, const std::string &name) -> py::object {
                 Document *document = h.get().document(toQt(name));
                 if (!document)
                     return py::none();
                 return py::cast(DocumentHandle{document});
             },
             py::arg("name"), "Returns the document with this name, or None.")
        .def("new_document",
             [](const ProjectHandle &h, const std::string &type, const std::string &name) {
                 QString error;
                 Document *document = h.get().createDocument(typeArgument(type), toQt(name), &error);
                 if (!document)
                     fail(error);
                 return DocumentHandle{document};
             },
             py::arg("type"), py::arg("name"), "Creates an empty document and its file.")
        .def("remove_document",
             [](const ProjectHandle &h, const DocumentHandle &document, bool deleteFile) {
                 QString error;
                 if (!h.get().removeDocument(&document.get(), deleteFile, &error))
                     fail(error);
             },
             py::arg("document"), py::arg("delete_file") = false,
             "Removes a document from the project, optionally deleting its file.")
        .def("save",
             [](const ProjectHandle &h) {
                 QString error;
                 if (!h.get().saveAll(&error))
                     fail(error);
             },
             "Saves the project file and all modified documents.")
        .def("__repr__", [](const ProjectHandle &h) {
            if (!h.pointer)
                return std::string("<Project (closed)>");
            return "<Project '" + toStd(h.pointer->name()) + "'>";
        });

    py::class_<EditScope>(m, "_EditScope")
        .def("__enter__",
             [](EditScope &scope) {
                 scope.schematic.get().beginEdit(scope.description);
                 return scope;
             })
        .def("__exit__", [](EditScope &scope, const py::object &, const py::object &, const py::object &) {
            if (scope.schematic.document)
                scope.schematic.get().endEdit();
            return false;
        });

    py::class_<SchematicHandle>(m, "Schematic",
                                "Schematic of a document. Components can be given by id ('c3') or reference ('A1').\n"
                                "Coordinates are drawing units on a 10-unit grid, Y pointing down.")
        .def("components",
             [](const SchematicHandle &h) {
                 py::list result;
                 for (const PlacedComponent &component : h.get().components())
                     result.append(componentDict(component));
                 return result;
             })
        .def("component",
             [](const SchematicHandle &h, const std::string &idOrReference) {
                 Schematic &schematic = h.get();
                 return componentDict(*schematic.component(componentId(schematic, idOrReference)));
             },
             py::arg("component"))
        .def("wires",
             [](const SchematicHandle &h) {
                 py::list result;
                 for (const Wire &wire : h.get().wires()) {
                     py::list points;
                     for (const QPoint &point : wire.points)
                         points.append(pointTuple(point));
                     result.append(py::dict(py::arg("id") = wire.id.toStdString(), py::arg("points") = points));
                 }
                 return result;
             })
        .def("labels",
             [](const SchematicHandle &h) {
                 py::list result;
                 for (const NetLabel &label : h.get().labels())
                     result.append(py::dict(py::arg("id") = label.id.toStdString(), py::arg("net") = label.net.toStdString(),
                                            py::arg("at") = pointTuple(label.at)));
                 return result;
             })
        .def("junctions",
             [](const SchematicHandle &h) {
                 py::list result;
                 for (const Junction &junction : h.get().junctions())
                     result.append(py::dict(py::arg("id") = junction.id.toStdString(), py::arg("at") = pointTuple(junction.at)));
                 return result;
             })
        .def("add_component",
             [](const SchematicHandle &h, const std::string &definition, const py::object &at, int rotation, bool mirror,
                const py::object &parameters, const py::object &reference) {
                 Schematic &schematic = h.get();
                 const QString key = QString::fromStdString(definition);
                 const DefinitionPtr found = h.project().resolve(schematic, key).definition;
                 if (!found)
                     throw py::key_error("definition '" + definition + "' not found");
                 // Check everything before changing the schematic, so a failed call leaves no trace.
                 QList<std::pair<QString, QString>> values;
                 if (!parameters.is_none()) {
                     for (const auto &[name, value] : parameters.cast<py::dict>()) {
                         const QString parameterName = QString::fromStdString(py::str(name));
                         if (!found->parameter(parameterName))
                             throw py::key_error(found->name.toStdString() + " has no parameter '" + parameterName.toStdString() + "'");
                         values.append({parameterName, QString::fromStdString(py::str(value))});
                     }
                 }
                 const QPoint position = pointArgument(at);
                 const QString newReference = reference.is_none() ? QString() : QString::fromStdString(py::str(reference)).trimmed();
                 if (!reference.is_none() && newReference.isEmpty())
                     throw py::value_error("the reference is empty");

                 Schematic::Edit edit(&schematic, QStringLiteral("Place %1").arg(found->name));
                 const QString id = schematic.addComponent(key, found.get(), position, rotation, mirror);
                 for (const auto &[name, value] : values)
                     schematic.setParameter(id, name, value);
                 if (!newReference.isEmpty())
                     schematic.setReference(id, newReference);
                 return id.toStdString();
             },
             py::arg("definition"), py::arg("at"), py::arg("rotation") = 0, py::arg("mirror") = false,
             py::arg("parameters") = py::none(), py::arg("reference") = py::none(),
             "Places a component, e.g. add_component('rf-system:amplifier', (100, 0), parameters={'gain': '17'}).\n"
             "Returns the new item id.")
        .def("parameter",
             [](const SchematicHandle &h, const std::string &component, const std::string &name) {
                 Schematic &schematic = h.get();
                 const PlacedComponent &placed = *schematic.component(componentId(schematic, component));
                 const DefinitionPtr definition = h.project().resolve(schematic, placed.definition).definition;
                 const QString parameterName = QString::fromStdString(name);
                 if (!definition || !definition->parameter(parameterName)) {
                     const auto it = placed.parameters.constFind(parameterName);
                     if (it == placed.parameters.cend())
                         throw py::key_error("no parameter '" + name + "'");
                     return it.value().toStdString();
                 }
                 return parameterText(placed, *definition, parameterName).toStdString();
             },
             py::arg("component"), py::arg("name"), "The parameter text: the component's own value or the default.")
        .def("set_parameter",
             [](const SchematicHandle &h, const std::string &component, const std::string &name, const py::object &value) {
                 Schematic &schematic = h.get();
                 schematic.setParameter(componentId(schematic, component), QString::fromStdString(name),
                                        QString::fromStdString(py::str(value)));
             },
             py::arg("component"), py::arg("name"), py::arg("value"))
        .def("reset_parameter",
             [](const SchematicHandle &h, const std::string &component, const std::string &name) {
                 Schematic &schematic = h.get();
                 schematic.resetParameter(componentId(schematic, component), QString::fromStdString(name));
             },
             py::arg("component"), py::arg("name"))
        .def("set_reference",
             [](const SchematicHandle &h, const std::string &component, const std::string &reference) {
                 Schematic &schematic = h.get();
                 if (!schematic.setReference(componentId(schematic, component), QString::fromStdString(reference)))
                     throw py::value_error("invalid reference");
             },
             py::arg("component"), py::arg("reference"))
        .def("pin_position", [](const SchematicHandle &h, const std::string &component, const std::string &pin) {
                 return pointTuple(pinPoint(h, component, pin));
             },
             py::arg("component"), py::arg("pin"))
        .def("connect",
             [](const SchematicHandle &h, const std::string &a, const std::string &pinA, const std::string &b,
                const std::string &pinB) {
                 const QPoint start = pinPoint(h, a, pinA);
                 const QPoint end = pinPoint(h, b, pinB);
                 QList<QPoint> points{start};
                 if (start.x() != end.x() && start.y() != end.y())
                     points.append(QPoint(end.x(), start.y()));
                 points.append(end);
                 Schematic &schematic = h.get();
                 Schematic::Edit edit(&schematic, QStringLiteral("Connect"));
                 const QString id = schematic.addWire(points);
                 // A wire between pins at the same point is not needed.
                 const DefinitionLookup lookup = h.project().lookupFor(schematic);
                 for (const QPoint &point : missingJunctions(schematic, lookup))
                     schematic.addJunction(point);
                 return id.toStdString();
             },
             py::arg("a"), py::arg("pin_a"), py::arg("b"), py::arg("pin_b"),
             "Draws a wire from a pin to another pin (with one bend if needed). Returns the wire id.")
        .def("add_wire",
             [](const SchematicHandle &h, const py::sequence &points) {
                 QList<QPoint> list;
                 for (const py::handle &point : points)
                     list.append(pointArgument(point));
                 const QString id = h.get().addWire(list);
                 if (id.isEmpty())
                     throw py::value_error("a wire needs at least two distinct points");
                 return id.toStdString();
             },
             py::arg("points"))
        .def("add_label",
             [](const SchematicHandle &h, const std::string &net, const py::object &at) {
                 const QString id = h.get().addLabel(QString::fromStdString(net), pointArgument(at));
                 if (id.isEmpty())
                     throw py::value_error("a label needs a net name");
                 return id.toStdString();
             },
             py::arg("net"), py::arg("at"))
        .def("add_junction",
             [](const SchematicHandle &h, const py::object &at) { return h.get().addJunction(pointArgument(at)).toStdString(); },
             py::arg("at"))
        .def("add_text",
             [](const SchematicHandle &h, const std::string &text, const py::object &at, int size) {
                 return h.get().addText(QString::fromStdString(text), pointArgument(at), size).toStdString();
             },
             py::arg("text"), py::arg("at"), py::arg("size") = 12)
        .def("remove",
             [](const SchematicHandle &h, const py::object &ids) {
                 Schematic &schematic = h.get();
                 QStringList list;
                 for (const QString &id : idList(ids))
                     list.append(schematic.contains(id) ? id : componentId(schematic, id.toStdString()));
                 schematic.removeItems(list);
             },
             py::arg("items"), "Removes items given by id (or component reference); one id or a list.")
        .def("move",
             [](const SchematicHandle &h, const py::object &ids, int dx, int dy, bool dragWires) {
                 Schematic &schematic = h.get();
                 QStringList list;
                 QHash<QString, QList<QPoint>> pins;
                 const DefinitionLookup lookup = h.project().lookupFor(schematic);
                 for (const QString &raw : idList(ids)) {
                     const QString id = schematic.contains(raw) ? raw : componentId(schematic, raw.toStdString());
                     list.append(id);
                     if (const PlacedComponent *component = schematic.component(id)) {
                         if (const DefinitionPtr definition = lookup(component->definition))
                             pins.insert(id, pinPositions(*component, *definition));
                     }
                 }
                 schematic.moveItems(list, QPoint(dx, dy), pins, dragWires);
             },
             py::arg("items"), py::arg("dx"), py::arg("dy"), py::arg("drag_wires") = true)
        .def("rotate",
             [](const SchematicHandle &h, const py::object &ids, const py::object &center, int degrees) {
                 Schematic &schematic = h.get();
                 QStringList list;
                 for (const QString &id : idList(ids))
                     list.append(schematic.contains(id) ? id : componentId(schematic, id.toStdString()));
                 schematic.rotateItems(list, pointArgument(center), degrees);
             },
             py::arg("items"), py::arg("center"), py::arg("degrees") = 90,
             "Rotates counter-clockwise on screen about a centre point.")
        .def("mirror",
             [](const SchematicHandle &h, const py::object &ids, const py::object &center) {
                 Schematic &schematic = h.get();
                 QStringList list;
                 for (const QString &id : idList(ids))
                     list.append(schematic.contains(id) ? id : componentId(schematic, id.toStdString()));
                 schematic.mirrorItems(list, pointArgument(center));
             },
             py::arg("items"), py::arg("center"), "Mirrors about the vertical line through the centre.")
        .def_property(
            "variables",
            [](const SchematicHandle &h) {
                py::list result;
                for (const Variable &variable : h.get().variables())
                    result.append(py::make_tuple(variable.name.toStdString(), variable.value.toStdString()));
                return result;
            },
            [](const SchematicHandle &h, const py::object &value) {
                QList<Variable> variables;
                const auto add = [&variables](const py::handle &name, const py::handle &expression) {
                    variables.append(Variable{QString::fromStdString(py::str(name)), QString::fromStdString(py::str(expression))});
                };
                if (py::isinstance<py::dict>(value)) {
                    for (const auto &[name, expression] : value.cast<py::dict>())
                        add(name, expression);
                } else {
                    for (const py::handle &item : value) {
                        const auto pair = item.cast<py::sequence>();
                        add(pair[0], pair[1]);
                    }
                }
                h.get().setVariables(variables);
            },
            "Variables as a list of (name, expression) pairs; assign a list of pairs or a dict.")
        .def("edit",
             [](const SchematicHandle &h, const std::string &description) {
                 return EditScope{h, QString::fromStdString(description)};
             },
             py::arg("description") = "Script edit", "Context manager: everything inside is one undo step.")
        .def("undo", [](const SchematicHandle &h) { h.get().undoStack()->undo(); })
        .def("redo", [](const SchematicHandle &h) { h.get().undoStack()->redo(); })
        .def_property_readonly("can_undo", [](const SchematicHandle &h) { return h.get().undoStack()->canUndo(); })
        .def_property_readonly("can_redo", [](const SchematicHandle &h) { return h.get().undoStack()->canRedo(); })
        .def("nets",
             [](const SchematicHandle &h) {
                 Schematic &schematic = h.get();
                 const Connectivity connectivity = computeConnectivity(schematic, h.project().lookupFor(schematic));
                 py::dict result;
                 for (const SheetNet &net : connectivity.nets) {
                     std::vector<std::string> pins;
                     for (const NetPin &pin : net.pins) {
                         const PlacedComponent *component = schematic.component(pin.componentId);
                         pins.push_back((component ? component->reference : pin.componentId).toStdString() + "."
                                        + pin.pin.toStdString());
                     }
                     result[py::str(net.name.toStdString())] = pins;
                 }
                 return result;
             },
             "Nets of this sheet: {net name: ['A1.in', ...]}.")
        .def("net_of",
             [](const SchematicHandle &h, const std::string &component, const std::string &pin) -> py::object {
                 Schematic &schematic = h.get();
                 const Connectivity connectivity = computeConnectivity(schematic, h.project().lookupFor(schematic));
                 const int index = connectivity.netOfPin(componentId(schematic, component), QString::fromStdString(pin));
                 if (index < 0)
                     return py::none();
                 return py::str(connectivity.nets.at(index).name.toStdString());
             },
             py::arg("component"), py::arg("pin"))
        .def("netlist",
             [](const SchematicHandle &h) {
                 const Netlist netlist = buildNetlist(h.project(), h.doc());
                 const auto componentDict = [](const FlatComponent &flat) {
                     py::dict pins, texts, values;
                     for (auto it = flat.pinNets.begin(); it != flat.pinNets.end(); ++it)
                         pins[py::str(it.key().toStdString())] = it.value().toStdString();
                     for (auto it = flat.parameterText.begin(); it != flat.parameterText.end(); ++it)
                         texts[py::str(it.key().toStdString())] = it.value().toStdString();
                     for (auto it = flat.parameterValues.begin(); it != flat.parameterValues.end(); ++it)
                         values[py::str(it.key().toStdString())] = it.value();
                     py::dict item;
                     item["path"] = flat.path.toStdString();
                     item["definition"] = flat.definitionKey.toStdString();
                     item["document"] = flat.document.toStdString();
                     item["pins"] = pins;
                     item["parameters"] = texts;
                     item["values"] = values;
                     return item;
                 };
                 py::list components, substrates;
                 for (const FlatComponent &flat : netlist.components)
                     components.append(componentDict(flat));
                 // Substrate parts hold data, not circuit elements: listed apart.
                 for (const FlatComponent &flat : netlist.substrates)
                     substrates.append(componentDict(flat));
                 std::vector<std::string> nets, errors;
                 for (const QString &net : netlist.nets)
                     nets.push_back(net.toStdString());
                 for (const QString &message : netlist.errors)
                     errors.push_back(message.toStdString());
                 py::dict result;
                 result["components"] = components;
                 result["substrates"] = substrates;
                 result["nets"] = nets;
                 result["errors"] = errors;
                 result["text"] = netlist.toText().toStdString();
                 return result;
             },
             "Flattened netlist of this sheet and everything below it, with evaluated parameters;\n"
             "'substrates' lists the Substrate parts (rf-circuit:substrate) apart from the components.")
        .def("check",
             [](const SchematicHandle &h) {
                 py::list result;
                 for (const RuleViolation &violation : checkSchematic(h.project(), h.doc())) {
                     py::dict item;
                     item["severity"] = severityName(violation.severity).toStdString();
                     item["document"] = violation.document.toStdString();
                     item["item"] = violation.itemId.toStdString();
                     item["message"] = violation.message.toStdString();
                     result.append(item);
                 }
                 return result;
             },
             "Electrical rule check of this sheet and its sub-sheets.")
        .def_property_readonly("analyses",
                               [](const SchematicHandle &h) {
                                   py::list result;
                                   for (const rf::LinearAnalysisSetup &analysis : rf::linearAnalyses(h.get().simulation()))
                                       result.append(analysisDict(analysis));
                                   return result;
                               },
                               "Analysis setups stored in the schematic.")
        .def("add_analysis",
             [](const SchematicHandle &h, const std::string &start, const std::string &stop, const py::object &points,
                const std::string &sweep, bool noise, const std::string &name, const std::string &id, bool plots) {
                 rf::LinearAnalysisSetup setup;
                 setup.id = QString::fromStdString(id);
                 setup.name = QString::fromStdString(name);
                 setup.sweep = QString::fromStdString(sweep);
                 setup.start = QString::fromStdString(start);
                 setup.stop = QString::fromStdString(stop);
                 setup.points = QString::fromStdString(py::str(points));
                 setup.noise = noise;
                 QString error;
                 if (!rf::sweepFrequencies(setup, rf::sheetScope(h.doc()), &error))
                     throw py::value_error(error.toStdString());
                 QString assigned;
                 Schematic &schematic = h.get();
                 const bool isNew = setup.id.isEmpty();
                 QJsonObject simulation = rf::withLinearAnalysis(schematic.simulation(), setup, &assigned);
                 if (plots && isNew)
                     simulation = withDefaultPlots(simulation, assigned, simulationPorts(h.project(), h.doc()), noise);
                 schematic.setSimulation(simulation);
                 return assigned.toStdString();
             },
             py::arg("start"), py::arg("stop"), py::arg("points") = 201, py::arg("sweep") = "linear",
             py::arg("noise") = true, py::arg("name") = "", py::arg("id") = "", py::arg("plots") = true,
             "Adds (or, with an existing id, replaces) an S-parameter analysis; returns its id.\n"
             "Values are expressions such as '1GHz' or 'f0*2'; sweep is 'linear' or 'log'.")
        .def("simulate",
             [](const SchematicHandle &h, const py::object &analysis, const py::object &start, const py::object &stop,
                const py::object &points, const std::string &sweep, bool noise) {
                 rf::LinearAnalysisSetup setup;
                 if (!analysis.is_none()) {
                     const QString wanted = QString::fromStdString(py::str(analysis));
                     bool found = false;
                     for (const rf::LinearAnalysisSetup &candidate : rf::linearAnalyses(h.get().simulation())) {
                         if (candidate.id == wanted || candidate.name == wanted) {
                             setup = candidate;
                             found = true;
                             break;
                         }
                     }
                     if (!found)
                         throw py::key_error("no analysis '" + wanted.toStdString() + "'");
                 } else if (!start.is_none() && !stop.is_none()) {
                     setup.start = QString::fromStdString(py::str(start));
                     setup.stop = QString::fromStdString(py::str(stop));
                     setup.points = QString::fromStdString(py::str(points));
                     setup.sweep = QString::fromStdString(sweep);
                     setup.noise = noise;
                 } else {
                     const auto analyses = rf::linearAnalyses(h.get().simulation());
                     if (analyses.isEmpty())
                         throw py::value_error("give an analysis, or start and stop");
                     setup = analyses.first();
                 }
                 auto result = std::make_shared<rf::LinearResult>(rf::runLinearAnalysis(h.project(), h.doc(), setup));
                 for (const QString &warning : result->warnings)
                     writePythonOutput(QStringLiteral("warning: %1\n").arg(warning), true);
                 if (!result->ok())
                     throw std::runtime_error(result->errors.join(QLatin1Char('\n')).toStdString());
                 return ResultHandle{std::move(result)};
             },
             py::arg("analysis") = py::none(), py::arg("start") = py::none(), py::arg("stop") = py::none(),
             py::arg("points") = 201, py::arg("sweep") = "linear", py::arg("noise") = true,
             "Runs the S-parameter and noise analysis: a stored analysis (id or name), an ad-hoc sweep\n"
             "(start, stop, points), or the first stored analysis. Returns a Result.")
        .def("add_budget",
             [](const SchematicHandle &h, const py::object &path, const std::string &start, const std::string &frequency,
                const std::string &power, const std::string &bandwidth, bool mismatch, const std::string &sweepStart,
                const std::string &sweepStop, const py::object &sweepPoints, bool spurs, const py::object &spurOrder,
                const std::string &name, const std::string &id, const std::string &pnStart, const std::string &pnStop,
                const std::string &blockerOffset, const std::string &blockerPower) {
                 rf::BudgetAnalysisSetup setup;
                 setup.phaseNoiseStart = QString::fromStdString(pnStart);
                 setup.phaseNoiseStop = QString::fromStdString(pnStop);
                 setup.blockerOffset = QString::fromStdString(blockerOffset);
                 setup.blockerPower = QString::fromStdString(blockerPower);
                 setup.id = QString::fromStdString(id);
                 setup.name = QString::fromStdString(name);
                 if (!path.is_none()) {
                     for (const py::handle &entry : path)
                         setup.path.append(QString::fromStdString(py::str(entry)));
                 }
                 setup.start = QString::fromStdString(start);
                 setup.frequency = QString::fromStdString(frequency);
                 setup.power = QString::fromStdString(power);
                 setup.bandwidth = QString::fromStdString(bandwidth);
                 setup.mismatch = mismatch;
                 setup.sweepStart = QString::fromStdString(sweepStart);
                 setup.sweepStop = QString::fromStdString(sweepStop);
                 setup.sweepPoints = QString::fromStdString(py::str(sweepPoints));
                 setup.spurs = spurs;
                 setup.spurOrder = QString::fromStdString(py::str(spurOrder));
                 QString assigned;
                 Schematic &schematic = h.get();
                 schematic.setSimulation(rf::withBudgetAnalysis(schematic.simulation(), setup, &assigned));
                 return assigned.toStdString();
             },
             py::arg("path") = py::none(), py::arg("start") = "", py::arg("frequency") = "", py::arg("power") = "",
             py::arg("bandwidth") = "1MHz", py::arg("mismatch") = true, py::arg("sweep_start") = "",
             py::arg("sweep_stop") = "", py::arg("sweep_points") = 101, py::arg("spurs") = true,
             py::arg("spur_order") = 5, py::arg("name") = "", py::arg("id") = "", py::arg("pn_start") = "",
             py::arg("pn_stop") = "", py::arg("blocker_offset") = "", py::arg("blocker_power") = "",
             "Stores a system budget analysis; returns its id. path: optional list of references,\n"
             "with 'REF.pin' to choose a branch; frequency and power override the start component.\n"
             "pn_start, pn_stop: phase-noise integration range (default bandwidth/1000 to bandwidth/2);\n"
             "blocker_offset, blocker_power: a blocker for reciprocal mixing (empty power: none).")
        .def("budget",
             [](const SchematicHandle &h, const py::object &analysis, const py::object &path, const std::string &frequency,
                const std::string &power, const std::string &bandwidth, bool mismatch, bool spurs,
                const std::string &pnStart, const std::string &pnStop, const std::string &blockerOffset,
                const std::string &blockerPower) {
                 rf::BudgetAnalysisSetup setup;
                 if (!analysis.is_none()) {
                     const QString wanted = QString::fromStdString(py::str(analysis));
                     bool found = false;
                     for (const rf::BudgetAnalysisSetup &candidate : rf::budgetAnalyses(h.get().simulation())) {
                         if (candidate.id == wanted || candidate.name == wanted) {
                             setup = candidate;
                             found = true;
                         }
                     }
                     if (!found)
                         throw py::key_error("no budget analysis '" + wanted.toStdString() + "'");
                 } else {
                     if (!path.is_none()) {
                         for (const py::handle &entry : path)
                             setup.path.append(QString::fromStdString(py::str(entry)));
                     }
                     setup.frequency = QString::fromStdString(frequency);
                     setup.power = QString::fromStdString(power);
                     setup.bandwidth = QString::fromStdString(bandwidth);
                     setup.mismatch = mismatch;
                     setup.spurs = spurs;
                     setup.phaseNoiseStart = QString::fromStdString(pnStart);
                     setup.phaseNoiseStop = QString::fromStdString(pnStop);
                     setup.blockerOffset = QString::fromStdString(blockerOffset);
                     setup.blockerPower = QString::fromStdString(blockerPower);
                 }
                 const rf::BudgetRun run = rf::runBudgetAnalysis(h.project(), h.doc(), setup);
                 for (const QString &warning : run.warnings)
                     writePythonOutput(QStringLiteral("warning: %1\n").arg(warning), true);
                 if (!run.ok()) {
                     const QStringList errors = run.errors + run.chain.errors + run.budget.errors;
                     throw std::runtime_error(errors.join(QLatin1Char('\n')).toStdString());
                 }
                 return budgetRunDict(run);
             },
             py::arg("analysis") = py::none(), py::arg("path") = py::none(), py::arg("frequency") = "",
             py::arg("power") = "", py::arg("bandwidth") = "1MHz", py::arg("mismatch") = true, py::arg("spurs") = true,
             py::arg("pn_start") = "", py::arg("pn_stop") = "", py::arg("blocker_offset") = "",
             py::arg("blocker_power") = "",
             "Runs a system budget: a stored analysis (id or name) or an ad-hoc one. Returns a dictionary with\n"
             "'stages' (per-stage and cumulative figures), 'frequency_plan', 'adc', 'dac', 'detector',\n"
             "'phase_noise', 'spurs', 'sweep', 'warnings'.")
        .def("optimize",
             [](const SchematicHandle &h, const py::sequence &variables, const py::sequence &goals,
                const py::object &analysis, const std::string &method, int maxEvaluations, bool apply, bool snapped) {
                 rf::OptimizationSetup setup;
                 for (const py::handle &item : variables)
                     setup.variables.append(designVariableArgument(item));
                 for (const py::handle &item : goals)
                     setup.goals.append(goalArgument(item));
                 setup.method = method == "levenberg-marquardt" || method == "lm"
                                    ? rf::OptimizationSetup::Method::LevenbergMarquardt
                                    : rf::OptimizationSetup::Method::NelderMead;
                 setup.maxEvaluations = maxEvaluations;
                 if (!analysis.is_none()) {
                     const QString wanted = QString::fromStdString(py::str(analysis));
                     for (const rf::LinearAnalysisSetup &candidate : rf::linearAnalyses(h.get().simulation())) {
                         if (candidate.id == wanted || candidate.name == wanted)
                             setup.analysis = candidate.id;
                     }
                     if (setup.analysis.isEmpty())
                         throw py::key_error("no analysis '" + wanted.toStdString() + "'");
                 }
                 auto snapshot = h.project().snapshot();
                 const rf::OptimizationResult result = rf::optimize(*snapshot, h.doc().name(), setup);
                 for (const QString &warning : result.warnings)
                     writePythonOutput(QStringLiteral("warning: %1\n").arg(warning), true);
                 if (!result.ok())
                     throw std::runtime_error(result.errors.join(QLatin1Char('\n')).toStdString());
                 const std::vector<double> &chosen = snapped ? result.snapped : result.best;
                 if (apply) {
                     // One undo step per touched schematic, grouped in one edit each.
                     QList<Schematic *> touched{h.doc().schematic()};
                     for (const rf::DesignVariable &variable : setup.variables) {
                         if (variable.kind == rf::DesignVariable::Kind::Parameter && !variable.document.isEmpty()) {
                             if (Document *sheet = h.project().document(variable.document); sheet && sheet->schematic()
                                 && !touched.contains(sheet->schematic()))
                                 touched.append(sheet->schematic());
                         }
                     }
                     for (Schematic *schematic : touched)
                         schematic->beginEdit(QStringLiteral("Apply optimisation"));
                     QString error;
                     const bool ok = rf::applyDesignValues(h.project(), h.doc(), setup.variables, chosen, &error);
                     for (Schematic *schematic : touched)
                         schematic->endEdit();
                     if (!ok)
                         throw std::runtime_error(error.toStdString());
                 }
                 py::dict d;
                 std::vector<std::string> labels;
                 for (const rf::DesignVariable &variable : setup.variables)
                     labels.push_back(variable.label().toStdString());
                 d["variables"] = labels;
                 d["initial"] = result.initial;
                 d["best"] = result.best;
                 d["snapped"] = result.snapped;
                 d["initial_cost"] = result.initialCost;
                 d["best_cost"] = result.bestCost;
                 d["snapped_cost"] = result.snappedCost;
                 d["goals"] = goalEvaluations(snapped ? result.goalsSnapped : result.goalsBest);
                 d["evaluations"] = result.evaluations;
                 d["converged"] = result.converged;
                 d["applied"] = apply;
                 return d;
             },
             py::arg("variables"), py::arg("goals"), py::arg("analysis") = py::none(), py::arg("method") = "nelder-mead",
             py::arg("max_evaluations") = 500, py::arg("apply") = true, py::arg("snapped") = true,
             "Optimises design variables to meet goals, on a copy of the project; with apply=True the result\n"
             "is written to the schematic as one undo step. Variables: {'variable': 'Lm', 'min': '0.5nH',\n"
             "'max': '20nH'} or {'component': 'C1', 'parameter': 'c', 'min': .., 'max': .., 'series': 'E24'}.\n"
             "Goals: {'measurement': 'dB(S11)', 'below': -20, 'band': ('2.4GHz', '2.5GHz')}.")
        .def_property_readonly("spice_analyses",
                               [](const SchematicHandle &h) {
                                   py::list result;
                                   for (const spice::SpiceAnalysisSetup &setup : spice::spiceAnalyses(h.get().simulation()))
                                       result.append(jsonToPython(setup.toJson()));
                                   return result;
                               },
                               "SPICE analysis setups stored in the schematic, as dictionaries.")
        .def("add_spice_analysis",
             [](const SchematicHandle &h, const std::string &type, const std::string &name, const std::string &id,
                const py::kwargs &fields) {
                 QString typeName = QString::fromStdString(type);
                 if (!typeName.startsWith(QLatin1String("spice-")))
                     typeName.prepend(QStringLiteral("spice-"));
                 const auto kind = spice::analysisKindFromType(typeName);
                 if (!kind)
                     throw py::value_error("unknown SPICE analysis '" + type
                                           + "'; use op, dc, ac, tran, noise, fourier, tf, pz or sens");
                 spice::SpiceAnalysisSetup setup = spice::SpiceAnalysisSetup::create(*kind, QString::fromStdString(name));
                 setup.id = QString::fromStdString(id);
                 const QJsonObject given = pythonToJson(fields);
                 for (auto it = given.begin(); it != given.end(); ++it) {
                     // Numbers are stored as expression text, like typed values.
                     if (it.value().isDouble())
                         setup.set(it.key(), QString::number(it.value().toDouble(), 'g', 15));
                     else
                         setup.set(it.key(), it.value());
                 }
                 QString assigned;
                 Schematic &schematic = h.get();
                 schematic.setSimulation(spice::withSpiceAnalysis(schematic.simulation(), setup, &assigned));
                 return assigned.toStdString();
             },
             py::arg("type"), py::arg("name") = "", py::arg("id") = "",
             "Adds (or, with an existing id, replaces) a SPICE analysis; returns its id.\n"
             "type: op, dc, ac, tran, noise, fourier, tf, pz or sens. Fields as keyword arguments, e.g.\n"
             "add_spice_analysis('tran', stop='5ms', step='1us') or add_spice_analysis('ac', start='10Hz',\n"
             "stop='1MHz', points=20). See docs/file-format.md section 5.1 for the fields of each type.")
        .def("spice_sweep",
             [](const SchematicHandle &h, const py::list &axes, const py::object &measures, const py::object &analysis, int workers) {
                 const spice::SpiceAnalysisSetup setup = findSpiceAnalysis(h.get(), analysis);
                 QList<spice::SweepAxis> list;
                 for (const py::handle &axis : axes)
                     list << spice::SweepAxis::fromJson(pythonToJson(py::reinterpret_borrow<py::object>(axis)));
                 const std::unique_ptr<Project> snapshot = h.project().snapshot();
                 const spice::BatchResult result = spice::runParameterSweep(*snapshot, h.doc().name(), setup, list,
                                                                            measuresFromPython(measures), batchOptions(workers));
                 if (!result.ok())
                     throw std::runtime_error(result.errors.join(QLatin1Char('\n')).toStdString());
                 return batchToPython(result);
             },
             py::arg("axes"), py::arg("measures"), py::arg("analysis") = py::none(), py::arg("workers") = 0,
             "Runs a SPICE analysis once per swept value on a copy of the project. axes: [{'target': 'R1.r',\n"
             "'mode': 'list', 'list': ['1k', '2k']}] or {'target': 'temperature', 'mode': 'lin', 'start': '-40',\n"
             "'stop': '85', 'points': '6'}; measures: ['v(out)'] or [{'expression': 'v(out)', 'function': 'max'}].\n"
             "workers: parallel processes (0: one per core, 1: this process). Returns runs with values and measures.")
        .def("spice_monte_carlo",
             [](const SchematicHandle &h, const py::list &tolerances, const py::object &measures, int runs, int seed,
                const py::object &analysis, int workers) {
                 const spice::SpiceAnalysisSetup setup = findSpiceAnalysis(h.get(), analysis);
                 spice::MonteCarloSetup mc;
                 for (const py::handle &t : tolerances)
                     mc.tolerances << spice::Tolerance::fromJson(pythonToJson(py::reinterpret_borrow<py::object>(t)));
                 mc.runs = runs;
                 mc.seed = quint32(seed);
                 mc.measures = measuresFromPython(py::object(measures));
                 const std::unique_ptr<Project> snapshot = h.project().snapshot();
                 const spice::BatchResult result = spice::runMonteCarlo(*snapshot, h.doc().name(), setup, mc, batchOptions(workers));
                 if (!result.ok())
                     throw std::runtime_error(result.errors.join(QLatin1Char('\n')).toStdString());
                 py::dict out = batchToPython(result);
                 py::list stats;
                 for (const spice::MeasureStatistics &s : spice::statistics(result)) {
                     py::dict item;
                     item["name"] = s.name.toStdString();
                     item["nominal"] = s.nominal;
                     item["mean"] = s.mean;
                     item["sigma"] = s.sigma;
                     item["min"] = s.minimum;
                     item["max"] = s.maximum;
                     item["count"] = s.count;
                     stats.append(item);
                 }
                 out["statistics"] = stats;
                 return out;
             },
             py::arg("tolerances"), py::arg("measures"), py::arg("runs") = 100, py::arg("seed") = 1,
             py::arg("analysis") = py::none(), py::arg("workers") = 0,
             "Monte Carlo on a copy of the project. tolerances: [{'target': 'R*.r', 'tolerance': 0.01,\n"
             "'distribution': 'gaussian' or 'uniform'}]. Run 0 is the nominal circuit; 'statistics' summarises.")
        .def("spice_worst_case",
             [](const SchematicHandle &h, const py::list &tolerances, const py::object &measures, const py::object &analysis,
                int workers) {
                 const spice::SpiceAnalysisSetup setup = findSpiceAnalysis(h.get(), analysis);
                 QList<spice::Tolerance> list;
                 for (const py::handle &t : tolerances)
                     list << spice::Tolerance::fromJson(pythonToJson(py::reinterpret_borrow<py::object>(t)));
                 QList<spice::WorstCaseMeasure> summary;
                 const std::unique_ptr<Project> snapshot = h.project().snapshot();
                 const spice::BatchResult result = spice::runWorstCase(*snapshot, h.doc().name(), setup, list,
                                                                       measuresFromPython(measures), &summary, batchOptions(workers));
                 if (!result.ok())
                     throw std::runtime_error(result.errors.join(QLatin1Char('\n')).toStdString());
                 py::list out;
                 for (const spice::WorstCaseMeasure &w : summary) {
                     py::dict item;
                     item["name"] = w.name.toStdString();
                     item["nominal"] = w.nominal;
                     item["min"] = w.minimum;
                     item["max"] = w.maximum;
                     item["min_corner"] = w.minimumCorner;
                     item["max_corner"] = w.maximumCorner;
                     out.append(item);
                 }
                 return out;
             },
             py::arg("tolerances"), py::arg("measures"), py::arg("analysis") = py::none(), py::arg("workers") = 0,
             "Worst-case analysis: per measure the nominal, lowest and highest values over the tolerance corners.")
        .def("spice_optimize",
             [](const SchematicHandle &h, const py::list &variables, const py::list &goals, const py::object &analysis,
                int maxEvaluations, bool apply) {
                 const spice::SpiceAnalysisSetup setup = findSpiceAnalysis(h.get(), analysis);
                 spice::SpiceOptimizationSetup optimization;
                 for (const py::handle &v : variables)
                     optimization.variables << spice::SpiceOptimizationVariable::fromJson(pythonToJson(py::reinterpret_borrow<py::object>(v)));
                 for (const py::handle &g : goals)
                     optimization.goals << spice::SpiceGoal::fromJson(pythonToJson(py::reinterpret_borrow<py::object>(g)));
                 optimization.maxEvaluations = maxEvaluations;
                 const std::unique_ptr<Project> snapshot = h.project().snapshot();
                 const spice::SpiceOptimizationResult result =
                     spice::optimizeSpice(*snapshot, h.doc().name(), setup, optimization, batchOptions(1));
                 if (!result.ok())
                     throw std::runtime_error(result.errors.join(QLatin1Char('\n')).toStdString());
                 if (apply) {
                     Schematic &schematic = h.get();
                     Schematic::Edit edit(&schematic, QStringLiteral("Apply optimised values"));
                     QList<Variable> list = schematic.variables();
                     for (qsizetype i = 0; i < optimization.variables.size(); ++i) {
                         const spice::SpiceTarget &target = optimization.variables.at(i).target;
                         const QString text = QString::number(result.snapped[std::size_t(i)], 'g', 6);
                         if (target.isParameter()) {
                             if (const PlacedComponent *component = schematic.componentByReference(target.component()))
                                 schematic.setParameter(component->id, target.parameter(), text);
                         } else {
                             for (Variable &variable : list)
                                 if (variable.name == target.text)
                                     variable.value = text;
                         }
                     }
                     schematic.setVariables(list);
                 }
                 py::dict out;
                 out["initial"] = result.initial;
                 out["best"] = result.best;
                 out["snapped"] = result.snapped;
                 out["cost"] = result.snappedCost;
                 out["measures"] = result.bestMeasures;
                 out["goals_met"] = result.goalsMet;
                 out["evaluations"] = result.evaluations;
                 out["converged"] = result.converged;
                 return out;
             },
             py::arg("variables"), py::arg("goals"), py::arg("analysis") = py::none(), py::arg("max_evaluations") = 200,
             py::arg("apply") = false,
             "Optimises toward goals. variables: [{'target': 'R2.r', 'min': 100, 'max': 1e5, 'series': 'E24'}];\n"
             "goals: [{'measure': {'expression': 'v(out)'}, 'type': 'equal', 'target': 7.5}]. With apply=True the\n"
             "values (rounded to their series) are written into the schematic as one undo step.")        .def("spice_netlist",
             [](const SchematicHandle &h, const py::object &analysis) {
                 const spice::SpiceAnalysisSetup setup = findSpiceAnalysis(h.get(), analysis);
                 const spice::SpiceSimulation prepared = spice::prepareSpiceAnalysis(h.project(), h.doc(), setup);
                 if (!prepared.errors.isEmpty())
                     throw std::runtime_error(prepared.errors.join(QLatin1Char('\n')).toStdString());
                 return prepared.netlist.toStdString();
             },
             py::arg("analysis") = py::none(), "The SPICE netlist of an analysis (id or name; default: the first).")
        .def("spice",
             [](const SchematicHandle &h, const py::object &analysis) {
                 const spice::SpiceAnalysisSetup setup = findSpiceAnalysis(h.get(), analysis);
                 spice::SpiceSimulation prepared = spice::prepareSpiceAnalysis(h.project(), h.doc(), setup);
                 if (!prepared.errors.isEmpty())
                     throw std::runtime_error(prepared.errors.join(QLatin1Char('\n')).toStdString());
                 {
                     py::gil_scoped_release release;
                     spice::executeSpiceAnalysis(prepared, spice::defaultEngine());
                 }
                 for (const QString &warning : prepared.warnings)
                     writePythonOutput(QStringLiteral("warning: %1\n").arg(warning), true);
                 if (!prepared.ok())
                     throw std::runtime_error(prepared.errors.join(QLatin1Char('\n')).toStdString());
                 return SpiceResultHandle{std::make_shared<const spice::SpiceSimulation>(std::move(prepared))};
             },
             py::arg("analysis") = py::none(),
             "Runs a SPICE analysis (id or name; default: the first) on ngspice and returns a SpiceResult.")        .def("__repr__", [](const SchematicHandle &h) {
            if (!h.document)
                return std::string("<Schematic (removed)>");
            return "<Schematic of '" + h.document->name().toStdString() + "'>";
        });

    py::class_<SpiceResultHandle>(m, "SpiceResult", "Result of a SPICE analysis.")
        .def_property_readonly("netlist", [](const SpiceResultHandle &h) { return h.simulation->netlist.toStdString(); })
        .def_property_readonly("plots",
                               [](const SpiceResultHandle &h) {
                                   py::list result;
                                   for (const spice::SpicePlot &p : h.simulation->run.plots) {
                                       py::dict item;
                                       item["name"] = p.name.toStdString();
                                       item["type"] = p.type.toStdString();
                                       item["scale"] = p.scaleName.toStdString();
                                       std::vector<std::string> names;
                                       for (const QString &n : p.vectorNames())
                                           names.push_back(n.toStdString());
                                       item["vectors"] = names;
                                       result.append(item);
                                   }
                                   return result;
                               },
                               "The result plots: name, type (op, dc, ac, tran, noise, ...), scale and vector names.")
        .def_property_readonly("warnings",
                               [](const SpiceResultHandle &h) {
                                   std::vector<std::string> list;
                                   for (const QString &warning : h.simulation->warnings)
                                       list.push_back(warning.toStdString());
                                   return list;
                               })
        .def_property_readonly("log",
                               [](const SpiceResultHandle &h) { return h.simulation->run.log.join(QLatin1Char('\n')).toStdString(); },
                               "Everything the simulator printed.")
        .def("vector",
             [](const SpiceResultHandle &h, const std::string &name, const py::object &plot) -> py::object {
                 const spice::SpicePlot &p = h.plot(plot);
                 const spice::SpiceVector *v = p.vector(QString::fromStdString(name));
                 if (!v)
                     throw py::key_error("no vector '" + name + "' in " + p.name.toStdString());
                 if (v->isComplex)
                     return py::cast(v->complex);
                 return py::cast(v->real);
             },
             py::arg("name"), py::arg("plot") = py::none(),
             "A result vector by name ('v(out)', 'i(vv1)', 'time', ...) of a plot (name or type; default: the first).")
        .def("evaluate",
             [](const SpiceResultHandle &h, const std::string &expression, const py::object &plot) {
                 QString error;
                 const auto w = spice::evaluateOnPlot(QString::fromStdString(expression), h.plot(plot),
                                                      &h.simulation->deck, &error);
                 if (!w)
                     throw py::value_error(error.toStdString());
                 return py::make_tuple(w->x, waveformValues(*w));
             },
             py::arg("expression"), py::arg("plot") = py::none(),
             "Evaluates a trace expression such as 'db(v(out)/v(in))' or 'i(V1)*1000'; returns (x, values).")
        .def("value",
             [](const SpiceResultHandle &h, const std::string &expression, const py::object &plot) -> py::object {
                 QString error;
                 const auto w = spice::evaluateOnPlot(QString::fromStdString(expression), h.plot(plot),
                                                      &h.simulation->deck, &error);
                 if (!w || w->y.empty())
                     throw py::value_error(error.isEmpty() ? std::string("no value") : error.toStdString());
                 if (w->isComplex)
                     return py::cast(w->y.front());
                 return py::cast(w->y.front().real());
             },
             py::arg("expression"), py::arg("plot") = py::none(),
             "The first value of a trace expression, e.g. an operating-point voltage: value('v(out)').")
        .def_property_readonly("fourier",
                               [](const SpiceResultHandle &h) {
                                   py::list result;
                                   for (const spice::FourierSignal &signal : h.simulation->fourier) {
                                       py::dict item;
                                       item["signal"] = signal.expression.toStdString();
                                       item["error"] = signal.error.toStdString();
                                       if (signal.result) {
                                           item["fundamental"] = signal.result->fundamental;
                                           item["dc"] = signal.result->dc;
                                           item["thd_percent"] = signal.result->thdPercent;
                                           py::list harmonics;
                                           for (const spice::Harmonic &harmonic : signal.result->harmonics) {
                                               py::dict entry;
                                               entry["n"] = harmonic.n;
                                               entry["frequency"] = harmonic.frequency;
                                               entry["magnitude"] = harmonic.magnitude;
                                               entry["phase"] = harmonic.phaseDeg;
                                               harmonics.append(entry);
                                           }
                                           item["harmonics"] = harmonics;
                                       }
                                       result.append(item);
                                   }
                                   return result;
                               },
                               "Fourier analyses: per signal the fundamental, DC, THD and harmonics.")
        .def("__repr__", [](const SpiceResultHandle &h) {
            QStringList plots;
            for (const spice::SpicePlot &p : h.simulation->run.plots)
                plots << p.name;
            return "<SpiceResult: " + plots.join(QStringLiteral(", ")).toStdString() + ">";
        });
    py::class_<ResultHandle>(m, "Result", "S-parameter and noise result of a linear analysis.")
        .def_property_readonly("frequencies",
                               [](const ResultHandle &h) { return h.result->network.frequencies; })
        .def_property_readonly("ports", [](const ResultHandle &h) { return h.result->portNumbers; },
                               "Port numbers, in matrix order.")
        .def_property_readonly("z0", [](const ResultHandle &h) { return h.result->network.z0; })
        .def_property_readonly("warnings",
                               [](const ResultHandle &h) {
                                   std::vector<std::string> list;
                                   for (const QString &warning : h.result->warnings)
                                       list.push_back(warning.toStdString());
                                   return list;
                               })
        .def("s",
             [](const ResultHandle &h, int row, int col) {
                 const rf::Measurement m =
                     rf::evaluateMeasurement(*h.result, QStringLiteral("S(%1,%2)").arg(row).arg(col));
                 if (!m.ok())
                     throw py::key_error(m.error.toStdString());
                 return m.values;
             },
             py::arg("row"), py::arg("col"), "S(row, col) at every frequency, by port number.")
        .def("measure",
             [](const ResultHandle &h, const std::string &expression) -> py::object {
                 const rf::Measurement m = rf::evaluateMeasurement(*h.result, QString::fromStdString(expression));
                 if (!m.ok())
                     throw py::value_error(m.error.toStdString());
                 if (m.isComplex)
                     return py::cast(m.values);
                 return py::cast(m.real());
             },
             py::arg("expression"),
             "Evaluates a measurement such as 'dB(S21)', 'NF', 'GD(S21)', 'Zin(1)' or 'K'.\n"
             "Returns floats, or complex numbers for complex measurements.")
        .def("save_touchstone",
             [](const ResultHandle &h, const py::object &path) {
                 QString error;
                 if (!rf::writeTouchstone(pathArgument(path), h.result->network, {}, &error))
                     throw std::runtime_error(error.toStdString());
             },
             py::arg("path"), "Writes the S-parameters as a Touchstone file.")
        .def("__repr__", [](const ResultHandle &h) {
            return "<Result: " + std::to_string(h.result->network.portCount()) + " ports, "
                   + std::to_string(h.result->network.frequencyCount()) + " frequencies>";
        });

    m.def("substrates",
          [] {
              py::list result;
              for (const SubstrateMaterial &material : SubstrateCatalog::builtIn().materials) {
                  py::dict item;
                  item["id"] = material.id.toStdString();
                  item["name"] = material.name.toStdString();
                  item["er"] = material.er;
                  item["tand"] = material.tand;
                  item["frequency"] = material.frequency;
                  item["thicknesses"] = std::vector<double>(material.thicknesses.begin(), material.thicknesses.end());
                  item["note"] = material.note.toStdString();
                  item["source"] = material.source.toStdString();
                  result.append(item);
              }
              return result;
          },
          "Built-in substrate materials (libraries/substrates.json): id, name, er, tand, frequency (Hz at\n"
          "which er and tand apply), thicknesses (standard, metres), note, source.");

    m.def("libraries",
          [] {
              std::vector<std::string> names;
              for (const QString &name : session().libraries()->libraryNames())
                  names.push_back(name.toStdString());
              return names;
          },
          "Names of the available component libraries.");

    m.def("library",
          [](const std::string &name) {
              LibraryManager *libraries = session().libraries();
              const Library *library = libraries->library(QString::fromStdString(name));
              if (!library)
                  throw py::key_error("no library '" + name + "'");
              std::vector<std::string> components;
              for (const DefinitionPtr &definition : library->components)
                  components.push_back(definition->id.toStdString());
              py::dict result;
              result["name"] = library->name.toStdString();
              result["description"] = library->description.toStdString();
              result["path"] = library->filePath.toStdString();
              result["scope"] = libraryScopeName(libraries->scopeOf(library->name)).toStdString();
              result["components"] = components;
              return result;
          },
          py::arg("name"));

    m.def("save_component",
          [](const std::string &libraryName, const py::dict &definitionDict, const std::string &scopeName) {
              QString error;
              const auto definition = ComponentDefinition::fromJson(pythonToJson(definitionDict), &error);
              if (!definition)
                  throw py::value_error(error.toStdString());
              LibraryManager *libraries = session().libraries();
              const QString name = QString::fromStdString(libraryName);
              Library library;
              LibraryScope scope = scopeName == "user" ? LibraryScope::User : LibraryScope::Project;
              if (scope == LibraryScope::Project && !session().project())
                  throw std::runtime_error("open a project first, or use scope='user'");
              if (const Library *existing = libraries->library(name)) {
                  if (libraries->scopeOf(name) == LibraryScope::BuiltIn)
                      throw py::value_error("'" + libraryName + "' is a built-in library");
                  library = *existing;
                  scope = libraries->scopeOf(name);
              } else {
                  library.name = name;
              }
              library.setComponent(*definition);
              if (!libraries->saveLibrary(library, scope, &error))
                  throw std::runtime_error(error.toStdString());
              return LibraryManager::makeKey(name, definition->id).toStdString();
          },
          py::arg("library"), py::arg("definition"), py::arg("scope") = "project",
          "Adds or replaces a component in a project (or user) library and returns its key 'library:id'.\n"
          "definition is a dictionary in the library file format (docs/file-format.md, section 4), e.g.\n"
          "with a 'python' model whose source defines spice(ref, nodes, params).");
    m.def("component_info",
          [](const std::string &key) {
              const DefinitionPtr definition = session().libraries()->find(QString::fromStdString(key));
              if (!definition)
                  throw py::key_error("no component '" + key + "'");
              return definitionDict(*definition);
          },
          py::arg("key"), "Pins, parameters and models of a library component, e.g. component_info('basic:resistor').");

    m.def("evaluate",
          [](const std::string &expression, const py::object &variables) {
              QHash<QString, double> values;
              if (!variables.is_none()) {
                  for (const auto &[name, value] : variables.cast<py::dict>())
                      values.insert(QString::fromStdString(py::str(name)), value.cast<double>());
              }
              QString error;
              const auto result = Expression::parse(QString::fromStdString(expression))
                                      .evaluate([&values](const QString &name) -> std::optional<double> {
                                          if (const auto it = values.constFind(name); it != values.cend())
                                              return *it;
                                          return std::nullopt;
                                      },
                                                &error);
              if (!result)
                  throw py::value_error(error.toStdString());
              return *result;
          },
          py::arg("expression"), py::arg("variables") = py::none(),
          "Evaluates a value or expression such as '2.4GHz' or 'db10(2) + gain'.");

    m.def("format_value",
          [](double value, const std::string &unit, int digits) {
              return formatEngineering(value, QString::fromStdString(unit), digits).toStdString();
          },
          py::arg("value"), py::arg("unit") = "", py::arg("digits") = 4, "Formats with an engineering prefix: 2.4 GHz.");

    m.def("document_types",
          [] {
              std::vector<std::string> ids;
              for (const auto &info : documentTypes())
                  ids.push_back(toStd(info.id));
              return ids;
          },
          "Identifiers accepted by Project.new_document().");

    m.def("project",
          []() -> py::object {
              Project *project = session().project();
              if (!project)
                  return py::none();
              return py::cast(ProjectHandle{project});
          },
          "Returns the open project, or None.");

    m.def("new_project",
          [](const py::object &directory, const py::object &name) {
              const QString dir = pathArgument(directory);
              const QString projectName = name.is_none() ? QFileInfo(dir).fileName() : toQt(py::str(name));
              QString error;
              Project *project = session().newProject(dir, projectName, &error);
              if (!project)
                  fail(error);
              return ProjectHandle{project};
          },
          py::arg("directory"), py::arg("name") = py::none(),
          "Creates a project in a folder and opens it. The name defaults to the folder name.");

    m.def("open_project",
          [](const py::object &path) {
              QString error;
              Project *project = session().openProject(pathArgument(path), &error);
              if (!project)
                  fail(error);
              return ProjectHandle{project};
          },
          py::arg("path"), "Opens a project file.");

    m.def("close_project", [] { session().closeProject(); }, "Closes the open project.");
}

} // namespace lvd
