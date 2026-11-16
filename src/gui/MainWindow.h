#pragma once

#include "core/DocumentType.h"
#include "core/RuleCheck.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>

class QAction;
class QDockWidget;
class QMenu;
class QPlainTextEdit;
class QStackedWidget;
class QTabWidget;
class QTextBrowser;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;

namespace lvd {

class Document;
class LibraryBrowser;
class MatchingWindow;
class Project;
class PythonConsole;
class PythonInterpreter;
class SchematicEditor;
class SchematicWorkspace;
class Session;
class SymbolEditor;
class TouchstoneEditor;
class ViolationsPanel;

namespace ant {
class AntennaLibraryManager;
}
namespace antui {
class AntennaLibraryPanel;
class ArrayWorkspace;
class ElementWorkspace;
} // namespace antui

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(Session *session, PythonInterpreter *python, QWidget *parent = nullptr);
    ~MainWindow() override;

    void openDocument(Document *document);
    // S-parameter editor tabs.
    TouchstoneEditor *openTouchstoneFile(const QString &path);
    TouchstoneEditor *addTouchstoneTab();
    bool openProjectFile(const QString &path);
    void logMessage(const QString &message);

    int openDocumentCount() const;
    int projectTreeDocumentCount() const;
    PythonConsole *pythonConsole() const { return m_console; }
    LibraryBrowser *libraryBrowser() const { return m_libraryBrowser; }
    ViolationsPanel *violationsPanel() const { return m_violations; }
    // The editor of the current tab, if it is a schematic.
    SchematicEditor *currentSchematicEditor() const;
    SchematicEditor *schematicEditor(Document *document) const;
    SchematicWorkspace *currentWorkspace() const;
    // Antenna workspaces of the current tab (null otherwise).
    antui::ArrayWorkspace *currentArrayWorkspace() const;
    antui::ElementWorkspace *currentElementWorkspace() const;
    ant::AntennaLibraryManager *antennaLibraries() const { return m_antennaLibraries; }
    antui::AntennaLibraryPanel *antennaLibraryPanel() const { return m_antennaLibraryPanel; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void createActions();
    void createDocks();

    void newProject();
    void openProject();
    void closeProject();
    void newDocument(DocumentType type);
    void saveCurrentDocument();
    void saveAll();
    void removeSelectedDocument();
    void showAbout();

    void editComponent(const QString &definitionKey);
    void newComponent(const QString &libraryName);
    void manageLibraries();
    void showMatchingWindow();
    void showManual();
    // A file or folder shipped with the application (docs, examples); empty if missing.
    static QString documentationPath(const QString &relative);
    void updateMatchingTarget();

    void onProjectAboutToClose(Project *project);
    void onProjectChanged(Project *project);
    void onDocumentAboutToBeRemoved(Document *document);
    void onCurrentTabChanged();
    void onRuleCheckFinished(Document *document, const QList<RuleViolation> &violations);
    void showViolation(const QString &documentName, const QString &itemId);

    // Returns false if the user cancelled.
    bool confirmDiscardChanges();
    // Returns false if the user kept the tab (unsaved S-parameter edits).
    bool closeTab(int index, bool ask = true);
    void closeDocumentTabs();
    void rebuildProjectTree();
    void updateTabTitle(Document *document);
    void updateWindowState();
    Document *documentAt(int tabIndex) const;
    Document *currentDocument() const;
    Document *selectedTreeDocument() const;
    void reportError(const QString &title, const QString &message);
    void applyAntennaLibraryEntry(const QString &key);

    Session *m_session;
    PythonInterpreter *m_python;

    QTabWidget *m_tabs;
    QTreeWidget *m_projectTree;
    QPlainTextEdit *m_messages;
    PythonConsole *m_console;
    LibraryBrowser *m_libraryBrowser;
    ant::AntennaLibraryManager *m_antennaLibraries = nullptr;
    antui::AntennaLibraryPanel *m_antennaLibraryPanel = nullptr;
    QStackedWidget *m_libraryStack = nullptr;
    QStackedWidget *m_propertiesStack;
    QStackedWidget *m_variablesStack;
    QWidget *m_noProperties;
    QWidget *m_noVariables;
    ViolationsPanel *m_violations;
    QToolBar *m_editorToolBar;
    QMenu *m_editMenu = nullptr;
    QMenu *m_viewMenu = nullptr;
    QDockWidget *m_violationsDock = nullptr;
    QHash<Document *, QWidget *> m_views;
    QPointer<SymbolEditor> m_symbolEditor;
    QPointer<MatchingWindow> m_matching;
    QPointer<QTextBrowser> m_manual;

    QAction *m_closeProjectAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAllAction = nullptr;
    QAction *m_removeDocumentAction = nullptr;
    QAction *m_runSimulationAction = nullptr;
    QAction *m_newAnalysisAction = nullptr;
    QAction *m_showResultsAction = nullptr;
    QAction *m_resultToEditorAction = nullptr;
    QAction *m_tuneAction = nullptr;
    QAction *m_optimizeAction = nullptr;
    QList<QAction *> m_newDocumentActions;
};

} // namespace lvd
