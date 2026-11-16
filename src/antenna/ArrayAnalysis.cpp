#include "antenna/ArrayAnalysis.h"

#include "antenna/AntennaLibrary.h"
#include "antenna/PatternFile.h"
#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "rfsim/Budget.h"

#include <QDir>
#include <QFileInfo>

namespace lvd::ant {

ElementResolution elementFromSpec(ElementSpec spec, const QString &baseDirectory, const QString &label)
{
    ElementResolution r;
    r.label = label.isEmpty() ? spec.summary() : label;
    if (spec.model == ElementModelKind::Imported && !spec.pattern) {
        if (spec.patternFile.isEmpty()) {
            r.error = QStringLiteral("the imported element has no pattern file");
            return r;
        }
        const QString path = QDir(baseDirectory).absoluteFilePath(spec.patternFile);
        QString error;
        auto read = readPatternFile(path, &error, spec.frequency);
        if (!read) {
            r.error = error;
            return r;
        }
        r.warnings << read->warnings;
        spec.pattern = std::make_shared<PatternTable>(std::move(read->table));
    }
    r.pattern = std::make_shared<ElementPattern>(std::move(spec));
    return r;
}

ElementResolution resolveElement(const ArrayDesign &design, const Project *project, const AntennaLibraryManager *library)
{
    const QString ref = design.elementRef;
    const QString projectDir = project ? project->directory() : QString();
    if (ref.isEmpty())
        return elementFromSpec(design.inlineElement, projectDir, QStringLiteral("Inline: %1").arg(design.inlineElement.summary()));
    if (ref.startsWith(QLatin1String("element:"))) {
        const QString name = ref.mid(8);
        Document *doc = project ? project->document(name) : nullptr;
        if (!doc || doc->type() != DocumentType::AntennaElement) {
            ElementResolution r;
            r.error = QStringLiteral("element document \"%1\" not found in the project").arg(name);
            return r;
        }
        const ElementDesign element = ElementDesign::fromJson(doc->content());
        return elementFromSpec(element.element, projectDir, name);
    }
    if (ref.startsWith(QLatin1String("library:"))) {
        const QString key = ref.mid(8);
        const auto found = library ? library->find(key) : AntennaLibraryManager::Found{};
        if (!found.entry || (found.entry->kind != AntennaEntryKind::Element && found.entry->kind != AntennaEntryKind::Pattern)) {
            ElementResolution r;
            r.error = QStringLiteral("antenna library element \"%1\" not found").arg(key);
            return r;
        }
        return elementFromSpec(ElementSpec::fromJson(found.entry->data), found.library->directory(), found.entry->name);
    }
    ElementResolution r;
    r.error = QStringLiteral("unknown element reference \"%1\"").arg(ref);
    return r;
}

void applyTrPreset(const QJsonObject &d, TrModuleSpec *tr, AesaSpec *aesa)
{
    const QJsonObject tx = d.value(QStringLiteral("tx")).toObject();
    const QJsonObject rx = d.value(QStringLiteral("rx")).toObject();
    if (tr) {
        tr->tx.powerDbm = tx.value(QStringLiteral("power")).toDouble(tr->tx.powerDbm);
        tr->tx.gainDb = tx.value(QStringLiteral("gain")).toDouble(tr->tx.gainDb);
        tr->rx.gainDb = rx.value(QStringLiteral("gain")).toDouble(tr->rx.gainDb);
        tr->rx.nfDb = rx.value(QStringLiteral("nf")).toDouble(tr->rx.nfDb);
        tr->frontLossDb = d.value(QStringLiteral("frontLoss")).toDouble(tr->frontLossDb);
    }
    if (aesa) {
        aesa->phaseBits = d.value(QStringLiteral("phaseBits")).toInt(aesa->phaseBits);
        aesa->attenuatorBits = d.value(QStringLiteral("attenuatorBits")).toInt(aesa->attenuatorBits);
        aesa->attenuatorStepDb = d.value(QStringLiteral("attenuatorStep")).toDouble(aesa->attenuatorStepDb);
        aesa->amplitudeErrorDb = d.value(QStringLiteral("amplitudeError")).toDouble(aesa->amplitudeErrorDb);
        aesa->phaseErrorDeg = d.value(QStringLiteral("phaseError")).toDouble(aesa->phaseErrorDeg);
    }
}

TrValues resolveTrSide(const TrSideSpec &side, bool transmit, const Project *project, const AntennaLibraryManager *library)
{
    TrValues v;
    v.powerDbm = side.powerDbm;
    v.gainDb = side.gainDb;
    v.nfDb = side.nfDb;
    v.origin = QStringLiteral("typed");
    if (side.source == QLatin1String("preset")) {
        const auto found = library ? library->find(side.preset) : AntennaLibraryManager::Found{};
        if (!found.entry || found.entry->kind != AntennaEntryKind::TrModule) {
            v.error = QStringLiteral("T/R module preset \"%1\" not found; using the typed values").arg(side.preset);
            return v;
        }
        TrModuleSpec tr;
        tr.tx = side;
        tr.rx = side;
        applyTrPreset(found.entry->data, &tr, nullptr);
        v.powerDbm = tr.tx.powerDbm;
        v.gainDb = transmit ? tr.tx.gainDb : tr.rx.gainDb;
        v.nfDb = tr.rx.nfDb;
        v.origin = QStringLiteral("preset %1").arg(found.entry->name);
        return v;
    }
    if (side.source != QLatin1String("linked"))
        return v;

    Document *doc = project ? project->document(side.document) : nullptr;
    if (!doc || doc->type() != DocumentType::RfSchematic || !doc->schematic()) {
        v.error = QStringLiteral("RF schematic \"%1\" not found; using the typed values").arg(side.document);
        return v;
    }
    const QList<rf::BudgetAnalysisSetup> analyses = rf::budgetAnalyses(doc->schematic()->simulation());
    const rf::BudgetAnalysisSetup *setup = nullptr;
    for (const rf::BudgetAnalysisSetup &a : analyses)
        if (side.analysis.isEmpty() || a.id == side.analysis || a.name == side.analysis) {
            setup = &a;
            break;
        }
    if (!setup) {
        v.error = QStringLiteral("%1 has no budget analysis%2; using the typed values")
                      .arg(side.document, side.analysis.isEmpty() ? QString() : QStringLiteral(" \"%1\"").arg(side.analysis));
        return v;
    }
    const rf::BudgetRun run = rf::runBudgetAnalysis(*project, *doc, *setup);
    if (run.budget.stages.empty() || !run.errors.isEmpty() || !run.budget.ok()) {
        QStringList errors = run.errors + run.budget.errors;
        v.error = QStringLiteral("the budget of %1 failed: %2").arg(side.document, errors.join(QStringLiteral("; ")));
        return v;
    }
    const rf::BudgetStage &last = run.budget.stages.back();
    v.gainDb = last.cumulativeGainDb;
    v.nfDb = last.cumulativeNfDb;
    v.powerDbm = last.signalDbm;
    v.origin = QStringLiteral("%1 / %2").arg(side.document, setup->name.isEmpty() ? setup->id : setup->name);
    if (transmit && last.compressed)
        v.warnings << QStringLiteral("the transmit chain %1 is compressed at its output").arg(side.document);
    v.warnings << run.warnings;
    return v;
}

ArrayEvaluation evaluateArray(const ArrayDesign &design, std::shared_ptr<const ElementPattern> element, double frequency)
{
    ArrayEvaluation e;
    e.frequency = frequency > 0 ? frequency : design.frequency;
    e.element = element;
    e.layout = buildLayout(design.geometry, design.designFrequency);
    e.warnings << e.layout.warnings;
    e.excitation = computeExcitation(e.layout, design.excitation, design.aesa, e.frequency, design.designFrequency, true);
    e.warnings << e.excitation.warnings;
    if (!element)
        return e;
    element->normalisation(e.frequency);
    e.engine = std::make_shared<PatternEngine>(e.layout, e.excitation.weights, element, e.frequency);
    if (design.aesa.isIdeal()) {
        e.ideal = e.excitation;
        e.idealEngine = e.engine;
    } else {
        e.ideal = computeExcitation(e.layout, design.excitation, design.aesa, e.frequency, design.designFrequency, false);
        e.idealEngine = std::make_shared<PatternEngine>(e.layout, e.ideal.weights, element, e.frequency);
    }
    return e;
}

namespace {

double eirpAt(const ArrayEvaluation &e, const TrModuleSpec &tr, double powerDbm, const Vec3 &dir, double *totalPower, int *active)
{
    const auto &w = e.excitation.weights;
    double peak = 0;
    for (const cplx &x : w)
        peak = std::max(peak, std::abs(x));
    const double perElement = fromDb(powerDbm - 30.0); // W
    const double radiated = perElement * fromDb(-tr.frontLossDb);
    std::vector<cplx> tx(w.size(), 0.0);
    double total = 0;
    int count = 0;
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (e.excitation.failed[i])
            continue;
        const double a = tr.txTaper ? (peak > 0 ? std::abs(w[i]) / peak : 0.0) : 1.0;
        tx[i] = std::polar(std::sqrt(radiated) * a, std::arg(w[i]));
        total += perElement * a * a;
        ++count;
    }
    if (totalPower)
        *totalPower = total;
    if (active)
        *active = count;
    const PatternEngine engine(e.layout, tx, e.element, e.frequency);
    double theta, phi;
    directionAngles(dir, &theta, &phi);
    return powerDb(engine.rawField(theta, phi).power());
}

double systemTemperature(const TrModuleSpec &tr, const TrValues &rx)
{
    const double l = fromDb(tr.frontLossDb);
    const double f = fromDb(rx.nfDb) + (fromDb(tr.beamformerLossDb) - 1.0) / fromDb(rx.gainDb);
    return tr.antennaTemperatureK / l + tr.ambientTemperatureK * (1.0 - 1.0 / l) + kT0 * (f - 1.0);
}

} // namespace

BudgetValues computeArrayBudget(const ArrayEvaluation &e, const ArrayDesign &design, const TrValues &tx, const TrValues &rx)
{
    BudgetValues b;
    if (!e.engine)
        return b;
    const Vec3 steer = radialUnit(toRadians(design.excitation.steerThetaDeg), toRadians(design.excitation.steerPhiDeg));
    b.eirpDbw = eirpAt(e, design.trModule, tx.powerDbm, steer, &b.totalPowerW, &b.activeElements);
    b.gainDbi = powerDb(e.engine->fieldAt(steer).power());
    b.systemTemperatureK = systemTemperature(design.trModule, rx);
    b.gOverTDbK = b.gainDbi - design.trModule.frontLossDb - powerDb(b.systemTemperatureK);
    return b;
}

std::vector<ScanPoint> scanSweep(const ArrayDesign &design, std::shared_ptr<const ElementPattern> element, const TrValues &tx,
                                 const TrValues &rx, const std::atomic_bool *cancel)
{
    std::vector<ScanPoint> points;
    if (!element)
        return points;
    const SweepSpec &s = design.sweeps;
    for (double a = -s.scanMaxDeg; a <= s.scanMaxDeg + 1e-9; a += s.scanStepDeg)
        points.push_back({a});
    const ArrayLayout layout = buildLayout(design.geometry, design.designFrequency);
    element->normalisation(design.frequency);
    const double tsys = systemTemperature(design.trModule, rx);
    std::optional<GratingLobeInfo> lobes;
    if (layout.lattice)
        lobes = gratingLobes(*layout.lattice, design.frequency, 3);
    const double phiPlane = s.scanPlanePhiDeg;
    const Vec3 inPlane = radialUnit(kPi / 2, toRadians(phiPlane));

    parallelFor(points.size(), [&](std::size_t i) {
        ScanPoint &p = points[i];
        ArrayDesign d = design;
        d.excitation.steerThetaDeg = std::abs(p.scanDeg);
        d.excitation.steerPhiDeg = p.scanDeg < 0 ? phiPlane + 180.0 : phiPlane;
        ArrayEvaluation e;
        e.frequency = design.frequency;
        e.element = element;
        e.layout = layout;
        e.excitation = computeExcitation(layout, d.excitation, d.aesa, e.frequency, d.designFrequency, true);
        auto engine = std::make_shared<PatternEngine>(layout, e.excitation.weights, element, e.frequency);
        e.engine = engine;
        const Vec3 steer = radialUnit(toRadians(d.excitation.steerThetaDeg), toRadians(d.excitation.steerPhiDeg));
        const double g = engine->fieldAt(steer).power();
        p.gainAtSteerDbi = powerDb(g);
        double peak = 0;
        const Vec3 peakDir = findPeakNear(*engine, steer, &peak);
        p.peakGainDbi = powerDb(peak);
        // Signed angle of the peak from the array normal (uv frame boresight) in the plane.
        const Vec3 normal = engine->uvFrame().col(2);
        p.pointingDeg = toDegrees(std::atan2(peakDir.dot(inPlane), peakDir.dot(normal)));
        if (layout.kind == LatticeKind::Cylinder)
            p.pointingDeg = toDegrees(std::atan2(peakDir.dot(engine->uvFrame().col(0)), peakDir.dot(normal)));
        p.eirpDbw = eirpAt(e, design.trModule, tx.powerDbm, steer, nullptr, nullptr);
        p.gOverTDbK = p.gainAtSteerDbi - design.trModule.frontLossDb - powerDb(tsys);
        if (lobes && layout.kind != LatticeKind::Cylinder) {
            const double u0 = steer.x(), v0 = steer.y();
            p.gratingLobe = !visibleGratingLobes(*lobes, u0, v0).empty();
        }
    }, cancel);
    return points;
}

std::vector<BandPoint> bandSweep(const ArrayDesign &design, std::shared_ptr<const ElementPattern> element,
                                 const std::atomic_bool *cancel)
{
    std::vector<BandPoint> points;
    if (!element)
        return points;
    const SweepSpec &s = design.sweeps;
    double start = s.bandStart, stop = s.bandStop;
    if (start <= 0 || stop <= start) {
        start = design.frequency * 0.9;
        stop = design.frequency * 1.1;
    }
    for (int i = 0; i < s.bandPoints; ++i)
        points.push_back({start + (stop - start) * i / std::max(1, s.bandPoints - 1)});
    const ArrayLayout layout = buildLayout(design.geometry, design.designFrequency);
    const Vec3 steer = radialUnit(toRadians(design.excitation.steerThetaDeg), toRadians(design.excitation.steerPhiDeg));
    for (BandPoint &p : points)
        element->normalisation(p.frequency); // prepared outside the worker threads
    parallelFor(points.size(), [&](std::size_t i) {
        BandPoint &p = points[i];
        const ExcitationResult x = computeExcitation(layout, design.excitation, design.aesa, p.frequency, design.designFrequency, true);
        const PatternEngine engine(layout, x.weights, element, p.frequency);
        p.gainAtSteerDbi = powerDb(engine.fieldAt(steer).power());
        double peak = 0;
        const Vec3 dir = findPeakNear(engine, steer, &peak);
        p.peakGainDbi = powerDb(peak);
        double theta, phi;
        directionAngles(dir, &theta, &phi);
        p.peakThetaDeg = toDegrees(theta);
        p.peakPhiDeg = toDegrees(phi);
        p.squintDeg = toDegrees(std::atan2(dir.cross(steer).norm(), dir.dot(steer)));
    }, cancel);
    return points;
}

} // namespace lvd::ant
