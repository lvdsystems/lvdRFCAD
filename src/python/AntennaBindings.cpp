// Python API of the antenna documents (requirements AN-1 to AN-6, decision 9
// of the requirements): lvdrfcad.Document.array / .element, pattern
// evaluation, metrics, scan and band sweeps, budget, synthesis and the
// antenna libraries.

#include "python/AntennaBindings.h"

#include "antenna/AntennaLibrary.h"
#include "antenna/ArrayAnalysis.h"
#include "antenna/PatternFile.h"
#include "core/Document.h"
#include "core/Project.h"
#include "core/Session.h"
#include "python/Bindings.h"

#include <pybind11/complex.h>
#include <pybind11/stl.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>

#include <memory>

namespace py = pybind11;

namespace lvd {
namespace {

using namespace ant;

QString toQt(const std::string &s)
{
    return QString::fromStdString(s);
}

std::string toStd(const QString &s)
{
    return s.toStdString();
}

Session &currentSession()
{
    Session *s = pythonSession();
    if (!s)
        throw std::runtime_error("no application session");
    return *s;
}

AntennaLibraryManager &antennaLibraries()
{
    static std::unique_ptr<AntennaLibraryManager> manager;
    static LibraryManager *source = nullptr;
    LibraryManager *components = currentSession().libraries();
    if (!manager || source != components) {
        manager = std::make_unique<AntennaLibraryManager>(components);
        source = components;
    }
    return *manager;
}

py::object toPython(const QJsonObject &o)
{
    return py::module_::import("json").attr("loads")(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());
}

QJsonObject fromPython(const py::object &o)
{
    const std::string text = py::str(py::module_::import("json").attr("dumps")(o));
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    if (!doc.isObject())
        throw py::value_error("expected a dict");
    return doc.object();
}

// Objects merge key by key; anything else replaces.
QJsonObject deepMerge(QJsonObject base, const QJsonObject &changes)
{
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        if (it.value().isObject() && base.value(it.key()).isObject())
            base.insert(it.key(), deepMerge(base.value(it.key()).toObject(), it.value().toObject()));
        else
            base.insert(it.key(), it.value());
    }
    return base;
}

PatternComponent componentArgument(const std::string &id)
{
    for (PatternComponent c : patternComponents())
        if (toStd(patternComponentId(c)) == id)
            return c;
    throw py::value_error("unknown component '" + id + "'; use total, co, cross, rhcp, lhcp, theta or phi");
}

CoPolarisation coArgument(const std::string &co)
{
    if (co == "x")
        return CoPolarisation::X;
    if (co == "y")
        return CoPolarisation::Y;
    throw py::value_error("co must be 'x' or 'y'");
}

py::dict metricsDict(const BeamMetrics &m)
{
    py::dict d;
    d["peak_gain"] = m.peakGainDbi;
    d["peak_theta"] = m.peakThetaDeg;
    d["peak_phi"] = m.peakPhiDeg;
    d["gain_at_steer"] = m.gainAtSteerDbi;
    d["pointing_error"] = m.pointingErrorDeg;
    d["beamwidth"] = py::make_tuple(m.beamwidth1Deg, m.beamwidth2Deg);
    d["null_width"] = py::make_tuple(m.nullWidth1Deg, m.nullWidth2Deg);
    d["side_lobe_cuts"] = py::make_tuple(m.sideLobe1Db, m.sideLobe2Db);
    d["side_lobe"] = m.sideLobeDb;
    d["cross_pol"] = m.crossPolDb;
    d["axial_ratio"] = m.axialRatioDb;
    d["directivity"] = m.directivityDbi;
    return d;
}

py::dict cutDict(const PatternCut &cut, PatternComponent component, CoPolarisation co)
{
    std::vector<double> gain(cut.fields.size());
    for (std::size_t i = 0; i < gain.size(); ++i)
        gain[i] = powerDb(componentPower(cut.fields[i], toRadians(cut.phi[i]), component, co));
    py::dict d;
    d["angle"] = cut.angle;
    d["gain"] = gain;
    return d;
}

// ---- Handles

struct ArrayPatternHandle {
    std::shared_ptr<const ArrayEvaluation> evaluation;
    bool ideal = false;
    double steerTheta = 0, steerPhi = 0;
    const PatternEngine &engine() const
    {
        const auto &e = ideal ? evaluation->idealEngine : evaluation->engine;
        if (!e)
            throw std::runtime_error("the array has no element pattern");
        return *e;
    }
};

struct ArrayHandle {
    QPointer<Document> pointer;
    Document &get() const
    {
        if (!pointer)
            throw std::runtime_error("the antenna array document no longer exists");
        return *pointer;
    }
    ArrayDesign design() const { return ArrayDesign::fromJson(get().content()); }
    void setDesign(const ArrayDesign &d) const { get().setContent(d.toJson()); }
    Project *project() const { return currentSession().project(); }
    std::shared_ptr<const ElementPattern> element(const ArrayDesign &d) const
    {
        const ElementResolution r = resolveElement(d, project(), &antennaLibraries());
        if (!r.pattern)
            throw std::runtime_error(toStd(r.error));
        return r.pattern;
    }
};

struct ElementHandle {
    QPointer<Document> pointer;
    Document &get() const
    {
        if (!pointer)
            throw std::runtime_error("the antenna element document no longer exists");
        return *pointer;
    }
    ElementDesign design() const { return ElementDesign::fromJson(get().content()); }
    std::shared_ptr<const ElementPattern> pattern() const
    {
        Project *project = currentSession().project();
        const ElementResolution r = elementFromSpec(design().element, project ? project->directory() : QString());
        if (!r.pattern)
            throw std::runtime_error(toStd(r.error));
        return r.pattern;
    }
};

void applyEntry(ArrayDesign &d, const QString &key)
{
    const AntennaLibraryManager::Found found = antennaLibraries().find(key);
    if (!found.entry)
        throw py::key_error("no antenna library entry '" + toStd(key) + "'");
    switch (found.entry->kind) {
    case AntennaEntryKind::Element:
    case AntennaEntryKind::Pattern:
        d.elementRef = QStringLiteral("library:") + key;
        break;
    case AntennaEntryKind::TrModule:
        applyTrPreset(found.entry->data, &d.trModule, &d.aesa);
        d.trModule.tx.source = d.trModule.rx.source = QStringLiteral("preset");
        d.trModule.tx.preset = d.trModule.rx.preset = key;
        break;
    case AntennaEntryKind::Array:
        d = ArrayDesign::fromJson(deepMerge(d.toJson(), found.entry->data));
        break;
    }
}

} // namespace

py::object antennaArrayObject(Document *document)
{
    if (!document || document->type() != DocumentType::AntennaArray)
        return py::none();
    return py::cast(ArrayHandle{document});
}

py::object antennaElementObject(Document *document)
{
    if (!document || document->type() != DocumentType::AntennaElement)
        return py::none();
    return py::cast(ElementHandle{document});
}

void registerAntennaBindings(py::module_ &m)
{
    py::class_<ArrayPatternHandle>(m, "ArrayPattern", "The far field of an antenna array at one frequency.")
        .def_property_readonly("frequency", [](const ArrayPatternHandle &h) { return h.evaluation->frequency; })
        .def_property_readonly("weights",
                               [](const ArrayPatternHandle &h) {
                                   return h.ideal ? h.evaluation->ideal.weights : h.evaluation->excitation.weights;
                               },
                               "Complex element weights (with the AESA effects unless ideal).")
        .def_property_readonly("failed", [](const ArrayPatternHandle &h) { return h.evaluation->excitation.failed; })
        .def_property_readonly("positions",
                               [](const ArrayPatternHandle &h) {
                                   std::vector<std::tuple<double, double, double>> p;
                                   for (const ArrayElement &e : h.evaluation->layout.elements)
                                       p.emplace_back(e.position.x(), e.position.y(), e.position.z());
                                   return p;
                               },
                               "Element positions in metres.")
        .def("gain",
             [](const ArrayPatternHandle &h, double theta, double phi, const std::string &component, const std::string &co) {
                 const Field f = h.engine().field(toRadians(theta), toRadians(phi));
                 return powerDb(componentPower(f, toRadians(phi), componentArgument(component), coArgument(co)));
             },
             py::arg("theta"), py::arg("phi") = 0.0, py::arg("component") = "total", py::arg("co") = "x",
             "Gain in dBi towards (theta, phi) in degrees; component: total, co, cross, rhcp, lhcp, theta, phi.")
        .def("field",
             [](const ArrayPatternHandle &h, double theta, double phi) {
                 const Field f = h.engine().field(toRadians(theta), toRadians(phi));
                 return py::make_tuple(f.eTheta, f.ePhi);
             },
             py::arg("theta"), py::arg("phi") = 0.0, "(E_theta, E_phi), normalised so that |E|^2 is the gain.")
        .def("cut",
             [](const ArrayPatternHandle &h, double phi, double step, const std::string &component, const std::string &co) {
                 return cutDict(computePhiCut(h.engine(), phi, step), componentArgument(component), coArgument(co));
             },
             py::arg("phi") = 0.0, py::arg("step") = 0.5, py::arg("component") = "total", py::arg("co") = "x",
             "A constant-phi cut: {'angle': signed theta in degrees, 'gain': dBi}.")
        .def("azimuth_cut",
             [](const ArrayPatternHandle &h, double theta, double step, const std::string &component, const std::string &co) {
                 return cutDict(computeThetaCut(h.engine(), theta, step), componentArgument(component), coArgument(co));
             },
             py::arg("theta") = 90.0, py::arg("step") = 0.5, py::arg("component") = "total", py::arg("co") = "x",
             "A conical cut at constant theta (90: the horizon): {'angle': phi in degrees, 'gain': dBi}.")
        .def("uv",
             [](const ArrayPatternHandle &h, int size, const std::string &component, const std::string &co) {
                 const UvGrid g = computeUvGrid(h.engine(), size);
                 const PatternComponent c = componentArgument(component);
                 const CoPolarisation cp = coArgument(co);
                 std::vector<std::vector<double>> rows(std::size_t(g.size), std::vector<double>(std::size_t(g.size), NAN));
                 for (int iv = 0; iv < g.size; ++iv)
                     for (int iu = 0; iu < g.size; ++iu) {
                         const std::size_t k = std::size_t(iv) * std::size_t(g.size) + std::size_t(iu);
                         if (g.valid[k])
                             rows[std::size_t(iv)][std::size_t(iu)] = powerDb(componentPower(g.fields[k], g.phi[k], c, cp));
                     }
                 py::dict d;
                 d["axis"] = g.axis;
                 d["gain"] = rows;
                 return d;
             },
             py::arg("size") = 101, py::arg("component") = "total", py::arg("co") = "x",
             "Gain over the UV plane: {'axis': u and v values, 'gain': rows (v) of dBi, NaN outside the unit circle}.")
        .def("metrics",
             [](const ArrayPatternHandle &h, bool integrate, const std::string &co) {
                 MetricsOptions o;
                 o.steerThetaDeg = h.steerTheta;
                 o.steerPhiDeg = h.steerPhi;
                 o.integrate = integrate;
                 o.co = coArgument(co);
                 return metricsDict(computeMetrics(h.engine(), o));
             },
             py::arg("integrate") = false, py::arg("co") = "x",
             "Peak gain and direction, beamwidths, side lobes, cross-polarisation, axial ratio\n"
             "(and the integrated directivity when integrate=True).");

    py::class_<ArrayHandle>(m, "AntennaArray", "The design of an antenna array document.")
        .def_property(
            "design", [](const ArrayHandle &h) { return toPython(h.design().toJson()); },
            [](const ArrayHandle &h, const py::object &design) { h.setDesign(ArrayDesign::fromJson(fromPython(design))); },
            "The whole design as a dict (docs/file-format.md, section 7.2); assigning replaces it.")
        .def("update",
             [](const ArrayHandle &h, const py::object &changes) {
                 h.setDesign(ArrayDesign::fromJson(deepMerge(h.design().toJson(), fromPython(changes))));
             },
             py::arg("changes"),
             "Merges a partial design, for example {'geometry': {'nx': 16}, 'excitation': {'steerTheta': 30}}.")
        .def("use_library",
             [](const ArrayHandle &h, const std::string &key) {
                 ArrayDesign d = h.design();
                 applyEntry(d, toQt(key));
                 h.setDesign(d);
             },
             py::arg("key"), "Applies an antenna library entry ('library:entry'): element, T/R module or array preset.")
        .def("to_custom",
             [](const ArrayHandle &h) {
                 ArrayDesign d = h.design();
                 d.geometry.custom = toCustomElements(d.geometry, d.designFrequency);
                 d.geometry.kind = LatticeKind::Custom;
                 h.setDesign(d);
             },
             "Turns the geometry into a custom element table (positions, normals and rotations kept), for editing "
             "single elements through design['geometry']['elements'].")
        .def("evaluate",
             [](const ArrayHandle &h, const py::object &frequency, bool ideal) {
                 const ArrayDesign d = h.design();
                 auto e = std::make_shared<ArrayEvaluation>(
                     evaluateArray(d, h.element(d), frequency.is_none() ? 0.0 : frequency.cast<double>()));
                 for (const QString &w : e->warnings)
                     writePythonOutput(QStringLiteral("warning: %1\n").arg(w), true);
                 return ArrayPatternHandle{e, ideal, d.excitation.steerThetaDeg, d.excitation.steerPhiDeg};
             },
             py::arg("frequency") = py::none(), py::arg("ideal") = false,
             "Evaluates the array (at its operating frequency unless given); ideal=True leaves out the AESA effects.")
        .def("metrics",
             [](const ArrayHandle &h, bool integrate, const std::string &co) {
                 const ArrayDesign d = h.design();
                 const ArrayEvaluation e = evaluateArray(d, h.element(d));
                 MetricsOptions o;
                 o.steerThetaDeg = d.excitation.steerThetaDeg;
                 o.steerPhiDeg = d.excitation.steerPhiDeg;
                 o.integrate = integrate;
                 o.co = coArgument(co);
                 const UvGrid uv = computeUvGrid(*e.engine, 151);
                 o.uv = &uv;
                 return metricsDict(computeMetrics(*e.engine, o));
             },
             py::arg("integrate") = false, py::arg("co") = "x",
             "Beam metrics at the operating frequency, including the side-lobe level over the UV plane.")
        .def("budget",
             [](const ArrayHandle &h) {
                 const ArrayDesign d = h.design();
                 const ArrayEvaluation e = evaluateArray(d, h.element(d));
                 const TrValues tx = resolveTrSide(d.trModule.tx, true, h.project(), &antennaLibraries());
                 const TrValues rx = resolveTrSide(d.trModule.rx, false, h.project(), &antennaLibraries());
                 for (const TrValues *v : {&tx, &rx})
                     if (!v->error.isEmpty())
                         writePythonOutput(QStringLiteral("warning: %1\n").arg(v->error), true);
                 const BudgetValues b = computeArrayBudget(e, d, tx, rx);
                 py::dict r;
                 r["eirp"] = b.eirpDbw;
                 r["g_over_t"] = b.gOverTDbK;
                 r["gain"] = b.gainDbi;
                 r["system_temperature"] = b.systemTemperatureK;
                 r["total_power"] = b.totalPowerW;
                 r["active_elements"] = b.activeElements;
                 r["tx"] = py::dict(py::arg("power") = tx.powerDbm, py::arg("gain") = tx.gainDb, py::arg("origin") = toStd(tx.origin));
                 r["rx"] = py::dict(py::arg("nf") = rx.nfDb, py::arg("gain") = rx.gainDb, py::arg("origin") = toStd(rx.origin));
                 return r;
             },
             "EIRP (dBW), G/T (dB/K), system temperature and the T/R values used, at the steering direction.")
        .def("scan",
             [](const ArrayHandle &h) {
                 const ArrayDesign d = h.design();
                 const TrValues tx = resolveTrSide(d.trModule.tx, true, h.project(), &antennaLibraries());
                 const TrValues rx = resolveTrSide(d.trModule.rx, false, h.project(), &antennaLibraries());
                 py::list out;
                 for (const ScanPoint &p : scanSweep(d, h.element(d), tx, rx)) {
                     py::dict item;
                     item["scan"] = p.scanDeg;
                     item["gain"] = p.gainAtSteerDbi;
                     item["peak_gain"] = p.peakGainDbi;
                     item["pointing"] = p.pointingDeg;
                     item["eirp"] = p.eirpDbw;
                     item["g_over_t"] = p.gOverTDbK;
                     item["grating_lobe"] = p.gratingLobe;
                     out.append(item);
                 }
                 return out;
             },
             "Scan sweep in the plane and range of design['sweeps']: gain at the steering direction, peak gain,\n"
             "pointing, EIRP, G/T and whether a grating lobe is visible.")
        .def("band",
             [](const ArrayHandle &h) {
                 const ArrayDesign d = h.design();
                 py::list out;
                 for (const BandPoint &p : bandSweep(d, h.element(d))) {
                     py::dict item;
                     item["frequency"] = p.frequency;
                     item["gain"] = p.gainAtSteerDbi;
                     item["peak_gain"] = p.peakGainDbi;
                     item["peak_theta"] = p.peakThetaDeg;
                     item["peak_phi"] = p.peakPhiDeg;
                     item["squint"] = p.squintDeg;
                     out.append(item);
                 }
                 return out;
             },
             "Frequency sweep: gain at the steering direction, peak direction and beam squint.")
        .def("grating_lobe_free_scan",
             [](const ArrayHandle &h, double phi) -> py::object {
                 const ArrayDesign d = h.design();
                 const ArrayLayout layout = buildLayout(d.geometry, d.designFrequency);
                 if (!layout.lattice)
                     return py::none();
                 return py::float_(gratingLobeFreeScan(*layout.lattice, d.frequency, phi));
             },
             py::arg("phi") = 0.0, "Largest scan angle in the plane phi without a grating lobe; None for non-periodic arrays.")
        .def("synthesise",
             [](const ArrayHandle &h, const py::object &sll, const py::object &phaseOnly, const py::object &mainBeam,
                const py::object &segments, const py::object &iterations, bool apply) {
                 ArrayDesign d = h.design();
                 SynthesisSpec s = d.synthesis;
                 if (!sll.is_none())
                     s.sllDb = std::min(0.0, sll.cast<double>());
                 if (!phaseOnly.is_none())
                     s.phaseOnly = phaseOnly.cast<bool>();
                 if (!mainBeam.is_none())
                     s.mainBeamDeg = mainBeam.cast<double>();
                 if (!iterations.is_none())
                     s.iterations = iterations.cast<int>();
                 if (!segments.is_none()) {
                     s.segments.clear();
                     for (const py::handle &item : segments) {
                         const auto pair = item.cast<std::pair<double, double>>();
                         s.segments.push_back({pair.first, std::min(0.0, pair.second)});
                     }
                 }
                 d.synthesis = s;
                 const ArrayLayout layout = buildLayout(d.geometry, d.designFrequency);
                 const SynthesisResult r = synthesise(layout, {}, d.designFrequency, s);
                 if (apply) {
                     d.excitation.x.kind = TaperKind::Custom;
                     d.excitation.weights.clear();
                     for (const cplx &w : r.weights)
                         d.excitation.weights.emplace_back(20 * std::log10(std::max(std::abs(w), 1e-6)), toDegrees(std::arg(w)));
                 }
                 h.setDesign(d);
                 for (const QString &w : r.warnings)
                     writePythonOutput(QStringLiteral("warning: %1\n").arg(w), true);
                 py::dict out;
                 out["converged"] = r.converged;
                 out["side_lobe"] = r.achievedSllDb;
                 out["worst_excess"] = r.worstExcessDb;
                 out["iterations"] = r.iterations + 1;
                 out["main_beam"] = r.mainBeamDeg;
                 out["weights"] = r.weights;
                 return out;
             },
             py::arg("sll") = py::none(), py::arg("phase_only") = py::none(), py::arg("main_beam") = py::none(),
             py::arg("segments") = py::none(), py::arg("iterations") = py::none(), py::arg("apply") = true,
             "Synthesises weights for a side-lobe mask (segments: [(from_deg, level_db), ...]); with apply=True\n"
             "they become the custom taper.")
        .def("__repr__", [](const ArrayHandle &h) {
            return h.pointer ? "<AntennaArray '" + toStd(h.pointer->name()) + "'>" : std::string("<AntennaArray (removed)>");
        });

    py::class_<ElementHandle>(m, "AntennaElement", "The design of an antenna element document.")
        .def_property(
            "design", [](const ElementHandle &h) { return toPython(h.design().toJson()); },
            [](const ElementHandle &h, const py::object &design) {
                h.get().setContent(ElementDesign::fromJson(fromPython(design)).toJson());
            },
            "The whole design as a dict (docs/file-format.md, section 7.1).")
        .def("update",
             [](const ElementHandle &h, const py::object &changes) {
                 h.get().setContent(ElementDesign::fromJson(deepMerge(h.design().toJson(), fromPython(changes))).toJson());
             },
             py::arg("changes"), "Merges a partial design, for example {'element': {'model': 'patch'}}.")
        .def("use_library",
             [](const ElementHandle &h, const std::string &key) {
                 const AntennaLibraryManager::Found found = antennaLibraries().find(toQt(key));
                 if (!found.entry || (found.entry->kind != AntennaEntryKind::Element && found.entry->kind != AntennaEntryKind::Pattern))
                     throw py::key_error("no antenna library element '" + key + "'");
                 ElementDesign d = h.design();
                 d.element = ElementSpec::fromJson(found.entry->data);
                 if (d.element.model == ElementModelKind::Imported)
                     d.element.patternFile = QDir(found.library->directory()).absoluteFilePath(d.element.patternFile);
                 h.get().setContent(d.toJson());
             },
             py::arg("key"), "Copies an element or pattern entry of the antenna library.")
        .def("import_pattern",
             [](const ElementHandle &h, const py::object &path) {
                 const QString source = toQt(py::str(py::module_::import("os").attr("fspath")(path)));
                 ElementDesign d = h.design();
                 QString error;
                 const auto read = readPatternFile(source, &error, d.element.frequency);
                 if (!read)
                     throw std::runtime_error(toStd(error));
                 QString target = source;
                 if (Project *project = currentSession().project()) {
                     QDir dir(project->directory());
                     dir.mkpath(QStringLiteral("patterns"));
                     target = dir.filePath(QStringLiteral("patterns/%1.csv").arg(QFileInfo(source).completeBaseName()));
                     if (!writePatternCsv(read->table, target, &error))
                         throw std::runtime_error(toStd(error));
                     target = dir.relativeFilePath(target);
                 }
                 d.element.model = ElementModelKind::Imported;
                 d.element.patternFile = target;
                 if (read->table.frequencies.size() == 1 && read->table.frequencies.front() > 0)
                     d.element.frequency = read->table.frequencies.front();
                 h.get().setContent(d.toJson());
                 for (const QString &w : read->warnings)
                     writePythonOutput(QStringLiteral("warning: %1\n").arg(w), true);
             },
             py::arg("path"), "Imports a pattern file (CSV, CST ASCII, FEKO .ffe) into the project's patterns folder.")
        .def("directivity",
             [](const ElementHandle &h, const py::object &frequency) {
                 const auto p = h.pattern();
                 return p->peakDirectivityDbi(frequency.is_none() ? p->spec().frequency : frequency.cast<double>());
             },
             py::arg("frequency") = py::none(), "Peak directivity in dBi.")
        .def("gain",
             [](const ElementHandle &h, double theta, double phi, const py::object &frequency, const std::string &component,
                const std::string &co) {
                 const auto p = h.pattern();
                 const double f = frequency.is_none() ? p->spec().frequency : frequency.cast<double>();
                 const Field field = p->field(toRadians(theta), toRadians(phi), f);
                 return powerDb(p->efficiency() * componentPower(field, toRadians(phi), componentArgument(component), coArgument(co)));
             },
             py::arg("theta"), py::arg("phi") = 0.0, py::arg("frequency") = py::none(), py::arg("component") = "total",
             py::arg("co") = "x", "Gain in dBi (directivity times efficiency) towards (theta, phi).")
        .def("cut",
             [](const ElementHandle &h, double phi, double step, const py::object &frequency, const std::string &component,
                const std::string &co) {
                 const auto p = h.pattern();
                 const double f = frequency.is_none() ? p->spec().frequency : frequency.cast<double>();
                 ArrayDesign single;
                 single.frequency = single.designFrequency = f;
                 single.geometry.kind = LatticeKind::Linear;
                 single.geometry.nx = 1;
                 const ArrayEvaluation e = evaluateArray(single, p, f);
                 return cutDict(computePhiCut(*e.engine, phi, step), componentArgument(component), coArgument(co));
             },
             py::arg("phi") = 0.0, py::arg("step") = 0.5, py::arg("frequency") = py::none(), py::arg("component") = "total",
             py::arg("co") = "x", "A constant-phi cut of the gain: {'angle': signed theta, 'gain': dBi}.")
        .def("__repr__", [](const ElementHandle &h) {
            return h.pointer ? "<AntennaElement '" + toStd(h.pointer->name()) + "'>" : std::string("<AntennaElement (removed)>");
        });

    m.def("antenna_libraries",
          [] {
              py::list out;
              for (const AntennaLibrary &lib : antennaLibraries().libraries()) {
                  py::list entries;
                  for (const AntennaLibraryEntry &e : lib.entries)
                      entries.append(py::dict(py::arg("key") = toStd(AntennaLibraryManager::makeKey(lib.name, e.id)),
                                              py::arg("name") = toStd(e.name), py::arg("kind") = toStd(antennaEntryKindId(e.kind)),
                                              py::arg("description") = toStd(e.description)));
                  out.append(py::dict(py::arg("name") = toStd(lib.name), py::arg("description") = toStd(lib.description),
                                      py::arg("path") = toStd(lib.filePath), py::arg("entries") = entries));
              }
              return out;
          },
          "The antenna libraries and their entries (keys for use_library()).");

    m.def("read_pattern",
          [](const py::object &path) {
              QString error;
              const auto r = readPatternFile(toQt(py::str(py::module_::import("os").attr("fspath")(path))), &error);
              if (!r)
                  throw std::runtime_error(toStd(error));
              py::dict d;
              d["frequencies"] = r->table.frequencies;
              d["theta"] = r->table.theta;
              d["phi"] = r->table.phi;
              d["has_phase"] = r->table.hasPhase;
              d["peak"] = r->peakDbi;
              std::vector<std::string> warnings;
              for (const QString &w : r->warnings)
                  warnings.push_back(toStd(w));
              d["warnings"] = warnings;
              return d;
          },
          py::arg("path"), "Reads a far-field pattern file and describes its grid.");
}

} // namespace lvd
