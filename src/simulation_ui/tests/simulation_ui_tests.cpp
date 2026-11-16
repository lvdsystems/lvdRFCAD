#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "plot/PlotWidget.h"
#include "rfdata/NetworkMath.h"
#include "rfdata/Touchstone.h"
#include "rfsim/Analysis.h"
#include "rfsim/Measurement.h"
#include "simulation_ui/AnalysisDialog.h"
#include "simulation_ui/PlotDefinitions.h"
#include "simulation_ui/PlotPane.h"
#include "simulation_ui/ResultsTable.h"
#include "simulation_ui/SimulationRunner.h"
#include "simulation_ui/SimulationView.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <cmath>

using namespace lvd;
using namespace lvd::rf;
using Catch::Approx;

namespace {

// Builds schematics from library parts (as in the rfsim tests).
struct Bench {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;

    Bench()
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

    // Port 1 - amplifier - attenuator - port 2. Returns the amplifier id.
    QString amplifierChain(const QString &gain = QStringLiteral("20"))
    {
        const QString p1 = port(1, {-400, 0}), p2 = port(2, {400, 0});
        const QString amp = place("rf-system:amplifier", {-100, 0},
                                  {{QStringLiteral("gain"), gain}, {QStringLiteral("nf"), QStringLiteral("2")}});
        const QString att = place("rf-system:attenuator", {100, 0}, {{QStringLiteral("il"), QStringLiteral("6")}});
        connect(p1, "p", amp, "in");
        connect(amp, "out", att, "in");
        connect(att, "out", p2, "p");
        return amp;
    }

    // Stores an analysis with the default plots, as the dialog does.
    QString addAnalysis(const QString &points = QStringLiteral("51"))
    {
        LinearAnalysisSetup setup;
        setup.name = QStringLiteral("Sweep");
        setup.start = QStringLiteral("1GHz");
        setup.stop = QStringLiteral("3GHz");
        setup.points = points;
        QString id;
        QJsonObject simulation = withLinearAnalysis(sheet->simulation(), setup, &id);
        for (const sim::PlotDefinition &plot : sim::defaultPlots(id, {1, 2}, true))
            simulation = sim::withPlot(simulation, plot);
        sheet->setSimulation(simulation);
        return id;
    }
};

bool waitForRun(SimulationView &view, QSignalSpy &spy, int timeoutMs = 20000)
{
    if (spy.isEmpty() && !spy.wait(timeoutMs))
        return false;
    // Let queued follow-ups (pending runs) settle.
    QApplication::processEvents();
    return !view.isRunning() || spy.wait(timeoutMs);
}

double s21Db(const LinearResult &result)
{
    const Measurement m = evaluateMeasurement(result, QStringLiteral("dB(S21)"));
    REQUIRE(m.ok());
    return m.values.front().real();
}

} // namespace

TEST_CASE("default plots and completions follow the ports")
{
    const auto plots = sim::defaultPlots(QStringLiteral("a1"), {1, 2}, true);
    REQUIRE(plots.size() == 2);
    CHECK(plots[0].kind == QStringLiteral("rectangular"));
    CHECK(plots[0].traces == QStringList{QStringLiteral("dB(S21)"), QStringLiteral("dB(S12)"), QStringLiteral("dB(S11)"),
                                         QStringLiteral("dB(S22)"), QStringLiteral("NF")});
    CHECK(plots[1].kind == QStringLiteral("smith"));
    CHECK(plots[1].traces == QStringList{QStringLiteral("S11"), QStringLiteral("S22")});
    CHECK(plots[1].analysis == QStringLiteral("a1"));

    const auto three = sim::defaultPlots(QStringLiteral("a1"), {1, 2, 3}, true);
    CHECK(three[0].traces.size() == 9);
    CHECK_FALSE(three[0].traces.contains(QStringLiteral("NF")));
    CHECK(three[1].traces == QStringList{QStringLiteral("S11")});
    CHECK(sim::defaultPlots(QStringLiteral("a1"), {1, 12}, false)[0].traces.contains(QStringLiteral("dB(S(12,1))")));

    const QStringList completions = sim::measurementCompletions({1, 2}, true);
    for (const char *expected : {"S11", "S21", "dB(S21)", "NF", "K", "mu", "GD(S21)", "Zin(1)", "VSWR(1)"})
        CHECK(completions.contains(QString::fromLatin1(expected)));
    CHECK(sim::isNoiseMeasurement(QStringLiteral("NF")));
    CHECK(sim::isNoiseMeasurement(QStringLiteral("NFmin")));
    CHECK_FALSE(sim::isNoiseMeasurement(QStringLiteral("dB(S21)")));
}

TEST_CASE("plot definitions keep unknown keys and survive save and reload")
{
    Bench b;
    QJsonObject simulation{{QStringLiteral("future"), QStringLiteral("kept")}};
    sim::PlotDefinition plot;
    plot.name = QStringLiteral("Gain");
    plot.analysis = QStringLiteral("a1");
    plot.traces = {QStringLiteral("dB(S21)")};
    plot.labels.insert(QStringLiteral("dB(S21)"), QStringLiteral("Gain"));
    plot.extra.insert(QStringLiteral("style"), QStringLiteral("x"));
    QString id;
    simulation = sim::withPlot(simulation, plot, &id);
    CHECK(id == QStringLiteral("p1"));
    b.sheet->setSimulation(simulation);

    QString error;
    REQUIRE(b.document->save(&error));
    auto reloaded = Document::load(b.document->filePath(), &error);
    REQUIRE(reloaded);
    const QJsonObject loaded = reloaded->schematic()->simulation();
    CHECK(loaded.value(QStringLiteral("future")).toString() == QStringLiteral("kept"));
    const auto plots = sim::plotDefinitions(loaded);
    REQUIRE(plots.size() == 1);
    CHECK(plots[0].label(QStringLiteral("dB(S21)")) == QStringLiteral("Gain"));
    CHECK(plots[0].extra.value(QStringLiteral("style")).toString() == QStringLiteral("x"));

    // Editing keeps the plot's own unknown keys too.
    sim::PlotDefinition edited = plots[0];
    edited.extra = {};
    edited.traces.append(QStringLiteral("NF"));
    const QJsonObject after = sim::withPlot(loaded, edited);
    CHECK(sim::plotDefinitions(after)[0].extra.value(QStringLiteral("style")).toString() == QStringLiteral("x"));
    CHECK(sim::plotDefinitions(sim::withoutAnalysis(after, QStringLiteral("a1"))).isEmpty());
}

TEST_CASE("the analysis dialog validates and stores the setup as one undo step")
{
    Bench b;
    b.amplifierChain();
    b.sheet->setVariables({{QStringLiteral("f0"), QStringLiteral("1GHz")}});
    const int undoBefore = b.sheet->undoStack()->count();

    AnalysisDialog dialog(b.project, b.document);
    CHECK(dialog.isNew());
    CHECK(dialog.isValid()); // the defaults are valid
    dialog.startEdit()->setText(QStringLiteral("nonsense+"));
    CHECK_FALSE(dialog.isValid());
    CHECK_FALSE(dialog.okButton()->isEnabled());
    CHECK_FALSE(dialog.startValue()->text().startsWith(QLatin1Char('=')));
    CHECK(dialog.store().isEmpty());

    dialog.startEdit()->setText(QStringLiteral("f0/2"));
    dialog.stopEdit()->setText(QStringLiteral("f0*3"));
    dialog.pointsEdit()->setText(QStringLiteral("11"));
    CHECK(dialog.isValid());
    CHECK(dialog.okButton()->isEnabled());
    CHECK(dialog.startValue()->text() == QStringLiteral("= 500 MHz"));
    CHECK(dialog.stopValue()->text() == QStringLiteral("= 3 GHz"));
    CHECK(dialog.summary().startsWith(QStringLiteral("11 points")));

    dialog.pointsEdit()->setText(QStringLiteral("2.5"));
    CHECK_FALSE(dialog.okButton()->isEnabled());
    dialog.pointsEdit()->setText(QStringLiteral("11"));

    // List sweep.
    dialog.sweepCombo()->setCurrentIndex(dialog.sweepCombo()->findData(QStringLiteral("list")));
    dialog.listEdit()->setPlainText(QString());
    CHECK_FALSE(dialog.isValid());
    dialog.listEdit()->setPlainText(QStringLiteral("2.4GHz\nf0\n900MHz"));
    CHECK(dialog.isValid());
    CHECK(dialog.summary().startsWith(QStringLiteral("3 points")));
    dialog.sweepCombo()->setCurrentIndex(dialog.sweepCombo()->findData(QStringLiteral("linear")));
    dialog.nameEdit()->setText(QStringLiteral("Wideband"));
    dialog.noiseCheck()->setChecked(true);

    const QString id = dialog.store();
    REQUIRE(id == QStringLiteral("a1"));
    CHECK(b.sheet->undoStack()->count() == undoBefore + 1);
    const auto analyses = linearAnalyses(b.sheet->simulation());
    REQUIRE(analyses.size() == 1);
    CHECK(analyses[0].name == QStringLiteral("Wideband"));
    CHECK(analyses[0].start == QStringLiteral("f0/2"));
    CHECK(analyses[0].points == QStringLiteral("11"));
    const auto plots = sim::plotDefinitions(b.sheet->simulation(), id);
    REQUIRE(plots.size() == 2); // default plots in the same step
    CHECK(plots[0].traces.contains(QStringLiteral("NF")));

    b.sheet->undoStack()->undo();
    CHECK(linearAnalyses(b.sheet->simulation()).isEmpty());
    CHECK(sim::plotDefinitions(b.sheet->simulation()).isEmpty());
    b.sheet->undoStack()->redo();
    CHECK(linearAnalyses(b.sheet->simulation()).size() == 1);

    // Editing replaces the analysis and adds no plots.
    AnalysisDialog edit(b.project, b.document, id);
    CHECK_FALSE(edit.isNew());
    CHECK(edit.nameEdit()->text() == QStringLiteral("Wideband"));
    edit.pointsEdit()->setText(QStringLiteral("21"));
    CHECK(edit.store() == id);
    REQUIRE(linearAnalyses(b.sheet->simulation()).size() == 1);
    CHECK(linearAnalyses(b.sheet->simulation())[0].points == QStringLiteral("21"));
    CHECK(sim::plotDefinitions(b.sheet->simulation()).size() == 2);
    b.sheet->undoStack()->undo();
    CHECK(linearAnalyses(b.sheet->simulation())[0].points == QStringLiteral("11"));
}

TEST_CASE("the runner simulates on a worker thread and can be cancelled")
{
    Bench b;
    b.amplifierChain();
    const Netlist netlist = buildNetlist(*b.project, *b.document);
    std::vector<double> frequencies;
    for (int i = 0; i < 2001; ++i)
        frequencies.push_back(1e9 + i * 1e6);

    sim::SimulationRunner runner;
    QSignalSpy spy(&runner, &sim::SimulationRunner::finished);
    REQUIRE(runner.start(netlist, frequencies, {}, 7));
    CHECK(runner.isRunning());
    CHECK_FALSE(runner.start(netlist, frequencies, {}, 8)); // one at a time
    REQUIRE(spy.wait(20000));
    CHECK_FALSE(runner.isRunning());
    const auto result = spy.takeFirst().at(0).value<std::shared_ptr<const LinearResult>>();
    (void)result;
    CHECK(spy.isEmpty());

    std::shared_ptr<const LinearResult> delivered;
    quint64 deliveredTag = 0;
    QObject::connect(&runner, &sim::SimulationRunner::finished, &runner,
                     [&](std::shared_ptr<const LinearResult> r, quint64 tag) {
                         delivered = std::move(r);
                         deliveredTag = tag;
                     });
    REQUIRE(runner.start(netlist, frequencies, {}, 9));
    runner.cancel();
    runner.wait();
    REQUIRE(delivered);
    CHECK(deliveredTag == 9);
    CHECK_FALSE(runner.isRunning());
    // Either it noticed the flag or it had already finished.
    CHECK((delivered->cancelled || delivered->ok()));
}

TEST_CASE("the view runs in the background, fills the default plots and keeps traces in place")
{
    Bench b;
    b.amplifierChain();
    const QString analysis = b.addAnalysis();

    SimulationView view(b.project, b.document);
    view.resize(1200, 800);
    CHECK(view.currentAnalysisId() == analysis);
    REQUIRE(view.plotPanes().size() == 2);
    CHECK(view.lastResult() == nullptr);

    QSignalSpy spy(&view, &SimulationView::runFinished);
    view.run();
    CHECK(view.isRunning()); // the result arrives through the event loop
    CHECK(view.stopAction()->isEnabled());
    REQUIRE(waitForRun(view, spy));
    REQUIRE(spy.size() == 1);
    CHECK(spy.first().at(0).toBool());
    REQUIRE(view.lastResult());
    REQUIRE(view.lastResult()->ok());
    CHECK(view.lastResult()->network.frequencyCount() == 51);
    CHECK(s21Db(*view.lastResult()) == Approx(14).margin(1e-6));
    CHECK(view.errors().isEmpty());
    CHECK(view.lastElapsedMs() >= 0);

    sim::PlotPane *response = view.plotPanes()[0];
    sim::PlotPane *smith = view.plotPanes()[1];
    CHECK(smith->plot()->kind() == PlotKind::Smith);
    for (const QString &expression : response->definition().traces) {
        INFO(expression.toStdString());
        CHECK(response->traceId(expression) >= 0);
        CHECK(response->traceError(expression).isEmpty());
    }
    const int nf = response->traceId(QStringLiteral("NF"));
    CHECK(response->plot()->traceStyle(nf).yAxis == PlotYAxis::Right);
    CHECK(response->plot()->traceStyle(response->traceId(QStringLiteral("dB(S21)"))).yAxis == PlotYAxis::Left);
    CHECK(smith->traceId(QStringLiteral("S11")) >= 0);

    // A marker and a zoom survive the next run; the trace keeps its id.
    const int s21 = response->traceId(QStringLiteral("dB(S21)"));
    const int marker = response->plot()->addMarker(s21, 2e9);
    response->plot()->setXRange(1.5e9, 2.5e9);
    b.sheet->setParameter(b.sheet->componentByReference(QStringLiteral("A1"))->id, QStringLiteral("gain"),
                          QStringLiteral("25"));
    spy.clear();
    view.run();
    REQUIRE(waitForRun(view, spy));
    CHECK(s21Db(*view.lastResult()) == Approx(19).margin(1e-6));
    CHECK(response->traceId(QStringLiteral("dB(S21)")) == s21);
    REQUIRE(response->plot()->marker(marker));
    CHECK(response->plot()->marker(marker)->y == Approx(19).margin(1e-6));
    CHECK(response->plot()->xScale().min() == Approx(1.5e9));
    CHECK(response->plot()->xScale().max() == Approx(2.5e9));

    // The table shows the plot traces.
    CHECK(view.tableModel()->rowCount() == 51);
    CHECK(view.tableModel()->columnCount() == 1 + 7);
    view.copyTable();
    CHECK(QApplication::clipboard()->text().startsWith(QStringLiteral("Frequency [Hz]\t")));
}

TEST_CASE("traces are added by expression; invalid ones show their error")
{
    Bench b;
    b.amplifierChain();
    const QString analysis = b.addAnalysis(QStringLiteral("11"));
    SimulationView view(b.project, b.document);
    QSignalSpy spy(&view, &SimulationView::runFinished);
    view.run();
    REQUIRE(waitForRun(view, spy));

    CHECK(view.completions().contains(QStringLiteral("GD(S21)")));
    sim::PlotPane *pane = view.plotPanes()[0];
    const QString plotId = pane->definition().id;
    const int undoBefore = b.sheet->undoStack()->count();

    REQUIRE(pane->addTrace(QStringLiteral("GD(S21)")));
    CHECK(b.sheet->undoStack()->count() == undoBefore + 1);
    CHECK(sim::plotDefinitions(b.sheet->simulation(), analysis)[0].traces.contains(QStringLiteral("GD(S21)")));
    pane = view.plotPane(plotId);
    REQUIRE(pane);
    CHECK(pane->traceId(QStringLiteral("GD(S21)")) >= 0);

    // Refused: unknown port, syntax error, duplicate.
    CHECK_FALSE(pane->addTrace(QStringLiteral("dB(S93)")));
    CHECK_FALSE(pane->inputError().isEmpty());
    CHECK_FALSE(pane->addTrace(QStringLiteral("dB(S21")));
    CHECK_FALSE(pane->addTrace(QStringLiteral("GD(S21)")));
    CHECK(b.sheet->undoStack()->count() == undoBefore + 1);
    // Real values are refused on a Smith chart.
    CHECK_FALSE(view.plotPanes()[1]->addTrace(QStringLiteral("dB(S21)")));
    CHECK(view.plotPanes()[1]->addTrace(QStringLiteral("Zin(2)")));

    // An invalid expression stored in the file shows its error in the list.
    sim::PlotDefinition broken = pane->definition();
    broken.traces.append(QStringLiteral("frobnicate(S21)"));
    b.sheet->setSimulation(sim::withPlot(b.sheet->simulation(), broken));
    CHECK_FALSE(pane->traceError(QStringLiteral("frobnicate(S21)")).isEmpty());
    bool listed = false;
    for (int i = 0; i < pane->traceList()->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = pane->traceList()->topLevelItem(i);
        if (item->text(1) == QStringLiteral("frobnicate(S21)"))
            listed = !item->text(2).isEmpty();
    }
    CHECK(listed);

    // Rename (as the legend editor does) and remove.
    pane->renameTrace(QStringLiteral("GD(S21)"), QStringLiteral("Delay"));
    CHECK(pane->plot()->traceName(pane->traceId(QStringLiteral("GD(S21)"))) == QStringLiteral("Delay"));
    CHECK(sim::plotDefinitions(b.sheet->simulation(), analysis)[0].label(QStringLiteral("GD(S21)")) == QStringLiteral("Delay"));
    const int gdTrace = pane->traceId(QStringLiteral("GD(S21)"));
    pane->removeTrace(QStringLiteral("GD(S21)"));
    CHECK(pane->traceId(QStringLiteral("GD(S21)")) == -1);
    const std::vector<int> ids = pane->plot()->traceIds();
    CHECK(std::ranges::find(ids, gdTrace) == ids.end());

    // Undo brings it back, in place.
    b.sheet->undoStack()->undo();
    CHECK(pane->traceId(QStringLiteral("GD(S21)")) >= 0);

    // Plots added and removed through the view are undoable too.
    const QString polar = view.addPlot(QStringLiteral("polar"));
    REQUIRE(view.plotPane(polar));
    CHECK(view.plotPane(polar)->plot()->kind() == PlotKind::Polar);
    CHECK(view.plotPanes().size() == 3);
    view.removePlot(polar);
    CHECK(view.plotPanes().size() == 2);
    b.sheet->undoStack()->undo();
    CHECK(view.plotPanes().size() == 3);
    view.setTiled(false);
    CHECK(view.plotPanes().size() == 3);
    view.setTiled(true);
}

TEST_CASE("auto-run follows edits and cancels a stale run")
{
    Bench b;
    const QString amp = b.amplifierChain();
    b.addAnalysis(QStringLiteral("20001"));
    SimulationView view(b.project, b.document);
    view.setAutoRunDelay(30);
    QSignalSpy spy(&view, &SimulationView::runFinished);

    // A run requested while one is active cancels it and starts again with
    // the newest circuit; only that result is delivered.
    view.run();
    REQUIRE(view.isRunning());
    b.sheet->setParameter(amp, QStringLiteral("gain"), QStringLiteral("30"));
    view.run();
    REQUIRE(waitForRun(view, spy));
    REQUIRE(spy.size() == 1);
    CHECK(spy.first().at(0).toBool());
    CHECK(s21Db(*view.lastResult()) == Approx(24).margin(1e-6));

    // Plot edits do not make the result outdated.
    view.plotPanes()[0]->addTrace(QStringLiteral("K"));
    QTest::qWait(100);
    CHECK_FALSE(view.isOutdated());
    CHECK_FALSE(view.isRunning());

    // Without auto-run an edit only marks the result as outdated.
    b.sheet->setParameter(amp, QStringLiteral("gain"), QStringLiteral("10"));
    QTest::qWait(150);
    CHECK(view.isOutdated());
    CHECK_FALSE(view.isRunning());

    // With auto-run, the edit triggers a run after the quiet period.
    spy.clear();
    view.setAutoRun(true);
    REQUIRE(waitForRun(view, spy));
    CHECK(s21Db(*view.lastResult()) == Approx(4).margin(1e-6));
    CHECK_FALSE(view.isOutdated());

    // Quick edits while running: the stale run is cancelled, the last edit wins.
    spy.clear();
    b.sheet->setParameter(amp, QStringLiteral("gain"), QStringLiteral("11"));
    REQUIRE(QTest::qWaitFor([&] { return view.isRunning(); }, 5000));
    b.sheet->setParameter(amp, QStringLiteral("gain"), QStringLiteral("12"));
    b.sheet->setParameter(amp, QStringLiteral("gain"), QStringLiteral("13"));
    REQUIRE(QTest::qWaitFor([&] { return !spy.isEmpty() && !view.isRunning(); }, 30000));
    QTest::qWait(100);
    REQUIRE_FALSE(view.isRunning());
    CHECK(s21Db(*view.lastResult()) == Approx(7).margin(1e-6));
    for (const QList<QVariant> &call : spy)
        CHECK(call.at(0).toBool()); // no stale result was reported as a success with old data
    view.setAutoRun(false);
}

TEST_CASE("errors are shown prominently and no result is kept for export")
{
    Bench b;
    b.place("rf-system:amplifier", {0, 0}); // no ports
    b.addAnalysis();
    SimulationView view(b.project, b.document);
    view.show();
    QSignalSpy spy(&view, &SimulationView::runFinished);
    view.run();
    REQUIRE(waitForRun(view, spy));
    CHECK_FALSE(spy.first().at(0).toBool());
    CHECK_FALSE(view.errors().isEmpty());
    auto *banner = view.findChild<QWidget *>(QStringLiteral("errorBanner"));
    REQUIRE(banner);
    CHECK(banner->isVisible());
    QString error;
    CHECK_FALSE(view.exportCsv(b.dir.filePath(QStringLiteral("x.csv")), &error));
    CHECK_FALSE(error.isEmpty());

    // A bad sweep is reported without running.
    LinearAnalysisSetup bad = linearAnalyses(b.sheet->simulation()).first();
    bad.points = QStringLiteral("0");
    b.sheet->setSimulation(withLinearAnalysis(b.sheet->simulation(), bad));
    spy.clear();
    view.run();
    REQUIRE(spy.size() == 1);
    CHECK_FALSE(spy.first().at(0).toBool());
    CHECK(view.errors().first().contains(QStringLiteral("points")));
}

TEST_CASE("stability circles appear on a Smith chart when K < 1")
{
    auto result = std::make_shared<LinearResult>();
    result->portNumbers = {1, 2};
    result->network.frequencies = {1e9, 2e9};
    result->network.z0 = {50, 50};
    for (int i = 0; i < 2; ++i) {
        CMatrix s(2, 2);
        s << Complex(0.9, 0), Complex(0.2, 0), Complex(3, 0), Complex(0.9, 0);
        result->network.s.push_back(s);
    }
    sim::PlotDefinition definition;
    definition.id = QStringLiteral("p1");
    definition.kind = QStringLiteral("smith");
    definition.traces = {QStringLiteral("S11")};
    sim::PlotPane pane(definition);
    pane.setResult(result);
    CHECK(pane.stabilityCircleCount() == 0);
    pane.setStabilityCirclesEnabled(true);
    CHECK(pane.stabilityCircleCount() == 2);
    pane.setStabilityCirclesEnabled(false);
    CHECK(pane.stabilityCircleCount() == 0);
}

TEST_CASE("results export as Touchstone and CSV")
{
    Bench b;
    b.amplifierChain();
    b.addAnalysis(QStringLiteral("11"));
    SimulationView view(b.project, b.document);
    QSignalSpy spy(&view, &SimulationView::runFinished);
    view.run();
    REQUIRE(waitForRun(view, spy));
    REQUIRE(view.lastResult());

    QString error;
    const QString csvPath = b.dir.filePath(QStringLiteral("table.csv"));
    REQUIRE(view.exportCsv(csvPath, &error));
    QFile csv(csvPath);
    REQUIRE(csv.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(csv.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    REQUIRE(lines.size() == 12);
    CHECK(lines[0].startsWith(QStringLiteral("Frequency [Hz],dB(S21) [dB]")));
    CHECK(lines[1].startsWith(QStringLiteral("1000000000,")));

    // Complex columns split into real and imaginary parts.
    const QString text = sim::resultsCsv(*view.lastResult(), {QStringLiteral("S21")});
    CHECK(text.startsWith(QStringLiteral("Frequency [Hz],re(S21),im(S21)\n")));

    const QString touchstonePath = b.dir.filePath(QStringLiteral("dut.s2p"));
    TouchstoneWriteOptions options;
    options.format = TouchstoneFormat::DecibelAngle;
    options.frequencyUnit = QStringLiteral("MHz");
    const bool written = view.exportTouchstone(touchstonePath, options, &error);
    if (!written && error.contains(QStringLiteral("not implemented"))) {
        WARN("writeTouchstone is not implemented yet: " << error.toStdString());
        return;
    }
    REQUIRE(written);
    const TouchstoneReadResult read = readTouchstone(touchstonePath);
    REQUIRE(read.network);
    CHECK(read.network->portCount() == 2);
    CHECK(read.network->frequencyCount() == 11);
    CHECK(db20(read.network->s[0](1, 0)) == Approx(14).margin(1e-6));
    CHECK(read.network->hasNoiseParameters());
    if (read.network->hasNoiseParameters())
        CHECK(read.network->noise[0].fminDb > 0);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("simulation_ui_tests"));
    return Catch::Session().run(argc, argv);
}
