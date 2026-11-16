#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/RuleCheck.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "core/StandardValues.h"
#include "rfsim/Measurement.h"
#include "rfsim/Optimizer.h"

#include <QTemporaryDir>

#include <cmath>

using namespace lvd;
using namespace lvd::rf;
using Catch::Approx;

namespace {

// An L-match from a 50 ohm port to 12 - j30 ohm at 2.4 GHz:
// PORT1 - shunt C1 to ground - series L1 - R_load 12 ohm - C_load to ground.
struct MatchBench {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;

    MatchBench()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("match"), &error);
        REQUIRE(document);
        sheet = document->schematic();
        sheet->setVariables({{QStringLiteral("Lm"), QStringLiteral("1nH")}, {QStringLiteral("Cm"), QStringLiteral("1pF")}});

        const QString port = place("rf-system:port", {-300, 0});
        const QString c1 = place("basic:capacitor", {-150, 100}, {{QStringLiteral("c"), QStringLiteral("Cm")}});
        sheet->rotateItems({c1}, QPoint(-150, 100), 90);
        const QString l1 = place("basic:inductor", {0, 0}, {{QStringLiteral("l"), QStringLiteral("Lm")}});
        const QString r = place("basic:resistor", {150, 0}, {{QStringLiteral("r"), QStringLiteral("12")}});
        const double cLoad = 1.0 / (2 * 3.14159265358979323846 * 2.4e9 * 30);
        const QString cl = place("basic:capacitor", {300, 100}, {{QStringLiteral("c"), QString::number(cLoad, 'g', 15)}});
        sheet->rotateItems({cl}, QPoint(300, 100), 90);
        wire(pin(port, "p"), pin(l1, "p"));
        wire(pin(c1, "n"), pin(l1, "p"));
        wire(pin(l1, "n"), pin(r, "p"));
        wire(pin(r, "n"), QPoint(pin(cl, "n").x(), pin(r, "n").y()));
        wire(QPoint(pin(cl, "n").x(), pin(r, "n").y()), pin(cl, "n"));
        ground(c1);
        ground(cl);

        // Every pin connected: a floating part would make the optimisation meaningless.
        for (const RuleViolation &violation : checkSchematic(*project, *document)) {
            INFO(violation.message.toStdString());
            REQUIRE(violation.severity == RuleViolation::Severity::Info);
        }

        LinearAnalysisSetup analysis;
        analysis.sweep = QStringLiteral("list");
        analysis.list = {QStringLiteral("2.4GHz")};
        analysis.noise = false;
        sheet->setSimulation(withLinearAnalysis({}, analysis));
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

    QPoint pin(const QString &id, const char *name)
    {
        const PlacedComponent *component = sheet->component(id);
        const DefinitionPtr definition = project->resolve(*sheet, component->definition).definition;
        return pinPosition(*component, *definition->symbol.pin(QString::fromLatin1(name)));
    }

    void wire(QPoint a, QPoint b)
    {
        if (a.x() != b.x() && a.y() != b.y())
            sheet->addWire({a, QPoint(b.x(), a.y()), b});
        else
            sheet->addWire({a, b});
        for (const QPoint &point : missingJunctions(*sheet, project->lookupFor(*sheet)))
            sheet->addJunction(point);
    }

    void ground(const QString &shunt)
    {
        const QPoint bottom = pin(shunt, "p");
        const QString g = place("basic:ground", bottom + QPoint(0, 40));
        wire(bottom, pin(g, "gnd"));
    }

    OptimizationSetup setup(OptimizationSetup::Method method) const
    {
        OptimizationSetup s;
        s.method = method;
        s.maxEvaluations = 600;
        DesignVariable l;
        l.name = QStringLiteral("Lm");
        l.minimum = 0.5e-9;
        l.maximum = 20e-9;
        DesignVariable c;
        c.name = QStringLiteral("Cm");
        c.minimum = 0.2e-12;
        c.maximum = 10e-12;
        s.variables = {l, c};
        Goal goal;
        goal.measurement = QStringLiteral("dB(S11)");
        goal.type = Goal::Type::Below;
        goal.target = -30;
        s.goals = {goal};
        return s;
    }
};

const double Omega = 2 * 3.14159265358979323846 * 2.4e9;
// Closed-form L-match: Q = sqrt(50/12 - 1), X_series = Q*12 + 30, B_shunt = Q/50.
const double ExpectedL = (std::sqrt(50.0 / 12 - 1) * 12 + 30) / Omega;
const double ExpectedC = std::sqrt(50.0 / 12 - 1) / 50 / Omega;

} // namespace

TEST_CASE("standard value series snap as IEC 60063 says")
{
    CHECK(seriesMantissas(ESeries::E12).size() == 12);
    CHECK(seriesMantissas(ESeries::E96).size() == 96);
    CHECK(seriesMantissas(ESeries::E192)[seriesMantissas(ESeries::E192).size() - 2] == Approx(9.76));
    CHECK(snapToSeries(4.6e-9, ESeries::E12) == Approx(4.7e-9));
    CHECK(snapToSeries(4.6e-9, ESeries::E12, Rounding::Down) == Approx(3.9e-9));
    CHECK(snapToSeries(3.3e3, ESeries::E6) == Approx(3.3e3)); // already in the series
    CHECK(snapToSeries(1.04e-12, ESeries::E24) == Approx(1.0e-12));
    CHECK(snapToSeries(9.3, ESeries::E24) == Approx(9.1)); // 9.3/9.1 < 10/9.3
    CHECK(snapToSeries(9.6, ESeries::E24) == Approx(10));  // 10/9.6 < 9.6/9.1
    CHECK(snapToSeries(49.9, ESeries::E96) == Approx(49.9));
    CHECK(seriesFromName(QStringLiteral("e24")) == ESeries::E24);
}

TEST_CASE("a snapshot is independent of the project")
{
    MatchBench b;
    auto copy = b.project->snapshot();
    REQUIRE(copy->isSnapshot());
    Document *cloned = copy->document(QStringLiteral("match"));
    REQUIRE(cloned);
    REQUIRE(cloned->schematic()->components().size() == b.sheet->components().size());
    // No libraries in the copy, yet every part resolves from the frozen cache.
    REQUIRE(copy->libraryManager() == nullptr);
    for (const PlacedComponent &component : cloned->schematic()->components())
        REQUIRE(copy->resolve(*cloned->schematic(), component.definition).source == Resolution::Source::Cache);

    QString error;
    REQUIRE(applyDesignValues(*copy, *cloned, {DesignVariable{DesignVariable::Kind::Variable, QStringLiteral("Lm")}},
                              {2e-9}, &error));
    CHECK(b.sheet->variables().first().value == QStringLiteral("1nH")); // original untouched
    CHECK(cloned->schematic()->undoStack()->count() == 0);              // snapshot history cleared
    CHECK_FALSE(cloned->save(&error));
}

TEST_CASE("the optimisers find the textbook L-match")
{
    for (const auto method : {OptimizationSetup::Method::NelderMead, OptimizationSetup::Method::LevenbergMarquardt}) {
        MatchBench b;
        auto copy = b.project->snapshot();
        int progressCalls = 0;
        const OptimizationResult result = optimize(*copy, QStringLiteral("match"), b.setup(method),
                                                   [&](const OptimizationProgress &) { ++progressCalls; });
        INFO((result.errors + result.warnings).join(QLatin1Char('\n')).toStdString());
        INFO("method " << (method == OptimizationSetup::Method::NelderMead ? "Nelder-Mead" : "Levenberg-Marquardt")
                       << ", evaluations " << result.evaluations << ", cost " << result.bestCost << ", converged "
                       << result.converged << ", L " << result.best[0] << ", C " << result.best[1]);
        REQUIRE(result.ok());
        CHECK(result.goalsBest.first().met);
        CHECK(result.goalsBest.first().worst < -30);
        // |S11| < -30 dB holds in a small region around the closed-form values.
        CHECK(result.best[0] == Approx(ExpectedL).epsilon(0.06));
        CHECK(result.best[1] == Approx(ExpectedC).epsilon(0.06));
        CHECK(result.initialCost > 0);
        CHECK(progressCalls == result.evaluations);
        // The original document is untouched.
        CHECK(b.sheet->variables().first().value == QStringLiteral("1nH"));
    }
}

TEST_CASE("snapping, cancelling and errors")
{
    MatchBench b;
    OptimizationSetup setup = b.setup(OptimizationSetup::Method::NelderMead);
    setup.variables[0].series = ESeries::E12;
    setup.variables[1].series = ESeries::E12;
    auto copy = b.project->snapshot();
    const OptimizationResult snapped = optimize(*copy, QStringLiteral("match"), setup);
    REQUIRE(snapped.ok());
    CHECK(snapped.snapped[0] == Approx(snapToSeries(snapped.best[0], ESeries::E12)));
    CHECK(snapped.snapped[1] == Approx(snapToSeries(snapped.best[1], ESeries::E12)));
    CHECK(snapped.snappedCost >= snapped.bestCost);

    std::atomic_bool cancel{false};
    auto copy2 = b.project->snapshot();
    const OptimizationResult cancelled = optimize(*copy2, QStringLiteral("match"), b.setup(OptimizationSetup::Method::NelderMead),
                                                  [&](const OptimizationProgress &p) {
                                                      if (p.evaluation >= 5)
                                                          cancel = true;
                                                  },
                                                  &cancel);
    CHECK(cancelled.cancelled);
    CHECK(cancelled.evaluations <= 6);

    OptimizationSetup bad = b.setup(OptimizationSetup::Method::NelderMead);
    bad.variables[0].name = QStringLiteral("nothing");
    auto copy3 = b.project->snapshot();
    CHECK_FALSE(optimize(*copy3, QStringLiteral("match"), bad).ok());
    bad = b.setup(OptimizationSetup::Method::NelderMead);
    bad.goals.first().measurement = QStringLiteral("dB(S31)");
    auto copy4 = b.project->snapshot();
    const OptimizationResult badGoal = optimize(*copy4, QStringLiteral("match"), bad);
    CHECK_FALSE(badGoal.goalsInitial.first().error.isEmpty());
}

TEST_CASE("parameters of components can be design variables, and setups round-trip")
{
    MatchBench b;
    DesignVariable r;
    r.kind = DesignVariable::Kind::Parameter;
    r.component = QStringLiteral("R1");
    r.name = QStringLiteral("r");
    r.minimum = 1;
    r.maximum = 100;
    QString error;
    REQUIRE(currentValue(*b.project, *b.document, r, &error) == 12.0);
    auto copy = b.project->snapshot();
    LinearAnalysisSetup analysis = linearAnalyses(b.sheet->simulation()).first();
    const LinearResult at50 = evaluateDesign(*copy, QStringLiteral("match"), analysis, {r}, {50.0});
    REQUIRE(at50.ok());
    REQUIRE(currentValue(*copy, *copy->document(QStringLiteral("match")), r, &error) == 50.0);

    OptimizationSetup setup = b.setup(OptimizationSetup::Method::LevenbergMarquardt);
    setup.variables.append(r);
    setup.goals.first().bandStart = 2.3e9;
    setup.goals.first().bandStop = 2.5e9;
    setup.variables[1].series = ESeries::E24;
    QString id;
    const QJsonObject simulation = withOptimizationSetup(b.sheet->simulation(), setup, &id);
    CHECK(id == QStringLiteral("o1"));
    const auto stored = optimizationSetups(simulation);
    REQUIRE(stored.size() == 1);
    const OptimizationSetup &back = stored.first();
    CHECK(back.method == OptimizationSetup::Method::LevenbergMarquardt);
    CHECK(back.variables.size() == 3);
    CHECK(back.variables[2].kind == DesignVariable::Kind::Parameter);
    CHECK(back.variables[2].component == QStringLiteral("R1"));
    CHECK(back.variables[1].series == ESeries::E24);
    CHECK(back.goals.first().bandStart == 2.3e9);
    CHECK(linearAnalyses(simulation).size() == 1); // the analyses are kept
}
