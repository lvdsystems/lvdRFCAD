// Tests of the step-3 system blocks: return loss and slopes, LO phase noise
// and reciprocal mixing, the DAC start, the new rf-system blocks and their
// tracing through the signal chain.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/Netlist.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "rfdata/NetworkMath.h"
#include "rfsim/BlockModels.h"
#include "rfsim/Budget.h"
#include "rfsim/ElementModels.h"
#include "rfsim/PhaseNoise.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>

using namespace lvd;
using namespace lvd::rf;
using Catch::Approx;

namespace {

const Complex J(0, 1);

double fromDb10(double db)
{
    return std::pow(10, db / 10);
}

// Places parts and wires them, as in budget_tests.cpp.
struct Sheet {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;

    Sheet()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("sys"), &error);
        REQUIRE(document);
        sheet = document->schematic();
    }

    QString place(const char *key, QPoint at, const QMap<QString, QString> &parameters = {})
    {
        const QString definitionKey = QString::fromLatin1(key);
        const DefinitionPtr definition = project->resolve(*sheet, definitionKey).definition;
        REQUIRE(definition);
        const QString id = sheet->addComponent(definitionKey, definition.get(), at);
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            sheet->setParameter(id, it.key(), it.value());
        return id;
    }

    QPoint pin(const QString &id, const QString &name)
    {
        const PlacedComponent *component = sheet->component(id);
        const DefinitionPtr definition = project->resolve(*sheet, component->definition).definition;
        const SymbolPin *symbolPin = definition->symbol.pin(name);
        REQUIRE(symbolPin);
        return pinPosition(*component, *symbolPin);
    }

    void wire(const QString &a, const char *pinA, const QString &b, const char *pinB)
    {
        const QPoint start = pin(a, QString::fromLatin1(pinA)), end = pin(b, QString::fromLatin1(pinB));
        sheet->addWire({start, QPoint(end.x(), start.y()), end});
    }

    BudgetRun run(const BudgetAnalysisSetup &setup = {})
    {
        BudgetRun result = runBudgetAnalysis(*project, *document, setup);
        INFO((result.errors + result.chain.errors + result.budget.errors).join(QLatin1Char('\n')).toStdString());
        return result;
    }

    // The linear model of one placed component.
    Element element(const QString &reference, const std::vector<double> &frequencies)
    {
        const Netlist netlist = buildNetlist(*project, *document);
        const FlatComponent *component = netlist.component(reference);
        REQUIRE(component);
        ElementContext context;
        QString error;
        QStringList warnings;
        const auto element = buildElement(*component, frequencies, context, &warnings, &error);
        INFO(error.toStdString());
        REQUIRE(element);
        return *element;
    }
};

using Parameters = QMap<QString, QString>;

QString q(const char *text)
{
    return QString::fromUtf8(text);
}

bool isReciprocal(const CMatrix &s)
{
    return (s - s.transpose()).norm() < 1e-12;
}

bool isLossless(const CMatrix &s)
{
    return (s * s.adjoint() - CMatrix::Identity(s.rows(), s.cols())).norm() < 1e-9;
}

double phaseDegrees(Complex value)
{
    return std::arg(value) * 180 / Pi;
}

} // namespace

// ---------------------------------------------------------------- return loss and slopes

TEST_CASE("return loss adds quadrature reflections and keeps passive blocks passive")
{
    CHECK(returnLossToGamma(0) == 0);
    CHECK(returnLossToGamma(20) == Approx(0.1));
    CHECK(sloped(20, 0.5, 3e9, 1e9, false) == Approx(21));
    CHECK(sloped(0.5, -1, 2e9, 1e9, true) == 0);    // a loss never goes below 0
    CHECK(sloped(-0.5, 0, 2e9, 1e9, true) == -0.5); // no slope: unchanged

    CMatrix through(2, 2);
    through << 0.0, 1.0, 1.0, 0.0;
    SECTION("no reflection leaves the matrix bit for bit")
    {
        CHECK(withPortReflections(through, {0.0, 0.0}) == through);
    }
    SECTION("a lossless through with 20 dB return loss is limited to |S21|^2 = 1 - |S11|^2")
    {
        const CMatrix s = withPortReflections(through, {0.1, 0.1});
        CHECK(s(0, 0) == J * 0.1);
        CHECK(std::abs(s(1, 0)) == Approx(std::sqrt(1 - 0.01)).epsilon(1e-9));
        CHECK(largestSingularValue(s) <= 1 + 1e-12);
        CHECK(isLossless(s)); // quadrature reflection: the limit is a lossless two-port
    }
    SECTION("a lossy block keeps its transmission")
    {
        const double t = std::pow(10, -3.0 / 20);
        CMatrix s(2, 2);
        s << 0.0, t, t, 0.0;
        const CMatrix r = withPortReflections(s, {0.1, 0.3});
        CHECK(std::abs(r(1, 0)) == Approx(t));
        CHECK(std::abs(r(1, 1)) == Approx(0.3));
        CHECK(largestSingularValue(r) <= 1 + 1e-12);
        // passiveNoise needs I - S S^H >= 0
        const Eigen::SelfAdjointEigenSolver<CMatrix> eig(passiveNoise(r, T0));
        CHECK(eig.eigenvalues().minCoeff() >= -1e-9);
    }
    SECTION("unequal reflections on a lossless through are clamped to passive")
    {
        const CMatrix s = withPortReflections(through, {0.5, 0.05});
        CHECK(largestSingularValue(s) <= 1 + 1e-12);
        CHECK(largestSingularValue(s) == Approx(1).epsilon(1e-9));
        CHECK(std::abs(s(0, 0)) == Approx(0.5));
    }
    SECTION("one reflecting port, the other matched: |S21|^2 <= 1 - |S11|")
    {
        // det(I - S S^H) >= 0 with S22 = 0 gives (1 - |S21|^2)^2 >= |S11|^2.
        const CMatrix s = withPortReflections(through, {0.2, 0.0});
        CHECK(std::norm(s(1, 0)) == Approx(1 - 0.2).epsilon(1e-9));
        const CMatrix lossy = withPortReflections(through * 0.85, {0.2, 0.0}); // 0.7225 < 0.8: kept
        CHECK(std::abs(lossy(1, 0)) == Approx(0.85));
    }
    SECTION("active blocks are not limited")
    {
        CMatrix gain(2, 2);
        gain << 0.0, 0.01, 10.0, 0.0;
        const CMatrix s = withPortReflections(gain, {0.1, 0.2}, 1.0, false);
        CHECK(s(1, 0) == Complex(10, 0));
        CHECK(s(1, 1) == J * 0.2);
    }
}

TEST_CASE("return losses and slopes reach the linear model, the budget and both mismatch modes")
{
    Sheet c;
    const QString src = c.place("rf-system:source", {0, 0}, {{q("freq"), q("3GHz")}, {q("power"), q("-40")}});
    const QString amp = c.place("rf-system:amplifier", {200, 0},
                                {{q("gain"), q("20")}, {q("nf"), q("2")}, {q("gain_slope"), q("-1")}, {q("nf_slope"), q("0.5")},
                                 {q("freq"), q("1GHz")}, {q("rl_in"), q("10")}, {q("rl_out"), q("15")}, {q("isolation"), q("200")}});
    const QString att = c.place("rf-system:attenuator", {400, 0},
                                {{q("il"), q("3")}, {q("il_slope"), q("0.25")}, {q("freq"), q("1GHz")}, {q("rl_in"), q("10")}});
    const QString term = c.place("rf-system:termination", {600, 0});
    c.wire(src, "out", amp, "in");
    c.wire(amp, "out", att, "in");
    c.wire(att, "out", term, "p");

    // Linear model at 3 GHz: gain 20 - 2, |S11| = -10 dB at +90 degrees.
    const Element a = c.element(q("A1"), {1e9, 3e9});
    CHECK(db20(a.s[0](1, 0)) == Approx(20).margin(1e-9));
    CHECK(db20(a.s[1](1, 0)) == Approx(18).margin(1e-9));
    CHECK(a.s[1](0, 0).real() == Approx(0).margin(1e-12));
    CHECK(a.s[1](0, 0).imag() == Approx(std::pow(10, -0.5)));
    CHECK(db20(a.s[1](1, 1)) == Approx(-15));
    const Element at = c.element(q("AT1"), {3e9});
    CHECK(db20(at.s[0](1, 0)) == Approx(-3.5).margin(1e-9));

    BudgetAnalysisSetup setup;
    setup.mismatch = false;
    const BudgetRun ideal = c.run(setup);
    REQUIRE(ideal.ok());
    CHECK(ideal.budget.stages[0].gainDb == Approx(18).margin(1e-9));
    CHECK(ideal.budget.stages[0].nfDb == Approx(3).margin(1e-9)); // 2 + 0.5 * 2 GHz
    CHECK(ideal.budget.stages[0].inputReturnLossDb == Approx(10).margin(1e-9));
    CHECK(ideal.budget.stages[1].gainDb == Approx(-3.5).margin(1e-9));

    setup.mismatch = true;
    const BudgetRun actual = c.run(setup);
    REQUIRE(actual.ok());
    // S21 is the gain in a matched system, so the first stage keeps 18 dB; between the stages
    // S22 = j 0.178 meets S11 = j 0.316: S21 = S21a S21b / (1 - S22a S11b) = ... / (1 + 0.0562).
    CHECK(actual.budget.stages[0].gainDb == Approx(18).margin(1e-9));
    const double interaction = std::pow(10, -15.0 / 20) * std::pow(10, -10.0 / 20);
    CHECK(actual.budget.stages.back().cumulativeGainDb == Approx(14.5 - 20 * std::log10(1 + interaction)).margin(1e-9));
    CHECK(ideal.budget.stages.back().cumulativeGainDb == Approx(14.5).margin(1e-9));

    // A swept budget follows the slopes.
    setup.sweepStart = q("1GHz");
    setup.sweepStop = q("3GHz");
    setup.sweepPoints = q("3");
    setup.mismatch = false;
    const BudgetRun swept = c.run(setup);
    REQUIRE(swept.ok());
    REQUIRE(swept.sweep.size() == 3);
    CHECK(swept.sweep[0].stages[0].gainDb == Approx(20).margin(1e-9));
    CHECK(swept.sweep[1].stages[0].gainDb == Approx(19).margin(1e-9));
    CHECK(swept.sweep[0].stages[1].gainDb == Approx(-3).margin(1e-9));
}

TEST_CASE("new parameters at their defaults leave the budget unchanged")
{
    // The same chain with the new parameters left at their defaults and set
    // explicitly to the same values: identical numbers, and the textbook values
    // of the matched cascade.
    const auto build = [](Sheet &c, bool explicitDefaults) {
        const Parameters zeros = explicitDefaults ? Parameters{{q("rl_in"), q("0")}, {q("rl_out"), q("0")}, {q("il_slope"), q("0")}}
                                                  : Parameters{};
        Parameters amplifier{{q("gain"), q("20")}, {q("nf"), q("2")}};
        if (explicitDefaults) {
            amplifier.insert(q("gain_slope"), q("0"));
            amplifier.insert(q("nf_slope"), q("0"));
            amplifier.insert(q("rl_in"), q("0"));
            amplifier.insert(q("rl_out"), q("0"));
        }
        const QString src = c.place("rf-system:source", {0, 0}, {{q("freq"), q("1.7GHz")}});
        const QString a1 = c.place("rf-system:amplifier", {200, 0}, amplifier);
        const QString at = c.place("rf-system:attenuator", {400, 0}, zeros);
        const QString iso = c.place("rf-system:isolator", {600, 0}, zeros);
        const QString ps = c.place("rf-system:phase_shifter", {800, 0}, zeros);
        const QString cbl = c.place("rf-system:cable", {1000, 0}, zeros);
        const QString sw = c.place("rf-system:switch_spdt", {1200, 0}, zeros);
        const QString term = c.place("rf-system:termination", {1400, 0});
        c.wire(src, "out", a1, "in");
        c.wire(a1, "out", at, "in");
        c.wire(at, "out", iso, "in");
        c.wire(iso, "out", ps, "in");
        c.wire(ps, "out", cbl, "in");
        c.wire(cbl, "out", sw, "com");
        c.wire(sw, "t1", term, "p");
    };
    Sheet plain, explicitSheet;
    build(plain, false);
    build(explicitSheet, true);
    for (const bool mismatch : {true, false}) {
        BudgetAnalysisSetup setup;
        setup.mismatch = mismatch;
        const BudgetRun a = plain.run(setup), b = explicitSheet.run(setup);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        REQUIRE(a.budget.stages.size() == b.budget.stages.size());
        for (size_t i = 0; i < a.budget.stages.size(); ++i) {
            CHECK(a.budget.stages[i].cumulativeGainDb == b.budget.stages[i].cumulativeGainDb);
            CHECK(a.budget.stages[i].cumulativeNfDb == b.budget.stages[i].cumulativeNfDb);
            CHECK(a.budget.stages[i].snrDb == b.budget.stages[i].snrDb);
            CHECK(a.budget.stages[i].snrDb == a.budget.stages[i].thermalSnrDb); // no phase noise: total = thermal
            CHECK(a.budget.stages[i].inputReturnLossDb == INFINITY);
        }
        const double cableLoss = 0.1 + 0.5 * std::sqrt(1.7);
        CHECK(a.budget.stages.back().cumulativeGainDb == Approx(20 - 3 - 0.5 - 1 - cableLoss - 0.5).margin(1e-9));
        CHECK_FALSE(a.budget.hasPhaseNoise);
        CHECK_FALSE(a.budget.hasDac);
    }
}

// ---------------------------------------------------------------- phase noise

TEST_CASE("phase-noise profiles interpolate in log frequency and integrate exactly")
{
    QString error;
    const auto flat = parsePhaseNoise("# flat\n1e3 -100\n1e5, -100 ! comment\n", &error);
    REQUIRE(flat);
    // Flat L over [a, b]: L + 10 log10(b - a).
    CHECK(integratePhaseNoiseSsb(*flat, 1e3, 1e5) == Approx(-100 + db10(1e5 - 1e3)).margin(1e-9));
    // Held beyond the table.
    CHECK(integratePhaseNoiseSsb(*flat, 10, 1e6) == Approx(-100 + db10(1e6 - 10)).margin(1e-9));

    // A -20 dB/decade segment: L = L0 (f0/f)^2, integral L0 f0^2 (1/a - 1/b).
    const auto slope = parsePhaseNoise("1e5 -120\n1e3 -80\n", &error); // unsorted on purpose
    REQUIRE(slope);
    CHECK(slope->level(1e4) == Approx(-100));
    CHECK(slope->level(3e3) == Approx(-80 - 20 * std::log10(3)).margin(1e-9));
    const double expected = 1e-8 * 1e6 * (1 / 2e3 - 1 / 5e4);
    CHECK(integratePhaseNoiseSsb(*slope, 2e3, 5e4) == Approx(db10(expected)).margin(1e-9));

    // Both sidebands, RMS phase and jitter.
    const IntegratedPhaseNoise total = integratePhaseNoise(*slope, 2e3, 5e4, 1e9);
    CHECK(total.dbc == Approx(db10(2 * expected)).margin(1e-9));
    CHECK(total.rmsPhaseDegrees == Approx(std::sqrt(2 * expected) * 180 / Pi));
    CHECK(total.rmsJitterSeconds == Approx(std::sqrt(2 * expected) / (2 * Pi * 1e9)));

    CHECK_FALSE(parsePhaseNoise("1e3 -100 5\n", &error));
    CHECK(error.contains(q("line 1")));
    CHECK_FALSE(parsePhaseNoise("1e3 -100\n1e3 -90\n", &error));
    CHECK_FALSE(parsePhaseNoise("-5 -100\n", &error));
}

TEST_CASE("VCO and PLL phase-noise models follow their formulas")
{
    CHECK(vcoLevel(1e5, -110, 1e5, -300) == Approx(-110).margin(1e-9));
    CHECK(vcoLevel(1e6, -110, 1e5, -300) == Approx(-130).margin(1e-9));
    CHECK(vcoLevel(1e9, -110, 1e5, -160) == Approx(db10(fromDb10(-190) + fromDb10(-160))).margin(1e-9));

    PllSettings pll;
    pll.frequency = 2e9;
    pll.reference = 10e6;
    pll.referencePn = -150;
    pll.fom = -226;
    pll.loopBandwidth = 100e3;
    pll.vcoPn = -110;
    pll.vcoPnOffset = 100e3;
    pll.vcoFloor = -160;
    const double n = 200;
    const double inBand = std::max(-150 + 20 * std::log10(n), -226 + 10 * std::log10(10e6) + 20 * std::log10(n));
    CHECK(inBand == Approx(-103.979).margin(1e-3)); // the reference dominates the -110 dBc/Hz PLL floor
    const auto expected = [&](double f) {
        const double x2 = std::pow(f / 100e3, 2);
        const double vco = fromDb10(-110 - 20 * std::log10(f / 100e3)) + fromDb10(-160);
        return db10(fromDb10(inBand) / (1 + x2) + vco * x2 / (1 + x2));
    };
    for (const double f : {10.0, 1e3, 1e5, 1e6, 1e8})
        CHECK(pllLevel(f, pll) == Approx(expected(f)).margin(1e-9));
    // Shape: flat in band, -3 dB plus VCO at the loop bandwidth, VCO far out, floor at the end.
    CHECK(pllLevel(10, pll) == Approx(db10(fromDb10(inBand) + 1e-11)).margin(1e-6));
    CHECK(pllLevel(1e9, pll) == Approx(-160).margin(0.05));
    CHECK(pllLevel(1e3, pll) > pllLevel(1e6, pll) + 10);
    // The tabulated profile matches the model between its points.
    const PhaseNoiseProfile table = tabulate([&](double f) { return pllLevel(f, pll); });
    CHECK(table.level(3.3e4) == Approx(pllLevel(3.3e4, pll)).margin(0.05));
}

namespace {

// Source -> mixer -> termination with the LO from `lo` (through an optional multiplier).
struct MixerChain {
    Sheet c;
    QString lo;

    MixerChain(const char *loKey, const Parameters &loParameters, int multiplier = 1)
    {
        const QString src = c.place("rf-system:source", {0, 0}, {{q("freq"), q("1GHz")}, {q("power"), q("-60")}});
        const QString mixer = c.place("rf-system:mixer", {200, 0}, {{q("gain"), q("-7")}, {q("nf"), q("7")}});
        const QString term = c.place("rf-system:termination", {400, 0});
        lo = c.place(loKey, {0, 300}, loParameters);
        c.wire(src, "out", mixer, "rf");
        c.wire(mixer, "if", term, "p");
        if (multiplier > 1) {
            const QString mul = c.place("rf-system:multiplier", {100, 300},
                                        {{q("factor"), QString::number(multiplier)}, {q("gain"), q("0")}});
            c.wire(lo, "out", mul, "in");
            c.wire(mul, "out", mixer, "lo");
        } else {
            c.wire(lo, "out", mixer, "lo");
        }
    }
};

} // namespace

TEST_CASE("a VCO's phase noise limits the SNR, raised by 20 log N through a multiplier")
{
    const Parameters vco{{q("freq"), q("0.225GHz")}, {q("power"), q("13")}, {q("pn"), q("-130")},
                         {q("pn_offset"), q("100kHz")}, {q("pn_floor"), q("-300")}};
    MixerChain direct("rf-system:vco", vco, 1);
    MixerChain multiplied("rf-system:vco", vco, 4);
    BudgetAnalysisSetup setup;
    setup.bandwidth = q("10kHz");
    setup.spurs = false;

    const BudgetRun a = direct.c.run(setup);
    REQUIRE(a.ok());
    REQUIRE(a.budget.hasPhaseNoise);
    CHECK(a.budget.phaseNoiseStart == Approx(10));   // bandwidth / 1000
    CHECK(a.budget.phaseNoiseStop == Approx(5000)); // bandwidth / 2
    // -20 dB/dec from -130 dBc/Hz at 100 kHz: SSB integral L0 f0^2 (1/a - 1/b), both sidebands x2.
    const double ssb = 1e-13 * 1e10 * (1.0 / 10 - 1.0 / 5000);
    const BudgetStage &mixer = a.budget.stages[0];
    CHECK(mixer.loPhaseNoiseDbc == Approx(db10(2 * ssb)).margin(1e-3));
    CHECK(mixer.phaseNoiseSnrDb == Approx(-db10(2 * ssb)).margin(1e-3));
    CHECK(mixer.phaseNoiseDbm == Approx(mixer.signalDbm + db10(2 * ssb)).margin(1e-3));
    CHECK(mixer.snrDb == Approx(mixer.signalDbm - db10(fromDb10(mixer.noiseDbm) + fromDb10(mixer.phaseNoiseDbm))).margin(1e-9));
    CHECK(mixer.snrDb < mixer.thermalSnrDb);

    const BudgetRun b = multiplied.c.run(setup);
    REQUIRE(b.ok());
    CHECK(b.chain.stages[0].loMultiplication == 4);
    CHECK(b.chain.stages[0].loFrequency == Approx(0.9e9));
    CHECK(b.budget.stages[0].loPhaseNoiseDbc == Approx(mixer.loPhaseNoiseDbc + 20 * std::log10(4.0)).margin(1e-3));
    CHECK(b.budget.stages[0].loJitterS == Approx(mixer.loJitterS * 4 * 0.225 / 0.9).epsilon(1e-6)); // same jitter x N / N
}

TEST_CASE("reciprocal mixing of a blocker follows blocker + L(offset) + 10 log B")
{
    MixerChain m("rf-system:vco",
                 {{q("freq"), q("0.9GHz")}, {q("power"), q("13")}, {q("pn"), q("-130")}, {q("pn_offset"), q("100kHz")},
                  {q("pn_floor"), q("-300")}});
    BudgetAnalysisSetup setup;
    setup.bandwidth = q("10kHz");
    setup.blockerOffset = q("1MHz");
    setup.blockerPower = q("0");
    setup.spurs = false;
    const BudgetRun run = m.c.run(setup);
    REQUIRE(run.ok());
    REQUIRE(run.budget.hasBlocker);
    const BudgetStage &mixer = run.budget.stages[0];
    CHECK(mixer.blockerDbm == Approx(0).margin(1e-9));
    CHECK(mixer.loPhaseNoiseAtOffsetDbcHz == Approx(-150).margin(1e-6));
    // 0 dBm - 150 dBc/Hz + 40 dB = -110 dBm at the mixer input, -117 dBm after its -7 dB.
    CHECK(mixer.reciprocalMixingDbm == Approx(-117).margin(1e-6));
    CHECK(mixer.reciprocalMixingSnrDb == Approx(-67 - -117).margin(1e-6));
    const double total = fromDb10(mixer.noiseDbm) + fromDb10(mixer.phaseNoiseDbm) + fromDb10(mixer.reciprocalMixingDbm);
    CHECK(mixer.totalNoiseDbm == Approx(db10(total)).margin(1e-9));
    CHECK(mixer.snrDb == Approx(mixer.signalDbm - db10(total)).margin(1e-9));

    // Stored setup keys round-trip.
    QString id;
    m.c.sheet->setSimulation(withBudgetAnalysis(m.c.sheet->simulation(), setup, &id));
    const BudgetAnalysisSetup stored = budgetAnalyses(m.c.sheet->simulation()).first();
    CHECK(stored.blockerOffset == q("1MHz"));
    CHECK(stored.blockerPower == q("0"));
    CHECK(stored.phaseNoiseStart.isEmpty());
}

TEST_CASE("an LO with a phase-noise file is read relative to the project")
{
    MixerChain m("rf-system:lo", {{q("freq"), q("0.9GHz")}, {q("power"), q("13")}, {q("phase_noise"), q("lo.pn")}});
    QFile file(QDir(m.c.project->directory()).filePath(q("lo.pn")));
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("# offset L\n100 -90\n1e6 -90\n");
    file.close();
    BudgetAnalysisSetup setup;
    setup.bandwidth = q("100kHz");
    setup.phaseNoiseStart = q("1kHz");
    setup.phaseNoiseStop = q("11kHz");
    setup.spurs = false;
    const BudgetRun run = m.c.run(setup);
    REQUIRE(run.ok());
    CHECK(run.budget.stages[0].loPhaseNoiseDbc == Approx(-90 + db10(2 * 1e4)).margin(1e-9));

    // A missing file is a warning, not an error.
    m.c.sheet->setParameter(m.lo, q("phase_noise"), q("missing.pn"));
    const BudgetRun missing = m.c.run(setup);
    REQUIRE(missing.ok());
    CHECK_FALSE(missing.budget.hasPhaseNoise);
    CHECK(missing.warnings.join(QString()).contains(q("phase noise is not used")));
}

TEST_CASE("a PLL drives a mixer like an LO")
{
    MixerChain m("rf-system:pll", {{q("freq"), q("0.9GHz")}, {q("ref_freq"), q("10MHz")}});
    BudgetAnalysisSetup setup;
    setup.spurs = false;
    const BudgetRun run = m.c.run(setup);
    REQUIRE(run.ok());
    CHECK(run.chain.stages[0].loSource == q("PLL1"));
    CHECK(run.chain.stages[0].outputFrequency == Approx(100e6));
    CHECK(run.budget.hasPhaseNoise);
    CHECK(std::isfinite(run.budget.stages[0].loRmsPhaseDeg));
}

// ---------------------------------------------------------------- DAC

TEST_CASE("a DAC starts a transmit chain with its sinc roll-off and noise density")
{
    const DacFigures d = dacFigures(100e6, 1e9, 14, 0, 6, 0);
    const double sinc = 20 * std::log10(std::sin(Pi * 0.1) / (Pi * 0.1));
    CHECK(d.sincDb == Approx(sinc));
    CHECK(d.sincDb == Approx(-0.14335).margin(1e-5));
    CHECK(d.signalDbm == Approx(-6 + sinc));
    CHECK(d.nsdFromQuantisation);
    CHECK(d.nsdDbmHz == Approx(0 - 6.02 * 14 - 1.76 - 10 * std::log10(0.5e9)));
    CHECK(d.imageFrequency == Approx(900e6));
    CHECK(d.imageDbm == Approx(-6 + 20 * std::log10(std::sin(Pi * 0.9) / (Pi * 0.9))));
    CHECK(dacFigures(100e6, 1e9, 14, 0, 6, -155).nsdDbmHz == -155);
    CHECK(sincDb(0, 1e9) == 0);

    Sheet c;
    const QString dac = c.place("rf-system:dac", {0, 0},
                                {{q("freq"), q("100MHz")}, {q("fs"), q("1GHz")}, {q("bits"), q("14")}, {q("full_scale"), q("0")},
                                 {q("backoff"), q("6")}});
    const QString lpf = c.place("rf-system:filter", {200, 0},
                                {{q("type"), q("lowpass")}, {q("fc"), q("200MHz")}, {q("il"), q("0.5")}, {q("order"), q("7")}});
    const QString amp = c.place("rf-system:amplifier", {400, 0}, {{q("gain"), q("20")}, {q("nf"), q("5")}});
    const QString det = c.place("rf-system:detector", {600, 0}, {{q("sensitivity"), q("-20")}, {q("dynamic_range"), q("40")}});
    c.wire(dac, "out", lpf, "in");
    c.wire(lpf, "out", amp, "in");
    c.wire(amp, "out", det, "in");

    BudgetAnalysisSetup setup;
    setup.mismatch = false;
    const BudgetRun run = c.run(setup);
    REQUIRE(run.ok());
    CHECK(run.chain.startKind == q("dac"));
    CHECK(run.chain.endKind == q("detector"));
    const BudgetResult &b = run.budget;
    REQUIRE(b.hasDac);
    CHECK(b.inputPowerDbm == Approx(d.signalDbm));
    CHECK(b.inputNoiseDbm == Approx(d.nsdDbmHz + 60).margin(1e-6)); // NSD + 10 log10(1 MHz)
    CHECK(b.dacImageFrequency == Approx(900e6));
    CHECK(b.dacImageDbm == Approx(d.imageDbm));
    // The low-pass filter attenuates the image far more than the wanted signal.
    const double imageRejection = (b.stages.back().signalDbm - b.inputPowerDbm) - (b.stages.back().dacImageDbm - b.dacImageDbm);
    CHECK(imageRejection > 60);
    // Detector margin over its sensitivity and headroom to the top of its range.
    REQUIRE(b.hasDetector);
    const double out = b.stages.back().signalDbm;
    CHECK(b.detectorMarginDb == Approx(out - -20));
    CHECK(b.detectorHeadroomDb == Approx(20 - out));
    // The input noise is the DAC's density, T = NSD / k, plus the cascade's T0 (F - 1).
    const double f = fromDb10(b.stages.back().cumulativeNfDb);
    const double noise = db10(Boltzmann * (d.noiseTemperature + T0 * (f - 1)) * 1e6 * 1000);
    CHECK(b.stages.back().thermalSnrDb == Approx(b.inputPowerDbm - noise).margin(1e-9));
    CHECK(b.stages.back().snrDb == b.stages.back().thermalSnrDb);
}

// ---------------------------------------------------------------- new blocks: S-matrices

TEST_CASE("hybrids, balun, diplexer, switches and DC block have the documented S-matrices")
{
    SECTION("90 degree hybrid: lossless, reciprocal, quadrature")
    {
        const CMatrix s = hybrid90(0, 400, 0, 0);
        CHECK(isReciprocal(s));
        CHECK(isLossless(s));
        CHECK(std::norm(s(1, 0)) == Approx(0.5));
        CHECK(phaseDegrees(s(2, 0) / s(1, 0)) == Approx(-90)); // out90 lags out0
        CHECK(std::abs(s(3, 0)) < 1e-15);
        // A phase imbalance moves the output phases and stays lossless.
        const CMatrix skewed = hybrid90(0, 400, 0, 6);
        CHECK(isLossless(skewed));
        CHECK(phaseDegrees(skewed(2, 0) / skewed(1, 0)) == Approx(-90 + 6));
        const CMatrix lossy = hybrid90(0.3, 20, 0.2, 2);
        CHECK(largestSingularValue(lossy) <= 1);
        CHECK(db20(lossy(1, 0)) - db20(lossy(2, 0)) == Approx(-0.2)); // out0 path +imbalance/2 loss
    }
    SECTION("180 degree hybrid: sum in phase, difference in antiphase")
    {
        const CMatrix s = hybrid180(0, 400, 0, 0);
        CHECK(isReciprocal(s));
        CHECK(isLossless(s));
        CHECK(std::abs(s(2, 0) - s(3, 0)) < 1e-15);
        CHECK(std::abs(s(2, 1) + s(3, 1)) < 1e-15);
    }
    SECTION("balun: 180 degrees between the balanced pins")
    {
        const CMatrix s = balun(0.5, 20, 0, 0);
        CHECK(isReciprocal(s));
        CHECK(std::abs(phaseDegrees(s(2, 0) / s(1, 0))) == Approx(180));
        CHECK(largestSingularValue(s) <= 1);
    }
    SECTION("diplexer: complementary, matched at com, lossless without il")
    {
        for (const double f : {0.1e9, 0.7e9, 1e9, 1.3e9, 5e9}) {
            const CMatrix s = diplexer(f, 1e9, 3, 0);
            CHECK(isReciprocal(s));
            CHECK(isLossless(s));
            CHECK(std::abs(s(0, 0)) == 0);
            CHECK(std::norm(s(1, 0)) + std::norm(s(2, 0)) == Approx(1));
        }
        CHECK(db20(diplexer(1e9, 1e9, 3, 0)(1, 0)) == Approx(-3.0103).margin(1e-4));
        CHECK(db20(diplexer(1e8, 1e9, 3, 0)(2, 0)) == Approx(-60).margin(0.01)); // 3rd order: 60 dB/decade
        CHECK(db20(diplexer(1e8, 1e9, 3, 0.5)(1, 0)) == Approx(-0.5).margin(1e-6));
    }
    SECTION("switches: the selected throw passes, the others are isolated")
    {
        const CMatrix s = multiThrowSwitch(4, 3, 0.8, 30);
        CHECK(isReciprocal(s));
        CHECK(db20(s(3, 0)) == Approx(-0.8));
        CHECK(db20(s(1, 0)) == Approx(-30));
        CHECK(db20(s(2, 4)) == Approx(-30));
        CHECK(largestSingularValue(s) <= 1);
    }
    SECTION("DC block: blocks DC, -3 dB at fc, lossless without il")
    {
        CHECK(std::abs(dcBlock(0, 1e6, 0)(1, 0)) == 0);
        CHECK(std::norm(dcBlock(1e6, 1e6, 0)(1, 0)) == Approx(0.5));
        CHECK(isLossless(dcBlock(3e6, 1e6, 0)));
        CHECK(db20(dcBlock(1e9, 1e6, 0.1)(1, 0)) == Approx(-0.1).margin(1e-5));
    }
}

TEST_CASE("the library blocks build passive linear models with their parameters")
{
    Sheet c;
    c.place("rf-system:bias_tee", {0, 0},
            {{q("il"), q("0.3")}, {q("isolation"), q("40")}, {q("rl_in"), q("15")}, {q("rl_out"), q("15")}});
    c.place("rf-system:hybrid_90", {0, 200}, {{q("rl"), q("20")}});
    c.place("rf-system:hybrid_180", {0, 400});
    c.place("rf-system:balun", {0, 600});
    c.place("rf-system:switch_sp3t", {0, 800}, {{q("state"), q("t2")}});
    c.place("rf-system:switch_sp4t", {0, 1000}, {{q("state"), q("t4")}});
    c.place("rf-system:limiter", {0, 1200}, {{q("il"), q("0.5")}});
    c.place("rf-system:diplexer", {0, 1400});
    c.place("rf-system:dc_block", {0, 1600});
    c.place("rf-system:splitter", {0, 1800}, {{q("rl_in"), q("12")}, {q("rl_out"), q("18")}});
    c.place("rf-system:coupler", {0, 2000}, {{q("rl"), q("10")}});
    c.place("rf-system:mixer", {0, 2200}, {{q("rl_rf"), q("10")}});
    for (const char *reference : {"BT1", "HYB1", "HYB2", "BAL1", "SW1", "SW2", "LIM1", "DPX1", "DCB1", "SPL1", "DC1", "MX1"}) {
        INFO(reference);
        const Element e = c.element(q(reference), {0.5e9, 1e9, 2e9});
        for (const CMatrix &s : e.s) {
            CHECK(largestSingularValue(s) <= 1 + 1e-9);
            CHECK(isReciprocal(s));
        }
        const Eigen::SelfAdjointEigenSolver<CMatrix> eig(e.noise[1]);
        CHECK(eig.eigenvalues().minCoeff() >= -1e-6);
    }
    const Element bt = c.element(q("BT1"), {1e9});
    CHECK(db20(bt.s[0](1, 0)) == Approx(-0.3).margin(1e-9));
    CHECK(db20(bt.s[0](2, 0)) == Approx(-40).margin(1e-9));
    CHECK(db20(bt.s[0](0, 0)) == Approx(-15).margin(1e-9));
    const Element sw = c.element(q("SW1"), {1e9});
    CHECK(db20(sw.s[0](2, 0)) == Approx(-0.6).margin(1e-9));
    const Element sw4 = c.element(q("SW2"), {1e9});
    CHECK(db20(sw4.s[0](4, 0)) == Approx(-0.8).margin(1e-9));
    const Element spl = c.element(q("SPL1"), {1e9});
    CHECK(db20(spl.s[0](0, 0)) == Approx(-12).margin(1e-9));
    CHECK(db20(spl.s[0](1, 1)) == Approx(-18).margin(1e-9));
}

// ---------------------------------------------------------------- tracing

TEST_CASE("the chain is traced through hybrids, diplexers, bias tees, baluns and multi-throw switches")
{
    SECTION("90 degree hybrid: default out0 with a warning, out90 by path")
    {
        Sheet c;
        const QString src = c.place("rf-system:source", {0, 0}, {{q("freq"), q("1GHz")}});
        const QString hyb = c.place("rf-system:hybrid_90", {200, 0},
                                    {{q("il"), q("0")}, {q("imbalance"), q("0")}, {q("isolation"), q("200")}});
        const QString t1 = c.place("rf-system:termination", {400, -100});
        const QString at = c.place("rf-system:attenuator", {400, 100}, {{q("il"), q("6")}});
        const QString t2 = c.place("rf-system:termination", {600, 100});
        const QString t3 = c.place("rf-system:termination", {200, 200});
        c.wire(src, "out", hyb, "in");
        c.wire(hyb, "out0", t1, "p");
        c.wire(hyb, "out90", at, "in");
        c.wire(at, "out", t2, "p");
        c.wire(hyb, "iso", t3, "p");
        BudgetAnalysisSetup setup;
        setup.mismatch = false;
        const BudgetRun automatic = c.run(setup);
        REQUIRE(automatic.ok());
        CHECK(automatic.chain.end == q("TERM1"));
        CHECK(automatic.warnings.join(QString()).contains(q("HYB1")));
        setup.path = {q("SRC1"), q("HYB1.out90"), q("AT1"), q("TERM2")};
        const BudgetRun chosen = c.run(setup);
        REQUIRE(chosen.ok());
        CHECK(chosen.budget.stages.back().cumulativeGainDb == Approx(-3.0103 - 6).margin(1e-4));
    }
    SECTION("diplexer: the band of the signal's frequency")
    {
        for (const char *frequency : {"500MHz", "2GHz"}) {
            Sheet c;
            const QString src = c.place("rf-system:source", {0, 0}, {{q("freq"), q(frequency)}});
            const QString dpx = c.place("rf-system:diplexer", {200, 0}, {{q("crossover"), q("1GHz")}});
            const QString t1 = c.place("rf-system:termination", {400, -100});
            const QString t2 = c.place("rf-system:termination", {400, 100});
            c.wire(src, "out", dpx, "com");
            c.wire(dpx, "low", t1, "p");
            c.wire(dpx, "high", t2, "p");
            const BudgetRun run = c.run();
            REQUIRE(run.ok());
            CHECK(run.chain.end == (std::string(frequency) == "2GHz" ? q("TERM2") : q("TERM1")));
            CHECK_FALSE(run.warnings.join(QString()).contains(q("DPX1")));
            CHECK(run.budget.stages[0].gainDb > -0.5);
        }
    }
    SECTION("bias tee: the dc pin is never on the path")
    {
        Sheet c;
        const QString src = c.place("rf-system:source", {0, 0}, {{q("freq"), q("1GHz")}});
        const QString bt = c.place("rf-system:bias_tee", {200, 0}, {{q("il"), q("0.3")}});
        const QString supply = c.place("rf-system:termination", {300, 200});
        const QString amp = c.place("rf-system:amplifier", {400, 0}, {{q("gain"), q("10")}});
        const QString term = c.place("rf-system:termination", {600, 0});
        c.wire(src, "out", bt, "rf_in");
        c.wire(bt, "dc", supply, "p");
        c.wire(bt, "rf_out", amp, "in");
        c.wire(amp, "out", term, "p");
        BudgetAnalysisSetup setup;
        setup.mismatch = false;
        const BudgetRun run = c.run(setup);
        REQUIRE(run.ok());
        REQUIRE(run.chain.stages.size() == 2);
        CHECK(run.chain.end == q("TERM2"));
        CHECK(run.budget.stages.back().cumulativeGainDb == Approx(9.7).margin(1e-6));
        setup.path = {q("SRC1"), q("BT1.dc"), q("TERM1")};
        CHECK_FALSE(c.run(setup).ok());
    }
    SECTION("balun: the budget counts the power of both balanced pins")
    {
        Sheet c;
        const QString src = c.place("rf-system:source", {0, 0}, {{q("freq"), q("1GHz")}});
        const QString bal = c.place("rf-system:balun", {200, 0},
                                    {{q("il"), q("0.5")}, {q("imbalance"), q("0.2")}, {q("isolation"), q("200")}});
        const QString t1 = c.place("rf-system:termination", {400, -100});
        const QString t2 = c.place("rf-system:termination", {400, 100});
        c.wire(src, "out", bal, "unbal");
        c.wire(bal, "bal_p", t1, "p");
        c.wire(bal, "bal_n", t2, "p");
        BudgetAnalysisSetup setup;
        setup.mismatch = false;
        const BudgetRun run = c.run(setup);
        REQUIRE(run.ok());
        CHECK_FALSE(run.warnings.join(QString()).contains(q("BAL1")));
        const double expected = db10(0.5 * fromDb10(-0.6) + 0.5 * fromDb10(-0.4));
        CHECK(run.budget.stages[0].gainDb == Approx(expected).margin(1e-9));
        CHECK(run.budget.stages[0].nfDb == Approx(-expected).margin(1e-9));
    }
    SECTION("SP3T: the selected throw")
    {
        Sheet c;
        const QString src = c.place("rf-system:source", {0, 0}, {{q("freq"), q("1GHz")}});
        const QString sw = c.place("rf-system:switch_sp3t", {200, 0}, {{q("state"), q("t3")}});
        const QString t1 = c.place("rf-system:termination", {400, -200});
        const QString t2 = c.place("rf-system:termination", {500, 0});
        const QString t3 = c.place("rf-system:termination", {600, 200});
        c.wire(src, "out", sw, "com");
        c.wire(sw, "t1", t1, "p");
        c.wire(sw, "t2", t2, "p");
        c.wire(sw, "t3", t3, "p");
        const BudgetRun run = c.run();
        REQUIRE(run.ok());
        CHECK(run.chain.end == q("TERM3"));
        CHECK_FALSE(run.warnings.join(QString()).contains(q("SW1")));
    }
    SECTION("a limiter acts as a hard compression point")
    {
        Sheet c;
        const QString src = c.place("rf-system:source", {0, 0}, {{q("freq"), q("1GHz")}, {q("power"), q("20")}});
        const QString lim = c.place("rf-system:limiter", {200, 0}, {{q("limit"), q("10")}, {q("il"), q("0.5")}});
        const QString term = c.place("rf-system:termination", {400, 0});
        c.wire(src, "out", lim, "in");
        c.wire(lim, "out", term, "p");
        const BudgetRun run = c.run();
        REQUIRE(run.ok());
        const BudgetStage &stage = run.budget.stages[0];
        CHECK(stage.op1dbDbm == 10);
        CHECK(stage.headroomDb == Approx(10 - 19.5).margin(1e-6));
        CHECK(stage.compressed);
    }
}

TEST_CASE("the spur analysis runs from a DAC start")
{
    Sheet c;
    const QString dac = c.place("rf-system:dac", {0, 0}, {{q("freq"), q("100MHz")}, {q("fs"), q("1GHz")}});
    const QString mixer = c.place("rf-system:mixer", {200, 0}, {{q("product"), q("sum")}});
    const QString lo = c.place("rf-system:pll", {200, 200}, {{q("freq"), q("2GHz")}});
    const QString bpf = c.place("rf-system:filter", {400, 0}, {{q("fc"), q("2.1GHz")}, {q("bw"), q("100MHz")}});
    const QString term = c.place("rf-system:termination", {600, 0});
    c.wire(dac, "out", mixer, "rf");
    c.wire(lo, "out", mixer, "lo");
    c.wire(mixer, "if", bpf, "in");
    c.wire(bpf, "out", term, "p");
    const BudgetRun run = c.run();
    REQUIRE(run.ok());
    CHECK(run.chain.stages[0].outputFrequency == Approx(2.1e9));
    CHECK(run.spurs.ok());
    CHECK(run.spurs.wantedOutputDbm == Approx(run.budget.inputPowerDbm - 7 - 1).margin(0.01));
    CHECK_FALSE(run.spurs.spurs.empty());
}

TEST_CASE("every rf-system behavioural block has a linear model")
{
    QFile file(QDir(QStringLiteral(LVD_RF_TEST_DATA)).filePath(QStringLiteral("../../../../libraries/rf-system.lvdlib")));
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QJsonArray components = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("components")).toArray();
    int checked = 0;
    for (const QJsonValue &value : components) {
        const QJsonObject component = value.toObject();
        const QJsonObject model = component.value(QStringLiteral("models")).toArray().first().toObject();
        const QString id = component.value(QStringLiteral("id")).toString();
        if (model.value(QStringLiteral("kind")).toString() != QLatin1String("rf-behavioural") || id == QLatin1String("port"))
            continue;
        INFO(id.toStdString());
        Sheet c;
        const QByteArray key = QStringLiteral("rf-system:%1").arg(id).toLatin1();
        c.place(key.constData(), {0, 0});
        const Netlist netlist = buildNetlist(*c.project, *c.document);
        REQUIRE(netlist.components.size() == 1);
        ElementContext context;
        QString error;
        QStringList warnings;
        const auto element = buildElement(netlist.components.front(), {1e9}, context, &warnings, &error);
        INFO(error.toStdString());
        REQUIRE(element);
        CHECK(element->s.front().allFinite());
        ++checked;
    }
    CHECK(checked >= 30);
}
