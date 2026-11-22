#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "rfsim/Analysis.h"
#include "rfsim/Budget.h"
#include "rfsim/Measurement.h"

#include <QDir>
#include <algorithm>

#include <QJsonArray>
#include <QTemporaryDir>

#include <cmath>

using namespace lvd;
using namespace lvd::rf;
using Catch::Approx;

namespace {

// Places parts left to right on one line and wires each output to the next input.
struct Chain {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;
    int x = 0;

    Chain()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("rx"), &error);
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
};

double friis(std::initializer_list<std::pair<double, double>> stages) // (gain dB, nf dB)
{
    double f = 1, g = 1;
    bool first = true;
    for (const auto &[gainDb, nfDb] : stages) {
        const double fi = std::pow(10, nfDb / 10);
        f = first ? fi : f + (fi - 1) / g;
        g *= std::pow(10, gainDb / 10);
        first = false;
    }
    return 10 * std::log10(f);
}

} // namespace

TEST_CASE("a simple chain gives the textbook budget")
{
    Chain c;
    const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("1GHz")}, {QStringLiteral("power"), QStringLiteral("-30")}});
    const QString amp = c.place("rf-system:amplifier", {200, 0},
                                {{QStringLiteral("gain"), QStringLiteral("20")}, {QStringLiteral("nf"), QStringLiteral("2")},
                                 {QStringLiteral("oip3"), QStringLiteral("30")}, {QStringLiteral("op1db"), QStringLiteral("20")},
                                 {QStringLiteral("isolation"), QStringLiteral("200")}});
    const QString att = c.place("rf-system:attenuator", {400, 0}, {{QStringLiteral("il"), QStringLiteral("6")}});
    const QString term = c.place("rf-system:termination", {600, 0});
    c.wire(src, "out", amp, "in");
    c.wire(amp, "out", att, "in");
    c.wire(att, "out", term, "p");

    const BudgetRun run = c.run();
    REQUIRE(run.ok());
    REQUIRE(run.chain.start == QStringLiteral("SRC1"));
    REQUIRE(run.chain.end == QStringLiteral("TERM1"));
    REQUIRE(run.budget.stages.size() == 2);
    const BudgetStage &last = run.budget.stages.back();
    CHECK(run.budget.stages[0].gainDb == Approx(20).margin(1e-9));
    CHECK(last.gainDb == Approx(-6).margin(1e-9));
    CHECK(last.cumulativeGainDb == Approx(14).margin(1e-9));
    CHECK(last.nfDb == Approx(6).margin(1e-9));
    CHECK(last.cumulativeNfDb == Approx(friis({{20, 2}, {-6, 6}})).margin(1e-6));
    CHECK(last.iip3Dbm == Approx(10).margin(1e-9));   // the attenuator is linear
    CHECK(last.oip3CumulativeDbm == Approx(24).margin(1e-9));
    CHECK(last.signalDbm == Approx(-16).margin(1e-9));
    const double inputNoise = 10 * std::log10(1.380649e-23 * 290 * 1e6 * 1000);
    CHECK(run.budget.inputNoiseDbm == Approx(inputNoise).margin(1e-9));
    CHECK(last.noiseDbm == Approx(inputNoise + last.cumulativeNfDb + 14).margin(1e-6));
    CHECK(last.snrDb == Approx(-30 - inputNoise - last.cumulativeNfDb).margin(1e-6));
    CHECK(last.headroomDb == Approx(14 - -16).margin(1e-9)); // OP1dB 20 - 6, signal -16
    CHECK_FALSE(last.compressed);
}

TEST_CASE("intercept points cascade with the input-referred formula")
{
    Chain c;
    const QString src = c.place("rf-system:source", {0, 0});
    const QString a1 = c.place("rf-system:amplifier", {200, 0},
                               {{QStringLiteral("gain"), QStringLiteral("10")}, {QStringLiteral("oip3"), QStringLiteral("20")}});
    const QString a2 = c.place("rf-system:amplifier", {400, 0},
                               {{QStringLiteral("gain"), QStringLiteral("10")}, {QStringLiteral("oip3"), QStringLiteral("30")}});
    const QString term = c.place("rf-system:termination", {600, 0});
    c.wire(src, "out", a1, "in");
    c.wire(a1, "out", a2, "in");
    c.wire(a2, "out", term, "p");
    BudgetAnalysisSetup setup;
    setup.mismatch = false;
    const BudgetRun run = c.run(setup);
    REQUIRE(run.ok());
    // IIP3: 10 mW and 100 mW; 1/IIP3 = 1/10 + 10/100 -> 5 mW.
    CHECK(run.budget.stages.back().iip3Dbm == Approx(10 * std::log10(5.0)).margin(1e-9));
}

TEST_CASE("a superheterodyne receiver gets its frequency plan, LO drive and ADC alias")
{
    Chain c;
    c.sheet->setVariables({{QStringLiteral("f_rf"), QStringLiteral("2.45GHz")}, {QStringLiteral("f_if"), QStringLiteral("70MHz")}});
    const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("f_rf")}, {QStringLiteral("power"), QStringLiteral("-60")}});
    const QString bpf = c.place("rf-system:filter", {200, 0},
                                {{QStringLiteral("fc"), QStringLiteral("f_rf")}, {QStringLiteral("bw"), QStringLiteral("100MHz")},
                                 {QStringLiteral("il"), QStringLiteral("1.5")}});
    const QString lna = c.place("rf-system:amplifier", {400, 0}, {{QStringLiteral("gain"), QStringLiteral("18")}, {QStringLiteral("nf"), QStringLiteral("1")}});
    const QString mixer = c.place("rf-system:mixer", {600, 0});
    const QString lo = c.place("rf-system:lo", {600, 200}, {{QStringLiteral("freq"), QStringLiteral("f_rf - f_if")}, {QStringLiteral("power"), QStringLiteral("13")}});
    const QString lpf = c.place("rf-system:filter", {800, 0},
                                {{QStringLiteral("type"), QStringLiteral("lowpass")}, {QStringLiteral("fc"), QStringLiteral("100MHz")},
                                 {QStringLiteral("il"), QStringLiteral("0.5")}});
    const QString adc = c.place("rf-system:adc", {1000, 0}, {{QStringLiteral("fs"), QStringLiteral("100MHz")}});
    c.wire(src, "out", bpf, "in");
    c.wire(bpf, "out", lna, "in");
    c.wire(lna, "out", mixer, "rf");
    c.wire(lo, "out", mixer, "lo");
    c.wire(mixer, "if", lpf, "in");
    c.wire(lpf, "out", adc, "in");

    const BudgetRun run = c.run();
    REQUIRE(run.ok());
    const SignalChain &chain = run.chain;
    REQUIRE(chain.stages.size() == 4);
    REQUIRE(chain.endKind == QStringLiteral("adc"));
    const ChainStage &mix = chain.stages[2];
    CHECK(mix.translation == ChainStage::Translation::Mixer);
    CHECK(mix.loSource == QStringLiteral("LO1"));
    CHECK(mix.loFrequency == Approx(2.38e9));
    CHECK(mix.loPowerDbm == Approx(13));
    CHECK(mix.outputFrequency == Approx(70e6));
    CHECK(chain.stages[3].inputFrequency == Approx(70e6));

    const BudgetResult &b = run.budget;
    REQUIRE(b.stages.size() == 5); // four stages and the ADC
    CHECK(b.stages[0].gainDb == Approx(-1.5).margin(1e-6));
    CHECK(b.stages[2].gainDb == Approx(-7).margin(1e-9));
    // The 5th-order low-pass at 70 MHz of 100 MHz (Omega = 0.7) adds its roll-off to il.
    const double lowpassRolloff = 10 * std::log10(1 + std::pow(0.7, 10));
    CHECK(b.stages.back().cumulativeGainDb == Approx(-1.5 + 18 - 7 - 0.5 - lowpassRolloff).margin(1e-3));
    CHECK(b.hasAdc);
    CHECK(b.adcAliasFrequency == Approx(30e6));
    CHECK(b.adcQuantisationSnrDb == Approx(6.02 * 12 + 1.76));
    CHECK(b.adcHeadroomDb == Approx(4 - b.stages.back().signalDbm));
}

TEST_CASE("the mixer's product parameter selects sum or difference, with a warning for the likely wrong one")
{
    // An up-converter wired left to right into the rf pin: 1 GHz + 15 GHz LO,
    // followed by a 16 GHz band-pass filter (the case that motivated `product`).
    const auto build = [](Chain &c, const char *product, bool enterAtIf) {
        const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("1GHz")}});
        QMap<QString, QString> mixerParameters;
        if (product)
            mixerParameters.insert(QStringLiteral("product"), QString::fromLatin1(product));
        const QString mixer = c.place("rf-system:mixer", {200, 0}, mixerParameters);
        if (enterAtIf)
            c.sheet->mirrorItems({mixer}, QPoint(200, 0));
        const QString lo = c.place("rf-system:lo", {200, 200},
                                   {{QStringLiteral("freq"), QStringLiteral("15GHz")}, {QStringLiteral("power"), QStringLiteral("13")}});
        const QString bpf = c.place("rf-system:filter", {400, 0},
                                    {{QStringLiteral("fc"), QStringLiteral("16GHz")}, {QStringLiteral("bw"), QStringLiteral("1GHz")}});
        const QString term = c.place("rf-system:termination", {600, 0});
        c.wire(src, "out", mixer, enterAtIf ? "if" : "rf");
        c.wire(lo, "out", mixer, "lo");
        c.wire(mixer, enterAtIf ? "rf" : "if", bpf, "in");
        c.wire(bpf, "out", term, "p");
    };
    const auto warningsText = [](const BudgetRun &run) { return run.warnings.join(QLatin1Char('\n')); };

    SECTION("auto, entered at rf: the difference, and a warning pointing to the sum")
    {
        Chain c;
        build(c, nullptr, false);
        const BudgetRun run = c.run();
        REQUIRE(run.ok());
        CHECK(run.chain.stages[0].outputFrequency == Approx(14e9));
        CHECK(mixerProductName(run.chain.stages[0]) == QStringLiteral("difference"));
        CHECK(warningsText(run).contains(QStringLiteral("\"product\" to \"sum\"")));
    }
    SECTION("sum, entered at rf: 16 GHz through the filter, no warning")
    {
        Chain c;
        build(c, "sum", false);
        const BudgetRun run = c.run();
        REQUIRE(run.ok());
        CHECK(run.chain.stages[0].outputFrequency == Approx(16e9));
        CHECK(run.budget.stages.back().cumulativeGainDb == Approx(-7 - 1).margin(1e-3)); // mixer and filter il
        CHECK_FALSE(warningsText(run).contains(QStringLiteral("product")));
    }
    SECTION("auto, entered at if with side low: the sum, as before the parameter existed")
    {
        Chain c;
        build(c, nullptr, true);
        const BudgetRun run = c.run();
        REQUIRE(run.ok());
        CHECK(run.chain.stages[0].upConversion);
        CHECK(run.chain.stages[0].outputFrequency == Approx(16e9));
    }
    SECTION("difference, entered at if: overrides side")
    {
        Chain c;
        build(c, "difference", true);
        const BudgetRun run = c.run();
        REQUIRE(run.ok());
        CHECK(run.chain.stages[0].outputFrequency == Approx(14e9));
        CHECK(warningsText(run).contains(QStringLiteral("\"product\" to \"sum\"")));
    }
}

TEST_CASE("actual mismatch differs from the ideal cascade (RF-7)")
{
    // Two quarter-wave 100 ohm lines make a transparent half-wave line, but
    // each alone is mismatched to 50 ohm.
    Chain c;
    const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("1GHz")}});
    const QMap<QString, QString> line{{QStringLiteral("z0"), QStringLiteral("100")},
                                      {QStringLiteral("elen"), QStringLiteral("90")},
                                      {QStringLiteral("freq"), QStringLiteral("1GHz")}};
    const QString l1 = c.place("rf-circuit:tline", {200, 0}, line);
    const QString l2 = c.place("rf-circuit:tline", {400, 0}, line);
    const QString term = c.place("rf-system:termination", {600, 0});
    c.wire(src, "out", l1, "in");
    c.wire(l1, "out", l2, "in");
    c.wire(l2, "out", term, "p");

    BudgetAnalysisSetup setup;
    const BudgetRun actual = c.run(setup);
    REQUIRE(actual.ok());
    CHECK(actual.budget.stages.back().cumulativeGainDb == Approx(0).margin(1e-9));
    setup.mismatch = false;
    const BudgetRun ideal = c.run(setup);
    REQUIRE(ideal.ok());
    const double oneLine = 10 * std::log10(1 - 0.6 * 0.6); // |Gamma| = (200-50)/(200+50)
    CHECK(ideal.budget.stages.back().cumulativeGainDb == Approx(2 * oneLine).margin(1e-9));
    CHECK_FALSE(ideal.budget.mismatch);
}

TEST_CASE("explicit paths choose branches; problems are reported")
{
    Chain c;
    const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("1GHz")}});
    const QString spl = c.place("rf-system:splitter", {200, 0});
    const QString t1 = c.place("rf-system:termination", {400, -100});
    const QString a2 = c.place("rf-system:attenuator", {400, 100}, {{QStringLiteral("il"), QStringLiteral("10")}});
    const QString t2 = c.place("rf-system:termination", {600, 100});
    c.wire(src, "out", spl, "in");
    c.wire(spl, "out1", t1, "p");
    c.wire(spl, "out2", a2, "in");
    c.wire(a2, "out", t2, "p");

    BudgetAnalysisSetup setup;
    setup.path = {QStringLiteral("SRC1"), QStringLiteral("SPL1.out2"), QStringLiteral("AT1"), QStringLiteral("TERM2")};
    const BudgetRun run = c.run(setup);
    REQUIRE(run.ok());
    REQUIRE(run.budget.stages.size() == 2);
    CHECK(run.budget.stages.back().cumulativeGainDb == Approx(-(3.0103 + 0.5 - 0.05) - 10).margin(1e-3));

    // With an explicit start, the path may repeat it or leave it out.
    setup.start = QStringLiteral("SRC1");
    REQUIRE(c.run(setup).ok());
    setup.path.removeFirst();
    REQUIRE(c.run(setup).ok());
    setup.start.clear();

    // Unknown keys of a stored analysis survive an update.
    QJsonObject simulation = withBudgetAnalysis({}, setup);
    QJsonArray analyses = simulation.value(QStringLiteral("analyses")).toArray();
    QJsonObject stored = analyses.first().toObject();
    stored.insert(QStringLiteral("futureKey"), 42);
    analyses.replace(0, stored);
    simulation.insert(QStringLiteral("analyses"), analyses);
    BudgetAnalysisSetup edited = budgetAnalyses(simulation).first();
    edited.path.clear();
    simulation = withBudgetAnalysis(simulation, edited);
    const QJsonObject updated = simulation.value(QStringLiteral("analyses")).toArray().first().toObject();
    CHECK(updated.value(QStringLiteral("futureKey")).toInt() == 42);
    CHECK_FALSE(updated.contains(QStringLiteral("path"))); // a cleared known key is removed

    setup.path = {QStringLiteral("SRC1"), QStringLiteral("AT1")};
    CHECK_FALSE(c.run(setup).ok()); // AT1 is not next to SRC1

    // Default: the splitter's first output, with a warning.
    const BudgetRun automatic = c.run();
    REQUIRE(automatic.ok());
    CHECK(automatic.chain.end == QStringLiteral("TERM1"));
    CHECK(automatic.warnings.join(QString()).contains(QStringLiteral("SPL1")));
}

TEST_CASE("a mixer without an LO and a chain without a start are errors")
{
    Chain c;
    const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("1GHz")}});
    const QString mixer = c.place("rf-system:mixer", {200, 0});
    const QString term = c.place("rf-system:termination", {400, 0});
    c.wire(src, "out", mixer, "rf");
    c.wire(mixer, "if", term, "p");
    const BudgetRun run = c.run();
    CHECK_FALSE(run.ok());
    CHECK(run.chain.errors.join(QString()).contains(QStringLiteral("no LO")));

    Chain empty;
    empty.place("rf-system:termination", {0, 0});
    CHECK(empty.run().chain.errors.join(QString()).contains(QStringLiteral("no start")));
}

TEST_CASE("a swept budget follows the filter response and the setup round-trips")
{
    Chain c;
    const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("1GHz")}});
    const QString bpf = c.place("rf-system:filter", {200, 0}, {{QStringLiteral("il"), QStringLiteral("0")}});
    const QString term = c.place("rf-system:termination", {400, 0});
    c.wire(src, "out", bpf, "in");
    c.wire(bpf, "out", term, "p");

    BudgetAnalysisSetup setup;
    setup.sweepStart = QStringLiteral("0.8GHz");
    setup.sweepStop = QStringLiteral("1.2GHz");
    setup.sweepPoints = QStringLiteral("41");
    setup.spurs = false;
    QString id;
    c.sheet->setSimulation(withBudgetAnalysis(c.sheet->simulation(), setup, &id));
    const auto stored = budgetAnalyses(c.sheet->simulation());
    REQUIRE(stored.size() == 1);
    CHECK(stored.first().sweepPoints == QStringLiteral("41"));
    CHECK_FALSE(stored.first().spurs);
    CHECK(linearAnalyses(c.sheet->simulation()).isEmpty()); // types do not mix

    const BudgetRun run = c.run(stored.first());
    REQUIRE(run.ok());
    REQUIRE(run.sweep.size() == 41);
    CHECK(run.sweep[20].stages.back().cumulativeGainDb == Approx(0).margin(1e-6)); // centre
    CHECK(run.sweep[0].stages.back().cumulativeGainDb < -20);                     // far outside the band
}

TEST_CASE("every example's S-parameter analyses run and every plot trace has values")
{
    // LVD_RF_TEST_DATA is <source>/src/rfdata/tests/data.
    QDir examples(QStringLiteral(LVD_RF_TEST_DATA));
    REQUIRE(examples.cd(QStringLiteral("../../../../examples")));
    int analysesRun = 0;
    for (const QString &folder : examples.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir dir(examples.filePath(folder));
        const QStringList projects = dir.entryList({QStringLiteral("*.lvdproj")}, QDir::Files);
        if (projects.isEmpty() || dir.entryList({QStringLiteral("*.lvdrf")}, QDir::Files).isEmpty())
            continue;
        Session session;
        QString error;
        Project *project = session.openProject(dir.filePath(projects.first()), &error);
        INFO(folder.toStdString() << ": " << error.toStdString());
        REQUIRE(project);
        for (Document *document : project->documents()) {
            if (document->type() != DocumentType::RfSchematic || !document->schematic())
                continue;
            const QJsonObject simulation = document->schematic()->simulation();
            for (const LinearAnalysisSetup &setup : linearAnalyses(simulation)) {
                INFO(folder.toStdString() << "/" << document->name().toStdString() << " analysis "
                                          << setup.name.toStdString());
                const LinearResult result = runLinearAnalysis(*project, *document, setup);
                INFO(result.errors.join(QLatin1Char('\n')).toStdString());
                REQUIRE(result.ok());
                ++analysesRun;
                for (const QJsonValue &plotValue : simulation.value(QStringLiteral("plots")).toArray()) {
                    const QJsonObject plot = plotValue.toObject();
                    if (plot.value(QStringLiteral("analysis")).toString() != setup.id)
                        continue;
                    for (const QJsonValue &trace : plot.value(QStringLiteral("traces")).toArray()) {
                        INFO("trace " << trace.toString().toStdString());
                        const Measurement m = evaluateMeasurement(result, trace.toString());
                        INFO(m.error.toStdString());
                        REQUIRE(m.ok());
                        const std::vector<double> values = m.real();
                        CHECK(std::any_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); }));
                    }
                }
            }
        }
    }
    CHECK(analysesRun >= 4); // lowpass_filter, lna_match, upconverter, pll_receiver/front_end
}
