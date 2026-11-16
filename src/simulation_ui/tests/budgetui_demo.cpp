// Builds a superheterodyne receiver, runs its RF budget in the simulation
// view on the native platform, saves screenshots of the budget views and
// quits. Not a test: used to check the look in the light and dark themes.
//
// Usage: budgetui_demo <output folder> [light|dark]

#include "core/Connectivity.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "rfsim/Budget.h"
#include "simulation_ui/BudgetDialog.h"
#include "simulation_ui/BudgetView.h"
#include "simulation_ui/SimulationView.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QStyleHints>
#include <QTabWidget>
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
    void wire(const QString &a, const char *pinA, const QString &b, const char *pinB)
    {
        const QPoint start = pin(a, pinA), end = pin(b, pinB);
        sheet->addWire({start, QPoint(end.x(), start.y()), end});
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
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
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
    QApplication::setApplicationName(QStringLiteral("budgetui_demo"));
    if (argc < 2) {
        std::fprintf(stderr, "usage: budgetui_demo <output folder> [light|dark]\n");
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
    Document *document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("receiver"), &error);
    Circuit c{project, document, document->schematic()};

    // 2.45 GHz superheterodyne receiver with a 70 MHz IF and an ADC.
    c.sheet->setVariables({{QStringLiteral("f_rf"), QStringLiteral("2.45GHz")}, {QStringLiteral("f_if"), QStringLiteral("70MHz")}});
    const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("f_rf")}, {QStringLiteral("power"), QStringLiteral("-60")}});
    const QString bpf = c.place("rf-system:filter", {200, 0},
                                {{QStringLiteral("fc"), QStringLiteral("f_rf")}, {QStringLiteral("bw"), QStringLiteral("100MHz")},
                                 {QStringLiteral("il"), QStringLiteral("1.5")}});
    const QString lna = c.place("rf-system:amplifier", {400, 0},
                                {{QStringLiteral("gain"), QStringLiteral("18")}, {QStringLiteral("nf"), QStringLiteral("1")},
                                 {QStringLiteral("oip3"), QStringLiteral("25")}, {QStringLiteral("op1db"), QStringLiteral("12")}});
    const QString mixer = c.place("rf-system:mixer", {600, 0});
    const QString lo = c.place("rf-system:lo", {600, 200}, {{QStringLiteral("freq"), QStringLiteral("f_rf - f_if")}, {QStringLiteral("power"), QStringLiteral("13")}});
    const QString lpf = c.place("rf-system:filter", {800, 0},
                                {{QStringLiteral("type"), QStringLiteral("lowpass")}, {QStringLiteral("fc"), QStringLiteral("100MHz")},
                                 {QStringLiteral("il"), QStringLiteral("0.5")}});
    const QString ifAmp = c.place("rf-system:amplifier", {1000, 0},
                                  {{QStringLiteral("gain"), QStringLiteral("30")}, {QStringLiteral("nf"), QStringLiteral("4")},
                                   {QStringLiteral("oip3"), QStringLiteral("30")}, {QStringLiteral("op1db"), QStringLiteral("18")}});
    const QString adc = c.place("rf-system:adc", {1200, 0}, {{QStringLiteral("fs"), QStringLiteral("100MHz")}});
    c.wire(src, "out", bpf, "in");
    c.wire(bpf, "out", lna, "in");
    c.wire(lna, "out", mixer, "rf");
    c.wire(lo, "out", mixer, "lo");
    c.wire(mixer, "if", lpf, "in");
    c.wire(lpf, "out", ifAmp, "in");
    c.wire(ifAmp, "out", adc, "in");

    rf::BudgetAnalysisSetup setup;
    setup.name = QStringLiteral("Receiver budget");
    setup.sweepStart = QStringLiteral("2.3GHz");
    setup.sweepStop = QStringLiteral("2.6GHz");
    setup.sweepPoints = QStringLiteral("151");
    setup.bandwidth = QStringLiteral("2MHz");
    QString id;
    c.sheet->setSimulation(rf::withBudgetAnalysis(c.sheet->simulation(), setup, &id));

    bool ok = true;
    SimulationView view(project, document);
    view.resize(1400, 820);
    view.show();
    ok = runAndWait(view) && ok;
    sim::BudgetView *budget = view.budgetView();
    const struct {
        sim::BudgetView::Tab tab;
        const char *name;
    } pages[] = {{sim::BudgetView::TableTab, "budget-table"},
                 {sim::BudgetView::LevelTab, "budget-levels"},
                 {sim::BudgetView::SweepTab, "budget-sweep"},
                 {sim::BudgetView::SpurTab, "budget-spurs"},
                 {sim::BudgetView::PlanTab, "budget-plan"}};
    for (const auto &page : pages) {
        budget->tabs()->setCurrentIndex(page.tab);
        ok = save(view, file(QString::fromLatin1(page.name))) && ok;
    }

    // The same receiver driven hard: compressed stages are highlighted.
    rf::BudgetAnalysisSetup strong = rf::budgetAnalyses(c.sheet->simulation()).first();
    strong.power = QStringLiteral("-20");
    strong.spurs = false;
    c.sheet->setSimulation(rf::withBudgetAnalysis(c.sheet->simulation(), strong));
    ok = runAndWait(view) && ok;
    budget->tabs()->setCurrentIndex(sim::BudgetView::TableTab);
    ok = save(view, file(QStringLiteral("budget-compressed"))) && ok;
    budget->tabs()->setCurrentIndex(sim::BudgetView::LevelTab);
    ok = save(view, file(QStringLiteral("budget-compressed-levels"))) && ok;

    BudgetDialog dialog(project, document, id, &view);
    dialog.show();
    ok = save(dialog, file(QStringLiteral("budget-dialog"))) && ok;
    dialog.tracePath();
    ok = save(dialog, file(QStringLiteral("budget-dialog-path"))) && ok;
    dialog.reject();
    return ok ? 0 : 1;
}
