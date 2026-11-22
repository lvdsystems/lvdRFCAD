#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Autosave.h"
#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "gui/AboutDialog.h"
#include "gui/MainWindow.h"
#include "gui/PythonConsole.h"
#include "gui/SchematicWorkspace.h"
#include "gui/ViolationsPanel.h"
#include "rfsim/LinearSolver.h"
#include "simulation_ui/SimulationView.h"
#include "sparam_ui/TouchstoneEditor.h"
#include "library_ui/LibraryBrowser.h"
#include "python/PythonInterpreter.h"
#include "schematic_editor/SchematicEditor.h"

#include <QApplication>
#include <QJsonDocument>
#include <QLabel>
#include <QPixmap>
#include <QSignalSpy>
#include <QTableWidget>

#include <cmath>
#include <QTemporaryDir>

using namespace lvd;

// One test case: the Python interpreter can exist only once per process.
TEST_CASE("the main window follows the project, from the GUI and from Python")
{
    QTemporaryDir dir;
    Session session;
    PythonInterpreter python(&session);
    MainWindow window(&session, &python);
    window.show();

    REQUIRE(window.projectTreeDocumentCount() == 0);
    REQUIRE(window.openDocumentCount() == 0);

    QString error;
    Project *project = session.newProject(dir.filePath(QStringLiteral("demo")), QStringLiteral("demo"), &error);
    REQUIRE(project);
    REQUIRE(window.windowTitle().startsWith(QStringLiteral("demo")));

    Document *receiver = project->createDocument(DocumentType::RfSchematic, QStringLiteral("receiver"), &error);
    REQUIRE(receiver);
    REQUIRE(window.projectTreeDocumentCount() == 1);

    window.openDocument(receiver);
    window.openDocument(receiver); // opening twice reuses the tab
    REQUIRE(window.openDocumentCount() == 1);

    // Changes made from the Python console show up in the window.
    PythonConsole *console = window.pythonConsole();
    console->execute(QStringLiteral("p = lvdrfcad.project()"));
    console->execute(QStringLiteral("d = p.new_document('spice', 'bias')"));
    REQUIRE(window.projectTreeDocumentCount() == 2);
    REQUIRE(project->document(QStringLiteral("bias")));

    console->execute(QStringLiteral("print([x.name for x in p.documents])"));
    REQUIRE(console->outputText().contains(QStringLiteral("['receiver', 'bias']")));

    // Multi-line statements are continued until the block is closed.
    console->execute(QStringLiteral("for t in lvdrfcad.document_types():"));
    console->execute(QStringLiteral("    print('type', t)"));
    console->execute(QString());
    REQUIRE(console->outputText().contains(QStringLiteral("type antenna")));

    // Errors are reported in the console and do not escape.
    console->execute(QStringLiteral("p.new_document('pcb', 'board')"));
    REQUIRE(console->outputText().contains(QStringLiteral("ValueError")));
    console->execute(QStringLiteral("exit()"));
    REQUIRE(console->outputText().contains(QStringLiteral("ignored")));

    // Removing an open document closes its tab; the stale Python object is safe.
    console->execute(QStringLiteral("r = p.document('receiver'); p.remove_document(r)"));
    REQUIRE(window.openDocumentCount() == 0);
    REQUIRE(window.projectTreeDocumentCount() == 1);
    console->execute(QStringLiteral("r.name"));
    REQUIRE(console->outputText().contains(QStringLiteral("no longer exists")));

    window.openDocument(project->document(QStringLiteral("bias")));
    const QByteArray screenshot = qgetenv("LVD_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QApplication::processEvents();
        REQUIRE(window.grab().save(QString::fromLocal8Bit(screenshot)));
    }

    // A schematic opens in the schematic editor, with its panels in the docks.
    console->execute(QStringLiteral("rx = p.new_document('rf', 'rx').schematic"));
    console->execute(QStringLiteral("rx.add_component('rf-system:source', (-200, 0))"));
    console->execute(QStringLiteral("rx.add_component('rf-system:amplifier', (0, 0))"));
    console->execute(QStringLiteral("rx.connect('SRC1', 'out', 'A1', 'in')"));
    Document *rx = project->document(QStringLiteral("rx"));
    REQUIRE(rx);
    window.openDocument(rx);
    SchematicEditor *editor = window.currentSchematicEditor();
    REQUIRE(editor);
    REQUIRE(editor->document() == rx);
    REQUIRE(editor->propertiesWidget()->isVisibleTo(&window));

    // Library browser activation starts placement in the current editor.
    QSignalSpy placed(window.libraryBrowser(), &LibraryBrowser::componentActivated);
    REQUIRE(window.libraryBrowser()->selectComponent(QStringLiteral("rf-system:termination")));
    Q_EMIT window.libraryBrowser()->componentActivated(QStringLiteral("rf-system:termination"));
    REQUIRE(placed.count() == 1);

    // Edits from Python reach the open editor; the rule check fills its panel.
    console->execute(QStringLiteral("rx.add_component('rf-system:termination', (200, 0))"));
    QApplication::processEvents();
    REQUIRE(editor->schematic()->components().size() == 3);
    const QList<RuleViolation> violations = editor->runRuleCheck();
    REQUIRE_FALSE(violations.isEmpty()); // A1.out and TERM1 are not connected
    REQUIRE(window.violationsPanel()->count() == violations.size());

    console->execute(QStringLiteral("rx.connect('A1', 'out', 'TERM1', 'p')"));
    editor->zoomToFit();

    // Simulation: an amplifier between two ports, run in the background.
    const qsizetype outputBefore = console->outputText().size();
    console->execute(QStringLiteral("amp = p.new_document('rf', 'amp').schematic"));
    console->execute(QStringLiteral("amp.add_component('rf-system:port', (-200, 0), parameters={'num': 1})"));
    console->execute(QStringLiteral("amp.add_component('rf-system:port', (200, 0), parameters={'num': 2})"));
    console->execute(QStringLiteral("amp.add_component('rf-system:amplifier', (0, 0), parameters={'gain': '12'})"));
    console->execute(QStringLiteral("amp.connect('PORT1', 'p', 'A1', 'in'); amp.connect('A1', 'out', 'PORT2', 'p')"));
    console->execute(QStringLiteral("amp.add_analysis('1GHz', '2GHz', 11, name='Gain')"));
    INFO(console->outputText().mid(outputBefore).toStdString());
    REQUIRE_FALSE(console->outputText().mid(outputBefore).contains(QStringLiteral("Error")));
    window.openDocument(project->document(QStringLiteral("amp")));
    SchematicWorkspace *workspace = window.currentWorkspace();
    REQUIRE(workspace);
    QSignalSpy finished(workspace->simulation(), &SimulationView::runFinished);
    workspace->runSimulation();
    REQUIRE((finished.count() > 0 || finished.wait(20000)));
    REQUIRE(finished.first().first().toBool());
    const rf::LinearResult *result = workspace->simulation()->lastResult();
    REQUIRE(result);
    REQUIRE(std::abs(20 * std::log10(std::abs(result->network.s[0](1, 0))) - 12) < 1e-9);
    REQUIRE(workspace->isSimulationVisible());
    // The result opens in the S-parameter editor; it survives closing the project.
    const int tabsBefore = window.openDocumentCount();
    TouchstoneEditor *sparams = window.addTouchstoneTab();
    sparams->setNetwork(result->network, QStringLiteral("amp"));
    REQUIRE(window.openDocumentCount() == tabsBefore + 1);
    REQUIRE(sparams->network().portCount() == 2);
    const QString touchstonePath = dir.filePath(QStringLiteral("amp.s2p"));
    rf::TouchstoneWriteOptions writeOptions;
    QString saveError;
    REQUIRE(sparams->saveAs(touchstonePath, writeOptions, &saveError));
    REQUIRE(window.openTouchstoneFile(touchstonePath) == sparams); // already open: same tab
    window.openDocument(project->document(QStringLiteral("amp")));

    const QByteArray simulationShot = qgetenv("LVD_SCREENSHOT_SIMULATION");
    if (!simulationShot.isEmpty()) {
        QApplication::processEvents();
        REQUIRE(window.grab().save(QString::fromLocal8Bit(simulationShot)));
    }
    window.openDocument(rx);
    const QByteArray editorShot = qgetenv("LVD_SCREENSHOT_EDITOR");
    if (!editorShot.isEmpty()) {
        QApplication::processEvents();
        REQUIRE(window.grab().save(QString::fromLocal8Bit(editorShot)));
    }

    console->execute(QStringLiteral("lvdrfcad.close_project()"));
    REQUIRE(session.project() == nullptr);
    REQUIRE(window.openDocumentCount() == 1); // the saved S-parameter tab stays
    REQUIRE(window.projectTreeDocumentCount() == 0);

    // Crash recovery: another session leaves an autosaved copy behind ...
    Autosave::setBaseDirectory(dir.filePath(QStringLiteral("autosave")));
    QString crashedProject;
    {
        Session crashed;
        Autosave autosave(&crashed);
        Project *p = crashed.newProject(dir.filePath(QStringLiteral("crashed")), QStringLiteral("crashed"), &error);
        REQUIRE(p);
        crashedProject = p->filePath();
        Document *doc = p->createDocument(DocumentType::RfSchematic, QStringLiteral("sheet"), &error);
        REQUIRE(doc);
        REQUIRE(p->saveAll(&error));
        doc->schematic()->addText(QStringLiteral("work in progress"), QPoint(0, 0));
        REQUIRE(autosave.saveNow(&error));
    } // no clean close: the copy stays
    REQUIRE(Autosave::pendingRecovery(crashedProject));
    // ... which opening the project restores.
    window.setRecoveryAnswerForTests(1);
    REQUIRE(window.openProjectFile(crashedProject));
    Document *sheet = session.project()->document(QStringLiteral("sheet"));
    REQUIRE(sheet);
    CHECK(sheet->isModified());
    CHECK(QJsonDocument(sheet->content()).toJson().contains("work in progress"));
    CHECK_FALSE(Autosave::pendingRecovery(crashedProject));
    // The window's own autosave writes a copy; destroying the window (an orderly end) removes it.
    REQUIRE(window.autosave()->saveNow(&error));
    CHECK(Autosave::pendingRecovery(crashedProject));
    session.closeProject();
    CHECK_FALSE(Autosave::pendingRecovery(crashedProject));
    Autosave::setBaseDirectory(QString());

    // Help > About lists the bundled components and finds the notices.
    AboutDialog about(QStringLiteral("3.14"), [](const QString &relative) { return MainWindow::documentationPath(relative); });
    auto *components = about.findChild<QTableWidget *>(QStringLiteral("components"));
    REQUIRE(components);
    CHECK(components->rowCount() == AboutDialog::components(QStringLiteral("3.14")).size());
    // The logo and the icon are compiled into the program; the website is shown as a link.
    auto *logo = about.findChild<QLabel *>(QStringLiteral("logo"));
    REQUIRE(logo);
    CHECK_FALSE(logo->pixmap().isNull());
    auto *title = about.findChild<QLabel *>(QStringLiteral("title"));
    REQUIRE(title);
    CHECK(title->text().contains(AboutDialog::website()));
    CHECK(MainWindow::applicationIcon().availableSizes().size() == 7);
    CHECK_FALSE(MainWindow::documentationPath(QStringLiteral("THIRD-PARTY-NOTICES.md")).isEmpty());
    CHECK_FALSE(MainWindow::documentationPath(QStringLiteral("LICENSE")).isEmpty());
    if (const QByteArray shot = qgetenv("LVD_SCREENSHOT_ABOUT"); !shot.isEmpty()) {
        about.show();
        QApplication::processEvents();
        REQUIRE(about.grab().save(QString::fromLocal8Bit(shot)));
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Keep test runs away from the real application's stored settings.
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("gui_tests"));
    return Catch::Session().run(argc, argv);
}
