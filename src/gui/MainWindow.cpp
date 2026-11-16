#include "gui/MainWindow.h"

#include "antenna/AntennaLibrary.h"
#include "antenna_ui/AntennaLibraryPanel.h"
#include "antenna_ui/ArrayWorkspace.h"
#include "antenna_ui/ElementWorkspace.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "gui/DocumentView.h"
#include "gui/NewProjectDialog.h"
#include "gui/PythonConsole.h"
#include "gui/SchematicWorkspace.h"
#include "gui/ViolationsPanel.h"
#include "simulation_ui/SimulationView.h"
#include "spice/SpiceImport.h"
#include "spice_ui/ModelImportDialog.h"
#include "spice_ui/SpiceSimulationView.h"
#include "sparam_ui/TouchstoneEditor.h"
#include "matching/MatchingWindow.h"
#include "rfdata/NetworkMath.h"
#include "rfsim/LinearSolver.h"
#include "library_ui/LibraryBrowser.h"
#include "library_ui/LibraryManagerDialog.h"
#include "library_ui/SymbolEditor.h"
#include "python/PythonInterpreter.h"
#include "schematic_editor/SchematicEditor.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QDesktopServices>
#include <QUrl>
#include <QTime>
#include <QToolBar>
#include <QTreeWidget>

namespace lvd {

namespace {

const int DocumentRole = Qt::UserRole;
const char *DocumentProperty = "lvdDocument";

Document *itemDocument(const QTreeWidgetItem *item)
{
    return item ? static_cast<Document *>(item->data(0, DocumentRole).value<void *>()) : nullptr;
}

QWidget *placeholder(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setMargin(12);
    return label;
}

} // namespace

MainWindow::MainWindow(Session *session, PythonInterpreter *python, QWidget *parent)
    : QMainWindow(parent), m_session(session), m_python(python)
{
    resize(1400, 900);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    setCentralWidget(m_tabs);

    m_editorToolBar = new QToolBar(tr("Schematic"), this);
    m_editorToolBar->setObjectName(QStringLiteral("schematicToolBar"));
    m_editorToolBar->setIconSize(QSize(20, 20));
    addToolBar(Qt::TopToolBarArea, m_editorToolBar);

    createDocks();
    createActions();
    statusBar();

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) { closeTab(index); });
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
    connect(m_session, &Session::projectAboutToClose, this, &MainWindow::onProjectAboutToClose);
    connect(m_session, &Session::projectChanged, this, &MainWindow::onProjectChanged);

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("mainWindow/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("mainWindow/state")).toByteArray());

    for (const QString &warning : m_session->libraries()->warnings())
        logMessage(tr("Library warning: %1").arg(warning));
    onProjectChanged(m_session->project());
}

MainWindow::~MainWindow()
{
    // Editors own their panels; close them before the docks go away.
    while (m_tabs->count() > 0)
        closeTab(0, false);
}

void MainWindow::closeDocumentTabs()
{
    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        if (documentAt(i))
            closeTab(i, false);
    }
}

void MainWindow::createDocks()
{
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    const auto dock = [this](const QString &title, const QString &name, QWidget *widget, Qt::DockWidgetArea area) {
        auto *result = new QDockWidget(title, this);
        result->setObjectName(name);
        result->setWidget(widget);
        addDockWidget(area, result);
        return result;
    };

    // Left: project and library
    m_projectTree = new QTreeWidget(this);
    m_projectTree->setHeaderLabels({tr("Name"), tr("Type")});
    m_projectTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_projectTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_projectTree->header()->setStretchLastSection(false);
    m_projectTree->setFrameShape(QFrame::NoFrame);
    m_projectTree->setContextMenuPolicy(Qt::CustomContextMenu);
    QDockWidget *projectDock = dock(tr("Project"), QStringLiteral("projectDock"), m_projectTree, Qt::LeftDockWidgetArea);

    m_libraryBrowser = new LibraryBrowser(m_session->libraries(), this);
    // Antenna documents show the antenna library in the same dock.
    m_antennaLibraries = new ant::AntennaLibraryManager(m_session->libraries(), this);
    m_antennaLibraryPanel = new antui::AntennaLibraryPanel(m_antennaLibraries, this);
    m_libraryStack = new QStackedWidget(this);
    m_libraryStack->addWidget(m_libraryBrowser);
    m_libraryStack->addWidget(m_antennaLibraryPanel);
    QDockWidget *libraryDock = dock(tr("Library"), QStringLiteral("libraryDock"), m_libraryStack, Qt::LeftDockWidgetArea);
    splitDockWidget(projectDock, libraryDock, Qt::Vertical);

    // Right: properties and variables of the current schematic
    m_propertiesStack = new QStackedWidget(this);
    m_noProperties = placeholder(tr("Open a schematic and select an item to see its properties."), m_propertiesStack);
    m_propertiesStack->addWidget(m_noProperties);
    QDockWidget *propertiesDock =
        dock(tr("Properties"), QStringLiteral("propertiesDock"), m_propertiesStack, Qt::RightDockWidgetArea);

    m_variablesStack = new QStackedWidget(this);
    m_noVariables = placeholder(tr("Variables of the current schematic appear here."), m_variablesStack);
    m_variablesStack->addWidget(m_noVariables);
    QDockWidget *variablesDock =
        dock(tr("Variables"), QStringLiteral("variablesDock"), m_variablesStack, Qt::RightDockWidgetArea);
    splitDockWidget(propertiesDock, variablesDock, Qt::Vertical);

    // Bottom: console, messages, rule check
    m_console = new PythonConsole(m_python, this);
    QDockWidget *consoleDock = dock(tr("Python Console"), QStringLiteral("pythonConsoleDock"), m_console, Qt::BottomDockWidgetArea);

    m_messages = new QPlainTextEdit(this);
    m_messages->setReadOnly(true);
    m_messages->setFrameShape(QFrame::NoFrame);
    m_messages->setMaximumBlockCount(5000);
    QDockWidget *messagesDock = dock(tr("Messages"), QStringLiteral("messagesDock"), m_messages, Qt::BottomDockWidgetArea);

    m_violations = new ViolationsPanel(this);
    m_violationsDock = dock(tr("Rule Check"), QStringLiteral("ruleCheckDock"), m_violations, Qt::BottomDockWidgetArea);

    tabifyDockWidget(consoleDock, messagesDock);
    tabifyDockWidget(messagesDock, m_violationsDock);
    consoleDock->raise();

    resizeDocks({projectDock, propertiesDock}, {280, 360}, Qt::Horizontal);
    resizeDocks({projectDock, libraryDock}, {200, 500}, Qt::Vertical);
    resizeDocks({consoleDock}, {200}, Qt::Vertical);

    m_viewMenu = new QMenu(tr("&View"), this);
    for (QDockWidget *widget : {projectDock, libraryDock, propertiesDock, variablesDock, consoleDock, messagesDock,
                                m_violationsDock})
        m_viewMenu->addAction(widget->toggleViewAction());
    m_viewMenu->addSeparator();
    m_viewMenu->addAction(m_editorToolBar->toggleViewAction());

    connect(m_projectTree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (Document *document = itemDocument(item))
            openDocument(document);
    });
    connect(m_projectTree, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateWindowState);
    connect(m_projectTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        if (!m_session->project())
            return;
        QMenu menu(this);
        if (Document *document = itemDocument(m_projectTree->itemAt(position))) {
            menu.addAction(tr("Open"), this, [this, document] { openDocument(document); });
            if (document->isSchematic()) {
                if (SchematicEditor *editor = currentSchematicEditor(); editor && editor->document() != document) {
                    menu.addAction(tr("Place as Sheet in %1").arg(editor->document()->name()), this, [this, document] {
                        if (SchematicEditor *target = currentSchematicEditor())
                            target->startPlacing(Project::sheetKey(document->name()));
                    });
                }
            }
            menu.addAction(m_removeDocumentAction);
        } else {
            menu.addActions(m_newDocumentActions);
        }
        menu.exec(m_projectTree->viewport()->mapToGlobal(position));
    });

    connect(m_libraryBrowser, &LibraryBrowser::componentActivated, this, [this](const QString &key) {
        SchematicEditor *editor = currentSchematicEditor();
        if (!editor) {
            statusBar()->showMessage(tr("Open a schematic to place %1").arg(key), 4000);
            return;
        }
        editor->startPlacing(key);
        editor->setFocus();
    });
    connect(m_antennaLibraryPanel, &antui::AntennaLibraryPanel::entryActivated, this, &MainWindow::applyAntennaLibraryEntry);
    connect(m_libraryBrowser, &LibraryBrowser::editComponentRequested, this, &MainWindow::editComponent);
    connect(m_libraryBrowser, &LibraryBrowser::newComponentRequested, this, &MainWindow::newComponent);
    connect(m_libraryBrowser, &LibraryBrowser::manageLibrariesRequested, this, &MainWindow::manageLibraries);
    connect(m_violations, &ViolationsPanel::itemActivated, this, &MainWindow::showViolation);
}

void MainWindow::createActions()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&New Project..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), this,
                        &MainWindow::newProject);
    fileMenu->addAction(tr("&Open Project..."), QKeySequence::Open, this, &MainWindow::openProject);
    m_closeProjectAction = fileMenu->addAction(tr("&Close Project"), this, &MainWindow::closeProject);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Open &Touchstone File..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open Touchstone File"), m_session->project() ? m_session->project()->directory() : QString(),
            tr("Touchstone files (*.s*p *.ts);;All files (*)"));
        if (!path.isEmpty())
            openTouchstoneFile(path);
    });
    fileMenu->addAction(tr("Import SPICE &Netlist..."), this, [this] {
        Project *project = m_session->project();
        if (!project) {
            statusBar()->showMessage(tr("Open or create a project first."), 4000);
            return;
        }
        const QString path = QFileDialog::getOpenFileName(this, tr("Import SPICE Netlist"), project->directory(),
                                                          tr("SPICE netlists (*.cir *.sp *.spi *.net *.ckt);;All files (*)"));
        if (path.isEmpty())
            return;
        QString error;
        QStringList warnings;
        Document *document = spice::importNetlistDocument(*project, path, QString(), &error, &warnings);
        if (!document) {
            reportError(tr("Import SPICE Netlist"), error);
            return;
        }
        openDocument(document);
        statusBar()->showMessage(warnings.isEmpty() ? tr("Imported %1.").arg(QFileInfo(path).fileName())
                                                    : warnings.join(QLatin1Char(' ')),
                                 8000);
    });
    fileMenu->addSeparator();

    QMenu *newDocumentMenu = fileMenu->addMenu(tr("New &Document"));
    for (const DocumentTypeInfo &info : documentTypes()) {
        const DocumentType type = info.type;
        m_newDocumentActions.append(
            newDocumentMenu->addAction(tr("New %1...").arg(info.displayName), this, [this, type] { newDocument(type); }));
    }
    m_removeDocumentAction =
        fileMenu->addAction(tr("&Remove Document from Project..."), this, &MainWindow::removeSelectedDocument);
    fileMenu->addSeparator();

    m_saveAction = fileMenu->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::saveCurrentDocument);
    m_saveAllAction =
        fileMenu->addAction(tr("Save &All"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), this, &MainWindow::saveAll);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    // Filled with the current editor's actions.
    m_editMenu = menuBar()->addMenu(tr("&Edit"));

    menuBar()->addMenu(m_viewMenu);

    QMenu *simulateMenu = menuBar()->addMenu(tr("&Simulate"));
    m_runSimulationAction = simulateMenu->addAction(tr("&Run Simulation"), QKeySequence(Qt::CTRL | Qt::Key_R), this, [this] {
        if (SchematicWorkspace *workspace = currentWorkspace())
            workspace->runSimulation();
    });
    m_newAnalysisAction = simulateMenu->addAction(tr("&New Analysis..."), this, [this] {
        if (SchematicWorkspace *workspace = currentWorkspace())
            workspace->newAnalysis();
    });
    m_resultToEditorAction = simulateMenu->addAction(tr("Open Result in S-Parameter &Editor"), this, [this] {
        SchematicWorkspace *workspace = currentWorkspace();
        const rf::LinearResult *result = workspace && workspace->simulation() ? workspace->simulation()->lastResult() : nullptr;
        if (!result || !result->ok()) {
            statusBar()->showMessage(tr("Run a simulation first."), 4000);
            return;
        }
        Document *document = currentDocument();
        TouchstoneEditor *editor = addTouchstoneTab();
        editor->setNetwork(result->network, document ? document->name() : tr("result"));
    });
    simulateMenu->addSeparator();
    m_tuneAction = simulateMenu->addAction(tr("&Tune..."), this, [this] {
        if (SchematicWorkspace *workspace = currentWorkspace(); workspace && workspace->simulation()) {
            workspace->setSimulationVisible(true);
            workspace->simulation()->startTuning();
        }
    });
    m_optimizeAction = simulateMenu->addAction(tr("&Optimize..."), this, [this] {
        if (SchematicWorkspace *workspace = currentWorkspace()) {
            workspace->setSimulationVisible(true);
            if (workspace->simulation())
                workspace->simulation()->openOptimization(QString());
            else
                workspace->spiceSimulation()->optimizeAction()->trigger();
        }
    });
    simulateMenu->addAction(tr("&Matching Network Synthesis..."), this, &MainWindow::showMatchingWindow);
    simulateMenu->addSeparator();
    m_showResultsAction = simulateMenu->addAction(tr("Show &Results"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    m_showResultsAction->setCheckable(true);
    connect(m_showResultsAction, &QAction::triggered, this, [this](bool checked) {
        if (SchematicWorkspace *workspace = currentWorkspace())
            workspace->setSimulationVisible(checked);
    });
    connect(simulateMenu, &QMenu::aboutToShow, this, [this] {
        const SchematicWorkspace *workspace = currentWorkspace();
        m_showResultsAction->setChecked(workspace && workspace->isSimulationVisible());
    });

    QMenu *libraryMenu = menuBar()->addMenu(tr("&Library"));
    libraryMenu->addAction(tr("&New Component..."), this, [this] { newComponent(QString()); });
    libraryMenu->addAction(tr("&Manage Libraries..."), this, &MainWindow::manageLibraries);
    libraryMenu->addAction(tr("Import SPICE &Models..."), this, [this] {
        spiceui::ModelImportDialog dialog(m_session->libraries(), m_session->project(), this);
        if (dialog.exec() == QDialog::Accepted)
            statusBar()->showMessage(dialog.summary(), 8000);
    });

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&User Manual"), QKeySequence::HelpContents, this, &MainWindow::showManual);
    helpMenu->addAction(tr("Open &Examples Folder"), this, [this] {
        const QString folder = documentationPath(QStringLiteral("examples"));
        if (folder.isEmpty())
            reportError(tr("Examples"), tr("The examples folder was not found."));
        else
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    helpMenu->addSeparator();
    helpMenu->addAction(tr("&About lvd RF cad"), this, &MainWindow::showAbout);
    helpMenu->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);
}

void MainWindow::openDocument(Document *document)
{
    if (!document)
        return;
    QWidget *view = m_views.value(document);
    if (!view) {
        if (document->isSchematic() && m_session->project()) {
            auto *workspace = new SchematicWorkspace(m_session->project(), document, m_tabs);
            SchematicEditor *editor = workspace->editor();
            if (workspace->simulation())
                connect(workspace->simulation(), &SimulationView::statusMessage, this,
                        [this](const QString &text) { statusBar()->showMessage(text, 5000); });
            else
                connect(workspace->spiceSimulation(), &spiceui::SpiceSimulationView::statusMessage, this,
                        [this](const QString &text) { statusBar()->showMessage(text, 5000); });
            m_propertiesStack->addWidget(editor->propertiesWidget());
            m_variablesStack->addWidget(editor->variablesWidget());
            connect(editor, &SchematicEditor::statusMessage, this,
                    [this](const QString &text) { statusBar()->showMessage(text, 5000); });
            connect(editor, &SchematicEditor::ruleCheckFinished, this,
                    [this, document](const QList<RuleViolation> &violations) { onRuleCheckFinished(document, violations); });
            view = workspace;
        } else if (document->type() == DocumentType::AntennaArray) {
            auto *workspace = new antui::ArrayWorkspace(m_session->project(), document, m_antennaLibraries, m_tabs);
            connect(workspace, &antui::ArrayWorkspace::statusMessage, this,
                    [this](const QString &text) { statusBar()->showMessage(text, 5000); });
            view = workspace;
        } else if (document->type() == DocumentType::AntennaElement) {
            auto *workspace = new antui::ElementWorkspace(m_session->project(), document, m_antennaLibraries, m_tabs);
            connect(workspace, &antui::ElementWorkspace::statusMessage, this,
                    [this](const QString &text) { statusBar()->showMessage(text, 5000); });
            view = workspace;
        } else {
            view = new DocumentView(document, m_tabs);
        }
        view->setProperty(DocumentProperty, QVariant::fromValue(static_cast<void *>(document)));
        m_views.insert(document, view);
        m_tabs->addTab(view, QString());
        m_tabs->setTabToolTip(m_tabs->indexOf(view), QDir::toNativeSeparators(document->filePath()));
        // The view is the context object, so the connection ends with the tab.
        connect(document, &Document::modifiedChanged, view, [this, document] {
            updateTabTitle(document);
            updateWindowState();
        });
        updateTabTitle(document);
    }
    m_tabs->setCurrentWidget(view);
    view->setFocus();
}

TouchstoneEditor *MainWindow::addTouchstoneTab()
{
    auto *editor = new TouchstoneEditor(m_tabs);
    const int index = m_tabs->addTab(editor, editor->title());
    connect(editor, &TouchstoneEditor::titleChanged, this, [this, editor](const QString &title) {
        m_tabs->setTabText(m_tabs->indexOf(editor), title);
        m_tabs->setTabToolTip(m_tabs->indexOf(editor), QDir::toNativeSeparators(editor->filePath()));
    });
    connect(editor, &TouchstoneEditor::statusMessage, this,
            [this](const QString &text) { statusBar()->showMessage(text, 5000); });
    m_tabs->setCurrentIndex(index);
    return editor;
}

TouchstoneEditor *MainWindow::openTouchstoneFile(const QString &path)
{
    // An already open file comes to the front.
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *existing = qobject_cast<TouchstoneEditor *>(m_tabs->widget(i));
        if (existing && QFileInfo(existing->filePath()) == QFileInfo(path)) {
            m_tabs->setCurrentIndex(i);
            return existing;
        }
    }
    TouchstoneEditor *editor = addTouchstoneTab();
    QString error;
    if (!editor->openFile(path, &error)) {
        closeTab(m_tabs->indexOf(editor), false);
        reportError(tr("Open Touchstone File"), error);
        return nullptr;
    }
    m_tabs->setTabText(m_tabs->indexOf(editor), editor->title());
    m_tabs->setTabToolTip(m_tabs->indexOf(editor), QDir::toNativeSeparators(path));
    for (const QString &warning : editor->readWarnings())
        logMessage(tr("%1: %2").arg(QFileInfo(path).fileName(), warning));
    return editor;
}

bool MainWindow::openProjectFile(const QString &path)
{
    QString error;
    Project *project = m_session->openProject(path, &error);
    if (!project) {
        reportError(tr("Open Project"), error);
        return false;
    }
    for (const QString &warning : project->loadWarnings())
        logMessage(tr("Warning: %1").arg(warning));
    return true;
}

void MainWindow::logMessage(const QString &message)
{
    m_messages->appendPlainText(
        QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), message));
}

int MainWindow::openDocumentCount() const
{
    return m_tabs->count();
}

int MainWindow::projectTreeDocumentCount() const
{
    const QTreeWidgetItem *root = m_projectTree->topLevelItem(0);
    return root ? root->childCount() : 0;
}

SchematicEditor *MainWindow::currentSchematicEditor() const
{
    const auto *workspace = qobject_cast<SchematicWorkspace *>(m_tabs->currentWidget());
    return workspace ? workspace->editor() : nullptr;
}

SchematicEditor *MainWindow::schematicEditor(Document *document) const
{
    const auto *workspace = qobject_cast<SchematicWorkspace *>(m_views.value(document));
    return workspace ? workspace->editor() : nullptr;
}

SchematicWorkspace *MainWindow::currentWorkspace() const
{
    return qobject_cast<SchematicWorkspace *>(m_tabs->currentWidget());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *touchstone = qobject_cast<TouchstoneEditor *>(m_tabs->widget(i));
        if (touchstone && !touchstone->maybeSave()) {
            m_tabs->setCurrentIndex(i);
            event->ignore();
            return;
        }
    }
    if (!confirmDiscardChanges()) {
        event->ignore();
        return;
    }
    if (m_symbolEditor && !m_symbolEditor->close()) {
        event->ignore();
        return;
    }
    // Docks hidden only while an antenna document is current are saved as shown.
    for (QDockWidget *dockWidget : findChildren<QDockWidget *>())
        if (dockWidget->property("lvdHiddenForAntenna").toBool()) {
            dockWidget->setProperty("lvdHiddenForAntenna", false);
            dockWidget->show();
        }
    QSettings settings;
    settings.setValue(QStringLiteral("mainWindow/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("mainWindow/state"), saveState());
    settings.setValue(QStringLiteral("lastProject"),
                      m_session->project() ? m_session->project()->filePath() : QString());
    event->accept();
}

void MainWindow::newProject()
{
    NewProjectDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted || !confirmDiscardChanges())
        return;
    QString error;
    if (!m_session->newProject(dialog.projectDirectory(), dialog.projectName(), &error))
        reportError(tr("New Project"), error);
}

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Project"), m_session->project() ? m_session->project()->directory() : QString(),
        tr("lvd RF cad projects (*.%1)").arg(Project::fileExtension()));
    if (path.isEmpty() || !confirmDiscardChanges())
        return;
    openProjectFile(path);
}

void MainWindow::closeProject()
{
    if (confirmDiscardChanges())
        m_session->closeProject();
}

void MainWindow::newDocument(DocumentType type)
{
    Project *project = m_session->project();
    if (!project)
        return;
    const DocumentTypeInfo &info = documentTypeInfo(type);
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("New %1").arg(info.displayName), tr("Document name:"),
                                               QLineEdit::Normal, QString(), &accepted);
    if (!accepted)
        return;
    QString error;
    Document *document = project->createDocument(type, name, &error);
    if (!document) {
        reportError(tr("New %1").arg(info.displayName), error);
        return;
    }
    openDocument(document);
}

void MainWindow::saveCurrentDocument()
{
    Document *document = currentDocument();
    if (!document)
        return;
    QString error;
    if (document->save(&error))
        statusBar()->showMessage(tr("Saved %1").arg(QDir::toNativeSeparators(document->filePath())), 4000);
    else
        reportError(tr("Save"), error);
}

void MainWindow::saveAll()
{
    Project *project = m_session->project();
    if (!project)
        return;
    QString error;
    if (project->saveAll(&error))
        statusBar()->showMessage(tr("Saved project %1").arg(project->name()), 4000);
    else
        reportError(tr("Save All"), error);
}

void MainWindow::removeSelectedDocument()
{
    Project *project = m_session->project();
    Document *document = selectedTreeDocument();
    if (!project || !document)
        return;
    const auto answer = QMessageBox::question(
        this, tr("Remove Document"),
        tr("Remove \"%1\" from the project?\n\nThe file stays on disk.").arg(document->name()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    QString error;
    if (!project->removeDocument(document, false, &error))
        reportError(tr("Remove Document"), error);
}

QString MainWindow::documentationPath(const QString &relative)
{
    // Installed next to the program, or in the source tree for development builds.
    for (const QString &base : {QCoreApplication::applicationDirPath(), QStringLiteral(LVD_SOURCE_TREE)}) {
        const QString path = QDir(base).filePath(relative);
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

void MainWindow::showManual()
{
    const QString path = documentationPath(QStringLiteral("docs/manual/manual.md"));
    if (path.isEmpty()) {
        reportError(tr("User Manual"), tr("The user manual was not found."));
        return;
    }
    if (!m_manual) {
        m_manual = new QTextBrowser(this);
        m_manual->setWindowFlag(Qt::Window);
        m_manual->setAttribute(Qt::WA_DeleteOnClose);
        m_manual->setWindowTitle(tr("lvd RF cad User Manual"));
        m_manual->setOpenExternalLinks(true);
        m_manual->resize(1000, 800);
        m_manual->setSource(QUrl::fromLocalFile(path), QTextDocument::MarkdownResource);
    }
    m_manual->show();
    m_manual->raise();
    m_manual->activateWindow();
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About lvd RF cad"),
                       tr("<b>lvd RF cad %1</b><br>RF, SPICE and antenna design CAD.<br><br>"
                          "Qt %2, Python %3.<br>Licensed under the GNU General Public License, version 3 or later.")
                           .arg(QStringLiteral(LVD_VERSION), QString::fromLatin1(qVersion()), m_python->pythonVersion()));
}

void MainWindow::editComponent(const QString &definitionKey)
{
    if (definitionKey.startsWith(QStringLiteral("sheet:"))) {
        if (Project *project = m_session->project())
            openDocument(project->document(definitionKey.mid(6)));
        return;
    }
    // One symbol editor window; unsaved work in it is never dropped silently.
    if (m_symbolEditor && m_symbolEditor->isModified()) {
        m_symbolEditor->show();
        m_symbolEditor->raise();
        if (!m_symbolEditor->close())
            return;
        m_symbolEditor = nullptr; // deleted on close
    }
    if (!m_symbolEditor) {
        m_symbolEditor = new SymbolEditor(m_session->libraries(), this);
        m_symbolEditor->setWindowFlag(Qt::Window);
        m_symbolEditor->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_symbolEditor, &SymbolEditor::componentSaved, this, [this](const QString &key) {
            logMessage(tr("Saved component %1").arg(key));
            m_libraryBrowser->selectComponent(key);
        });
    }
    QString error;
    if (!m_symbolEditor->openComponent(definitionKey, &error)) {
        reportError(tr("Edit Symbol"), error);
        return;
    }
    m_symbolEditor->show();
    m_symbolEditor->raise();
    m_symbolEditor->activateWindow();
}

void MainWindow::newComponent(const QString &libraryName)
{
    if (m_symbolEditor && m_symbolEditor->isModified()) {
        m_symbolEditor->show();
        m_symbolEditor->raise();
        if (!m_symbolEditor->close())
            return;
        m_symbolEditor = nullptr;
    }
    auto *editor = new SymbolEditor(m_session->libraries(), this);
    editor->setWindowFlag(Qt::Window);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &SymbolEditor::componentSaved, this, [this](const QString &key) {
        logMessage(tr("Saved component %1").arg(key));
        m_libraryBrowser->selectComponent(key);
    });
    if (!editor->newComponentInteractive(libraryName)) {
        delete editor;
        return;
    }
    if (m_symbolEditor)
        m_symbolEditor->close();
    m_symbolEditor = editor;
    editor->show();
    editor->raise();
    editor->activateWindow();
}

void MainWindow::showMatchingWindow()
{
    if (!m_matching) {
        m_matching = new MatchingWindow(this);
        m_matching->setWindowFlag(Qt::Window);
        m_matching->setWindowTitle(tr("Matching Network Synthesis"));
        m_matching->resize(1100, 750);
        connect(m_matching, &MatchingWindow::statusMessage, this,
                [this](const QString &text) { statusBar()->showMessage(text, 5000); });
        // Start from the load seen at port 1 of the latest result, at its middle frequency.
        if (SchematicWorkspace *workspace = currentWorkspace(); workspace && workspace->simulation()) {
            const rf::LinearResult *result = workspace->simulation()->lastResult();
            if (result && result->ok() && !result->network.frequencies.empty()) {
                const size_t middle = result->network.frequencies.size() / 2;
                const double z0 = result->network.z0.front();
                const std::complex<double> zl = rf::impedanceFromReflection(result->network.s[middle](0, 0), z0);
                if (std::isfinite(zl.real()) && zl.real() > 0)
                    m_matching->setImpedances(z0, zl, result->network.frequencies[middle]);
            }
        }
    }
    updateMatchingTarget();
    m_matching->show();
    m_matching->raise();
    m_matching->activateWindow();
}

void MainWindow::updateMatchingTarget()
{
    if (!m_matching)
        return;
    SchematicWorkspace *workspace = currentWorkspace();
    m_matching->setTarget(workspace ? m_session->project() : nullptr, workspace ? currentDocument() : nullptr);
}

void MainWindow::manageLibraries()
{
    LibraryManagerDialog dialog(m_session->libraries(), this);
    connect(&dialog, &LibraryManagerDialog::userLibraryPathChanged, this, [](const QString &path) {
        QSettings().setValue(QStringLiteral("userLibraryPath"), path);
    });
    dialog.exec();
}

void MainWindow::onProjectAboutToClose(Project *project)
{
    Q_UNUSED(project)
    // S-parameter files are independent of the project and stay open.
    closeDocumentTabs();
    if (m_matching)
        m_matching->setTarget(nullptr, nullptr);
    m_projectTree->clear();
    m_violations->clear();
    m_libraryBrowser->setProject(nullptr);
}

void MainWindow::onProjectChanged(Project *project)
{
    m_libraryBrowser->setProject(project);
    if (project) {
        connect(project, &Project::documentAdded, this, [this] {
            rebuildProjectTree();
            updateWindowState();
        });
        connect(project, &Project::documentAboutToBeRemoved, this, &MainWindow::onDocumentAboutToBeRemoved);
        logMessage(tr("Project %1 opened").arg(QDir::toNativeSeparators(project->filePath())));
    }
    rebuildProjectTree();
    updateWindowState();
}

void MainWindow::onDocumentAboutToBeRemoved(Document *document)
{
    if (QWidget *view = m_views.value(document))
        closeTab(m_tabs->indexOf(view));
    if (QTreeWidgetItem *root = m_projectTree->topLevelItem(0)) {
        for (int i = 0; i < root->childCount(); ++i) {
            if (itemDocument(root->child(i)) == document) {
                delete root->takeChild(i);
                break;
            }
        }
    }
    updateWindowState();
}

void MainWindow::onCurrentTabChanged()
{
    SchematicEditor *editor = currentSchematicEditor();

    m_propertiesStack->setCurrentWidget(editor ? editor->propertiesWidget() : m_noProperties);
    m_variablesStack->setCurrentWidget(editor ? editor->variablesWidget() : m_noVariables);

    m_editorToolBar->clear();
    m_editMenu->clear();
    if (editor) {
        m_editorToolBar->addActions(editor->editActions());
        m_editorToolBar->addSeparator();
        m_editorToolBar->addActions(editor->toolActions());
        m_editMenu->addActions(editor->editActions());
    }
    m_editMenu->setEnabled(editor);
    m_editorToolBar->setEnabled(editor);
    // Antenna workspaces: undo and redo, and the antenna library.
    QList<QAction *> antennaActions;
    if (antui::ArrayWorkspace *array = currentArrayWorkspace())
        antennaActions = array->editActions();
    else if (antui::ElementWorkspace *element = currentElementWorkspace())
        antennaActions = element->editActions();
    if (!antennaActions.isEmpty()) {
        m_editMenu->addActions(antennaActions);
        m_editMenu->setEnabled(true);
    }
    m_libraryStack->setCurrentWidget(antennaActions.isEmpty() ? static_cast<QWidget *>(m_libraryBrowser)
                                                              : static_cast<QWidget *>(m_antennaLibraryPanel));
    // Properties and variables belong to schematics: make room beside antenna
    // documents and bring them back afterwards (unless the user closed them).
    const bool antenna = !antennaActions.isEmpty();
    for (const char *name : {"propertiesDock", "variablesDock"}) {
        auto *dockWidget = findChild<QDockWidget *>(QLatin1String(name));
        if (!dockWidget)
            continue;
        if (antenna && dockWidget->isVisible()) {
            dockWidget->setProperty("lvdHiddenForAntenna", true);
            dockWidget->hide();
        } else if (!antenna && dockWidget->property("lvdHiddenForAntenna").toBool()) {
            dockWidget->setProperty("lvdHiddenForAntenna", false);
            dockWidget->show();
        }
    }
    // The library shows the parts the current schematic can simulate.
    if (editor && editor->document()) {
        const bool spice = editor->document()->type() == DocumentType::SpiceSchematic;
        m_libraryBrowser->setModelKinds(
            spice ? QStringList{QStringLiteral("spice"), QStringLiteral("subschematic"), QStringLiteral("python")}
                  : QStringList{QStringLiteral("rf-element"), QStringLiteral("rf-behavioural"), QStringLiteral("sparameters"),
                                QStringLiteral("subschematic"), QStringLiteral("python")});
    }
    updateMatchingTarget();
    updateWindowState();
}

void MainWindow::onRuleCheckFinished(Document *document, const QList<RuleViolation> &violations)
{
    m_violations->setViolations(document->name(), violations);
    m_violationsDock->show();
    m_violationsDock->raise();
}

void MainWindow::showViolation(const QString &documentName, const QString &itemId)
{
    Project *project = m_session->project();
    Document *document = project ? project->document(documentName) : nullptr;
    if (!document)
        return;
    openDocument(document);
    if (SchematicEditor *editor = schematicEditor(document); editor && !itemId.isEmpty())
        editor->zoomToItem(itemId);
}

bool MainWindow::confirmDiscardChanges()
{
    Project *project = m_session->project();
    if (!project || !project->hasModifiedDocuments())
        return true;
    const auto answer = QMessageBox::warning(
        this, tr("Unsaved Changes"), tr("Project \"%1\" has unsaved changes.").arg(project->name()),
        QMessageBox::SaveAll | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::SaveAll);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::SaveAll) {
        QString error;
        if (!project->saveAll(&error)) {
            reportError(tr("Save All"), error);
            return false;
        }
    }
    return true;
}

bool MainWindow::closeTab(int index, bool ask)
{
    QWidget *view = m_tabs->widget(index);
    if (auto *touchstone = qobject_cast<TouchstoneEditor *>(view)) {
        if (ask && !touchstone->maybeSave())
            return false;
        m_tabs->removeTab(index);
        delete touchstone;
        return true;
    }
    Document *document = documentAt(index);
    if (!view || !document)
        return true;
    m_views.remove(document);
    m_tabs->removeTab(index);
    // Deleted now, not later: the editor removes its panels from the docks.
    delete view;
    return true;
}

void MainWindow::rebuildProjectTree()
{
    m_projectTree->clear();
    Project *project = m_session->project();
    if (!project)
        return;
    auto *root = new QTreeWidgetItem(m_projectTree, {project->name(), tr("Project")});
    QFont bold = root->font(0);
    bold.setBold(true);
    root->setFont(0, bold);
    root->setToolTip(0, QDir::toNativeSeparators(project->filePath()));
    for (Document *document : project->documents()) {
        auto *item = new QTreeWidgetItem(root, {document->name(), documentTypeInfo(document->type()).displayName});
        item->setData(0, DocumentRole, QVariant::fromValue(static_cast<void *>(document)));
        item->setToolTip(0, QDir::toNativeSeparators(document->filePath()));
    }
    root->setExpanded(true);
}

void MainWindow::updateTabTitle(Document *document)
{
    if (QWidget *view = m_views.value(document))
        m_tabs->setTabText(m_tabs->indexOf(view), document->name() + (document->isModified() ? QStringLiteral(" *") : QString()));
}

void MainWindow::updateWindowState()
{
    Project *project = m_session->project();
    const Document *current = currentDocument();

    setWindowTitle(project ? tr("%1 - lvd RF cad").arg(project->name()) : tr("lvd RF cad"));
    m_closeProjectAction->setEnabled(project);
    m_saveAllAction->setEnabled(project);
    m_saveAction->setEnabled(current && current->isModified());
    m_removeDocumentAction->setEnabled(selectedTreeDocument());
    for (QAction *action : std::as_const(m_newDocumentActions))
        action->setEnabled(project);
    const bool schematic = currentWorkspace();
    m_runSimulationAction->setEnabled(schematic);
    m_newAnalysisAction->setEnabled(schematic);
    m_resultToEditorAction->setEnabled(schematic);
    m_tuneAction->setEnabled(schematic);
    m_optimizeAction->setEnabled(schematic);
    m_showResultsAction->setEnabled(schematic);
}

Document *MainWindow::documentAt(int tabIndex) const
{
    const QWidget *view = m_tabs->widget(tabIndex);
    return view ? static_cast<Document *>(view->property(DocumentProperty).value<void *>()) : nullptr;
}

Document *MainWindow::currentDocument() const
{
    return documentAt(m_tabs->currentIndex());
}

Document *MainWindow::selectedTreeDocument() const
{
    return itemDocument(m_projectTree->currentItem());
}

antui::ArrayWorkspace *MainWindow::currentArrayWorkspace() const
{
    return qobject_cast<antui::ArrayWorkspace *>(m_tabs->currentWidget());
}

antui::ElementWorkspace *MainWindow::currentElementWorkspace() const
{
    return qobject_cast<antui::ElementWorkspace *>(m_tabs->currentWidget());
}

void MainWindow::applyAntennaLibraryEntry(const QString &key)
{
    const ant::AntennaLibraryManager::Found found = m_antennaLibraries->find(key);
    if (!found.entry)
        return;
    if (antui::ArrayWorkspace *array = currentArrayWorkspace()) {
        array->applyLibraryEntry(key, *found.entry);
        statusBar()->showMessage(tr("Applied %1.").arg(found.entry->name), 4000);
    } else if (antui::ElementWorkspace *element = currentElementWorkspace()) {
        if (found.entry->kind == ant::AntennaEntryKind::Element || found.entry->kind == ant::AntennaEntryKind::Pattern) {
            element->applyLibraryEntry(key, *found.entry);
            statusBar()->showMessage(tr("Applied %1.").arg(found.entry->name), 4000);
        } else {
            statusBar()->showMessage(tr("%1 applies to antenna arrays.").arg(found.entry->name), 4000);
        }
    } else {
        statusBar()->showMessage(tr("Open an antenna array or element to use %1.").arg(found.entry->name), 4000);
    }
}

void MainWindow::reportError(const QString &title, const QString &message)
{
    logMessage(tr("Error: %1").arg(message));
    QMessageBox::warning(this, title, message);
}

} // namespace lvd
