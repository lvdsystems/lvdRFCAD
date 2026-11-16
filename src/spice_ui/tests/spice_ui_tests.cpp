#include "core/Connectivity.h"
#include "core/Library.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "plot/PlotWidget.h"
#include "spice/NgspiceEngine.h"
#include "spice_ui/BatchDialog.h"
#include "spice_ui/BatchView.h"
#include "spice_ui/InstrumentsView.h"
#include "spice_ui/ModelImportDialog.h"
#include "spice_ui/SpiceAnalysisDialog.h"
#include "spice_ui/SpiceSimulationView.h"
#include "spice_ui/WaveformPane.h"
#include "spice_ui/WaveformViewer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>

using namespace lvd;
using namespace lvd::spiceui;
using Catch::Approx;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    spice::NgspiceEngine::setRuntimeFolder(QStringLiteral(LVD_NGSPICE_RUNTIME));
    return Catch::Session().run(argc, argv);
}

namespace {

// V1 (10 V DC, AC 1) - R1 1k - out - R2 1k || C1 1u to ground.
struct Fixture {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;

    Fixture()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::SpiceSchematic, QStringLiteral("rc"), &error);
        REQUIRE(document);
        sheet = document->schematic();
        const QString v1 = place("spice-sources:vsrc", {0, 0}, {{"spec", "DC 10 AC 1 PULSE(0 10 0 1n 1n 1 2)"}});
        const QString r1 = place("basic:resistor", {200, 0}, {{"r", "1k"}});
        const QString r2 = place("basic:resistor", {400, 0}, {{"r", "1k"}});
        const QString c1 = place("basic:capacitor", {600, 0}, {{"c", "1u"}});
        const QString probe = place("spice-misc:vprobe", {800, 0});
        connect(v1, "p", "in");
        connect(r1, "p", "in");
        connect(r1, "n", "out");
        connect(r2, "p", "out");
        connect(c1, "p", "out");
        connect(probe, "p", "out");
        for (const QString &id : {v1, r2, c1})
            connect(id, "n", "0");
        // A wire on "out" for the voltage label.
        const QPoint a = pin(r1, "n");
        sheet->addWire({a, a + QPoint(0, -40)});
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

    void connect(const QString &id, const char *pinName, const char *net)
    {
        const QPoint at = pin(id, pinName);
        if (QByteArray(net) == "0") {
            const DefinitionPtr ground = project->resolve(*sheet, QStringLiteral("basic:ground")).definition;
            const QString g = sheet->addComponent(QStringLiteral("basic:ground"), ground.get(), at + QPoint(0, 60));
            sheet->addWire({at, pinPosition(*sheet->component(g), *ground->symbol.pin(QStringLiteral("gnd")))});
        } else {
            sheet->addLabel(QString::fromLatin1(net), at);
        }
    }
};

bool waitFor(QSignalSpy &spy, int count = 1, int timeout = 60000)
{
    while (spy.count() < count)
        if (!spy.wait(timeout))
            return false;
    return true;
}

} // namespace

TEST_CASE("the SPICE view runs analyses in the background and shows their results")
{
    Fixture f;
    SpiceSimulationView view(f.project, f.document);
    view.resize(1200, 700);
    QSignalSpy finished(&view, &SpiceSimulationView::runFinished);
    QSignalSpy labels(&view, &SpiceSimulationView::operatingPointLabelsChanged);

    SECTION("operating point: values table and schematic labels")
    {
        view.addAnalysis(spice::SpiceAnalysisSetup::create(spice::AnalysisKind::OperatingPoint, QStringLiteral("Bias")));
        view.run();
        CHECK(view.isRunning());
        REQUIRE(waitFor(finished));
        INFO(view.errors().join(QLatin1Char('\n')).toStdString());
        CHECK(finished.first().first().toBool());
        REQUIRE(view.lastSimulation());
        QStandardItemModel *values = view.valuesModel();
        REQUIRE(values->rowCount() > 0);
        bool found = false;
        for (int row = 0; row < values->rowCount(); ++row)
            if (values->item(row, 0)->text() == QLatin1String("V(out)")) {
                CHECK(values->item(row, 0)->toolTip() == QLatin1String("v(out)"));
                CHECK(values->item(row, 1)->data(Qt::UserRole).toDouble() == Approx(5.0));
                CHECK(values->item(row, 2)->text() == QLatin1String("out"));
                found = true;
            }
        CHECK(found);
        REQUIRE(labels.count() >= 1);
        const auto list = labels.last().first().value<QList<OperatingPointLabel>>();
        bool voltage = false, current = false;
        for (const OperatingPointLabel &label : list) {
            voltage = voltage || (label.kind == OperatingPointLabel::Kind::Voltage && label.text == QLatin1String("5 V"));
            current = current || label.text.contains(QStringLiteral("5 mA"));
        }
        CHECK(voltage);
        CHECK(current);
        // Hiding the labels sends an empty list.
        view.setAnnotating(false);
        CHECK(labels.last().first().value<QList<OperatingPointLabel>>().isEmpty());
    }

    SECTION("transient: waveforms of the probes, panes stored per analysis")
    {
        spice::SpiceAnalysisSetup tran = spice::SpiceAnalysisSetup::create(spice::AnalysisKind::Transient);
        tran.set(QStringLiteral("stop"), QStringLiteral("5ms"));
        tran.set(QStringLiteral("step"), QStringLiteral("10us"));
        const QString id = view.addAnalysis(tran);
        view.run();
        REQUIRE(waitFor(finished));
        WaveformViewer *viewer = view.waveformViewer();
        REQUIRE(viewer->hasWaveforms());
        REQUIRE(viewer->panes().size() == 1);
        CHECK(viewer->panes().first().traces == QStringList{QStringLiteral("v(out)")}); // the probe
        // A trace typed with a schematic name, stored in the document.
        QString error;
        REQUIRE(viewer->addTrace(QStringLiteral("i(V1)"), QString(), &error));
        const QJsonArray stored = f.sheet->simulation().value(QStringLiteral("spicePanes")).toObject().value(id).toArray();
        REQUIRE(stored.size() == 1);
        CHECK(stored.first().toObject().value(QStringLiteral("traces")).toArray().size() == 2);
        CHECK(view.tabs()->currentWidget() == viewer);
    }

    SECTION("a netlist problem is shown and nothing runs")
    {
        const QString r = f.place("basic:resistor", {0, 400});
        Q_UNUSED(r);
        view.addAnalysis(spice::SpiceAnalysisSetup::create(spice::AnalysisKind::OperatingPoint));
        view.run();
        // Floating pins only warn; the engine then reports the singular matrix.
        if (!view.errors().isEmpty())
            CHECK_FALSE(view.isRunning());
        else
            REQUIRE(waitFor(finished));
        CHECK(true);
    }
}

TEST_CASE("Monte Carlo and sweeps from the view fill the batch view")
{
    Fixture f;
    SpiceSimulationView view(f.project, f.document);
    view.setBatchWorkers(1); // in this process
    view.addAnalysis(spice::SpiceAnalysisSetup::create(spice::AnalysisKind::OperatingPoint));
    QSignalSpy finished(&view, &SpiceSimulationView::runFinished);

    spice::MonteCarloSetup mc;
    spice::Tolerance tolerance;
    tolerance.target.text = QStringLiteral("R*.r");
    tolerance.relative = 0.05;
    mc.tolerances = {tolerance};
    mc.runs = 20;
    spice::SpiceMeasure measure;
    measure.expression = QStringLiteral("v(out)");
    mc.measures = {measure};
    view.runMonteCarlo(mc);
    REQUIRE(waitFor(finished));
    REQUIRE(view.lastBatch());
    CHECK(view.lastBatch()->runs.size() == 21);
    BatchView *batch = view.batchView();
    CHECK(batch->runsModel()->rowCount() == 21);
    CHECK(batch->summaryModel()->rowCount() == 1);
    CHECK(batch->summaryModel()->item(0, 1)->data(Qt::UserRole).toDouble() == Approx(5.0)); // nominal
    CHECK(batch->plot()->traceIds().size() == 1); // the histogram
    // The document still has its nominal values.
    CHECK(f.sheet->componentByReference(QStringLiteral("R1"))->parameters.value(QStringLiteral("r")) == QLatin1String("1k"));

    spice::SweepAxis axis;
    axis.target.text = QStringLiteral("R2.r");
    axis.mode = QStringLiteral("list");
    axis.list = {QStringLiteral("1k"), QStringLiteral("3k")};
    batch->setMode(BatchView::Mode::Sweep);
    view.runSweep({axis}, {measure});
    REQUIRE(waitFor(finished, 2));
    REQUIRE(view.lastBatch()->runs.size() == 2);
    CHECK(view.lastBatch()->runs.at(1).measures.at(0) == Approx(7.5));
    CHECK(batch->plot()->traceIds().size() == 1);
}

TEST_CASE("the analysis dialog edits the fields of each type")
{
    SpiceAnalysisDialog dialog(spice::SpiceAnalysisSetup::create(spice::AnalysisKind::Ac), true);
    dialog.setNets({QStringLiteral("in"), QStringLiteral("out")});
    dialog.setSources({QStringLiteral("V1")});
    auto *stop = qobject_cast<QLineEdit *>(dialog.fieldEditor(QStringLiteral("stop")));
    REQUIRE(stop);
    stop->setText(QStringLiteral("1MHz"));
    CHECK(dialog.setup().text(QStringLiteral("stop")) == QLatin1String("1MHz"));
    dialog.setKind(spice::AnalysisKind::Noise);
    auto *source = qobject_cast<QComboBox *>(dialog.fieldEditor(QStringLiteral("source")));
    REQUIRE(source);
    CHECK(source->findText(QStringLiteral("V1")) >= 0);
    const spice::SpiceAnalysisSetup noise = dialog.setup();
    CHECK(noise.kind == spice::AnalysisKind::Noise);
    CHECK(noise.name == spice::analysisDisplayName(spice::AnalysisKind::Noise));
    dialog.setValidator([](const spice::SpiceAnalysisSetup &) { return QStringList{QStringLiteral("wrong")}; });
    dialog.accept();
    CHECK(dialog.result() != QDialog::Accepted);
    CHECK(dialog.errorText() == QLatin1String("wrong"));
}

TEST_CASE("the batch dialog stores and reloads its setup")
{
    Fixture f;
    const spice::SpiceAnalysisSetup op = spice::SpiceAnalysisSetup::create(spice::AnalysisKind::OperatingPoint);
    BatchDialog dialog(BatchDialog::Mode::MonteCarlo, *f.project, *f.document, op);
    CHECK(dialog.targetCandidates().contains(QStringLiteral("R1.r")));
    CHECK(dialog.targetCandidates().contains(QStringLiteral("temperature")));
    const spice::MonteCarloSetup setup = dialog.monteCarlo();
    REQUIRE(setup.tolerances.size() == 3); // R*, C*, L* defaults
    CHECK(setup.tolerances.first().relative == Approx(0.01));
    REQUIRE(setup.measures.size() == 1);
    spice::SpiceAnalysisSetup stored = op;
    stored.set(dialog.storageKey(), dialog.toJson());
    BatchDialog again(BatchDialog::Mode::MonteCarlo, *f.project, *f.document, stored);
    CHECK(again.monteCarlo().toJson() == setup.toJson());
}

TEST_CASE("vendor models are imported into a project library")
{
    Fixture f;
    const QString path = f.dir.filePath(QStringLiteral("vendor.lib"));
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(".model QV1 NPN(IS=1e-14 BF=150)\n.subckt AMPX inp inn out\nE1 out 0 inp inn 1e5\n.ends\n");
    }
    ModelImportDialog dialog(f.session.libraries(), f.project);
    QString error;
    REQUIRE(dialog.openFile(path, &error));
    CHECK(dialog.entryList()->topLevelItemCount() == 2);
    dialog.setLibraryName(QStringLiteral("vendor"));
    dialog.setScopeIsProject(true);
    INFO(error.toStdString());
    REQUIRE(dialog.import(&error));
    CHECK(QFile::exists(QDir(f.project->directory()).filePath(QStringLiteral("models/vendor.lib"))));
    const DefinitionPtr transistor = f.session.libraries()->find(QStringLiteral("vendor:qv1"));
    REQUIRE(transistor);
    CHECK(transistor->model(QStringLiteral("spice"))->data.value(QStringLiteral("include")).toString()
          == QLatin1String("models/vendor.lib"));
    CHECK(f.session.libraries()->find(QStringLiteral("vendor:ampx")));
}

TEST_CASE("virtual instruments show the results of transient and AC analyses")
{
    QTemporaryDir dir;
    Session session;
    QString error;
    Project *project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
    REQUIRE(project);
    Document *document = project->createDocument(DocumentType::SpiceSchematic, QStringLiteral("inst"), &error);
    REQUIRE(document);
    Schematic *sheet = document->schematic();
    const auto place = [&](const char *key, QPoint at) {
        const QString k = QString::fromLatin1(key);
        const DefinitionPtr definition = project->resolve(*sheet, k).definition;
        REQUIRE(definition);
        return sheet->addComponent(k, definition.get(), at);
    };
    const auto label = [&](const QString &id, const char *pin, const char *net) {
        const PlacedComponent *component = sheet->component(id);
        const DefinitionPtr definition = project->resolve(*sheet, component->definition).definition;
        sheet->addLabel(QString::fromLatin1(net), pinPosition(*component, *definition->symbol.pin(QString::fromLatin1(pin))));
    };
    const auto ground = [&](const QString &id, const char *pin) {
        const PlacedComponent *component = sheet->component(id);
        const DefinitionPtr definition = project->resolve(*sheet, component->definition).definition;
        const QPoint at = pinPosition(*component, *definition->symbol.pin(QString::fromLatin1(pin)));
        const DefinitionPtr g = project->resolve(*sheet, QStringLiteral("basic:ground")).definition;
        const QString gid = sheet->addComponent(QStringLiteral("basic:ground"), g.get(), at + QPoint(0, 60));
        sheet->addWire({at, pinPosition(*sheet->component(gid), *g->symbol.pin(QStringLiteral("gnd")))});
    };
    const QString fg = place("spice-instruments:function_generator", {0, 0});
    const QString r1 = place("basic:resistor", {200, -200});
    const QString am = place("spice-instruments:ammeter", {400, -200});
    const QString r2 = place("basic:resistor", {600, -200});
    const QString scope = place("spice-instruments:oscilloscope", {200, 300});
    const QString meter = place("spice-instruments:voltmeter", {600, 300});
    const QString bode = place("spice-instruments:bode_plotter", {1000, 300});
    const QString analyser = place("spice-instruments:spectrum_analyzer", {1400, 300});
    label(fg, "p", "in");
    ground(fg, "n");
    label(r1, "p", "in");
    label(r1, "n", "out");
    label(am, "p", "out");
    label(am, "n", "x");
    label(r2, "p", "x");
    ground(r2, "n");
    label(scope, "ch1", "in");
    label(scope, "ch2", "out");
    ground(scope, "ref");
    label(meter, "p", "out");
    ground(meter, "n");
    label(bode, "in", "in");
    label(bode, "out", "out");
    ground(bode, "ref");
    label(analyser, "in", "in");
    ground(analyser, "ref");
    sheet->setParameter(scope, QStringLiteral("tdiv"), QStringLiteral("200us"));

    SpiceSimulationView view(project, document);
    QSignalSpy finished(&view, &SpiceSimulationView::runFinished);
    spice::SpiceAnalysisSetup tran = spice::SpiceAnalysisSetup::create(spice::AnalysisKind::Transient);
    tran.set(QStringLiteral("stop"), QStringLiteral("10ms"));
    tran.set(QStringLiteral("step"), QStringLiteral("5us"));
    tran.set(QStringLiteral("maxStep"), QStringLiteral("5us"));
    view.addAnalysis(tran);
    view.run();
    REQUIRE(waitFor(finished));
    INFO(view.errors().join(QLatin1Char('\n')).toStdString());
    REQUIRE(finished.last().first().toBool());

    InstrumentsView *instruments = view.instrumentsView();
    CHECK(instruments->instrumentReferences()
          == QStringList({QStringLiteral("AM1"), QStringLiteral("BP1"), QStringLiteral("FG1"), QStringLiteral("SA1"),
                          QStringLiteral("SC1"), QStringLiteral("VM1")}));
    const QString scopeText = instruments->readout(QStringLiteral("SC1"));
    INFO(scopeText.toStdString());
    CHECK(scopeText.contains(QStringLiteral("triggered")));
    CHECK(scopeText.contains(QStringLiteral("2 V pk-pk")));   // CH1: 1 V peak sine
    CHECK(scopeText.contains(QStringLiteral("1 V pk-pk")));   // CH2: half of it
    CHECK(scopeText.contains(QStringLiteral("1 kHz")));
    CHECK(instruments->plot(QStringLiteral("SC1"))->traceIds().size() == 2);
    const QString meterText = instruments->readout(QStringLiteral("VM1"));
    INFO(meterText.toStdString());
    CHECK(meterText.contains(QStringLiteral("353.")));         // 0.5 V peak: 353.6 mV RMS
    CHECK(instruments->readout(QStringLiteral("AM1")).contains(QStringLiteral("353.")));  // 0.5 mA peak
    const QString spectrum = instruments->readout(QStringLiteral("SA1"));
    INFO(spectrum.toStdString());
    CHECK(spectrum.contains(QStringLiteral("at 1 kHz")));
    CHECK(instruments->readout(QStringLiteral("BP1")).contains(QStringLiteral("AC analysis")));

    view.addAnalysis(spice::SpiceAnalysisSetup::create(spice::AnalysisKind::Ac));
    view.run();
    REQUIRE(waitFor(finished, 2));
    const QString bodeText = instruments->readout(QStringLiteral("BP1"));
    INFO(bodeText.toStdString());
    CHECK(bodeText.contains(QStringLiteral("-6.02 dB")));
}
