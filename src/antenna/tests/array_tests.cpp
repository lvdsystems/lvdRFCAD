// Arrays against analytic cases: uniform and tapered lines, steering and
// squint, grating lobes, nulls, monopulse, AESA effects, synthesis and the
// T/R budget.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "antenna/AntennaLibrary.h"
#include "antenna/ArrayAnalysis.h"
#include "antenna/Synthesis.h"

#include <QTemporaryDir>

using namespace lvd::ant;
using Catch::Approx;

namespace {

constexpr double f0 = 10e9;

std::shared_ptr<const ElementPattern> isotropic()
{
    ElementSpec s;
    s.model = ElementModelKind::Isotropic;
    s.frequency = f0;
    return std::make_shared<ElementPattern>(s);
}

ArrayDesign line(int n, double spacing = 0.5)
{
    ArrayDesign d;
    d.frequency = d.designFrequency = f0;
    d.geometry.kind = LatticeKind::Linear;
    d.geometry.nx = n;
    d.geometry.dx = spacing;
    return d;
}

ArrayDesign planar(int nx, int ny, double spacing = 0.5)
{
    ArrayDesign d;
    d.frequency = d.designFrequency = f0;
    d.geometry.kind = LatticeKind::Rectangular;
    d.geometry.nx = nx;
    d.geometry.ny = ny;
    d.geometry.dx = d.geometry.dy = spacing;
    return d;
}

double gainDb(const PatternEngine &e, double thetaDeg, double phiDeg)
{
    return powerDb(e.gain(toRadians(thetaDeg), toRadians(phiDeg)));
}

} // namespace

TEST_CASE("A uniform half-wave line has the textbook pattern")
{
    const ArrayEvaluation e = evaluateArray(line(16), isotropic());
    REQUIRE(e.engine);
    CHECK(gainDb(*e.engine, 0, 0) == Approx(10 * std::log10(16.0)).margin(1e-9));

    MetricsOptions options;
    options.integrate = true;
    const BeamMetrics m = computeMetrics(*e.engine, options);
    CHECK(m.peakGainDbi == Approx(12.041).margin(0.01));
    CHECK(m.peakThetaDeg == Approx(0).margin(0.01));
    CHECK(m.beamwidth1Deg == Approx(6.35).margin(0.05));
    CHECK(m.sideLobe1Db == Approx(-13.15).margin(0.1));
    // Integrated directivity equals the power-sum gain at half-wave spacing.
    CHECK(m.directivityDbi == Approx(12.041).margin(0.03));
    // Isotropic elements along x: no variation in the yz-plane.
    CHECK(std::isnan(m.beamwidth2Deg));
}

TEST_CASE("Phase steering squints with frequency, time delay does not")
{
    ArrayDesign d = line(32);
    d.excitation.steerThetaDeg = 30;
    const auto element = isotropic();

    MetricsOptions options;
    options.steerThetaDeg = 30;
    BeamMetrics m = computeMetrics(*evaluateArray(d, element).engine, options);
    CHECK(m.peakThetaDeg == Approx(30).margin(0.02));
    CHECK(m.pointingErrorDeg < 0.02);

    // A line's beam is a cone: compare u = sin(theta) cos(phi).
    auto peakU = [](const BeamMetrics &b) { return std::sin(toRadians(b.peakThetaDeg)) * std::cos(toRadians(b.peakPhiDeg)); };
    m = computeMetrics(*evaluateArray(d, element, 1.1 * f0).engine, options);
    CHECK(peakU(m) == Approx(0.5 / 1.1).margin(1e-4));

    d.excitation.steering = SteeringMode::TimeDelay;
    m = computeMetrics(*evaluateArray(d, element, 1.1 * f0).engine, options);
    CHECK(peakU(m) == Approx(0.5).margin(1e-4));

    const std::vector<BandPoint> band = bandSweep(d, element);
    REQUIRE(band.size() == 21);
    for (const BandPoint &p : band)
        CHECK(p.squintDeg < 0.05);
}

TEST_CASE("Grating lobes appear where the lattice predicts")
{
    ArrayDesign d = line(16, 1.0);
    d.excitation.steerThetaDeg = 30;
    const ArrayEvaluation e = evaluateArray(d, isotropic());
    // sin(theta) = 0.5 - 1: a full grating lobe at -30 degrees.
    CHECK(gainDb(*e.engine, 30, 180) == Approx(gainDb(*e.engine, 30, 0)).margin(1e-6));

    const ArrayLayout layout = buildLayout(d.geometry, f0);
    REQUIRE(layout.lattice);
    CHECK(gratingLobeFreeScan(*layout.lattice, f0, 0) == Approx(0).margin(0.01));
    CHECK(gratingLobeFreeScan(*buildLayout(line(8, 0.5).geometry, f0).lattice, f0, 0) == Approx(90));
    CHECK(gratingLobeFreeScan(*buildLayout(line(8, 0.7).geometry, f0).lattice, f0, 0)
          == Approx(toDegrees(std::asin(1 / 0.7 - 1))).margin(0.05));

    const ArrayLayout rect = buildLayout(planar(8, 8, 0.6).geometry, f0);
    CHECK(gratingLobeFreeScan(*rect.lattice, f0, 0) == Approx(toDegrees(std::asin(1 / 0.6 - 1))).margin(0.05));
    const GratingLobeInfo info = gratingLobes(*rect.lattice, f0, 2);
    CHECK(info.centres.size() == 24);
    CHECK(visibleGratingLobes(info, 0, 0).empty());
    CHECK(visibleGratingLobes(info, 0.8, 0).size() == 1);

    // Equilateral triangular lattice: dx = 0.6, dy = 0.6 sin 60.
    ArrayDesign tri = planar(8, 8, 0.6);
    tri.geometry.kind = LatticeKind::Triangular;
    tri.geometry.dy = 0.6 * std::sqrt(3.0) / 2;
    const ArrayLayout triangular = buildLayout(tri.geometry, f0);
    CHECK(gratingLobeFreeScan(*triangular.lattice, f0, 0) > gratingLobeFreeScan(*rect.lattice, f0, 0));
}

TEST_CASE("Chebyshev and Taylor tapers set the side lobes of a planar array")
{
    ArrayDesign d = planar(20, 12);
    d.excitation.x.kind = TaperKind::Chebyshev;
    d.excitation.x.sllDb = 30;
    d.excitation.y.kind = TaperKind::Taylor;
    d.excitation.y.sllDb = 25;
    d.excitation.y.nbar = 4;
    const ArrayEvaluation e = evaluateArray(d, isotropic());
    const BeamMetrics m = computeMetrics(*e.engine, {});
    CHECK(m.sideLobe1Db == Approx(-30).margin(0.1));
    CHECK(m.sideLobe2Db == Approx(-25).margin(1.0));
    // Taper loss: gain below 10 log10(240).
    CHECK(m.peakGainDbi < 10 * std::log10(240.0) - 0.5);
}

TEST_CASE("A circular aperture with a radial Taylor taper")
{
    ArrayDesign d = planar(30, 30);
    d.geometry.aperture = QStringLiteral("circle");
    d.excitation.radial = true;
    d.excitation.x.kind = TaperKind::Taylor;
    d.excitation.x.sllDb = 25;
    d.excitation.x.nbar = 4;
    const ArrayEvaluation e = evaluateArray(d, isotropic());
    CHECK(e.layout.elements.size() < 900);
    CHECK(e.layout.elements.size() > 650);
    const BeamMetrics m = computeMetrics(*e.engine, {});
    CHECK(m.sideLobe1Db == Approx(-25).margin(1.5));
}

TEST_CASE("Nulls, extra beams and difference patterns")
{
    ArrayDesign d = line(16);
    d.excitation.nulls.push_back({20, 0});
    d.excitation.nulls.push_back({40, 180});
    ArrayEvaluation e = evaluateArray(d, isotropic());
    CHECK(gainDb(*e.engine, 20, 0) < gainDb(*e.engine, 0, 0) - 80);
    CHECK(gainDb(*e.engine, 40, 180) < gainDb(*e.engine, 0, 0) - 80);

    ArrayDesign two = line(32);
    two.excitation.steerThetaDeg = 20;
    two.excitation.extraBeams.push_back({30, 180, 0, 0});
    e = evaluateArray(two, isotropic());
    CHECK(gainDb(*e.engine, 20, 0) == Approx(gainDb(*e.engine, 30, 180)).margin(0.2));

    ArrayDesign delta = planar(8, 8);
    delta.excitation.pattern = BeamPattern::DeltaAzimuth;
    e = evaluateArray(delta, isotropic());
    CHECK(gainDb(*e.engine, 0, 0) < -100);
    CHECK(gainDb(*e.engine, 5, 0) > 5);
    // The azimuth difference is zero in the elevation plane.
    CHECK(gainDb(*e.engine, 5, 90) < -100);
}

TEST_CASE("AESA quantisation, errors and failures")
{
    ArrayDesign d = planar(16, 16);
    d.excitation.steerThetaDeg = 13;
    d.aesa.phaseBits = 3;
    ArrayEvaluation e = evaluateArray(d, isotropic());
    CHECK(e.excitation.rmsPhaseQuantisationDeg == Approx(45.0 / std::sqrt(12.0)).margin(3.0));
    for (const cplx &w : e.excitation.weights) {
        const double deg = toDegrees(std::arg(w));
        CHECK(std::abs(deg / 45.0 - std::round(deg / 45.0)) < 1e-6);
    }
    // The ideal reference is kept alongside.
    REQUIRE(e.idealEngine);
    CHECK(e.idealEngine != e.engine);

    d.aesa = {};
    d.aesa.attenuatorBits = 2;
    d.aesa.attenuatorStepDb = 3;
    d.excitation.x.kind = TaperKind::Taylor;
    e = evaluateArray(d, isotropic());
    double peak = 0;
    for (const cplx &w : e.excitation.weights)
        peak = std::max(peak, std::abs(w));
    for (const cplx &w : e.excitation.weights) {
        const double att = -20 * std::log10(std::abs(w) / peak);
        CHECK(std::abs(att / 3 - std::round(att / 3)) < 1e-6);
        CHECK(att <= 9 + 1e-9);
    }

    d.aesa = {};
    d.aesa.failedFraction = 0.25;
    d.aesa.failed = {0, 1};
    e = evaluateArray(d, isotropic());
    const auto failed = std::count(e.excitation.failed.begin(), e.excitation.failed.end(), true);
    CHECK(failed >= 64);
    CHECK(failed <= 66);
    CHECK(e.excitation.weights[0] == cplx(0.0));

    // Random errors raise the average side-lobe floor but keep the beam.
    d.aesa = {};
    d.aesa.amplitudeErrorDb = 1;
    d.aesa.phaseErrorDeg = 10;
    e = evaluateArray(d, isotropic());
    const BeamMetrics m = computeMetrics(*e.engine, {13, 0});
    CHECK(m.pointingErrorDeg < 0.5);
}

TEST_CASE("Sub-arrays with time delay remove squint at sub-array level")
{
    ArrayDesign d = line(32);
    d.excitation.steerThetaDeg = 40;
    d.excitation.subarrayX = 4;
    d.excitation.steering = SteeringMode::SubarrayTimeDelay;
    const auto element = isotropic();
    const ArrayEvaluation atF0 = evaluateArray(d, element);
    CHECK(atF0.excitation.subarrayCentres.size() == 8);
    MetricsOptions options;
    options.steerThetaDeg = 40;
    CHECK(computeMetrics(*atF0.engine, options).pointingErrorDeg < 0.02);

    // Compare u = sin(theta) cos(phi): a line's beam is a cone.
    auto uError = [&](const ArrayDesign &design) {
        const BeamMetrics b = computeMetrics(*evaluateArray(design, element, 1.05 * f0).engine, options);
        return std::abs(std::sin(toRadians(b.peakThetaDeg)) * std::cos(toRadians(b.peakPhiDeg)) - std::sin(toRadians(40.0)));
    };
    ArrayDesign phase = d;
    phase.excitation.steering = SteeringMode::Phase;
    const double phaseOnly = uError(phase);
    const double hybrid = uError(d);
    CHECK(phaseOnly == Approx(std::sin(toRadians(40.0)) * (1 - 1 / 1.05)).margin(2e-4));
    CHECK(hybrid < phaseOnly / 10);
}

TEST_CASE("Conformal cylinder elements face outward")
{
    ArrayDesign d;
    d.frequency = d.designFrequency = f0;
    d.geometry.kind = LatticeKind::Cylinder;
    d.geometry.cylinderRadius = 3;
    d.geometry.columns = 16;
    d.geometry.rows = 4;
    d.geometry.arcDeg = 120;
    d.excitation.steerThetaDeg = 90;
    d.excitation.steerPhiDeg = 20;
    ElementSpec s;
    s.model = ElementModelKind::CosN;
    s.exponent = 1.5;
    s.frequency = f0;
    const ArrayEvaluation e = evaluateArray(d, std::make_shared<ElementPattern>(s));
    CHECK_FALSE(e.layout.identicalOrientation);
    MetricsOptions options;
    options.steerThetaDeg = 90;
    options.steerPhiDeg = 20;
    const BeamMetrics m = computeMetrics(*e.engine, options);
    CHECK(m.peakThetaDeg == Approx(90).margin(0.5));
    CHECK(m.peakPhiDeg == Approx(20).margin(0.5));
    CHECK(m.peakGainDbi > 15);
    // Behind the cylinder the elements face away.
    CHECK(gainDb(*e.engine, 90, 180) < m.peakGainDbi - 30);
}

TEST_CASE("Pattern synthesis meets a side-lobe mask")
{
    const ArrayDesign d = line(32);
    const ArrayLayout layout = buildLayout(d.geometry, f0);
    SynthesisSpec spec;
    spec.sllDb = -25;
    SynthesisResult r = synthesise(layout, {}, f0, spec);
    CHECK(r.converged);
    CHECK(r.achievedSllDb < -24.8);

    // A stepped mask, and the result used as a custom taper.
    spec.sllDb = -20;
    spec.segments.push_back({20, -35});
    r = synthesise(layout, {}, f0, spec);
    CHECK(r.worstExcessDb < 0.2);
    ArrayDesign custom = d;
    custom.excitation.x.kind = TaperKind::Custom;
    for (const cplx &w : r.weights)
        custom.excitation.weights.emplace_back(20 * std::log10(std::max(std::abs(w), 1e-12)), toDegrees(std::arg(w)));
    const ArrayEvaluation e = evaluateArray(custom, isotropic());
    CHECK(gainDb(*e.engine, 40, 0) < gainDb(*e.engine, 0, 0) - 34);

    // Phase-only synthesis improves on the uniform -13 dB.
    spec = {};
    spec.sllDb = -17;
    spec.phaseOnly = true;
    r = synthesise(layout, {}, f0, spec);
    CHECK(r.achievedSllDb < -16.0);
    for (const cplx &w : r.weights)
        CHECK(std::abs(w) == Approx(1.0));
}

TEST_CASE("EIRP and G/T of a uniform array")
{
    ArrayDesign d = planar(10, 10);
    d.trModule.frontLossDb = 0;
    d.trModule.beamformerLossDb = 0;
    TrValues tx;
    tx.powerDbm = 30; // 1 W per element
    TrValues rx;
    rx.gainDb = 30;
    rx.nfDb = 10 * std::log10(2.0); // Te = 290 K
    const ArrayEvaluation e = evaluateArray(d, isotropic());
    const BudgetValues b = computeArrayBudget(e, d, tx, rx);
    CHECK(b.eirpDbw == Approx(40).margin(1e-6));          // N^2 P
    CHECK(b.totalPowerW == Approx(100));
    CHECK(b.systemTemperatureK == Approx(580).margin(0.5)); // 290 + 290
    CHECK(b.gOverTDbK == Approx(20 - 10 * std::log10(580.0)).margin(0.01));

    // A front loss reduces EIRP by itself and G/T by more (it adds noise).
    d.trModule.frontLossDb = 1;
    const BudgetValues lossy = computeArrayBudget(e, d, tx, rx);
    CHECK(lossy.eirpDbw == Approx(39).margin(1e-6));
    // With the antenna at ambient temperature the loss costs exactly its value;
    // a cold sky makes it cost more.
    CHECK(lossy.gOverTDbK == Approx(b.gOverTDbK - 1).margin(1e-9));
    d.trModule.antennaTemperatureK = 50;
    const BudgetValues cold = computeArrayBudget(e, d, tx, rx);
    d.trModule.frontLossDb = 0;
    const BudgetValues coldLossless = computeArrayBudget(e, d, tx, rx);
    CHECK(cold.gOverTDbK < coldLossless.gOverTDbK - 1.5);
    d.trModule.antennaTemperatureK = 290;

    const std::vector<ScanPoint> scan = scanSweep(d, isotropic(), tx, rx);
    REQUIRE(scan.size() == 61);
    CHECK(scan[30].gainAtSteerDbi == Approx(20).margin(1e-6));
    CHECK(scan.front().pointingDeg == Approx(-60).margin(0.1));
    CHECK_FALSE(scan.front().gratingLobe);
}

TEST_CASE("Designs, presets and libraries round-trip through JSON")
{
    ArrayDesign d = planar(12, 6, 0.55);
    d.geometry.kind = LatticeKind::Triangular;
    d.geometry.aperture = QStringLiteral("ellipse");
    d.excitation.x = {TaperKind::Taylor, 35, 6};
    d.excitation.steerThetaDeg = 25;
    d.excitation.steerPhiDeg = 45;
    d.excitation.steering = SteeringMode::SubarrayTimeDelay;
    d.excitation.subarrayX = 3;
    d.excitation.subarrayY = 2;
    d.excitation.nulls.push_back({30, 10});
    d.aesa.phaseBits = 6;
    d.aesa.failed = {3, 7};
    d.trModule.rx.source = QStringLiteral("linked");
    d.trModule.rx.document = QStringLiteral("rx_chain");
    d.elementRef = QStringLiteral("element:patch");
    const ArrayDesign back = ArrayDesign::fromJson(d.toJson());
    CHECK(back.toJson() == d.toJson());
    CHECK(back.excitation.steering == SteeringMode::SubarrayTimeDelay);
    CHECK(back.aesa.failed == std::vector<int>{3, 7});

    ElementSpec s;
    s.model = ElementModelKind::Patch;
    s.polarisation = Polarisation::Rhcp;
    CHECK(ElementSpec::fromJson(s.toJson()).toJson() == s.toJson());

    TrModuleSpec tr;
    AesaSpec aesa;
    applyTrPreset(QJsonObject{{QStringLiteral("tx"), QJsonObject{{QStringLiteral("power"), 43}}},
                              {QStringLiteral("rx"), QJsonObject{{QStringLiteral("nf"), 2.5}}},
                              {QStringLiteral("phaseBits"), 6}},
                  &tr, &aesa);
    CHECK(tr.tx.powerDbm == 43);
    CHECK(tr.rx.nfDb == 2.5);
    CHECK(aesa.phaseBits == 6);

    QTemporaryDir dir;
    AntennaLibrary lib;
    lib.name = QStringLiteral("test");
    lib.filePath = dir.filePath(QStringLiteral("test.lvdantlib"));
    lib.setEntry({QStringLiteral("p"), QStringLiteral("Patch"), {}, AntennaEntryKind::Element, s.toJson()});
    QString error;
    REQUIRE(lib.save(&error));
    const auto loaded = AntennaLibrary::load(lib.filePath, &error);
    REQUIRE(loaded);
    REQUIRE(loaded->entry(QStringLiteral("p")));
    CHECK(loaded->entry(QStringLiteral("p"))->kind == AntennaEntryKind::Element);
}

TEST_CASE("Steering mode none, horizon cuts and switched-off elements")
{
    // Two horns squinted +-10 deg on an arc: without steering phases they are
    // in phase along their bisector (broadside); with phase steering to 0 deg
    // the z offsets would be compensated instead.
    ArrayDesign d;
    d.frequency = d.designFrequency = f0;
    d.geometry.kind = LatticeKind::Custom;
    d.geometry.unit = QStringLiteral("m");
    const double r = 0.15;
    for (double a : {-10.0, 10.0}) {
        CustomElement e;
        e.x = r * std::sin(toRadians(a));
        e.z = r * (std::cos(toRadians(a)) - 1);
        e.nx = std::sin(toRadians(a));
        e.nz = std::cos(toRadians(a));
        d.geometry.custom.push_back(e);
    }
    d.excitation.x.kind = TaperKind::Custom;
    d.excitation.steering = SteeringMode::None;
    d.excitation.weights = {{0, 0}, {-100, 0}};
    ElementSpec s;
    s.model = ElementModelKind::CosN;
    s.exponent = 4;
    s.frequency = f0;
    const auto element = std::make_shared<ElementPattern>(s);
    ArrayEvaluation e = evaluateArray(d, element);
    CHECK(e.excitation.weights[1] == cplx(0.0)); // -100 dB switches it off
    CHECK(computeMetrics(*e.engine, {}).peakThetaDeg == Approx(10).margin(0.1));
    d.excitation.weights = {{0, 0}, {0, 0}};
    e = evaluateArray(d, element);
    const Field a = e.engine->field(0, 0);
    CHECK(std::abs(std::arg(e.excitation.weights[0]) - std::arg(e.excitation.weights[1])) < 1e-12);
    CHECK(a.power() > 0);

    // A ring of monopoles: the horizon cut is omnidirectional for the sum.
    ArrayDesign ring;
    ring.frequency = ring.designFrequency = 400e6;
    ring.geometry.kind = LatticeKind::Circular;
    ring.geometry.rings = {RingSpec{0.2, 4, 0}};
    ring.excitation.steering = SteeringMode::None;
    ElementSpec mono;
    mono.model = ElementModelKind::Monopole;
    mono.frequency = 400e6;
    const auto monopole = std::make_shared<ElementPattern>(mono);
    CHECK(monopole->field(kPi / 2, 0, 400e6).power() > 1); // the horizon is the maximum
    e = evaluateArray(ring, monopole);
    const PatternCut cut = computeThetaCut(*e.engine, 90, 1);
    REQUIRE(cut.angle.size() == 361);
    double lo = INFINITY, hi = -INFINITY;
    for (const Field &f : cut.fields) {
        lo = std::min(lo, powerDb(f.power()));
        hi = std::max(hi, powerDb(f.power()));
    }
    CHECK(hi - lo < 1.0);
    // The east-west difference is a figure eight: zero at phi = 90.
    ring.excitation.pattern = BeamPattern::DeltaAzimuth;
    e = evaluateArray(ring, monopole);
    CHECK(e.engine->field(kPi / 2, kPi / 2).power() < 1e-20);
    CHECK(e.engine->field(kPi / 2, 0).power() > 1);
}

TEST_CASE("Any geometry converts to an equivalent custom table")
{
    for (LatticeKind kind : {LatticeKind::Linear, LatticeKind::Rectangular, LatticeKind::Triangular, LatticeKind::Circular,
                             LatticeKind::Cylinder}) {
        GeometrySpec g;
        g.kind = kind;
        g.nx = 5;
        g.ny = 4;
        g.rings = {RingSpec{1.0, 6, 15}};
        g.columns = 7;
        g.rows = 3;
        const ArrayLayout a = buildLayout(g, f0);
        GeometrySpec c = g;
        c.custom = toCustomElements(g, f0);
        c.kind = LatticeKind::Custom;
        const ArrayLayout b = buildLayout(c, f0);
        INFO(latticeId(kind).toStdString());
        REQUIRE(a.elements.size() == b.elements.size());
        for (std::size_t i = 0; i < a.elements.size(); ++i) {
            CHECK((a.elements[i].position - b.elements[i].position).norm() < 1e-12);
            CHECK((a.elements[i].orientation - b.elements[i].orientation).norm() < 1e-9);
        }
    }
}
