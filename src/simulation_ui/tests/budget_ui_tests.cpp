// Tests of the budget analysis user interface (RF-6 to RF-9): the dialog,
// background runs in the simulation view, the budget table, level diagram,
// swept budget, spur table and frequency plan. Spur results are only used
// through synthetic data: the spur analysis may not be implemented yet.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "plot/PlotWidget.h"
#include "rfsim/Analysis.h"
#include "rfsim/Budget.h"
#include "simulation_ui/BudgetDialog.h"
#include "simulation_ui/BudgetModels.h"
#include "simulation_ui/BudgetView.h"
#include "simulation_ui/SimulationView.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QGroupBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace lvd;
using namespace lvd::rf;
using Catch::Approx;

namespace {

// Places parts and wires each output to the next input (as in the rfsim budget tests).
struct Receiver {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;
    QString lna;

    Receiver()
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

    QPoint pin(const QString &id, const char *name)
    {
        const PlacedComponent *component = sheet->component(id);
        const DefinitionPtr definition = project->resolve(*sheet, component->definition).definition;
        const SymbolPin *symbolPin = definition->symbol.pin(QString::fromLatin1(name));
        REQUIRE(symbolPin);
        return pinPosition(*component, *symbolPin);
    }

    void wire(const QString &a, const char *pinA, const QString &b, const char *pinB)
    {
        const QPoint start = pin(a, pinA), end = pin(b, pinB);
        sheet->addWire({start, QPoint(end.x(), start.y()), end});
    }

    // Source, bandpass, LNA, mixer with LO, low-pass, ADC.
    void superheterodyne()
    {
        sheet->setVariables({{QStringLiteral("f_rf"), QStringLiteral("2.45GHz")}, {QStringLiteral("f_if"), QStringLiteral("70MHz")}});
        const QString src = place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("f_rf")}, {QStringLiteral("power"), QStringLiteral("-60")}});
        const QString bpf = place("rf-system:filter", {200, 0},
                                  {{QStringLiteral("fc"), QStringLiteral("f_rf")}, {QStringLiteral("bw"), QStringLiteral("100MHz")},
                                   {QStringLiteral("il"), QStringLiteral("1.5")}});
        lna = place("rf-system:amplifier", {400, 0}, {{QStringLiteral("gain"), QStringLiteral("18")}, {QStringLiteral("nf"), QStringLiteral("1")}});
        const QString mixer = place("rf-system:mixer", {600, 0});
        const QString lo = place("rf-system:lo", {600, 200}, {{QStringLiteral("freq"), QStringLiteral("f_rf - f_if")}, {QStringLiteral("power"), QStringLiteral("13")}});
        const QString lpf = place("rf-system:filter", {800, 0},
                                  {{QStringLiteral("type"), QStringLiteral("lowpass")}, {QStringLiteral("fc"), QStringLiteral("100MHz")},
                                   {QStringLiteral("il"), QStringLiteral("0.5")}});
        const QString adc = place("rf-system:adc", {1000, 0}, {{QStringLiteral("fs"), QStringLiteral("100MHz")}});
        wire(src, "out", bpf, "in");
        wire(bpf, "out", lna, "in");
        wire(lna, "out", mixer, "rf");
        wire(lo, "out", mixer, "lo");
        wire(mixer, "if", lpf, "in");
        wire(lpf, "out", adc, "in");
    }

    QString addBudget(BudgetAnalysisSetup setup = {})
    {
        if (setup.name.isEmpty())
            setup.name = QStringLiteral("Receiver");
        QString id;
        sheet->setSimulation(withBudgetAnalysis(sheet->simulation(), setup, &id));
        return id;
    }
};

bool waitForBudget(SimulationView &view, QSignalSpy &spy, int timeoutMs = 30000)
{
    if (spy.isEmpty() && !spy.wait(timeoutMs))
        return false;
    QApplication::processEvents();
    return !view.isRunning() || spy.wait(timeoutMs);
}

bool same(double a, double b)
{
    if (std::isinf(a) || std::isinf(b))
        return a == b;
    if (std::isnan(a) || std::isnan(b))
        return std::isnan(a) && std::isnan(b);
    return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b));
}

double cell(const sim::BudgetTableModel &model, sim::BudgetTableModel::Row row, int column, int role = Qt::EditRole)
{
    const int r = model.rowOf(row);
    REQUIRE(r >= 0);
    return model.data(model.index(r, column), role).toDouble();
}

QString cellText(const sim::BudgetTableModel &model, sim::BudgetTableModel::Row row, int column)
{
    const int r = model.rowOf(row);
    REQUIRE(r >= 0);
    return model.data(model.index(r, column), Qt::DisplayRole).toString();
}

// y of a trace at x, through a marker (the plot widget interpolates).
double traceValue(PlotWidget *plot, int trace, double x)
{
    REQUIRE(trace >= 0);
    const int marker = plot->addMarker(trace, x);
    const auto info = plot->marker(marker);
    REQUIRE(info);
    REQUIRE(info->valid);
    const double y = info->y;
    plot->removeMarker(marker);
    return y;
}

} // namespace

TEST_CASE("the budget dialog validates and stores the setup as one undo step")
{
    Receiver r;
    r.superheterodyne();
    const int undoBefore = r.sheet->undoStack()->count();

    BudgetDialog dialog(r.project, r.document);
    CHECK(dialog.isNew());
    CHECK(dialog.isValid()); // the defaults are valid
    CHECK(dialog.startCandidates() == QStringList{QStringLiteral("SRC1")});
    CHECK(dialog.chainPreview().startsWith(QStringLiteral("SRC1 → ")));
    CHECK(dialog.chainPreview().count(QStringLiteral("→")) == 5);
    CHECK(dialog.frequencyValue()->text().startsWith(QStringLiteral("from SRC1: 2.45 GHz")));
    CHECK(dialog.powerValue()->text().contains(QStringLiteral("-60")));

    dialog.frequencyEdit()->setText(QStringLiteral("f_rf + 1MHz"));
    CHECK(dialog.frequencyValue()->text() == QStringLiteral("= 2.451 GHz"));
    dialog.frequencyEdit()->clear();
    dialog.powerEdit()->setText(QStringLiteral("-50"));
    CHECK(dialog.powerValue()->text() == QStringLiteral("= -50 dBm"));

    dialog.bandwidthEdit()->setText(QStringLiteral("nonsense+"));
    CHECK_FALSE(dialog.isValid());
    CHECK_FALSE(dialog.okButton()->isEnabled());
    CHECK(dialog.store().isEmpty());
    dialog.bandwidthEdit()->setText(QStringLiteral("200kHz"));
    CHECK(dialog.isValid());
    CHECK(dialog.bandwidthValue()->text() == QStringLiteral("= 200 kHz"));

    // Swept budget: start and stop are required.
    dialog.sweepGroup()->setChecked(true);
    CHECK_FALSE(dialog.isValid());
    dialog.sweepStartEdit()->setText(QStringLiteral("2.3GHz"));
    dialog.sweepStopEdit()->setText(QStringLiteral("2.2GHz"));
    CHECK_FALSE(dialog.isValid()); // stop below start
    dialog.sweepStopEdit()->setText(QStringLiteral("2.6GHz"));
    dialog.sweepPointsEdit()->setText(QStringLiteral("31"));
    CHECK(dialog.isValid());

    dialog.spurOrderEdit()->setText(QStringLiteral("1"));
    CHECK_FALSE(dialog.isValid());
    dialog.spurOrderEdit()->setText(QStringLiteral("7"));
    CHECK(dialog.isValid());

    dialog.mismatchCombo()->setCurrentIndex(1);
    CHECK(dialog.mismatchHint()->text().contains(QStringLiteral("matched")));

    // Explicit path from the automatic trace; its first entry is the start.
    dialog.tracePath();
    const QStringList path = dialog.path();
    REQUIRE(path.size() == 6);
    CHECK(path.first() == QStringLiteral("SRC1"));
    CHECK_FALSE(dialog.startCombo()->isEnabled());
    CHECK(dialog.chainPreview().count(QStringLiteral("→")) == 5);
    dialog.nameEdit()->setText(QStringLiteral("Receiver"));

    const QString id = dialog.store();
    REQUIRE(id == QStringLiteral("a1"));
    CHECK(r.sheet->undoStack()->count() == undoBefore + 1);
    auto analyses = budgetAnalyses(r.sheet->simulation());
    REQUIRE(analyses.size() == 1);
    CHECK(analyses[0].name == QStringLiteral("Receiver"));
    CHECK(analyses[0].bandwidth == QStringLiteral("200kHz"));
    CHECK(analyses[0].power == QStringLiteral("-50"));
    CHECK(analyses[0].frequency.isEmpty());
    CHECK(analyses[0].sweepStart == QStringLiteral("2.3GHz"));
    CHECK(analyses[0].sweepPoints == QStringLiteral("31"));
    CHECK(analyses[0].spurOrder == QStringLiteral("7"));
    CHECK_FALSE(analyses[0].mismatch);
    CHECK(analyses[0].path == path);
    CHECK(analyses[0].start.isEmpty());
    CHECK(linearAnalyses(r.sheet->simulation()).isEmpty());

    r.sheet->undoStack()->undo();
    CHECK(budgetAnalyses(r.sheet->simulation()).isEmpty());
    r.sheet->undoStack()->redo();
    REQUIRE(budgetAnalyses(r.sheet->simulation()).size() == 1);

    // The stored path runs (the explicit path is accepted by the tracer).
    const BudgetRun run = runBudgetAnalysis(*r.project, *r.document, budgetAnalyses(r.sheet->simulation()).first());
    INFO((run.errors + run.chain.errors).join(QLatin1Char('\n')).toStdString());
    CHECK(run.chain.ok());

    // Editing keeps unknown keys of the analysis and replaces the rest.
    QJsonObject simulation = r.sheet->simulation();
    QJsonArray list = simulation.value(QStringLiteral("analyses")).toArray();
    QJsonObject object = list.at(0).toObject();
    object.insert(QStringLiteral("future"), QStringLiteral("kept"));
    list.replace(0, object);
    simulation.insert(QStringLiteral("analyses"), list);
    r.sheet->setSimulation(simulation);
    const int undoEdit = r.sheet->undoStack()->count();

    BudgetDialog edit(r.project, r.document, id);
    CHECK_FALSE(edit.isNew());
    CHECK(edit.nameEdit()->text() == QStringLiteral("Receiver"));
    CHECK(edit.sweepGroup()->isChecked());
    CHECK(edit.path() == path);
    edit.pathList()->clear();
    edit.sweepGroup()->setChecked(false);
    edit.bandwidthEdit()->setText(QStringLiteral("1MHz"));
    CHECK(edit.startCombo()->isEnabled());
    edit.startCombo()->setCurrentIndex(edit.startCombo()->findData(QStringLiteral("SRC1")));
    CHECK(edit.store() == id);
    CHECK(r.sheet->undoStack()->count() == undoEdit + 1);
    analyses = budgetAnalyses(r.sheet->simulation());
    REQUIRE(analyses.size() == 1);
    CHECK(analyses[0].bandwidth == QStringLiteral("1MHz"));
    CHECK(analyses[0].path.isEmpty());
    CHECK(analyses[0].start == QStringLiteral("SRC1"));
    CHECK(analyses[0].sweepStart.isEmpty());
    CHECK(r.sheet->simulation().value(QStringLiteral("analyses")).toArray().at(0).toObject().value(QStringLiteral("future"))
          == QStringLiteral("kept"));
    r.sheet->undoStack()->undo();
    CHECK(budgetAnalyses(r.sheet->simulation())[0].bandwidth == QStringLiteral("200kHz"));
}

TEST_CASE("the budget dialog chooses a branch by the exit pin")
{
    Receiver r;
    const QString src = r.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("1GHz")}});
    const QString spl = r.place("rf-system:splitter", {200, 0});
    const QString t1 = r.place("rf-system:termination", {400, -100});
    const QString a2 = r.place("rf-system:attenuator", {400, 100}, {{QStringLiteral("il"), QStringLiteral("10")}});
    const QString t2 = r.place("rf-system:termination", {600, 100});
    r.wire(src, "out", spl, "in");
    r.wire(spl, "out1", t1, "p");
    r.wire(spl, "out2", a2, "in");
    r.wire(a2, "out", t2, "p");

    BudgetDialog dialog(r.project, r.document);
    dialog.tracePath();
    REQUIRE(dialog.path() == (QStringList{QStringLiteral("SRC1"), QStringLiteral("SPL1.out1"), QStringLiteral("TERM1")}));
    dialog.pathList()->setCurrentRow(1);
    CHECK(dialog.exitPinCombo()->isEnabled());
    CHECK(dialog.exitPinCombo()->findData(QStringLiteral("out2")) >= 0);
    dialog.setExitPin(1, QStringLiteral("out2"));
    CHECK(dialog.path().at(1) == QStringLiteral("SPL1.out2"));
    // The rest of the path is traced automatically after the given entries.
    dialog.pathList()->takeItem(2);
    CHECK(dialog.chainPreview().isEmpty() == false);
    QString id;
    {
        BudgetAnalysisSetup setup = dialog.setup();
        CHECK(setup.path == (QStringList{QStringLiteral("SRC1"), QStringLiteral("SPL1.out2")}));
    }
    id = dialog.store();
    REQUIRE_FALSE(id.isEmpty());
    const BudgetRun run = runBudgetAnalysis(*r.project, *r.document, budgetAnalyses(r.sheet->simulation()).first());
    REQUIRE(run.chain.ok());
    CHECK(run.chain.end == QStringLiteral("TERM2"));
}

TEST_CASE("a budget runs in the background and fills the table, level diagram and frequency plan")
{
    Receiver r;
    r.superheterodyne();
    const QString id = r.addBudget();

    SimulationView view(r.project, r.document);
    view.resize(1300, 800);
    CHECK(view.currentAnalysisId() == id);
    CHECK(view.currentAnalysisType() == QStringLiteral("rf-budget"));
    auto *stack = view.findChild<QStackedWidget *>(QStringLiteral("resultStack"));
    REQUIRE(stack);
    CHECK(stack->currentWidget() == view.budgetView());
    CHECK_FALSE(view.runAction()->isEnabled() == false);

    QSignalSpy spy(&view, &SimulationView::runFinished);
    view.run();
    CHECK(view.isRunning());
    REQUIRE(waitForBudget(view, spy));
    REQUIRE(spy.size() == 1);
    CHECK(spy.first().at(0).toBool());
    const BudgetRun *run = view.lastBudgetRun();
    REQUIRE(run);
    REQUIRE(run->ok());
    CHECK(view.lastResult() == nullptr);
    CHECK(view.errors().isEmpty());
    const BudgetResult &b = run->budget;
    REQUIRE(b.stages.size() == 5); // four stages and the ADC
    CHECK(b.adcAliasFrequency == Approx(30e6));

    // Table: input column plus one per stage, values from the result.
    sim::BudgetView *budgetView = view.budgetView();
    const sim::BudgetTableModel &model = *budgetView->tableModel();
    using Row = sim::BudgetTableModel::Row;
    REQUIRE(model.columnCount() == 6);
    CHECK(model.rowOf(Row::AdcAlias) >= 0);
    CHECK(cell(model, Row::Signal, 0) == Approx(-60));
    CHECK(cell(model, Row::Noise, 0) == Approx(b.inputNoiseDbm));
    for (int c = 1; c <= 5; ++c) {
        const BudgetStage &s = b.stages[static_cast<size_t>(c - 1)];
        INFO("stage " << s.path.toStdString());
        CHECK(cellText(model, Row::Component, c) == s.path);
        CHECK(same(cell(model, Row::StageGain, c), s.gainDb));
        CHECK(same(cell(model, Row::StageNf, c), s.nfDb));
        CHECK(same(cell(model, Row::CumulativeGain, c), s.cumulativeGainDb));
        CHECK(same(cell(model, Row::CumulativeNf, c), s.cumulativeNfDb));
        CHECK(same(cell(model, Row::Ip3, c), s.iip3Dbm));
        CHECK(same(cell(model, Row::P1db, c), s.ip1dbDbm));
        CHECK(same(cell(model, Row::Iip2, c), s.iip2Dbm));
        CHECK(same(cell(model, Row::Signal, c), s.signalDbm));
        CHECK(same(cell(model, Row::Noise, c), s.noiseDbm));
        CHECK(same(cell(model, Row::Snr, c), s.snrDb));
        CHECK(same(cell(model, Row::Sfdr, c), s.sfdrDb));
        CHECK(same(cell(model, Row::Headroom, c), s.headroomDb));
        CHECK(cellText(model, Row::CumulativeGain, c) == QString::number(s.cumulativeGainDb, 'f', 2));
        CHECK(model.data(model.index(model.rowOf(Row::Headroom), c), Qt::BackgroundRole).isValid() == s.compressed);
    }
    CHECK(cellText(model, Row::Frequency, 3).contains(QStringLiteral("→"))); // the mixer
    CHECK(cellText(model, Row::Frequency, 3).endsWith(QStringLiteral("70 MHz (difference)"))); // with the product
    CHECK(cellText(model, Row::AdcAlias, 5) == QStringLiteral("30 MHz"));
    CHECK(same(cell(model, Row::AdcHeadroom, 5), b.adcHeadroomDb));
    CHECK(same(cell(model, Row::AdcSnr, 5), b.adcQuantisationSnrDb));
    CHECK(cellText(model, Row::AdcAlias, 4).isEmpty());
    CHECK(budgetView->summary().contains(QStringLiteral("actual S-parameters")));

    // Clipboard and CSV, with full precision and split pairs.
    view.copyTable();
    const QString copied = QApplication::clipboard()->text();
    CHECK(copied.startsWith(QStringLiteral("Quantity\tInput\t") + b.stages[0].path));
    CHECK(copied.contains(QStringLiteral("Output frequency [Hz]")));
    CHECK(copied.contains(QStringLiteral("OIP3 (cumulative) [dBm]")));
    QString error;
    const QString csvPath = r.dir.filePath(QStringLiteral("budget.csv"));
    REQUIRE(view.exportCsv(csvPath, &error));
    QFile csv(csvPath);
    REQUIRE(csv.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(csv.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    REQUIRE(lines.size() > 20);
    CHECK(lines[0].startsWith(QStringLiteral("Quantity,Input,")));
    bool foundGain = false;
    for (const QString &line : lines) {
        if (line.startsWith(QStringLiteral("Cumulative gain [dB],"))) {
            foundGain = true;
            const QStringList fields = line.split(QLatin1Char(','));
            REQUIRE(fields.size() == 7);
            CHECK(fields[5].toDouble() == Approx(b.stages[4].cumulativeGainDb).epsilon(1e-10));
        }
    }
    CHECK(foundGain);

    // Level diagram: x = stage number.
    PlotWidget *level = budgetView->levelPlot();
    using LT = sim::BudgetView::LevelTrace;
    CHECK(traceValue(level, budgetView->levelTrace(LT::Signal), 0) == Approx(-60));
    CHECK(traceValue(level, budgetView->levelTrace(LT::Signal), 3) == Approx(b.stages[2].signalDbm));
    CHECK(traceValue(level, budgetView->levelTrace(LT::Noise), 2) == Approx(b.stages[1].noiseDbm));
    CHECK(traceValue(level, budgetView->levelTrace(LT::Gain), 5) == Approx(b.stages[4].cumulativeGainDb));
    CHECK(traceValue(level, budgetView->levelTrace(LT::Nf), 4) == Approx(b.stages[3].cumulativeNfDb));
    CHECK(level->traceStyle(budgetView->levelTrace(LT::Gain)).yAxis == PlotYAxis::Right);
    CHECK(level->traceStyle(budgetView->levelTrace(LT::Signal)).yAxis == PlotYAxis::Left);
    const bool anyOp1db = std::ranges::any_of(b.stages, [](const BudgetStage &s) { return std::isfinite(s.op1dbCumulativeDbm); });
    CHECK((budgetView->levelTrace(LT::Op1db) >= 0) == anyOp1db);
    const QString legend = budgetView->stageLegend();
    CHECK(legend.contains(QStringLiteral("0 Input (SRC1)")));
    for (size_t i = 0; i < b.stages.size(); ++i)
        CHECK(legend.contains(QStringLiteral("%1 %2").arg(i + 1).arg(b.stages[i].path)));

    // Frequency plan: start, stages, end; the mixer's LO.
    QTableWidget *plan = budgetView->frequencyPlan();
    REQUIRE(plan->rowCount() == static_cast<int>(run->chain.stages.size()) + 2);
    int mixerRow = -1;
    for (int row = 0; row < plan->rowCount(); ++row) {
        if (plan->item(row, 6) && plan->item(row, 6)->text() == QStringLiteral("LO1"))
            mixerRow = row;
    }
    REQUIRE(mixerRow == 3);
    CHECK(plan->item(mixerRow, 7)->text() == QStringLiteral("2.38 GHz"));
    CHECK(plan->item(mixerRow, 8)->text() == QStringLiteral("13.0 dBm"));
    CHECK(plan->item(mixerRow, 4)->text() == QStringLiteral("70 MHz"));
    CHECK(plan->item(plan->rowCount() - 1, 1)->text() == run->chain.end);

    // Spurs: whatever the spur analysis does, the view stays consistent.
    if (!run->spurs.errors.isEmpty()) {
        CHECK(budgetView->spurMessage().contains(run->spurs.errors.first()));
        CHECK(view.warnings().join(QLatin1Char('\n')).contains(QStringLiteral("spur analysis")));
        CHECK(budgetView->spurModel()->rowCount() == 0);
    } else {
        CHECK(budgetView->spurModel()->rowCount() == static_cast<int>(run->spurs.spurs.size()));
    }
    CHECK_FALSE(view.findChild<QWidget *>(QStringLiteral("errorBanner"))->isVisibleTo(&view));

    // A strong input compresses the chain: the headroom cells are highlighted.
    BudgetAnalysisSetup strong = budgetAnalyses(r.sheet->simulation()).first();
    strong.power = QStringLiteral("10");
    strong.spurs = false;
    r.sheet->setSimulation(withBudgetAnalysis(r.sheet->simulation(), strong));
    spy.clear();
    view.run();
    REQUIRE(waitForBudget(view, spy));
    const BudgetRun *compressed = view.lastBudgetRun();
    REQUIRE(compressed);
    REQUIRE(compressed->ok());
    int highlighted = 0;
    for (int c = 1; c < model.columnCount(); ++c) {
        const BudgetStage &s = compressed->budget.stages[static_cast<size_t>(c - 1)];
        const bool bad = s.compressed || s.headroomDb < 0;
        CHECK(model.data(model.index(model.rowOf(Row::Headroom), c), Qt::BackgroundRole).isValid() == bad);
        highlighted += bad ? 1 : 0;
    }
    CHECK(highlighted > 0);
    CHECK(budgetView->spurMessage().contains(QStringLiteral("off")));
}

TEST_CASE("a swept budget shows gain, NF and IIP3 against the input frequency")
{
    Receiver r;
    r.superheterodyne();
    BudgetAnalysisSetup setup;
    setup.sweepStart = QStringLiteral("2.35GHz");
    setup.sweepStop = QStringLiteral("2.55GHz");
    setup.sweepPoints = QStringLiteral("21");
    setup.spurs = false;
    r.addBudget(setup);

    SimulationView view(r.project, r.document);
    QSignalSpy spy(&view, &SimulationView::runFinished);
    view.run();
    REQUIRE(waitForBudget(view, spy));
    const BudgetRun *run = view.lastBudgetRun();
    REQUIRE(run);
    REQUIRE(run->ok());
    REQUIRE(run->sweep.size() == 21);

    sim::BudgetView *budgetView = view.budgetView();
    REQUIRE(budgetView->hasSweep());
    using ST = sim::BudgetView::SweepTrace;
    PlotWidget *plot = budgetView->sweepPlot();
    const int gain = budgetView->sweepTrace(ST::Gain);
    const int nf = budgetView->sweepTrace(ST::Nf);
    REQUIRE(gain >= 0);
    REQUIRE(nf >= 0);
    CHECK(plot->traceStyle(nf).yAxis == PlotYAxis::Right);
    for (size_t i : {size_t(0), size_t(10), size_t(20)}) {
        const double f = run->sweepFrequencies[i];
        CHECK(traceValue(plot, gain, f) == Approx(run->sweep[i].stages.back().cumulativeGainDb).margin(1e-6));
        CHECK(traceValue(plot, nf, f) == Approx(run->sweep[i].stages.back().cumulativeNfDb).margin(1e-6));
    }
    const bool anyIip3 = std::ranges::any_of(run->sweep, [](const BudgetResult &point) {
        return !point.stages.empty() && std::isfinite(point.stages.back().iip3Dbm);
    });
    CHECK((budgetView->sweepTrace(ST::Iip3) >= 0) == anyIip3);
    CHECK(plot->markers().size() == 1); // the wanted input frequency

    // At the LNA output.
    budgetView->setSweepStage(2);
    CHECK(budgetView->sweepStageCombo()->currentData().toInt() == 2);
    const int lnaGain = budgetView->sweepTrace(ST::Gain);
    CHECK(traceValue(plot, lnaGain, run->sweepFrequencies[10])
          == Approx(run->sweep[10].stages[2].cumulativeGainDb).margin(1e-6));
    budgetView->setSweepStage(-1);
    CHECK(budgetView->sweepStageCombo()->currentIndex() == 0);
}

TEST_CASE("the spur table sorts, filters and highlights in-band spurs")
{
    auto run = std::make_shared<BudgetRun>();
    BudgetStage stage;
    stage.path = QStringLiteral("MIX1");
    stage.inputFrequency = 2.45e9;
    stage.outputFrequency = 70e6;
    stage.signalDbm = -40;
    stage.cumulativeGainDb = 20;
    run->budget.start = QStringLiteral("SRC1");
    run->budget.inputFrequency = 2.45e9;
    run->budget.inputPowerDbm = -60;
    run->budget.bandwidth = 2e6;
    run->budget.stages.push_back(stage);
    ChainStage mixer;
    mixer.path = QStringLiteral("MIX1");
    mixer.translation = ChainStage::Translation::Mixer;
    run->chain.stages.push_back(mixer);
    run->spurs.wantedOutputFrequency = 70e6;
    run->spurs.wantedOutputDbm = -40;
    const auto spur = [](Spur::Kind kind, int m, int n, double f, double level, bool inBand) {
        Spur s;
        s.kind = kind;
        s.mixer = QStringLiteral("MIX1");
        s.m = m;
        s.n = n;
        s.frequency = f;
        s.levelDbm = level;
        s.levelDbc = -40 - level;
        s.inBand = inBand;
        s.estimated = true;
        return s;
    };
    run->spurs.spurs = {spur(Spur::Kind::Product, 2, -2, 70.5e6, -95, true),
                        spur(Spur::Kind::Product, 1, 1, 4.83e9, -45, false),
                        spur(Spur::Kind::LoLeakage, 0, 1, 2.38e9, -60, false),
                        spur(Spur::Kind::Image, 1, -1, 70e6, -100, true)};
    run->spurs.spurs[3].inputFrequency = 2.31e9;
    run->spurs.spurs[3].rejectionDb = 60;

    sim::BudgetView view;
    view.resize(1000, 700);
    view.setRun(run);
    CHECK(view.spurMessage().isEmpty());
    sim::SpurTableModel *model = view.spurModel();
    REQUIRE(model->rowCount() == 4);
    CHECK(model->data(model->index(0, sim::SpurTableModel::Order)).toString() == QStringLiteral("2×-2"));
    CHECK(model->data(model->index(3, sim::SpurTableModel::Kind)).toString() == QStringLiteral("Image"));
    CHECK(model->data(model->index(3, sim::SpurTableModel::InputFrequency)).toString() == QStringLiteral("2.31 GHz"));
    CHECK(model->data(model->index(0, sim::SpurTableModel::InBand), Qt::BackgroundRole).isValid());
    CHECK_FALSE(model->data(model->index(1, sim::SpurTableModel::InBand), Qt::BackgroundRole).isValid());

    // Sorting by level, strongest first.
    view.spurView()->sortByColumn(sim::SpurTableModel::Level, Qt::DescendingOrder);
    sim::SpurFilterModel *filter = view.spurFilter();
    CHECK(filter->data(filter->index(0, sim::SpurTableModel::Level), Qt::EditRole).toDouble() == Approx(-45));
    CHECK(filter->data(filter->index(3, sim::SpurTableModel::Level), Qt::EditRole).toDouble() == Approx(-100));
    view.spurView()->sortByColumn(sim::SpurTableModel::Frequency, Qt::AscendingOrder);
    CHECK(filter->data(filter->index(0, sim::SpurTableModel::Frequency), Qt::EditRole).toDouble() == Approx(70e6));

    // In-band only.
    view.inBandOnlyCheck()->setChecked(true);
    REQUIRE(filter->rowCount() == 2);
    for (int row = 0; row < 2; ++row)
        CHECK(filter->data(filter->index(row, sim::SpurTableModel::InBand), Qt::EditRole).toBool());
    view.inBandOnlyCheck()->setChecked(false);
    CHECK(filter->rowCount() == 4);

    // Text export and the chart (products and LO leakage, the wanted output and band).
    const QString text = model->text(QLatin1Char(','));
    CHECK(text.startsWith(QStringLiteral("Kind,Mixer,m,n,Frequency [Hz]")));
    CHECK(text.count(QLatin1Char('\n')) == 5);
    QStringList names;
    for (int id : view.spurPlot()->traceIds())
        names.append(view.spurPlot()->traceName(id));
    CHECK(names.contains(QStringLiteral("Wanted band")));
    CHECK(names.contains(QStringLiteral("Wanted output")));
    CHECK(names.contains(QStringLiteral("Spurs")));
    CHECK(names.contains(QStringLiteral("In-band spurs")));

    // A failed spur analysis is explained, not shown as an empty table.
    auto failed = std::make_shared<BudgetRun>();
    failed->budget = run->budget;
    failed->chain.stages = run->chain.stages;
    failed->spurs.errors.append(QStringLiteral("not implemented yet"));
    view.setRun(failed);
    CHECK(view.spurMessage().contains(QStringLiteral("not implemented yet")));
    CHECK(model->rowCount() == 0);
    CHECK(view.spurPlot()->traceIds().empty());

    // No mixer: no spurs, said so.
    auto linear = std::make_shared<BudgetRun>();
    linear->budget = run->budget;
    view.setRun(linear);
    CHECK(view.spurMessage().contains(QStringLiteral("no mixer")));
    view.setRun(nullptr);
    CHECK(view.tableModel()->rowCount() == 0);
}

TEST_CASE("budget analyses auto-run after edits and sit next to linear ones")
{
    Receiver r;
    r.superheterodyne();
    BudgetAnalysisSetup setup;
    setup.spurs = false;
    const QString budget = r.addBudget(setup);
    LinearAnalysisSetup linear;
    linear.name = QStringLiteral("Sweep");
    QString linearId;
    r.sheet->setSimulation(withLinearAnalysis(r.sheet->simulation(), linear, &linearId));

    SimulationView view(r.project, r.document);
    view.setAutoRunDelay(30);
    auto *combo = view.findChild<QComboBox *>(QStringLiteral("analysis"));
    REQUIRE(combo);
    REQUIRE(combo->count() == 2);
    CHECK(combo->itemText(0).contains(QStringLiteral("budget")));
    CHECK(combo->itemText(1).contains(QStringLiteral("linear")));
    CHECK(view.analysisType(linearId) == QStringLiteral("rf-linear"));
    CHECK(view.currentAnalysisId() == budget);

    QSignalSpy spy(&view, &SimulationView::runFinished);
    view.setAutoRun(true); // no result yet: runs
    REQUIRE(waitForBudget(view, spy));
    REQUIRE(view.lastBudgetRun());
    CHECK(view.lastBudgetRun()->budget.stages[1].gainDb == Approx(18).margin(1e-6));

    spy.clear();
    r.sheet->setParameter(r.lna, QStringLiteral("gain"), QStringLiteral("24"));
    REQUIRE(waitForBudget(view, spy));
    REQUIRE(view.lastBudgetRun());
    CHECK(view.lastBudgetRun()->budget.stages[1].gainDb == Approx(24).margin(1e-6));
    CHECK_FALSE(view.isOutdated());
    CHECK(view.budgetView()->tableModel()->data(
              view.budgetView()->tableModel()->index(
                  view.budgetView()->tableModel()->rowOf(sim::BudgetTableModel::Row::StageGain), 2),
              Qt::EditRole)
              .toDouble()
          == Approx(24).margin(1e-6));
    view.setAutoRun(false);

    // Switching to the linear analysis shows the plots; back again, the budget is still there.
    auto *stack = view.findChild<QStackedWidget *>(QStringLiteral("resultStack"));
    view.setCurrentAnalysis(linearId);
    CHECK(stack->currentWidget() != view.budgetView());
    CHECK(view.lastBudgetRun() == nullptr);
    CHECK(view.addPlot(QStringLiteral("rectangular")).isEmpty() == false);
    view.setCurrentAnalysis(budget);
    CHECK(stack->currentWidget() == view.budgetView());
    REQUIRE(view.lastBudgetRun());
    CHECK(view.addPlot(QStringLiteral("rectangular")).isEmpty());

    // Without auto-run an edit only marks the budget as outdated.
    r.sheet->setParameter(r.lna, QStringLiteral("gain"), QStringLiteral("20"));
    QTest::qWait(150);
    CHECK(view.isOutdated());
    CHECK_FALSE(view.isRunning());

    // A bad expression is reported without a crash, and deleting removes the result.
    BudgetAnalysisSetup bad = budgetAnalyses(r.sheet->simulation()).first();
    bad.bandwidth = QStringLiteral("nonsense+");
    r.sheet->setSimulation(withBudgetAnalysis(r.sheet->simulation(), bad));
    spy.clear();
    view.run();
    REQUIRE(waitForBudget(view, spy));
    CHECK_FALSE(spy.first().at(0).toBool());
    CHECK(view.errors().join(QString()).contains(QStringLiteral("bandwidth")));
    view.deleteAnalysis(budget);
    CHECK(view.currentAnalysisId() == linearId);
    CHECK(budgetAnalyses(r.sheet->simulation()).isEmpty());
}

TEST_CASE("phase noise, a blocker and a DAC add their rows, traces and dialog fields")
{
    Receiver r;
    const QString dac = r.place("rf-system:dac", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("100MHz")}});
    const QString mixer = r.place("rf-system:mixer", {200, 0}, {{QStringLiteral("product"), QStringLiteral("sum")}});
    const QString pll = r.place("rf-system:pll", {200, 200}, {{QStringLiteral("freq"), QStringLiteral("2GHz")}});
    const QString bpf = r.place("rf-system:filter", {400, 0},
                                {{QStringLiteral("fc"), QStringLiteral("2.1GHz")}, {QStringLiteral("bw"), QStringLiteral("100MHz")}});
    const QString det = r.place("rf-system:detector", {600, 0});
    r.wire(dac, "out", mixer, "rf");
    r.wire(pll, "out", mixer, "lo");
    r.wire(mixer, "if", bpf, "in");
    r.wire(bpf, "out", det, "in");

    BudgetDialog dialog(r.project, r.document);
    CHECK(dialog.startCandidates() == QStringList{QStringLiteral("DAC1")});
    CHECK(dialog.isValid());
    dialog.blockerPowerEdit()->setText(QStringLiteral("-20"));
    CHECK_FALSE(dialog.isValid()); // the offset is required with a power
    dialog.blockerOffsetEdit()->setText(QStringLiteral("1MHz"));
    dialog.phaseNoiseStartEdit()->setText(QStringLiteral("1kHz"));
    dialog.phaseNoiseStopEdit()->setText(QStringLiteral("500Hz"));
    CHECK_FALSE(dialog.isValid()); // stop below start
    dialog.phaseNoiseStopEdit()->setText(QStringLiteral("400kHz"));
    CHECK(dialog.isValid());
    const QString id = dialog.store();
    REQUIRE_FALSE(id.isEmpty());
    const BudgetAnalysisSetup setup = budgetAnalyses(r.sheet->simulation()).first();
    CHECK(setup.blockerOffset == QStringLiteral("1MHz"));
    CHECK(setup.phaseNoiseStop == QStringLiteral("400kHz"));
    BudgetDialog again(r.project, r.document, id);
    CHECK(again.blockerPowerEdit()->text() == QStringLiteral("-20"));

    auto run = std::make_shared<BudgetRun>(runBudgetAnalysis(*r.project, *r.document, setup));
    INFO(run->errors.join(QLatin1Char('\n')).toStdString());
    REQUIRE(run->ok());
    sim::BudgetView view;
    view.setRun(run);
    const sim::BudgetTableModel &model = *view.tableModel();
    using Row = sim::BudgetTableModel::Row;
    for (const Row row : {Row::ThermalSnr, Row::LoPhaseNoise, Row::PhaseNoiseLevel, Row::BlockerLevel, Row::ReciprocalMixing,
                          Row::DacImageFrequency, Row::DacImage, Row::DetectorMargin, Row::DetectorHeadroom})
        CHECK(model.rowOf(row) >= 0);
    CHECK(model.rowOf(Row::AdcHeadroom) < 0);
    const BudgetStage &stage = run->budget.stages.front();
    CHECK(same(cell(model, Row::LoPhaseNoise, 1), stage.loPhaseNoiseDbc));
    CHECK(same(cell(model, Row::ReciprocalMixing, 1), stage.reciprocalMixingDbm));
    CHECK(same(cell(model, Row::DacImageFrequency, 0), 900e6)); // fs 1 GHz - 100 MHz
    CHECK(same(cell(model, Row::DetectorMargin, 2), run->budget.detectorMarginDb));
    CHECK(std::isnan(cell(model, Row::LoPhaseNoise, 2))); // only the mixer has an LO
    CHECK(view.summary().contains(QStringLiteral("SNR limits")));
    CHECK(view.summary().contains(QStringLiteral("DAC")));
    CHECK(view.levelTrace(sim::BudgetView::LevelTrace::TotalNoise) >= 0);
    // The export carries the new rows.
    const QString csv = view.tableText(QLatin1Char(','));
    CHECK(csv.contains(QStringLiteral("Reciprocal mixing level [dBm]")));
    CHECK(csv.contains(QStringLiteral("DAC image frequency [Hz]")));
}
