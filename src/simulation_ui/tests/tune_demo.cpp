// Tunes a low-pass filter and optimises an L-match on the native platform,
// saves screenshots of the tuning panel and the optimisation dialog, prints
// the tuning update rate and quits. Not a test: used to check the look in the
// light and dark themes.
//
// Usage: tune_demo <output folder> [light|dark]

#include "core/Connectivity.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "plot/PlotWidget.h"
#include "rfsim/Analysis.h"
#include "rfsim/Optimizer.h"
#include "simulation_ui/OptimizationDialog.h"
#include "simulation_ui/PlotDefinitions.h"
#include "simulation_ui/PlotPane.h"
#include "simulation_ui/SimulationView.h"
#include "simulation_ui/TuningPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSpinBox>
#include <QStyleHints>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>

using namespace lvd;

namespace {

void settle(int rounds = 15)
{
    for (int i = 0; i < rounds; ++i) {
        QApplication::processEvents();
        QThread::msleep(20);
    }
}

bool save(QWidget &widget, const QString &path)
{
    settle();
    const bool ok = widget.grab().save(path);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", qPrintable(QDir::toNativeSeparators(path)));
    return ok;
}

struct Circuit {
    Project *project;
    Document *document;
    Schematic *sheet;

    QString place(const char *key, QPoint at, const QMap<QString, QString> &parameters = {})
    {
        const QString definitionKey = QString::fromLatin1(key);
        const DefinitionPtr definition = project->resolve(*sheet, definitionKey).definition;
        if (!definition) {
            std::fprintf(stderr, "missing definition %s\n", key);
            std::exit(1);
        }
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
        return place("rf-system:port", at, {{QStringLiteral("num"), QString::number(number)}});
    }
    void ground(const QString &id, const char *pinName)
    {
        const QString g = place("basic:ground", pin(id, pinName) + QPoint(0, 60));
        connect(id, pinName, g, "gnd");
    }
    QString analysis(const QString &name, const QString &start, const QString &stop, const QString &points,
                     const QList<sim::PlotDefinition> &plots)
    {
        rf::LinearAnalysisSetup setup;
        setup.name = name;
        setup.start = start;
        setup.stop = stop;
        setup.points = points;
        setup.noise = false;
        QString id;
        QJsonObject simulation = rf::withLinearAnalysis(sheet->simulation(), setup, &id);
        for (sim::PlotDefinition plot : plots) {
            plot.analysis = id;
            simulation = sim::withPlot(simulation, plot);
        }
        sheet->setSimulation(simulation);
        return id;
    }
};

bool runAndWait(SimulationView &view)
{
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&view, &SimulationView::runFinished, &loop, [&](bool success) {
        ok = success;
        loop.quit();
    });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    view.run();
    loop.exec();
    if (!ok)
        std::fprintf(stderr, "run failed: %s\n", qPrintable(view.errors().join(QStringLiteral("; "))));
    return ok;
}

void waitUntil(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
}

rf::DesignVariable parameter(const char *component, const char *name)
{
    rf::DesignVariable variable;
    variable.kind = rf::DesignVariable::Kind::Parameter;
    variable.component = QString::fromLatin1(component);
    variable.name = QString::fromLatin1(name);
    return variable;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("tune_demo"));
    if (argc < 2) {
        std::fprintf(stderr, "usage: tune_demo <output folder> [light|dark]\n");
        return 2;
    }
    const QString output = QString::fromLocal8Bit(argv[1]);
    const QString theme = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("light");
    QGuiApplication::styleHints()->setColorScheme(theme == QLatin1String("dark") ? Qt::ColorScheme::Dark
                                                                                  : Qt::ColorScheme::Light);
    QDir().mkpath(output);
    const auto file = [&](const QString &name) { return QDir(output).filePath(QStringLiteral("%1-%2.png").arg(name, theme)); };

    QTemporaryDir dir;
    Session session;
    QString error;
    Project *project = session.newProject(dir.filePath(QStringLiteral("demo")), QStringLiteral("demo"), &error);
    if (!project) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 1;
    }
    bool ok = true;

    // 1. Tuning a third-order LC low-pass (1 GHz Butterworth).
    {
        Document *document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("lowpass"), &error);
        Circuit c{project, document, document->schematic()};
        const QString p1 = c.port(1, {-400, 0}), p2 = c.port(2, {400, 0});
        const QString l1 = c.place("basic:inductor", {-200, 0}, {{QStringLiteral("l"), QStringLiteral("7.96nH")}});
        const QString l2 = c.place("basic:inductor", {200, 0}, {{QStringLiteral("l"), QStringLiteral("7.96nH")}});
        const QString cap = c.place("basic:capacitor", {0, 100}, {{QStringLiteral("c"), QStringLiteral("6.37pF")}});
        c.sheet->rotateItems({cap}, QPoint(0, 100), 90);
        c.connect(p1, "p", l1, "p");
        const QPoint node(c.pin(cap, "n").x(), c.pin(l1, "n").y());
        c.sheet->addWire({c.pin(l1, "n"), node, c.pin(cap, "n")});
        c.sheet->addWire({node, c.pin(l2, "p")});
        c.sheet->addJunction(node);
        c.ground(cap, "p");
        c.connect(l2, "n", p2, "p");
        sim::PlotDefinition response;
        response.name = QStringLiteral("Response");
        response.traces = {QStringLiteral("dB(S21)"), QStringLiteral("dB(S11)")};
        sim::PlotDefinition smith;
        smith.name = QStringLiteral("Input match");
        smith.kind = QStringLiteral("smith");
        smith.traces = {QStringLiteral("S11")};
        c.analysis(QStringLiteral("Low-pass"), QStringLiteral("10MHz"), QStringLiteral("3GHz"), QStringLiteral("1001"),
                   {response, smith});

        SimulationView view(project, document);
        view.resize(1500, 880);
        view.show();
        ok = runAndWait(view) && ok;
        view.startTuning();
        sim::TuningPanel *panel = view.tuningPanel();
        panel->addVariable(parameter("C1", "c"));
        panel->addVariable(parameter("L1", "l"));
        panel->addVariable(parameter("L2", "l"));
        panel->setSeries(0, ESeries::E24);
        waitUntil([&] { return !view.isTuningBusy(); }, 5000);

        // Rate: drag the C slider for two seconds, as fast as events allow.
        const int before = view.tuningResultCount();
        QElapsedTimer clock;
        clock.start();
        int position = 0, moves = 0;
        while (clock.elapsed() < 2000) {
            position = (position + 3) % (sim::TuningPanel::SliderSteps + 1);
            panel->setSliderPosition(1, position);
            ++moves;
            QApplication::processEvents();
            QThread::msleep(5);
        }
        waitUntil([&] { return !view.isTuningBusy(); }, 5000);
        const double seconds = static_cast<double>(clock.elapsed()) / 1000.0;
        std::printf("tuning rate (1001 points, 3 tunables): %d slider moves, %.1f plot updates/s, %.2f ms per simulation\n",
                    moves, (view.tuningResultCount() - before) / seconds, view.lastTuningMs());

        panel->setValue(0, 8.2e-12);
        panel->setValue(1, 6.8e-9);
        panel->setValue(2, 9.1e-9);
        waitUntil([&] { return !view.isTuningBusy(); }, 5000);
        if (sim::PlotPane *pane = view.plotPanes().value(0)) {
            const int s21 = pane->traceId(QStringLiteral("dB(S21)"));
            if (s21 >= 0)
                pane->plot()->addMarker(s21, 1e9);
        }
        ok = save(view, file(QStringLiteral("tuning"))) && ok;
        view.stopTuning();
    }

    // 2. Optimising an L-match from 50 ohm to 12 - j30 ohm at 2.4 GHz.
    {
        Document *document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("match"), &error);
        Circuit c{project, document, document->schematic()};
        const QString p1 = c.port(1, {-400, 0});
        const QString shunt = c.place("basic:capacitor", {-200, 100}, {{QStringLiteral("c"), QStringLiteral("1.5pF")}});
        c.sheet->rotateItems({shunt}, QPoint(-200, 100), 90);
        const QString series = c.place("basic:inductor", {0, 0}, {{QStringLiteral("l"), QStringLiteral("2nH")}});
        const QString r = c.place("basic:resistor", {200, 0}, {{QStringLiteral("r"), QStringLiteral("12")}});
        const QString load = c.place("basic:capacitor", {400, 100}, {{QStringLiteral("c"), QStringLiteral("2.2105pF")}});
        c.sheet->rotateItems({load}, QPoint(400, 100), 90);
        c.connect(p1, "p", series, "p");
        c.connect(p1, "p", shunt, "n");
        c.ground(shunt, "p");
        c.connect(series, "n", r, "p");
        c.connect(r, "n", load, "n");
        c.ground(load, "p");
        sim::PlotDefinition match;
        match.name = QStringLiteral("Match");
        match.traces = {QStringLiteral("dB(S11)")};
        c.analysis(QStringLiteral("Match"), QStringLiteral("2GHz"), QStringLiteral("2.8GHz"), QStringLiteral("161"), {match});

        SimulationView view(project, document);
        view.resize(1200, 700);
        view.show();
        ok = runAndWait(view) && ok;
        OptimizationDialog *dialog = view.openOptimization();
        rf::DesignVariable l = parameter("L1", "l");
        l.minimum = 0.5e-9;
        l.maximum = 10e-9;
        rf::DesignVariable cm = parameter("C1", "c");
        cm.minimum = 0.2e-12;
        cm.maximum = 10e-12;
        cm.series = ESeries::E24;
        l.series = ESeries::E24;
        dialog->addVariable(l);
        dialog->addVariable(cm);
        rf::Goal goal;
        goal.measurement = QStringLiteral("dB(S11)");
        goal.target = -20;
        goal.bandStart = 2.39e9;
        goal.bandStop = 2.41e9;
        dialog->addGoal(goal);
        rf::Goal wide;
        wide.measurement = QStringLiteral("dB(S11)");
        wide.target = -10;
        wide.weight = 0.5;
        wide.bandStart = 2.3e9;
        wide.bandStop = 2.5e9;
        dialog->addGoal(wide);
        dialog->evaluationsSpin()->setValue(300);
        bool finished = false;
        QObject::connect(dialog, &OptimizationDialog::finished, dialog, [&finished] { finished = true; });
        dialog->start();
        waitUntil([&] { return finished; }, 60000);
        dialog->tabs()->setCurrentIndex(0);
        ok = save(*dialog, file(QStringLiteral("optimization-progress"))) && ok;
        dialog->tabs()->setCurrentIndex(1);
        ok = save(*dialog, file(QStringLiteral("optimization"))) && ok;
        dialog->apply(true);
        waitUntil([&] { return !view.isRunning(); }, 10000);
        settle();
        ok = save(view, file(QStringLiteral("optimization-applied"))) && ok;
        dialog->close();
    }
    return ok ? 0 : 1;
}
