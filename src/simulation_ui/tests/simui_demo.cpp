// Simulates example circuits, shows the simulation view on the native
// platform, saves screenshots and quits. Not a test: used to check the look in
// the light and dark themes.
//
// Usage: simui_demo <output folder> [light|dark]

#include "core/Connectivity.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "plot/PlotWidget.h"
#include "rfsim/Analysis.h"
#include "simulation_ui/AnalysisDialog.h"
#include "simulation_ui/PlotDefinitions.h"
#include "simulation_ui/PlotPane.h"
#include "simulation_ui/SimulationView.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QLineEdit>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>

using namespace lvd;

namespace {

void settle()
{
    for (int i = 0; i < 15; ++i) {
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

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("simui_demo"));
    if (argc < 2) {
        std::fprintf(stderr, "usage: simui_demo <output folder> [light|dark]\n");
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

    // 1. 2.4 GHz receiver front end: port, bandpass filter, LNA, attenuator, port.
    {
        Document *document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("lna"), &error);
        Circuit c{project, document, document->schematic()};
        const QString p1 = c.port(1, {-600, 0}), p2 = c.port(2, {600, 0});
        const QString filter = c.place("rf-system:filter", {-300, 0},
                                       {{QStringLiteral("type"), QStringLiteral("bandpass")},
                                        {QStringLiteral("response"), QStringLiteral("chebyshev")},
                                        {QStringLiteral("order"), QStringLiteral("3")},
                                        {QStringLiteral("fc"), QStringLiteral("2.44GHz")},
                                        {QStringLiteral("bw"), QStringLiteral("120MHz")},
                                        {QStringLiteral("ripple"), QStringLiteral("0.5")},
                                        {QStringLiteral("il"), QStringLiteral("1.5")},
                                        {QStringLiteral("rejection"), QStringLiteral("60")}});
        const QString amp = c.place("rf-system:amplifier", {0, 0},
                                    {{QStringLiteral("gain"), QStringLiteral("18")}, {QStringLiteral("nf"), QStringLiteral("1.2")}});
        const QString att = c.place("rf-system:attenuator", {300, 0}, {{QStringLiteral("il"), QStringLiteral("3")}});
        c.connect(p1, "p", filter, "in");
        c.connect(filter, "out", amp, "in");
        c.connect(amp, "out", att, "in");
        c.connect(att, "out", p2, "p");

        rf::LinearAnalysisSetup setup;
        setup.name = QStringLiteral("Front end");
        setup.start = QStringLiteral("2.1GHz");
        setup.stop = QStringLiteral("2.8GHz");
        setup.points = QStringLiteral("701");
        QString id;
        QJsonObject simulation = rf::withLinearAnalysis(c.sheet->simulation(), setup, &id);
        sim::PlotDefinition response;
        response.name = QStringLiteral("Gain, match and noise");
        response.analysis = id;
        response.traces = {QStringLiteral("dB(S21)"), QStringLiteral("dB(S11)"), QStringLiteral("NF")};
        response.labels.insert(QStringLiteral("dB(S21)"), QStringLiteral("Gain"));
        simulation = sim::withPlot(simulation, response);
        sim::PlotDefinition smith;
        smith.name = QStringLiteral("Port match");
        smith.kind = QStringLiteral("smith");
        smith.analysis = id;
        smith.traces = {QStringLiteral("S11"), QStringLiteral("S22")};
        simulation = sim::withPlot(simulation, smith);
        c.sheet->setSimulation(simulation);

        SimulationView view(project, document);
        view.resize(1400, 860);
        view.show();
        ok = runAndWait(view) && ok;
        sim::PlotPane *pane = view.plotPanes().value(0);
        if (pane) {
            const int gain = pane->traceId(QStringLiteral("dB(S21)"));
            if (gain >= 0)
                pane->plot()->addMarker(gain, 2.44e9);
            const int nf = pane->traceId(QStringLiteral("NF"));
            if (nf >= 0)
                pane->plot()->addMarker(nf, 2.44e9);
        }
        if (sim::PlotPane *smithPane = view.plotPanes().value(1)) {
            const int s11 = smithPane->traceId(QStringLiteral("S11"));
            if (s11 >= 0)
                smithPane->plot()->addMarker(s11, 2.44e9);
        }
        ok = save(view, file(QStringLiteral("frontend"))) && ok;
        if (pane) {
            pane->setEditorVisible(true);
            ok = save(view, file(QStringLiteral("frontend-editor"))) && ok;
        }
    }

    // 2. Third-order LC low-pass (Butterworth T, 1 GHz), with the default plots.
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

        rf::LinearAnalysisSetup setup;
        setup.name = QStringLiteral("Low-pass");
        setup.start = QStringLiteral("10MHz");
        setup.stop = QStringLiteral("3GHz");
        setup.points = QStringLiteral("400");
        setup.noise = false;
        QString id;
        QJsonObject simulation = rf::withLinearAnalysis(c.sheet->simulation(), setup, &id);
        for (const sim::PlotDefinition &plot : sim::defaultPlots(id, {1, 2}, false))
            simulation = sim::withPlot(simulation, plot);
        c.sheet->setSimulation(simulation);

        SimulationView view(project, document);
        view.resize(1400, 860);
        view.show();
        ok = runAndWait(view) && ok;
        ok = save(view, file(QStringLiteral("lowpass"))) && ok;
        view.setTiled(false);
        ok = save(view, file(QStringLiteral("lowpass-tabs"))) && ok;

        // The analysis dialog for the same analysis.
        AnalysisDialog dialog(project, document, id, &view);
        dialog.show();
        ok = save(dialog, file(QStringLiteral("dialog"))) && ok;
        dialog.startEdit()->setText(QStringLiteral("3GHz+"));
        ok = save(dialog, file(QStringLiteral("dialog-error"))) && ok;
        dialog.reject();
    }

    // 3. A circuit that cannot be simulated: errors in the banner.
    {
        Document *document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("broken"), &error);
        Circuit c{project, document, document->schematic()};
        const QString p1 = c.port(1, {-300, 0});
        const QString amp = c.place("rf-system:amplifier", {0, 0});
        c.connect(p1, "p", amp, "in");
        c.place("rf-system:filter", {300, 200}, {{QStringLiteral("fc"), QStringLiteral("-1GHz")}});
        c.sheet->setSimulation(rf::withLinearAnalysis(c.sheet->simulation(), rf::LinearAnalysisSetup{}));
        SimulationView view(project, document);
        view.resize(1100, 600);
        view.show();
        runAndWait(view);
        ok = save(view, file(QStringLiteral("errors"))) && ok;
    }
    return ok ? 0 : 1;
}
