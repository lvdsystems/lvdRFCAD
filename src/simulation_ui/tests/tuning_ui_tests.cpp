// Tuning (RF-11) and optimisation (RF-12) in the simulation user interface.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "plot/PlotWidget.h"
#include "rfsim/Analysis.h"
#include "rfsim/Measurement.h"
#include "rfsim/Optimizer.h"
#include "simulation_ui/DesignVariables.h"
#include "simulation_ui/OptimizationDialog.h"
#include "simulation_ui/OptimizationRunner.h"
#include "simulation_ui/PlotDefinitions.h"
#include "simulation_ui/PlotPane.h"
#include "simulation_ui/SimulationView.h"
#include "simulation_ui/TuningPanel.h"
#include "simulation_ui/TuningRunner.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <cstdio>

using namespace lvd;
using namespace lvd::rf;
using Catch::Approx;

namespace {

struct TuneBench {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;

    TuneBench()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("dut"), &error);
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
    QPoint pin(const QString &id, const char *name)
    {
        const PlacedComponent *component = sheet->component(id);
        const DefinitionPtr definition = project->resolve(*sheet, component->definition).definition;
        return pinPosition(*component, *definition->symbol.pin(QString::fromLatin1(name)));
    }
    void connect(const QString &a, const char *pinA, const QString &b, const char *pinB)
    {
        sheet->addWire({pin(a, pinA), pin(b, pinB)});
    }
    QString port(int number, QPoint at)
    {
        return place("rf-system:port", at, {{QStringLiteral("num"), QString::number(number)}, {QStringLiteral("z0"), QStringLiteral("50")}});
    }
    void ground(const QString &id, const char *pinName)
    {
        const QString g = place("basic:ground", pin(id, pinName) + QPoint(0, 60));
        connect(id, pinName, g, "gnd");
    }

    // Third-order Butterworth LC low-pass, 1 GHz: port 1 - L1 - C1 to ground - L2 - port 2.
    void lowPass()
    {
        const QString p1 = port(1, {-400, 0}), p2 = port(2, {400, 0});
        const QString l1 = place("basic:inductor", {-200, 0}, {{QStringLiteral("l"), QStringLiteral("7.96nH")}});
        const QString l2 = place("basic:inductor", {200, 0}, {{QStringLiteral("l"), QStringLiteral("7.96nH")}});
        const QString cap = place("basic:capacitor", {0, 100}, {{QStringLiteral("c"), QStringLiteral("6.37pF")}});
        sheet->rotateItems({cap}, QPoint(0, 100), 90);
        connect(p1, "p", l1, "p");
        const QPoint node(pin(cap, "n").x(), pin(l1, "n").y());
        sheet->addWire({pin(l1, "n"), node, pin(cap, "n")});
        sheet->addWire({node, pin(l2, "p")});
        sheet->addJunction(node);
        ground(cap, "p");
        connect(l2, "n", p2, "p");
    }

    // A 50 ohm port, a shunt C1 and a series L1 in front of a load of
    // 12 - j30 ohm at 2.4 GHz (R1 = 12 ohm in series with C2 = 2.21 pF to ground).
    void matchingProblem()
    {
        const QString p1 = port(1, {-400, 0});
        const QString shunt = place("basic:capacitor", {-200, 100}, {{QStringLiteral("c"), QStringLiteral("1.5pF")}});
        sheet->rotateItems({shunt}, QPoint(-200, 100), 90);
        const QString series = place("basic:inductor", {0, 0}, {{QStringLiteral("l"), QStringLiteral("2nH")}});
        const QString r = place("basic:resistor", {200, 0}, {{QStringLiteral("r"), QStringLiteral("12")}});
        const QString load = place("basic:capacitor", {400, 100}, {{QStringLiteral("c"), QStringLiteral("2.2105pF")}});
        sheet->rotateItems({load}, QPoint(400, 100), 90);
        connect(p1, "p", series, "p");
        connect(p1, "p", shunt, "n");
        ground(shunt, "p");
        connect(series, "n", r, "p");
        connect(r, "n", load, "n");
        ground(load, "p");
    }

    QString addAnalysis(const QString &start, const QString &stop, const QString &points, bool noise = false)
    {
        LinearAnalysisSetup setup;
        setup.name = QStringLiteral("Sweep");
        setup.start = start;
        setup.stop = stop;
        setup.points = points;
        setup.noise = noise;
        QString id;
        QJsonObject simulation = withLinearAnalysis(sheet->simulation(), setup, &id);
        for (const sim::PlotDefinition &plot : sim::defaultPlots(id, {1, 2}, noise))
            simulation = sim::withPlot(simulation, plot);
        sheet->setSimulation(simulation);
        return id;
    }

    LinearAnalysisSetup analysis(const QString &id) const
    {
        for (const LinearAnalysisSetup &setup : linearAnalyses(sheet->simulation())) {
            if (setup.id == id)
                return setup;
        }
        FAIL("analysis not found");
        return {};
    }

    QString parameter(const char *reference, const char *name) const
    {
        const PlacedComponent *component = sheet->componentByReference(QString::fromLatin1(reference));
        REQUIRE(component);
        return component->parameters.value(QString::fromLatin1(name));
    }

    double value(const DesignVariable &variable) const
    {
        const auto v = currentValue(*project, *document, variable);
        REQUIRE(v);
        return *v;
    }
};

DesignVariable parameterVariable(const char *component, const char *name)
{
    DesignVariable variable;
    variable.kind = DesignVariable::Kind::Parameter;
    variable.component = QString::fromLatin1(component);
    variable.name = QString::fromLatin1(name);
    return variable;
}

bool runView(SimulationView &view)
{
    QSignalSpy spy(&view, &SimulationView::runFinished);
    view.run();
    if (spy.isEmpty() && !spy.wait(20000))
        return false;
    QApplication::processEvents();
    return !view.isRunning() && spy.first().at(0).toBool();
}

bool waitForTuning(SimulationView &view, int timeoutMs = 20000)
{
    return QTest::qWaitFor([&] { return !view.isTuningBusy() && view.tuningResultCount() > 0; }, timeoutMs);
}

double measure(const LinearResult &result, const QString &expression, double frequency)
{
    const Measurement m = evaluateMeasurement(result, expression);
    REQUIRE(m.ok());
    size_t best = 0;
    for (size_t i = 0; i < m.frequencies.size(); ++i) {
        if (std::abs(m.frequencies[i] - frequency) < std::abs(m.frequencies[best] - frequency))
            best = i;
    }
    return m.values[best].real();
}

// The plotted value of a trace at a frequency, read with a marker.
double plotted(sim::PlotPane *pane, const QString &expression, double frequency)
{
    const int trace = pane->traceId(expression);
    REQUIRE(trace >= 0);
    const int marker = pane->plot()->addMarker(trace, frequency);
    const auto info = pane->plot()->marker(marker);
    REQUIRE(info);
    const double y = info->y;
    pane->plot()->removeMarker(marker);
    return y;
}

} // namespace

TEST_CASE("design candidates list variables and numeric parameters with default ranges")
{
    TuneBench b;
    b.lowPass();
    b.sheet->setVariables({{QStringLiteral("f0"), QStringLiteral("1GHz")}});
    const QList<sim::DesignCandidate> candidates = sim::designCandidates(*b.project, *b.document);
    REQUIRE_FALSE(candidates.isEmpty());
    CHECK(candidates.first().variable.kind == DesignVariable::Kind::Variable);
    CHECK(candidates.first().variable.name == QStringLiteral("f0"));
    bool foundC = false;
    for (const sim::DesignCandidate &candidate : candidates) {
        if (candidate.title == QStringLiteral("C1.c")) {
            foundC = true;
            CHECK(candidate.value == Approx(6.37e-12));
            CHECK(candidate.variable.minimum == Approx(6.37e-12 / 2));
            CHECK(candidate.variable.maximum == Approx(6.37e-12 * 2));
            CHECK(candidate.variable.logarithmic);
            CHECK(candidate.variable.unit == QStringLiteral("F"));
        }
    }
    CHECK(foundC);
    CHECK(sim::parseValue(QStringLiteral("4.7pF")).value_or(0) == Approx(4.7e-12));
    CHECK(sim::parseValue(QStringLiteral("2*2.2p")).value_or(0) == Approx(4.4e-12));
    CHECK_FALSE(sim::parseValue(QStringLiteral("f0*2")));
}

TEST_CASE("tuning a capacitor updates the plots on a snapshot; apply is one undo step, reset restores")
{
    TuneBench b;
    b.lowPass();
    b.addAnalysis(QStringLiteral("10MHz"), QStringLiteral("3GHz"), QStringLiteral("300"));
    SimulationView view(b.project, b.document);
    view.resize(1300, 800);
    REQUIRE(runView(view));
    sim::PlotPane *pane = view.plotPanes().value(0);
    REQUIRE(pane);
    const QString s21 = QStringLiteral("dB(S21)");
    const double original = plotted(pane, s21, 1.5e9);
    const QString originalText = b.parameter("C1", "c");
    const int undoBefore = b.sheet->undoStack()->count();

    view.startTuning();
    REQUIRE(view.isTuning());
    REQUIRE(view.tuningPanel()->isVisibleTo(&view));
    REQUIRE(view.tuneAction()->isChecked());
    sim::TuningPanel *panel = view.tuningPanel();
    REQUIRE(panel->addVariable(parameterVariable("C1", "c")) == 0);
    CHECK(panel->value(0) == Approx(6.37e-12));
    CHECK(panel->slider(0)->value() == sim::TuningPanel::SliderSteps / 2); // the middle of value/2 .. value*2 (log)
    REQUIRE(waitForTuning(view));

    // Doubling C moves the cut-off down: |S21| at 1.5 GHz drops.
    panel->setValue(0, 12.74e-12);
    CHECK(view.isTuningBusy());
    REQUIRE(waitForTuning(view));
    REQUIRE(view.lastTunedValues() == std::vector<double>{12.74e-12});
    const double tuned = plotted(pane, s21, 1.5e9);
    CHECK(tuned < original - 3);
    REQUIRE(view.lastResult());
    CHECK(measure(*view.lastResult(), s21, 1.5e9) == Approx(tuned).margin(0.5));
    // The document is untouched.
    CHECK(b.parameter("C1", "c") == originalText);
    CHECK(b.sheet->undoStack()->count() == undoBefore);
    CHECK(panel->isModified());
    CHECK(panel->applyButton()->isEnabled());

    // Reset brings back the document's response.
    view.resetTuning();
    REQUIRE(waitForTuning(view));
    CHECK(panel->value(0) == Approx(6.37e-12));
    CHECK(plotted(pane, s21, 1.5e9) == Approx(original).margin(1e-6));
    CHECK_FALSE(panel->isModified());

    // The slider, as a user drags it: the value follows on a log scale.
    panel->slider(0)->setValue(sim::TuningPanel::SliderSteps);
    CHECK(panel->value(0) == Approx(12.74e-12));
    panel->setValue(0, 9e-12);
    REQUIRE(waitForTuning(view));
    const double beforeApply = plotted(pane, s21, 1.5e9);

    // Apply: one undo step, the value in the document, the plot unchanged.
    QString error;
    REQUIRE(view.applyTuning(&error));
    CHECK(b.sheet->undoStack()->count() == undoBefore + 1);
    CHECK(b.value(parameterVariable("C1", "c")) == Approx(9e-12));
    CHECK_FALSE(panel->isModified());
    REQUIRE(waitForTuning(view)); // the snapshot follows the edit
    CHECK(plotted(pane, s21, 1.5e9) == Approx(beforeApply).margin(1e-6));
    CHECK_FALSE(view.isOutdated());

    // Closing keeps the applied result; undo restores the old text.
    view.stopTuning();
    CHECK_FALSE(view.isTuning());
    CHECK_FALSE(view.tuningPanel()->isVisibleTo(&view));
    CHECK(plotted(pane, s21, 1.5e9) == Approx(beforeApply).margin(1e-6));
    b.sheet->undoStack()->undo();
    CHECK(b.parameter("C1", "c") == originalText);
}

TEST_CASE("closing tuning without applying discards the tuned values")
{
    TuneBench b;
    b.lowPass();
    b.addAnalysis(QStringLiteral("10MHz"), QStringLiteral("3GHz"), QStringLiteral("101"));
    SimulationView view(b.project, b.document);
    REQUIRE(runView(view));
    sim::PlotPane *pane = view.plotPanes().value(0);
    const double original = plotted(pane, QStringLiteral("dB(S21)"), 1.5e9);
    view.tuneAction()->trigger(); // as from the toolbar
    REQUIRE(view.isTuning());
    REQUIRE(view.tuningPanel()->addVariable(parameterVariable("L1", "l")) == 0);
    view.tuningPanel()->setValue(0, 3e-9);
    REQUIRE(waitForTuning(view));
    CHECK(plotted(pane, QStringLiteral("dB(S21)"), 1.5e9) != Approx(original).margin(0.01));
    view.tuneAction()->trigger();
    CHECK_FALSE(view.isTuning());
    CHECK(plotted(pane, QStringLiteral("dB(S21)"), 1.5e9) == Approx(original).margin(1e-6));
    CHECK(b.parameter("L1", "l") == QStringLiteral("7.96nH"));
    // Opening again starts from the document values.
    view.startTuning();
    REQUIRE(view.tuningPanel()->count() == 1);
    CHECK(view.tuningPanel()->value(0) == Approx(7.96e-9));
    CHECK_FALSE(view.tuningPanel()->isModified());
    view.stopTuning();
}

TEST_CASE("rapid slider moves are coalesced and the last value is shown")
{
    TuneBench b;
    b.lowPass();
    b.addAnalysis(QStringLiteral("10MHz"), QStringLiteral("3GHz"), QStringLiteral("1001"));
    SimulationView view(b.project, b.document);
    view.resize(1300, 800);
    view.show();
    REQUIRE(runView(view));
    view.startTuning();
    sim::TuningPanel *panel = view.tuningPanel();
    REQUIRE(panel->addVariable(parameterVariable("C1", "c")) == 0);
    REQUIRE(panel->addVariable(parameterVariable("L2", "l")) == 1);
    REQUIRE(waitForTuning(view));
    const int before = view.tuningResultCount();

    // A burst of moves with no event processing in between: one pending request at most.
    for (int position = 0; position <= sim::TuningPanel::SliderSteps; position += 5)
        panel->setSliderPosition(0, position);
    panel->setSliderPosition(1, 200);
    REQUIRE(waitForTuning(view));
    CHECK(view.tuningResultCount() - before <= 3);
    const std::vector<double> final = panel->values();
    REQUIRE(view.lastTunedValues() == final);

    // The plotted result is that of the final values.
    auto snapshot = b.project->snapshot();
    const LinearResult reference = evaluateDesign(*snapshot, b.document->name(), b.analysis(view.currentAnalysisId()),
                                                  panel->variables(), final);
    REQUIRE(reference.ok());
    sim::PlotPane *pane = view.plotPanes().value(0);
    for (int k : {150, 400, 830}) {
        const double f = 10e6 + k * (3e9 - 10e6) / 1000; // on the sweep grid: no interpolation
        CHECK(plotted(pane, QStringLiteral("dB(S21)"), f) == Approx(measure(reference, QStringLiteral("dB(S21)"), f)).margin(1e-6));
    }

    // Interactive rate: move the slider continuously for a second, letting
    // the event loop run between moves as a user's drag does.
    const int start = view.tuningResultCount();
    QElapsedTimer clock;
    clock.start();
    int position = 0, moves = 0;
    while (clock.elapsed() < 1000) {
        position = (position + 7) % (sim::TuningPanel::SliderSteps + 1);
        panel->setSliderPosition(0, position);
        ++moves;
        QTest::qWait(10);
    }
    REQUIRE(waitForTuning(view));
    const double seconds = static_cast<double>(clock.elapsed()) / 1000.0;
    const double rate = (view.tuningResultCount() - start) / seconds;
    std::printf("tuning, 3rd-order low-pass, 1001 points: %d moves, %.1f updates/s, %.2f ms per simulation\n", moves,
                rate, view.lastTuningMs());
    CHECK(rate >= 10.0);
    CHECK(view.lastTunedValues() == panel->values());
    view.stopTuning();
}

TEST_CASE("editing the real circuit while tuning refreshes the snapshot and keeps the tuned values")
{
    TuneBench b;
    b.lowPass();
    const QString id = b.addAnalysis(QStringLiteral("10MHz"), QStringLiteral("3GHz"), QStringLiteral("201"));
    SimulationView view(b.project, b.document);
    REQUIRE(runView(view));
    view.startTuning();
    sim::TuningPanel *panel = view.tuningPanel();
    REQUIRE(panel->addVariable(parameterVariable("C1", "c")) == 0);
    panel->setValue(0, 10e-12);
    REQUIRE(waitForTuning(view));
    const int before = view.tuningResultCount();

    b.sheet->setParameter(b.sheet->componentByReference(QStringLiteral("L2"))->id, QStringLiteral("l"), QStringLiteral("4nH"));
    REQUIRE(QTest::qWaitFor([&] { return view.tuningResultCount() > before && !view.isTuningBusy(); }, 10000));
    CHECK(panel->value(0) == Approx(10e-12)); // kept
    auto snapshot = b.project->snapshot();
    const LinearResult reference =
        evaluateDesign(*snapshot, b.document->name(), b.analysis(id), panel->variables(), panel->values());
    REQUIRE(view.lastResult());
    CHECK(measure(*view.lastResult(), QStringLiteral("dB(S21)"), 2e9)
          == Approx(measure(reference, QStringLiteral("dB(S21)"), 2e9)).margin(1e-9));

    // A tuned component that disappears is dropped.
    b.sheet->removeItem(b.sheet->componentByReference(QStringLiteral("C1"))->id);
    REQUIRE(QTest::qWaitFor([&] { return panel->count() == 0; }, 5000));
    view.stopTuning();
}

TEST_CASE("the tuning runner keeps only the newest request")
{
    TuneBench b;
    b.lowPass();
    const QString id = b.addAnalysis(QStringLiteral("10MHz"), QStringLiteral("3GHz"), QStringLiteral("2001"));
    sim::TuningRunner runner;
    QList<quint64> delivered;
    std::vector<double> last;
    QObject::connect(&runner, &sim::TuningRunner::evaluated, &runner,
                     [&](std::shared_ptr<const LinearResult> result, quint64 serial, std::vector<double> values, double) {
                         CHECK(result->ok());
                         delivered.append(serial);
                         last = values;
                     });
    runner.setSnapshot(std::shared_ptr<Project>(b.project->snapshot().release()), b.document->name());
    const QList<DesignVariable> variables{parameterVariable("C1", "c")};
    quint64 newest = 0;
    for (int i = 1; i <= 50; ++i)
        newest = runner.request(b.analysis(id), variables, {i * 1e-12});
    CHECK(runner.isBusy());
    REQUIRE(QTest::qWaitFor([&] { return !runner.isBusy(); }, 20000));
    REQUIRE_FALSE(delivered.isEmpty());
    CHECK(delivered.size() <= 2);
    CHECK(delivered.back() == newest);
    CHECK(last == std::vector<double>{50e-12});
}

TEST_CASE("the optimisation dialog matches a load in the background and applies as one undo step")
{
    TuneBench b;
    b.matchingProblem();
    const QString analysis = b.addAnalysis(QStringLiteral("2.3GHz"), QStringLiteral("2.5GHz"), QStringLiteral("21"));
    SimulationView view(b.project, b.document);
    view.resize(1300, 800);
    REQUIRE(runView(view));
    const double initialMatch = measure(*view.lastResult(), QStringLiteral("dB(S11)"), 2.4e9);
    CHECK(initialMatch > -10);

    OptimizationDialog *dialog = view.openOptimization();
    REQUIRE(dialog);
    CHECK(view.openOptimization() == dialog); // one dialog per view
    DesignVariable l = parameterVariable("L1", "l");
    l.minimum = 0.5e-9;
    l.maximum = 10e-9;
    DesignVariable c = parameterVariable("C1", "c");
    c.minimum = 0.2e-12;
    c.maximum = 10e-12;
    c.series = ESeries::E24;
    REQUIRE(dialog->addVariable(l));
    REQUIRE(dialog->addVariable(c));
    CHECK_FALSE(dialog->addVariable(l)); // no duplicates
    Goal goal;
    goal.measurement = QStringLiteral("dB(S11)");
    goal.type = Goal::Type::Below;
    goal.target = -20;
    goal.bandStart = 2.395e9;
    goal.bandStop = 2.405e9;
    dialog->addGoal(goal);
    dialog->evaluationsSpin()->setValue(400);
    CHECK(dialog->validate().isEmpty());
    CHECK(dialog->analysisCombo()->currentData().toString() == analysis);

    const int undoBefore = b.sheet->undoStack()->count();
    QSignalSpy finished(dialog, &OptimizationDialog::finished);
    REQUIRE(dialog->start());
    CHECK(dialog->isRunning()); // runs in the background
    CHECK(b.sheet->undoStack()->count() == undoBefore + 1); // the setup was stored
    REQUIRE(finished.wait(60000));
    REQUIRE(finished.first().at(0).toBool());
    const auto result = dialog->result();
    REQUIRE(result);
    REQUIRE(result->ok());
    REQUIRE(result->goalsBest.size() == 1);
    CHECK(result->goalsBest.first().met);
    CHECK(result->goalsBest.first().worst < -20);
    CHECK(result->snapped[1] == Approx(snapToSeries(result->best[1], ESeries::E24)));
    // The document is untouched until applied.
    CHECK(b.parameter("L1", "l") == QStringLiteral("2nH"));
    // Progress and results are shown.
    CHECK(dialog->bestTable()->rowCount() == 2);
    CHECK(dialog->goalStatusTable()->rowCount() == 1);
    CHECK(dialog->resultsTable()->rowCount() == 2 + 1 + 1);
    CHECK_FALSE(dialog->costPlot()->csvText().isEmpty());
    CHECK(dialog->applyButton()->isEnabled());
    CHECK(dialog->applyChoice()->currentData().toBool()); // snapped preselected with a series

    // Apply the best values: one undo step, the analysis runs again.
    QSignalSpy ran(&view, &SimulationView::runFinished);
    REQUIRE(dialog->apply(false));
    CHECK(b.sheet->undoStack()->count() == undoBefore + 2);
    CHECK(b.value(l) == Approx(result->best[0]).epsilon(1e-9));
    CHECK(b.value(c) == Approx(result->best[1]).epsilon(1e-9));
    CHECK_FALSE(dialog->applyButton()->isEnabled());
    REQUIRE((ran.size() > 0 || ran.wait(20000)));
    REQUIRE(view.lastResult());
    CHECK(measure(*view.lastResult(), QStringLiteral("dB(S11)"), 2.4e9) < -20);

    b.sheet->undoStack()->undo();
    CHECK(b.parameter("L1", "l") == QStringLiteral("2nH"));
    CHECK(b.parameter("C1", "c") == QStringLiteral("1.5pF"));
    dialog->close();
}

TEST_CASE("optimisation setups are stored with the document and selected again")
{
    TuneBench b;
    b.matchingProblem();
    const QString analysis = b.addAnalysis(QStringLiteral("2.3GHz"), QStringLiteral("2.5GHz"), QStringLiteral("21"));
    const int undoBefore = b.sheet->undoStack()->count();
    QString id;
    {
        OptimizationDialog dialog(b.project, b.document);
        CHECK(dialog.store().isEmpty()); // no variables, no goals
        CHECK(dialog.validate().size() >= 2);
        DesignVariable l = parameterVariable("L1", "l");
        REQUIRE(dialog.addVariable(l)); // default range around the value
        Goal goal;
        goal.measurement = QStringLiteral("dB(S11)");
        goal.target = -15;
        goal.bandStart = 2.35e9;
        goal.bandStop = 2.45e9;
        dialog.addGoal(goal);
        dialog.methodCombo()->setCurrentIndex(1); // Levenberg-Marquardt
        // The band is typed with SI prefixes.
        dialog.goalTable()->item(0, 4)->setText(QStringLiteral("2.38 GHz"));
        id = dialog.store();
        REQUIRE(id == QStringLiteral("o1"));
        CHECK(b.sheet->undoStack()->count() == undoBefore + 1);
        CHECK(dialog.store() == id); // unchanged: no new step
        CHECK(b.sheet->undoStack()->count() == undoBefore + 1);
    }
    QString error;
    REQUIRE(b.document->save(&error));
    auto reloaded = Document::load(b.document->filePath(), &error);
    REQUIRE(reloaded);
    const auto setups = optimizationSetups(reloaded->schematic()->simulation());
    REQUIRE(setups.size() == 1);
    CHECK(setups[0].analysis == analysis);
    CHECK(setups[0].method == OptimizationSetup::Method::LevenbergMarquardt);
    REQUIRE(setups[0].variables.size() == 1);
    CHECK(setups[0].variables[0].label() == QStringLiteral("L1.l"));
    CHECK(setups[0].variables[0].minimum == Approx(1e-9));
    CHECK(setups[0].variables[0].maximum == Approx(4e-9));
    REQUIRE(setups[0].goals.size() == 1);
    CHECK(setups[0].goals[0].bandStart == Approx(2.38e9));
    CHECK(setups[0].goals[0].bandStop == Approx(2.45e9));

    // Picked again later, from the setup list.
    OptimizationDialog again(b.project, b.document);
    CHECK(again.setupId().isEmpty());
    REQUIRE(again.setupCombo()->findData(id) >= 0);
    again.selectSetup(id);
    CHECK(again.setupId() == id);
    CHECK(again.setup().toJson() == setups[0].toJson());
    // A new setup next to it.
    again.selectSetup({});
    again.addVariable(parameterVariable("C1", "c"));
    Goal goal;
    goal.measurement = QStringLiteral("dB(S11)");
    again.addGoal(goal);
    CHECK(again.store() == QStringLiteral("o2"));
    CHECK(optimizationSetups(b.sheet->simulation()).size() == 2);
    b.sheet->undoStack()->undo();
    CHECK(optimizationSetups(b.sheet->simulation()).size() == 1);
}

TEST_CASE("a running optimisation can be cancelled")
{
    TuneBench b;
    b.matchingProblem();
    b.addAnalysis(QStringLiteral("1GHz"), QStringLiteral("4GHz"), QStringLiteral("2001"));
    OptimizationDialog dialog(b.project, b.document);
    dialog.addVariable(parameterVariable("L1", "l"));
    dialog.addVariable(parameterVariable("C1", "c"));
    dialog.addVariable(parameterVariable("R1", "r"));
    Goal goal;
    goal.measurement = QStringLiteral("dB(S11)");
    goal.target = -60; // out of reach
    dialog.addGoal(goal);
    dialog.evaluationsSpin()->setValue(100000);
    QSignalSpy finished(&dialog, &OptimizationDialog::finished);
    REQUIRE(dialog.start());
    CHECK(dialog.stopButton()->isEnabled());
    CHECK_FALSE(dialog.runButton()->isEnabled());
    QTest::qWait(150);
    dialog.cancel();
    REQUIRE((finished.size() > 0 || finished.wait(10000)));
    CHECK_FALSE(dialog.isRunning());
    const auto result = dialog.result();
    REQUIRE(result);
    CHECK(result->cancelled);
    CHECK(result->evaluations < 100000);
    CHECK(dialog.runButton()->isEnabled());
    CHECK(b.parameter("L1", "l") == QStringLiteral("2nH"));
    // The best point so far can still be applied or discarded.
    CHECK(dialog.applyButton()->isEnabled());
    dialog.discard();
    CHECK_FALSE(dialog.result());
    CHECK_FALSE(dialog.applyButton()->isEnabled());

    // Closing while running cancels and waits.
    dialog.show();
    REQUIRE(dialog.start());
    dialog.close();
    CHECK_FALSE(dialog.isRunning());
}
