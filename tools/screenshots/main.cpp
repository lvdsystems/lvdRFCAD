// Generates the screenshots of the user manual.
//
//   manual_screenshots <examples folder> <output folder>
//
// Every example project is copied into a temporary folder and opened from
// there; nothing is ever saved. The program runs unattended: modal dialogs
// that pop up are closed (and reported), and the whole run has a time limit.

#include "antenna_ui/AntennaLibraryPanel.h"
#include "antenna_ui/ArrayWorkspace.h"
#include "antenna_ui/ElementWorkspace.h"
#include "antenna_ui/PositionEditor.h"
#include "core/Document.h"
#include "core/Project.h"
#include "core/RuleCheck.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "gui/MainWindow.h"
#include "gui/PythonConsole.h"
#include "gui/SchematicWorkspace.h"
#include "library_ui/LibraryBrowser.h"
#include "library_ui/SymbolEditor.h"
#include "plot/PlotWidget.h"
#include "matching/MatchingWindow.h"
#include "python/PythonInterpreter.h"
#include "rfsim/Analysis.h"
#include "rfsim/Budget.h"
#include "rfsim/Optimizer.h"
#include "schematic_editor/SchematicEditor.h"
#include "simulation_ui/BudgetView.h"
#include "simulation_ui/OptimizationDialog.h"
#include "simulation_ui/PlotDefinitions.h"
#include "simulation_ui/PlotPane.h"
#include "simulation_ui/SimulationView.h"
#include "simulation_ui/TuningPanel.h"
#include "sparam_ui/TouchstoneEditor.h"
#include "spice/SpiceAnalysis.h"
#include "spice_ui/BatchView.h"
#include "spice_ui/InstrumentsView.h"
#include "spice_ui/SpiceAnalysisDialog.h"
#include "spice_ui/SpiceSimulationView.h"
#include "spice_ui/WaveformPane.h"
#include "spice_ui/WaveformViewer.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPixmap>
#include <QSettings>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <memory>

using namespace lvd;

namespace {

const QSize MainSize(1600, 1000);

QString g_outputDir;
QStringList g_problems;
QList<QStringList> g_readme; // file, size, description

void note(const QString &text)
{
    std::fprintf(stderr, "%s\n", qPrintable(text));
    std::fflush(stderr);
}

void problem(const QString &text)
{
    g_problems.append(text);
    note(QStringLiteral("PROBLEM: ") + text);
}

// Lets queued work, layouts and repaints happen for a while.
void settle(int milliseconds = 300)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
    QCoreApplication::processEvents();
}

// Runs `trigger`, then waits for `signal` of `sender` (or the timeout).
template <typename Sender, typename Signal>
bool triggerAndWait(Sender *sender, Signal signal, const std::function<void()> &trigger, int timeoutMs)
{
    QEventLoop loop;
    bool fired = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    const QMetaObject::Connection connection = QObject::connect(sender, signal, &loop, [&] {
        fired = true;
        loop.quit();
    });
    trigger();
    if (!fired) {
        timer.start(timeoutMs);
        loop.exec();
    }
    QObject::disconnect(connection);
    return fired;
}

void save(QWidget *widget, const QString &file, const QString &description)
{
    QCoreApplication::processEvents();
    QPixmap pixmap = widget->grab();
    pixmap.setDevicePixelRatio(1.0);
    const QString path = QDir(g_outputDir).filePath(file);
    if (!pixmap.save(path, "PNG")) {
        problem(QStringLiteral("cannot write %1").arg(path));
        return;
    }
    g_readme.append({file, QStringLiteral("%1 x %2").arg(pixmap.width()).arg(pixmap.height()), description});
    note(QStringLiteral("saved %1 (%2x%3)").arg(file).arg(pixmap.width()).arg(pixmap.height()));
}

bool copyFolder(const QString &from, const QString &to)
{
    if (!QDir().mkpath(to))
        return false;
    QDirIterator it(from, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const QDir source(from);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString target = QDir(to).filePath(source.relativeFilePath(path));
        if (it.fileInfo().isDir()) {
            if (!QDir().mkpath(target))
                return false;
        } else if (!QFile::copy(path, target)) {
            return false;
        }
    }
    return true;
}

class Shooter
{
public:
    Shooter(Session *session, PythonInterpreter *python, const QString &examples)
        : m_session(session), m_python(python), m_examples(examples)
    {
    }

    // A fresh main window with a copy of the example project open.
    std::unique_ptr<MainWindow> openExample(const QString &folder, const QString &projectFile)
    {
        m_session->closeProject();
        auto window = std::make_unique<MainWindow>(m_session, m_python);
        window->resize(MainSize);
        window->move(0, 0);
        window->show();
        settle(200);
        if (window->size() != MainSize) {
            window->resize(MainSize);
            settle(100);
        }
        const QString copy = m_temp.filePath(QStringLiteral("%1-%2").arg(folder).arg(++m_copies));
        if (!copyFolder(QDir(m_examples).filePath(folder), copy)) {
            problem(QStringLiteral("cannot copy example %1").arg(folder));
            return window;
        }
        if (!window->openProjectFile(QDir(copy).filePath(projectFile)))
            problem(QStringLiteral("cannot open %1").arg(projectFile));
        settle(200);
        return window;
    }

    QString exampleCopy(const QString &folder, const QString &file)
    {
        const QString copy = m_temp.filePath(QStringLiteral("%1-%2").arg(folder).arg(++m_copies));
        copyFolder(QDir(m_examples).filePath(folder), copy);
        return QDir(copy).filePath(file);
    }

    SchematicWorkspace *openSchematic(MainWindow *window, const QString &name)
    {
        Project *project = m_session->project();
        Document *document = project ? project->document(name) : nullptr;
        if (!document) {
            problem(QStringLiteral("document %1 not found").arg(name));
            return nullptr;
        }
        window->openDocument(document);
        settle(300);
        SchematicWorkspace *workspace = window->currentWorkspace();
        if (!workspace)
            problem(QStringLiteral("document %1 did not open as a schematic").arg(name));
        return workspace;
    }

    static QString analysisId(SchematicWorkspace *workspace, const QString &name)
    {
        const QJsonObject &simulation = workspace->editor()->schematic()->simulation();
        for (const rf::LinearAnalysisSetup &analysis : rf::linearAnalyses(simulation)) {
            if (analysis.name == name)
                return analysis.id;
        }
        for (const rf::BudgetAnalysisSetup &analysis : rf::budgetAnalyses(simulation)) {
            if (analysis.name == name)
                return analysis.id;
        }
        problem(QStringLiteral("analysis \"%1\" not found").arg(name));
        return {};
    }

    // Selects the analysis, opens the results area and runs it to the end.
    static bool runAnalysis(SchematicWorkspace *workspace, const QString &name)
    {
        SimulationView *simulation = workspace->simulation();
        const QString id = analysisId(workspace, name);
        if (id.isEmpty())
            return false;
        simulation->setCurrentAnalysis(id);
        workspace->setSimulationVisible(true);
        settle(100);
        bool ok = false;
        const bool fired = triggerAndWait(
            simulation, &SimulationView::runFinished, [simulation] { simulation->run(); }, 120000);
        if (fired) {
            ok = !simulation->isRunning();
        }
        if (!fired || !ok) {
            problem(QStringLiteral("run of \"%1\" did not finish").arg(name));
            return false;
        }
        if (!simulation->errors().isEmpty())
            problem(QStringLiteral("run of \"%1\" reported errors: %2").arg(name, simulation->errors().join(QStringLiteral("; "))));
        settle(400);
        return true;
    }

    static void fit(SchematicWorkspace *workspace)
    {
        settle(150);
        workspace->editor()->zoomToFit();
        settle(150);
    }

    static QDockWidget *dock(MainWindow *window, const QString &name)
    {
        auto *result = window->findChild<QDockWidget *>(name);
        if (!result)
            problem(QStringLiteral("dock %1 not found").arg(name));
        return result;
    }

    // More room for results: the bottom docks (console, messages, rule
    // check) closed, and a third of the tab for the schematic.
    static void hideBottomDocks(MainWindow *window)
    {
        for (const char *name : {"pythonConsoleDock", "messagesDock", "ruleCheckDock"}) {
            if (QDockWidget *bottom = dock(window, QString::fromLatin1(name)))
                bottom->hide();
        }
        settle(100);
    }

    static void focusResults(MainWindow *window, SchematicWorkspace *workspace)
    {
        hideBottomDocks(window);
        const int total = workspace->height();
        workspace->setSizes({total * 7 / 20, total * 13 / 20});
        settle(200);
        fit(workspace);
    }

    // ---- Scenarios

    void receiver()
    {
        auto window = openExample(QStringLiteral("receiver"), QStringLiteral("receiver.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("receiver"));
        if (!workspace)
            return;
        workspace->setSimulationVisible(false);
        fit(workspace);

        // 1. Main window with LNA1 selected.
        if (const PlacedComponent *lna = workspace->editor()->schematic()->componentByReference(QStringLiteral("LNA1")))
            workspace->editor()->selectItems({lna->id});
        else
            problem(QStringLiteral("LNA1 not found in receiver"));
        settle(300);
        save(window.get(), QStringLiteral("main-window.png"),
             QStringLiteral("Main window with the receiver example: schematic, project and library docks, Properties of LNA1."));

        // 2. Library dock, searching "filter".
        if (QDockWidget *libraryDock = dock(window.get(), QStringLiteral("libraryDock"))) {
            LibraryBrowser *browser = window->libraryBrowser();
            browser->setFilterText(QStringLiteral("filter"));
            settle(300);
            if (!browser->selectComponent(QStringLiteral("rf-system:filter")))
                problem(QStringLiteral("rf-system:filter not shown in the library browser"));
            settle(300);
            save(libraryDock, QStringLiteral("library-browser.png"),
                 QStringLiteral("Library dock searching \"filter\", the rf-system Filter selected with its symbol preview."));
        }
    }

    void ruleCheck()
    {
        // 18. Rule check.
        auto window = openExample(QStringLiteral("receiver"), QStringLiteral("receiver.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("receiver"));
        if (!workspace)
            return;
        workspace->setSimulationVisible(false);
        fit(workspace);
        const QList<RuleViolation> violations = workspace->editor()->runRuleCheck();
        for (const RuleViolation &violation : violations)
            note(QStringLiteral("  rule check: %1").arg(violation.message));
        settle(400);
        if (QDockWidget *ruleDock = dock(window.get(), QStringLiteral("ruleCheckDock")))
            ruleDock->raise();
        settle(200);
        save(window.get(), QStringLiteral("rule-check.png"),
             QStringLiteral("Receiver after a rule check, with the Rule Check dock raised listing the findings."));
    }

    void receiverBudget()
    {
        // 13. Budget views.
        auto window = openExample(QStringLiteral("receiver"), QStringLiteral("receiver.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("receiver"));
        if (!workspace)
            return;
        if (runAnalysis(workspace, QStringLiteral("Receiver budget"))) {
            sim::BudgetView *budget = workspace->simulation()->budgetView();
            focusResults(window.get(), workspace);
            const struct {
                sim::BudgetView::Tab tab;
                const char *file;
                const char *description;
            } tabs[] = {
                {sim::BudgetView::TableTab, "budget-table.png", "Receiver budget analysis: the budget table, stage by stage."},
                {sim::BudgetView::LevelTab, "level-diagram.png", "Receiver budget analysis: the level diagram along the chain."},
                {sim::BudgetView::SweepTab, "swept-budget.png", "Receiver budget analysis: gain, NF and IIP3 swept over the input frequency."},
                {sim::BudgetView::SpurTab, "spurs.png", "Receiver budget analysis: spur table and spur chart."},
                {sim::BudgetView::PlanTab, "frequency-plan.png", "Receiver budget analysis: the frequency plan."},
            };
            for (const auto &entry : tabs) {
                budget->tabs()->setCurrentIndex(entry.tab);
                settle(400);
                save(window.get(), QString::fromLatin1(entry.file), QString::fromLatin1(entry.description));
            }
        }
    }

    void hierarchy()
    {
        // 4. The sub-sheet.
        auto window = openExample(QStringLiteral("receiver"), QStringLiteral("receiver.lvdproj"));
        if (!openSchematic(window.get(), QStringLiteral("receiver")))
            return;
        if (SchematicWorkspace *sheet = openSchematic(window.get(), QStringLiteral("if_strip"))) {
            sheet->setSimulationVisible(false);
            fit(sheet);
            save(window.get(), QStringLiteral("hierarchy.png"),
                 QStringLiteral("The if_strip sub-sheet of the receiver open in its own tab."));
        }
    }

    void library()
    {
        // 3. Symbol editor.
        SymbolEditor editor(m_session->libraries());
        editor.resize(1200, 800);
        QString error;
        if (!editor.openComponent(QStringLiteral("rf-system:mixer"), &error)) {
            problem(QStringLiteral("symbol editor: %1").arg(error));
            return;
        }
        editor.show();
        settle(500);
        save(&editor, QStringLiteral("symbol-editor.png"),
             QStringLiteral("Symbol editor on the built-in rf-system:mixer component (read-only source)."));
        editor.hide();
    }

    void lowpass()
    {
        auto window = openExample(QStringLiteral("lowpass_filter"), QStringLiteral("lowpass_filter.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("lpf"));
        if (!workspace)
            return;

        // 5. Schematic alone.
        workspace->setSimulationVisible(false);
        fit(workspace);
        save(window.get(), QStringLiteral("lowpass-schematic.png"),
             QStringLiteral("Low-pass filter example schematic, results area collapsed."));

        // 7. Variables dock.
        if (QDockWidget *variables = dock(window.get(), QStringLiteral("variablesDock")))
            save(variables, QStringLiteral("variables-panel.png"),
                 QStringLiteral("Variables dock of the low-pass filter: fc, R0, w, C1, L2, C3 with evaluated values."));

        // 6. Results.
        if (!runAnalysis(workspace, QStringLiteral("Response")))
            return;
        focusResults(window.get(), workspace);
        // The well-matched S11 reaches -190 dB at 10 MHz, so the autoscaled
        // axis flattens the curves; show the useful range as a user would zoom.
        for (sim::PlotPane *pane : workspace->simulation()->plotPanes()) {
            if (pane->definition().kind == QLatin1String("rectangular"))
                pane->plot()->setYRange(-80, 5);
        }
        settle(200);
        save(window.get(), QStringLiteral("sparams-results.png"),
             QStringLiteral("Low-pass filter after running \"Response\": dB(S21)/dB(S11) (y axis zoomed to -80..5 dB) and Smith chart of S11."));

        // 8. Tuning.
        SimulationView *simulation = workspace->simulation();
        simulation->startTuning();
        settle(300);
        sim::TuningPanel *panel = simulation->tuningPanel();
        for (const char *name : {"C1", "L2"}) {
            rf::DesignVariable variable;
            variable.kind = rf::DesignVariable::Kind::Variable;
            variable.name = QString::fromLatin1(name);
            if (panel->addVariable(variable) < 0)
                problem(QStringLiteral("tuning: cannot add %1").arg(variable.name));
        }
        settle(500);
        if (panel->count() > 0) {
            const int position = sim::TuningPanel::SliderSteps * 3 / 4;
            const bool shown = triggerAndWait(
                simulation, &SimulationView::tuningResultShown,
                [panel, position] { panel->setSliderPosition(0, position); }, 30000);
            if (!shown)
                problem(QStringLiteral("tuning result not shown"));
            for (int i = 0; i < 100 && simulation->isTuningBusy(); ++i)
                settle(50);
            settle(400);
        }
        focusResults(window.get(), workspace);
        save(window.get(), QStringLiteral("tuning.png"),
             QStringLiteral("Tuning the low-pass filter: C1 and L2 on sliders, C1 moved, plots show the tuned response."));
        simulation->stopTuning();
        settle(100);
    }

    void python()
    {
        // 17. Python console.
        auto window = openExample(QStringLiteral("lowpass_filter"), QStringLiteral("lowpass_filter.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("lpf"));
        if (!workspace)
            return;
        if (QDockWidget *consoleDock = dock(window.get(), QStringLiteral("pythonConsoleDock"))) {
            workspace->setSimulationVisible(false);
            consoleDock->raise();
            window->resizeDocks({consoleDock}, {340}, Qt::Vertical);
            settle(200);
            PythonConsole *console = window->pythonConsole();
            for (const char *line : {"p = lvdrfcad.project()", "lpf = p.document('lpf').schematic",
                                     "r = lpf.simulate('Response')", "print(round(min(r.measure('dB(S21)')), 1))",
                                     "lpf.nets()"})
                console->execute(QString::fromLatin1(line));
            settle(400);
            if (console->outputText().contains(QStringLiteral("Error")))
                problem(QStringLiteral("python console output has an error:\n%1").arg(console->outputText()));
            fit(workspace);
            save(window.get(), QStringLiteral("python-console.png"),
                 QStringLiteral("Python Console dock after scripting the low-pass filter (simulate, measure, nets)."));
        }
    }

    void lna()
    {
        auto window = openExample(QStringLiteral("lna_match"), QStringLiteral("lna_match.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("lna"));
        if (!workspace)
            return;

        // 9. Results.
        if (runAnalysis(workspace, QStringLiteral("Band"))) {
            focusResults(window.get(), workspace);
            save(window.get(), QStringLiteral("lna-results.png"),
                 QStringLiteral("LNA example after running \"Band\": S11, S21 and NF, with the deliberately poor match."));
        }

        // 10. Optimisation.
        SimulationView *simulation = workspace->simulation();
        if (OptimizationDialog *dialog = simulation->openOptimization()) {
            dialog->resize(900, 650);
            rf::OptimizationSetup setup;
            setup.name = QStringLiteral("Input match");
            setup.analysis = analysisId(workspace, QStringLiteral("Band"));
            rf::DesignVariable lin;
            lin.name = QStringLiteral("Lin");
            lin.minimum = 0.3e-9;
            lin.maximum = 30e-9;
            lin.series = ESeries::E24;
            lin.unit = QStringLiteral("H");
            rf::DesignVariable cin;
            cin.name = QStringLiteral("Cin");
            cin.minimum = 0.1e-12;
            cin.maximum = 20e-12;
            cin.series = ESeries::E24;
            cin.unit = QStringLiteral("F");
            setup.variables = {lin, cin};
            rf::Goal goal;
            goal.measurement = QStringLiteral("dB(S11)");
            goal.type = rf::Goal::Type::Below;
            goal.target = -15;
            goal.bandStart = 2.4e9;
            goal.bandStop = 2.5e9;
            setup.goals = {goal};
            setup.method = rf::OptimizationSetup::Method::NelderMead;
            dialog->setSetup(setup);
            settle(200);
            const QStringList invalid = dialog->validate();
            if (!invalid.isEmpty())
                problem(QStringLiteral("optimisation setup invalid: %1").arg(invalid.join(QStringLiteral("; "))));
            bool started = false;
            const bool finished = triggerAndWait(
                dialog, &OptimizationDialog::finished, [&] { started = dialog->start(); }, 300000);
            if (!started || !finished)
                problem(QStringLiteral("optimisation did not run to the end"));
            settle(300);
            dialog->tabs()->setCurrentIndex(1);
            settle(300);
            save(dialog, QStringLiteral("optimization.png"),
                 QStringLiteral("Optimisation dialog for the LNA input match (Lin, Cin, E24; dB(S11) < -15 dB), Results tab."));
            delete dialog; // not applied
            settle(100);
        } else {
            problem(QStringLiteral("optimisation dialog did not open"));
        }

        // 12. Touchstone editor on a copy of amp.s2p.
        const QString s2p = exampleCopy(QStringLiteral("lna_match"), QStringLiteral("amp.s2p"));
        if (TouchstoneEditor *editor = window->openTouchstoneFile(s2p)) {
            editor->setSelectedParameters({{0, 0}, {1, 0}});
            editor->setPlotQuantity(SParamQuantity::MagnitudeDb);
            window->statusBar()->clearMessage();
            settle(500);
            save(window.get(), QStringLiteral("sparam-editor.png"),
                 QStringLiteral("S-parameter editor tab with amp.s2p: S11 and S21 in dB, Smith chart and table."));
        } else {
            problem(QStringLiteral("cannot open amp.s2p in the S-parameter editor"));
        }
    }

    void matching()
    {
        // 11. Matching window.
        MatchingWindow window;
        window.setWindowTitle(QStringLiteral("Matching Network Synthesis"));
        window.resize(1300, 850);
        window.show();
        settle(200);
        window.setTopology(rf::MatchTopology::LNetwork);
        window.setImpedances({50, 0}, {12, -30}, 2.4e9);
        window.recompute();
        settle(200);
        int pick = -1;
        const auto &solutions = window.result().solutions;
        for (int i = 0; i < static_cast<int>(solutions.size()); ++i) {
            note(QStringLiteral("  matching solution %1: %2").arg(i + 1).arg(solutions[static_cast<size_t>(i)].description));
            if (pick < 0 && solutions[static_cast<size_t>(i)].description.contains(QStringLiteral("shunt C, series L")))
                pick = i;
        }
        if (pick != 2)
            problem(QStringLiteral("matching: \"shunt C, series L\" is solution %1, not 3").arg(pick + 1));
        window.selectSolution(pick >= 0 ? pick : 2);
        settle(400);
        save(&window, QStringLiteral("matching.png"),
             QStringLiteral("Matching network synthesis: 50 ohm to 12 - j30 ohm at 2.4 GHz, L-network, shunt C / series L selected."));
    }

    void upconverter()
    {
        auto window = openExample(QStringLiteral("upconverter"), QStringLiteral("upconverter.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("upconverter"));
        if (!workspace)
            return;

        // 14. Schematic.
        workspace->setSimulationVisible(false);
        fit(workspace);
        save(window.get(), QStringLiteral("upconverter-schematic.png"),
             QStringLiteral("Up-converter example schematic, results area collapsed."));

        // 15. Budget.
        if (runAnalysis(workspace, QStringLiteral("Budget"))) {
            workspace->simulation()->budgetView()->tabs()->setCurrentIndex(sim::BudgetView::TableTab);
            focusResults(window.get(), workspace);
            save(window.get(), QStringLiteral("upconverter-budget.png"),
                 QStringLiteral("Up-converter \"Budget\" analysis: the budget table."));
        }

        // 16. S-parameters.
        if (runAnalysis(workspace, QStringLiteral("S-parameters"))) {
            focusResults(window.get(), workspace);
            save(window.get(), QStringLiteral("upconverter-sparams.png"),
                 QStringLiteral("Up-converter \"S-parameters\" analysis with its plots."));
        }
    }

    // ---- SPICE (release 0.2)

    static QString spiceAnalysisId(SchematicWorkspace *workspace, const QString &name)
    {
        for (const spice::SpiceAnalysisSetup &analysis : spice::spiceAnalyses(workspace->editor()->schematic()->simulation()))
            if (analysis.name == name)
                return analysis.id;
        problem(QStringLiteral("SPICE analysis \"%1\" not found").arg(name));
        return {};
    }

    static bool runSpice(SchematicWorkspace *workspace, const QString &name)
    {
        spiceui::SpiceSimulationView *view = workspace->spiceSimulation();
        if (!view) {
            problem(QStringLiteral("not a SPICE schematic"));
            return false;
        }
        const QString id = spiceAnalysisId(workspace, name);
        if (id.isEmpty())
            return false;
        view->setCurrentAnalysis(id);
        workspace->setSimulationVisible(true);
        settle(100);
        const bool fired =
            triggerAndWait(view, &spiceui::SpiceSimulationView::runFinished, [view] { view->run(); }, 120000);
        if (!fired || view->isRunning()) {
            problem(QStringLiteral("SPICE run of \"%1\" did not finish").arg(name));
            return false;
        }
        if (!view->errors().isEmpty())
            problem(QStringLiteral("SPICE run of \"%1\" reported errors: %2").arg(name, view->errors().join(QStringLiteral("; "))));
        settle(500);
        return true;
    }

    void spiceAmplifier()
    {
        auto window = openExample(QStringLiteral("opamp_amplifier"), QStringLiteral("opamp_amplifier.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("amplifier"));
        if (!workspace)
            return;
        hideBottomDocks(window.get());
        workspace->setSimulationVisible(false);
        fit(workspace);
        save(window.get(), QStringLiteral("spice-amplifier-schematic.png"),
             QStringLiteral("Op-amp amplifier example: SPICE schematic with a function generator, oscilloscope and Bode plotter."));

        if (runSpice(workspace, QStringLiteral("Bias"))) {
            workspace->setSimulationVisible(false);
            fit(workspace);
            save(window.get(), QStringLiteral("spice-operating-point.png"),
                 QStringLiteral("Operating point shown on the schematic: node voltages, currents and powers."));
        }
        if (runSpice(workspace, QStringLiteral("Waveforms"))) {
            focusResults(window.get(), workspace);
            spiceui::WaveformViewer *viewer = workspace->spiceSimulation()->waveformViewer();
            QString error;
            viewer->addTrace(QStringLiteral("v(in)"), QString(), &error);
            viewer->addTrace(QStringLiteral("v(out)"), QString(), &error);
            viewer->setCursorShown(0, true);
            viewer->setCursorShown(1, true);
            if (spiceui::WaveformPane *pane = viewer->currentPane())
                pane->setSelectedTrace(int(pane->traceCount()) - 1); // v(out)
            viewer->fftOfSelectedTrace(&error);
            if (spiceui::WaveformPane *fft = viewer->currentPane())
                fft->plot()->setXRange(0, 20e3); // the first harmonics

            settle(400);
            save(window.get(), QStringLiteral("spice-waveforms.png"),
                 QStringLiteral("Transient result in the waveform viewer: v(in), v(out), cursors, measurements and an FFT pane."));
            workspace->spiceSimulation()->tabs()->setCurrentWidget(workspace->spiceSimulation()->instrumentsView());
            settle(300);
        }
        if (runSpice(workspace, QStringLiteral("Frequency response"))) {
            spiceui::SpiceSimulationView *view = workspace->spiceSimulation();
            view->tabs()->setCurrentWidget(view->instrumentsView());
            focusResults(window.get(), workspace);
            settle(300);
            save(window.get(), QStringLiteral("spice-instruments.png"),
                 QStringLiteral("Instruments tab: oscilloscope (last transient) and Bode plotter (AC)."));
            // Monte Carlo of the stored setup, in this process for the screenshot.
            view->setBatchWorkers(1);
            const auto setup = view->analysis(view->currentAnalysisId());
            if (setup) {
                spice::MonteCarloSetup mc = spice::MonteCarloSetup::fromJson(setup->data.value(QStringLiteral("monteCarlo")).toObject());
                mc.runs = 60;
                const bool fired = triggerAndWait(view, &spiceui::SpiceSimulationView::runFinished,
                                                  [view, mc] { view->runMonteCarlo(mc); }, 300000);
                if (!fired)
                    problem(QStringLiteral("Monte Carlo did not finish"));
                view->batchView()->setOverlay(QStringLiteral("db(v(out)/v(in))"));
                settle(400);
                save(window.get(), QStringLiteral("spice-monte-carlo.png"),
                     QStringLiteral("Monte Carlo of the gain: statistics, runs and the overlaid responses of all runs."));
                view->batchView()->showMeasure(0);
                settle(200);
            }
        }
        // The analysis dialog.
        spiceui::SpiceSimulationView *view = workspace->spiceSimulation();
        if (const auto setup = view->analysis(spiceAnalysisId(workspace, QStringLiteral("Waveforms")))) {
            spiceui::SpiceAnalysisDialog dialog(*setup, false, window.get());
            dialog.show();
            settle(300);
            save(&dialog, QStringLiteral("spice-analysis-dialog.png"), QStringLiteral("Editing the transient analysis."));
            dialog.close();
        }
    }

    void spiceSupply()
    {
        auto window = openExample(QStringLiteral("power_supply"), QStringLiteral("power_supply.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("supply"));
        if (!workspace)
            return;
        if (runSpice(workspace, QStringLiteral("Start-up and ripple"))) {
            spiceui::SpiceSimulationView *view = workspace->spiceSimulation();
            view->tabs()->setCurrentWidget(view->instrumentsView());
            focusResults(window.get(), workspace);
            settle(300);
            save(window.get(), QStringLiteral("spice-supply.png"),
                 QStringLiteral("Power supply example: rectifier and regulator with oscilloscope, voltmeter and ammeter."));
        }
    }

    void spiceFilter()
    {
        auto window = openExample(QStringLiteral("opamp_filter"), QStringLiteral("opamp_filter.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("opamp_filter"));
        if (!workspace)
            return;
        if (runSpice(workspace, QStringLiteral("Bode"))) {
            focusResults(window.get(), workspace);
            save(window.get(), QStringLiteral("spice-opamp-filter.png"),
                 QStringLiteral("User example: op-amp stage with an RC low-pass, Bode plot of v(out)/v(in)."));
        }
    }

    void spiceBjt()
    {
        auto window = openExample(QStringLiteral("bjt_amplifier"), QStringLiteral("bjt_amplifier.lvdproj"));
        SchematicWorkspace *workspace = openSchematic(window.get(), QStringLiteral("common_emitter"));
        if (!workspace)
            return;
        if (runSpice(workspace, QStringLiteral("Bias"))) {
            spiceui::SpiceSimulationView *view = workspace->spiceSimulation();
            view->tabs()->setCurrentWidget(view->valuesModel() ? view->tabs()->widget(1) : nullptr);
            focusResults(window.get(), workspace);
            save(window.get(), QStringLiteral("spice-bjt-bias.png"),
                 QStringLiteral("BJT amplifier: operating point on the schematic and in the Values tab."));
        }
        if (runSpice(workspace, QStringLiteral("Noise"))) {
            focusResults(window.get(), workspace);
            save(window.get(), QStringLiteral("spice-noise.png"), QStringLiteral("Output noise spectral density of the BJT stage."));
        }
        if (SchematicWorkspace *imported = openSchematic(window.get(), QStringLiteral("imported_netlist"))) {
            if (runSpice(imported, QStringLiteral("AC"))) {
                focusResults(window.get(), imported);
                save(window.get(), QStringLiteral("spice-imported-netlist.png"),
                     QStringLiteral("A SPICE netlist imported as a simulation-only document, with its AC analysis."));
            }
        }
    }


    antui::ArrayWorkspace *openArray(MainWindow *window, const QString &name)
    {
        Project *project = m_session->project();
        Document *document = project ? project->document(name) : nullptr;
        if (!document) {
            problem(QStringLiteral("document %1 not found").arg(name));
            return nullptr;
        }
        window->openDocument(document);
        settle(200);
        antui::ArrayWorkspace *workspace = window->currentArrayWorkspace();
        if (!workspace || !workspace->waitForResults(120000)) {
            problem(QStringLiteral("array %1 did not compute").arg(name));
            return nullptr;
        }
        settle(300);
        return workspace;
    }

    void antennaAesa()
    {
        auto window = openExample(QStringLiteral("aesa_radar"), QStringLiteral("aesa_radar.lvdproj"));
        antui::ArrayWorkspace *w = openArray(window.get(), QStringLiteral("aesa"));
        if (!w)
            return;
        w->showSettingsPage(1);
        w->showView(QStringLiteral("cuts"));
        settle(300);
        save(window.get(), QStringLiteral("antenna-aesa-cuts.png"),
             QStringLiteral("AESA example: geometry settings and cuts through the beam steered to 30 degrees, with the ideal reference dashed."));
        w->showSettingsPage(2);
        w->showView(QStringLiteral("uv"));
        settle(300);
        save(window.get(), QStringLiteral("antenna-aesa-uv.png"),
             QStringLiteral("UV map of the steered AESA beam with the visible region and grating-lobe circles."));
        w->showSettingsPage(3);
        w->showView(QStringLiteral("layout"));
        settle(300);
        save(window.get(), QStringLiteral("antenna-aesa-layout.png"),
             QStringLiteral("Element layout coloured by amplitude, failed elements crossed; AESA settings on the left."));
        w->showView(QStringLiteral("3d"));
        settle(600);
        save(window.get(), QStringLiteral("antenna-aesa-3d.png"), QStringLiteral("3D gain pattern of the AESA with the element positions."));
        w->showSettingsPage(4);
        w->showView(QStringLiteral("scan"));
        settle(300);
        save(window.get(), QStringLiteral("antenna-aesa-scan.png"),
             QStringLiteral("T/R module linked to the RF chains; scan sweep of gain, EIRP and G/T, and the band sweep."));
        w->showView(QStringLiteral("summary"));
        settle(300);
        save(window.get(), QStringLiteral("antenna-aesa-summary.png"), QStringLiteral("Summary table of the AESA example."));
    }

    void antennaSatcom()
    {
        auto window = openExample(QStringLiteral("satcom_panel"), QStringLiteral("satcom_panel.lvdproj"));
        antui::ArrayWorkspace *w = openArray(window.get(), QStringLiteral("panel_phase"));
        if (!w)
            return;
        w->showSettingsPage(6);
        w->showView(QStringLiteral("scan"));
        settle(300);
        save(window.get(), QStringLiteral("antenna-satcom-phase.png"),
             QStringLiteral("Ku-band panel steered to 45 degrees with phase shifters: the band sweep shows the beam squint."));
        w = openArray(window.get(), QStringLiteral("panel_subarray_ttd"));
        if (!w)
            return;
        w->showSettingsPage(2);
        w->showView(QStringLiteral("scan"));
        settle(300);
        save(window.get(), QStringLiteral("antenna-satcom-ttd.png"),
             QStringLiteral("The same panel with time delay per 4 x 4 sub-array: the squint nearly disappears."));
    }

    void antennaBeamforming()
    {
        auto window = openExample(QStringLiteral("beamforming"), QStringLiteral("beamforming.lvdproj"));
        if (antui::ArrayWorkspace *w = openArray(window.get(), QStringLiteral("synthesised_line"))) {
            w->showSettingsPage(5);
            w->showView(QStringLiteral("cuts"));
            w->setViewOptions(QStringLiteral("total"), false, 60);
            settle(300);
            save(window.get(), QStringLiteral("antenna-synthesis.png"),
                 QStringLiteral("Pattern synthesis: a 24-element line meets a -30 dB mask stepping to -40 dB beyond 35 degrees."));
        }
        if (antui::ArrayWorkspace *w = openArray(window.get(), QStringLiteral("two_beams_nulls"))) {
            w->showSettingsPage(2);
            w->showView(QStringLiteral("cuts"));
            w->setViewOptions(QStringLiteral("total"), true, 50);
            settle(300);
            save(window.get(), QStringLiteral("antenna-two-beams.png"),
                 QStringLiteral("Two simultaneous beams and two nulls on a polar plot."));
        }
        if (antui::ArrayWorkspace *w = openArray(window.get(), QStringLiteral("monopulse"))) {
            w->showView(QStringLiteral("uv"));
            settle(300);
            save(window.get(), QStringLiteral("antenna-monopulse.png"), QStringLiteral("Azimuth difference pattern of a 16 x 16 array in UV."));
        }
    }

    void antennaElements()
    {
        auto window = openExample(QStringLiteral("element_patterns"), QStringLiteral("element_patterns.lvdproj"));
        Project *project = m_session->project();
        for (const auto &[name, file, text] : {std::tuple{QStringLiteral("patch_x"), QStringLiteral("antenna-element-patch.png"),
                                                          QStringLiteral("Element document: 10 GHz patch, cavity model, E- and H-plane cuts.")},
                                               std::tuple{QStringLiteral("measured_horn"), QStringLiteral("antenna-element-horn.png"),
                                                          QStringLiteral("Element document with a horn pattern imported from a file.")}}) {
            Document *document = project ? project->document(name) : nullptr;
            if (!document) {
                problem(QStringLiteral("element %1 not found").arg(name));
                continue;
            }
            window->openDocument(document);
            settle(500);
            save(window.get(), file, text);
        }
        if (window->antennaLibraryPanel()) {
            settle(200);
            save(window->antennaLibraryPanel()->window(), QStringLiteral("antenna-library.png"),
                 QStringLiteral("The antenna library in the library dock (shown while an antenna document is open)."));
        }
    }


    void antennaHorns()
    {
        auto window = openExample(QStringLiteral("horn_amplitude_steering"), QStringLiteral("horn_amplitude_steering.lvdproj"));
        for (const auto &[name, file, text] :
             {std::tuple{QStringLiteral("arc_fan"), QStringLiteral("antenna-horn-arc.png"),
                         QStringLiteral("Five horns on an arc, no steering phases: equal power in the 0 and 20 degree horns puts the beam at 10 degrees.")},
              std::tuple{QStringLiteral("linear_fan"), QStringLiteral("antenna-horn-line.png"),
                         QStringLiteral("The same horns on a line: the coherent pair interferes and the beam stays near broadside.")}}) {
            antui::ArrayWorkspace *w = openArray(window.get(), name);
            if (!w)
                continue;
            w->showSettingsPage(2);
            w->showView(QStringLiteral("cuts"));
            w->setViewOptions(QStringLiteral("total"), false, 40);
            settle(300);
            save(window.get(), file, text);
        }
        if (antui::ArrayWorkspace *w = openArray(window.get(), QStringLiteral("arc_fan"))) {
            antui::PositionEditor editor(w->design().geometry, w->design().designFrequency, window.get());
            editor.show();
            settle(200);
            if (auto *plane = editor.findChild<QComboBox *>())
                plane->setCurrentIndex(1); // side view (x, z): the arc
            editor.canvas()->setSelection({5, 6});
            settle(400);
            save(&editor, QStringLiteral("antenna-position-editor.png"),
                 QStringLiteral("Element position editor: the horns of arc_fan in side view (x, z) with their boresights; the 5 and 15 degree horns selected."));
            editor.close();
        }
    }

    void antennaDf()
    {
        auto window = openExample(QStringLiteral("circular_df"), QStringLiteral("circular_df.lvdproj"));
        if (antui::ArrayWorkspace *w = openArray(window.get(), QStringLiteral("adcock_ew"))) {
            w->showSettingsPage(1);
            w->showView(QStringLiteral("cuts"));
            settle(300);
            save(window.get(), QStringLiteral("antenna-adcock.png"),
                 QStringLiteral("Four-monopole Adcock: the east-west difference pattern at the horizon (figure eight), polar top view."));
        }
        if (antui::ArrayWorkspace *w = openArray(window.get(), QStringLiteral("uca8_beam"))) {
            w->showSettingsPage(2);
            w->showView(QStringLiteral("cuts"));
            settle(300);
            save(window.get(), QStringLiteral("antenna-uca-beam.png"),
                 QStringLiteral("Eight monopoles on a ring steered to 60 degrees azimuth: the horizon cut used for beamscan DF."));
        }
    }

    bool isValid() const { return m_temp.isValid(); }

private:
    Session *m_session;
    PythonInterpreter *m_python;
    QString m_examples;
    QTemporaryDir m_temp;
    int m_copies = 0;
};

bool writeReadme()
{
    QFile file(QDir(g_outputDir).filePath(QStringLiteral("README.md")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out << "# Manual screenshots\n\n"
           "Generated by `tools/screenshots` (target `manual_screenshots`):\n"
           "`manual_screenshots <examples folder> <output folder>`. Do not edit by hand.\n\n"
           "| Image | Size (px) | Shows |\n|---|---|---|\n";
    for (const QStringList &row : std::as_const(g_readme))
        out << "| " << row.at(0) << " | " << row.at(1) << " | " << row.at(2) << " |\n";
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    // One device pixel per pixel, whatever the screen scaling.
    // Fonts at 96 dpi too, so text and drawings keep their 100 % proportions.
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_FONT_DPI", "96");
    QApplication app(argc, argv);
    // Like the application, but settings under another organisation so the
    // user's own settings (window state, last project...) are not touched.
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-screenshots"));
    QApplication::setApplicationName(QStringLiteral("lvdRFcad"));
    QApplication::setApplicationDisplayName(QStringLiteral("lvd RF cad"));
    QApplication::setApplicationVersion(QStringLiteral(LVD_VERSION));
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    QSettings().clear();

    const QStringList arguments = QApplication::arguments();
    if (arguments.size() != 3) {
        note(QStringLiteral("usage: manual_screenshots <examples folder> <output folder>"));
        return 2;
    }
    const QString examples = QFileInfo(arguments.at(1)).absoluteFilePath();
    g_outputDir = QFileInfo(arguments.at(2)).absoluteFilePath();
    if (!QDir().mkpath(g_outputDir)) {
        note(QStringLiteral("cannot create %1").arg(g_outputDir));
        return 1;
    }

    // Unattended: close any modal dialog that appears, and stop after a while.
    QTimer modalWatch;
    QObject::connect(&modalWatch, &QTimer::timeout, [] {
        if (QWidget *modal = QApplication::activeModalWidget()) {
            problem(QStringLiteral("a modal dialog appeared and was closed: \"%1\" (%2)")
                        .arg(modal->windowTitle(), QString::fromLatin1(modal->metaObject()->className())));
            modal->close();
        }
    });
    modalWatch.start(500);
    QTimer::singleShot(20 * 60 * 1000, [] {
        note(QStringLiteral("time limit reached"));
        std::_Exit(3);
    });

    Session session;
    QTemporaryDir userLibraries; // empty: no user components in the screenshots
    session.libraries()->setSearchPath(LibraryScope::User, userLibraries.path());

    std::unique_ptr<PythonInterpreter> python;
    try {
        python = std::make_unique<PythonInterpreter>(&session);
    } catch (const std::exception &error) {
        note(QStringLiteral("Python could not be started: %1").arg(QString::fromLocal8Bit(error.what())));
        return 1;
    }

    {
        Shooter shooter(&session, python.get(), examples);
        if (!shooter.isValid()) {
            note(QStringLiteral("no temporary folder"));
            return 1;
        }
        shooter.receiver();
        shooter.ruleCheck();
        shooter.receiverBudget();
        shooter.hierarchy();
        shooter.library();
        shooter.lowpass();
        shooter.python();
        shooter.lna();
        shooter.matching();
        shooter.upconverter();
        shooter.spiceAmplifier();
        shooter.spiceSupply();
        shooter.spiceBjt();
        shooter.spiceFilter();
        shooter.antennaAesa();
        shooter.antennaSatcom();
        shooter.antennaBeamforming();
        shooter.antennaElements();
        shooter.antennaHorns();
        shooter.antennaDf();
        session.closeProject();
    }

    if (!writeReadme())
        problem(QStringLiteral("cannot write README.md"));
    note(QStringLiteral("%1 images, %2 problems").arg(g_readme.size()).arg(g_problems.size()));
    for (const QString &text : std::as_const(g_problems))
        note(QStringLiteral("  - ") + text);
    return 0;
}
