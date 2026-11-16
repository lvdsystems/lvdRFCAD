#include "core/Connectivity.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "spice/NgspiceEngine.h"
#include "spice/RawFile.h"
#include "spice/SpiceBatch.h"
#include "spice/SpiceProcessPool.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>

#include <cmath>

using namespace lvd;
using namespace lvd::spice;
using Catch::Approx;

namespace {

// V1 (10 V step) - R1 - out - R2 to ground, C1 across R2.
struct Circuit {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;

    Circuit()
    {
        NgspiceEngine::setRuntimeFolder(QStringLiteral(LVD_NGSPICE_RUNTIME));
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::SpiceSchematic, QStringLiteral("rc"), &error);
        REQUIRE(document);
        sheet = document->schematic();
        sheet->setVariables({{QStringLiteral("rload"), QStringLiteral("1k")}});
        const QString v1 = place("spice-sources:vpulse", {0, 0},
                                 {{"v1", "0"}, {"v2", "10"}, {"td", "0"}, {"tr", "1n"}, {"tf", "1n"}, {"pw", "1"}, {"per", "2"}});
        const QString r1 = place("basic:resistor", {200, 0}, {{"r", "1k"}});
        const QString r2 = place("basic:resistor", {400, 0}, {{"r", "rload"}});
        const QString c1 = place("basic:capacitor", {600, 0}, {{"c", "1u"}});
        label(v1, "p", "in");
        label(r1, "p", "in");
        label(r1, "n", "out");
        label(r2, "p", "out");
        label(c1, "p", "out");
        for (const QString &id : {v1, r2, c1})
            label(id, "n", "0");
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

    // Ground through a ground symbol on the pin; other nets through labels.
    void label(const QString &id, const char *pin, const char *net)
    {
        const PlacedComponent *component = sheet->component(id);
        const DefinitionPtr definition = project->resolve(*sheet, component->definition).definition;
        const QPoint at = pinPosition(*component, *definition->symbol.pin(QString::fromLatin1(pin)));
        if (QByteArray(net) == "0") {
            const DefinitionPtr ground = project->resolve(*sheet, QStringLiteral("basic:ground")).definition;
            const QString g = sheet->addComponent(QStringLiteral("basic:ground"), ground.get(), at + QPoint(0, 60));
            const PlacedComponent *gc = sheet->component(g);
            sheet->addWire({at, pinPosition(*gc, *ground->symbol.pin(QStringLiteral("gnd")))});
        } else {
            sheet->addLabel(QString::fromLatin1(net), at);
        }
    }
};

SpiceMeasure measure(const char *expression, const char *function = "value", const char *at = "")
{
    SpiceMeasure m;
    m.expression = QString::fromLatin1(expression);
    m.function = QString::fromLatin1(function);
    m.at = QString::fromLatin1(at);
    return m;
}

} // namespace

TEST_CASE("measures reduce a run to numbers")
{
    Circuit circuit;
    SpiceAnalysisSetup tran = SpiceAnalysisSetup::create(AnalysisKind::Transient);
    tran.set(QStringLiteral("step"), QStringLiteral("2u"));
    tran.set(QStringLiteral("stop"), QStringLiteral("5m"));
    tran.set(QStringLiteral("maxStep"), QStringLiteral("2u"));
    const SpiceSimulation sim = runSpiceAnalysis(*circuit.project, *circuit.document, tran, defaultEngine());
    INFO(sim.errors.join(QLatin1Char('\n')).toStdString());
    REQUIRE(sim.ok());
    // Thevenin: 5 V final value, tau = 500 ohm * 1 uF.
    const double tau = 500e-6;
    QString error;
    CHECK(*evaluateMeasure(measure("v(out)", "max"), sim) == Approx(5.0).epsilon(1e-3));
    CHECK(*evaluateMeasure(measure("v(out)", "value", "0.5m"), sim) == Approx(5.0 * (1 - std::exp(-1.0))).epsilon(2e-3));
    CHECK(*evaluateMeasure(measure("v(out)", "risetime"), sim, {}, &error) == Approx(tau * std::log(9.0)).epsilon(5e-3));
    SpiceMeasure crossing = measure("v(out)", "crossing");
    crossing.level = QStringLiteral("2.5");
    CHECK(*evaluateMeasure(crossing, sim) == Approx(tau * std::log(2.0)).epsilon(5e-3));
    CHECK_FALSE(evaluateMeasure(measure("v(nowhere)", "max"), sim, {}, &error));
    CHECK(error.contains(QStringLiteral("nowhere")));
    CHECK_FALSE(evaluateMeasure(measure("v(out)", "bogus"), sim, {}, &error));
}

TEST_CASE("parameter and temperature sweeps")
{
    Circuit circuit;
    const SpiceAnalysisSetup op = SpiceAnalysisSetup::create(AnalysisKind::OperatingPoint);
    SweepAxis axis;
    axis.target.text = QStringLiteral("R2.r");
    axis.mode = QStringLiteral("list");
    axis.list = {QStringLiteral("1k"), QStringLiteral("3k"), QStringLiteral("9k")};
    auto snapshot = circuit.project->snapshot();
    // The pulse source is at v1 = 0 at the operating point: use the variable
    // sweep on a DC operating point of the step's end value instead.
    circuit.sheet->setParameter(circuit.sheet->componentByReference(QStringLiteral("V1"))->id, QStringLiteral("v1"),
                                QStringLiteral("10"));
    snapshot = circuit.project->snapshot();
    const BatchResult sweep = runParameterSweep(*snapshot, QStringLiteral("rc"), op, {axis}, {measure("v(out)")});
    INFO(sweep.errors.join(QLatin1Char('\n')).toStdString());
    REQUIRE(sweep.ok());
    REQUIRE(sweep.runs.size() == 3);
    CHECK(sweep.runs.at(0).label == QLatin1String("R2.r = 1k"));
    CHECK(sweep.runs.at(0).measures.at(0) == Approx(5.0));
    CHECK(sweep.runs.at(1).measures.at(0) == Approx(7.5));
    CHECK(sweep.runs.at(2).measures.at(0) == Approx(9.0));

    // A variable, nested with the temperature.
    SweepAxis variable;
    variable.target.text = QStringLiteral("rload");
    variable.start = QStringLiteral("1k");
    variable.stop = QStringLiteral("2k");
    variable.points = QStringLiteral("2");
    SweepAxis temperature;
    temperature.target.text = QStringLiteral("temperature");
    temperature.mode = QStringLiteral("list");
    temperature.list = {QStringLiteral("-40"), QStringLiteral("85")};
    snapshot = circuit.project->snapshot(); // the first sweep left R2.r at 9k
    const BatchResult nested = runParameterSweep(*snapshot, QStringLiteral("rc"), op, {variable, temperature}, {measure("v(out)")});
    REQUIRE(nested.ok());
    REQUIRE(nested.runs.size() == 4);
    CHECK(nested.runs.at(3).values == std::vector<double>{2000.0, 85.0});
    CHECK(nested.runs.at(3).simulation->netlist.contains(QStringLiteral(".temp 85")));
    CHECK(nested.runs.at(3).measures.at(0) == Approx(10.0 * 2.0 / 3.0));

    SweepAxis bad;
    bad.target.text = QStringLiteral("R9.r");
    bad.mode = QStringLiteral("list");
    bad.list = {QStringLiteral("1")};
    CHECK_FALSE(runParameterSweep(*snapshot, QStringLiteral("rc"), op, {bad}, {}).ok());
}

TEST_CASE("Monte Carlo, worst case and optimisation of a divider")
{
    Circuit circuit;
    circuit.sheet->setParameter(circuit.sheet->componentByReference(QStringLiteral("V1"))->id, QStringLiteral("v1"),
                                QStringLiteral("10"));
    const SpiceAnalysisSetup op = SpiceAnalysisSetup::create(AnalysisKind::OperatingPoint);
    Tolerance tolerance;
    tolerance.target.text = QStringLiteral("R*.r");
    tolerance.relative = 0.05;
    tolerance.distribution = Tolerance::Distribution::Uniform;

    SECTION("Monte Carlo")
    {
        MonteCarloSetup setup;
        setup.tolerances = {tolerance};
        setup.runs = 40;
        setup.seed = 7;
        setup.measures = {measure("v(out)")};
        auto snapshot = circuit.project->snapshot();
        const BatchResult mc = runMonteCarlo(*snapshot, QStringLiteral("rc"), op, setup);
        INFO(mc.errors.join(QLatin1Char('\n')).toStdString());
        REQUIRE(mc.ok());
        REQUIRE(mc.runs.size() == 41);
        REQUIRE(mc.targets.size() == 2); // R1.r and R2.r
        const QList<MeasureStatistics> stats = statistics(mc);
        REQUIRE(stats.size() == 1);
        CHECK(stats.first().nominal == Approx(5.0));
        CHECK(stats.first().count == 41);
        // Uniform ±5 % on both: v(out) stays within the corners 10*0.95/(1.05+0.95) .. 10*1.05/2.
        CHECK(stats.first().minimum >= 10 * 0.95 / 2.0 - 1e-9);
        CHECK(stats.first().maximum <= 10 * 1.05 / 2.0 + 1e-9);
        CHECK(stats.first().sigma > 0.01);
        CHECK(std::abs(stats.first().mean - 5.0) < 0.1);
        for (const BatchRun &run : mc.runs)
            for (std::size_t i = 0; i < run.values.size(); ++i)
                CHECK(std::abs(run.values[i] / 1000.0 - 1.0) <= 0.05 + 1e-12);

        // The same seed gives the same runs; worker processes give the same
        // results as the engine in this process.
        auto again = circuit.project->snapshot();
        SpiceProcessPool pool(3);
        pool.setProgram(QStringLiteral(LVD_CLI));
        BatchOptions parallel;
        parallel.execute = [&](QList<SpiceSimulation *> &simulations, const std::atomic_bool *cancel,
                               const std::function<void(int)> &progress) { pool.execute(simulations, cancel, progress); };
        int lastProgress = 0;
        parallel.progress = [&](const BatchProgress &p) { lastProgress = p.done; };
        const BatchResult pooled = runMonteCarlo(*again, QStringLiteral("rc"), op, setup, parallel);
        INFO(pooled.warnings.join(QLatin1Char('\n')).toStdString());
        REQUIRE(pooled.ok());
        REQUIRE(pooled.runs.size() == mc.runs.size());
        CHECK(lastProgress == 41);
        for (qsizetype i = 0; i < mc.runs.size(); ++i) {
            CHECK(pooled.runs.at(i).values == mc.runs.at(i).values);
            CHECK(pooled.runs.at(i).measures.at(0) == Approx(mc.runs.at(i).measures.at(0)).epsilon(1e-12));
        }
    }

    SECTION("worst case")
    {
        auto snapshot = circuit.project->snapshot();
        QList<WorstCaseMeasure> summary;
        const BatchResult wc = runWorstCase(*snapshot, QStringLiteral("rc"), op, {tolerance}, {measure("v(out)")}, &summary);
        INFO(wc.errors.join(QLatin1Char('\n')).toStdString());
        REQUIRE(wc.ok());
        REQUIRE(summary.size() == 1);
        CHECK(summary.first().nominal == Approx(5.0));
        CHECK(summary.first().minimum == Approx(10 * 0.95 / (1.05 + 0.95)));
        CHECK(summary.first().maximum == Approx(10 * 1.05 / (1.05 + 0.95)));
        CHECK(summary.first().maximumCorner == std::vector<int>{-1, +1}); // R1 low, R2 high
        CHECK(wc.runs.size() == 1 + 2 + 2);
    }

    SECTION("optimisation with standard values")
    {
        SpiceOptimizationSetup setup;
        SpiceOptimizationVariable r2;
        r2.target.text = QStringLiteral("R2.r");
        r2.minimum = 100;
        r2.maximum = 100e3;
        r2.series = ESeries::E24;
        setup.variables = {r2};
        SpiceGoal goal;
        goal.measure = measure("v(out)");
        goal.type = SpiceGoal::Type::Equal;
        goal.target = 7.5; // R2 = 3k
        setup.goals = {goal};
        auto snapshot = circuit.project->snapshot();
        const SpiceOptimizationResult result = optimizeSpice(*snapshot, QStringLiteral("rc"), op, setup);
        INFO(result.errors.join(QLatin1Char('\n')).toStdString());
        REQUIRE(result.ok());
        CHECK(result.best.at(0) == Approx(3000).epsilon(0.01));
        CHECK(result.snapped.at(0) == Approx(3000));
        CHECK(result.bestMeasures.at(0) == Approx(7.5).epsilon(1e-6));
        CHECK(result.goalsMet == std::vector<bool>{true});
        CHECK(result.evaluations < 200);
    }
}

TEST_CASE("raw files written here read back exactly")
{
    SpicePlot tran;
    tran.name = QStringLiteral("tran1");
    tran.type = QStringLiteral("tran");
    tran.scaleName = QStringLiteral("time");
    tran.vectors.push_back({QStringLiteral("time"), VectorKind::Time, false, {0, 1e-3, 2e-3}, {}});
    tran.vectors.push_back({QStringLiteral("v(out)"), VectorKind::Voltage, false, {0, 0.5, 0.75}, {}});
    tran.vectors.push_back({QStringLiteral("i(vv1)"), VectorKind::Current, false, {0, -1e-3, -2e-3}, {}});
    SpicePlot ac;
    ac.name = QStringLiteral("ac1");
    ac.type = QStringLiteral("ac");
    ac.scaleName = QStringLiteral("frequency");
    ac.vectors.push_back({QStringLiteral("frequency"), VectorKind::Frequency, false, {1, 10}, {}});
    ac.vectors.push_back({QStringLiteral("v(out)"), VectorKind::Voltage, true, {}, {{1, -0.5}, {0.25, 0.125}}});
    QString error;
    const auto plots = parseRawData(rawData({tran, ac}), &error);
    INFO(error.toStdString());
    REQUIRE(plots);
    REQUIRE(plots->size() == 2);
    CHECK(plots->at(0).name == QLatin1String("tran1"));
    CHECK(plots->at(0).vector(QStringLiteral("i(vv1)"))->real == std::vector<double>{0, -1e-3, -2e-3});
    CHECK(plots->at(1).scale()->real == std::vector<double>{1, 10});
    CHECK(plots->at(1).vector(QStringLiteral("v(out)"))->complex.at(1) == std::complex<double>(0.25, 0.125));
}
