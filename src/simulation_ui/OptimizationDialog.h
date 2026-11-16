#pragma once

#include "rfsim/Optimizer.h"

#include <QDialog>
#include <QPointer>

#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStringListModel;
class QTabWidget;
class QTableWidget;
class QToolButton;

namespace lvd {

class Document;
class PlotWidget;
class Project;
class Schematic;

namespace sim {
class OptimizationRunner;
}

// Sets up, runs and applies optimisations of a schematic (requirement RF-12).
//
// The setup — analysis, variables with ranges and optional standard series,
// goals on measurements over a band, method and evaluation budget — is
// stored in the schematic's simulation object ("optimizations") as one undo
// step, so setups are saved with the document and can be picked and run
// again later. Running takes a snapshot of the project on the GUI thread and
// runs rf::optimize on it in a worker thread (sim::OptimizationRunner) with
// live progress: evaluation count, the best cost so far, the current best
// values and the goal status. When it ends, the initial, best and snapped
// values and goal results are shown side by side; applying writes the best or
// the snapped values into the document as one undo step (per touched
// schematic) and emits applied().
class OptimizationDialog : public QDialog
{
    Q_OBJECT
public:
    // An empty or unknown setupId starts a new setup.
    OptimizationDialog(Project *project, Document *document, const QString &setupId = {}, QWidget *parent = nullptr);
    ~OptimizationDialog() override;

    // ---- Setup
    rf::OptimizationSetup setup() const;   // as shown (the id is that of the selected stored setup)
    void setSetup(const rf::OptimizationSetup &setup);
    // Selects a stored setup (empty: a new one).
    void selectSetup(const QString &id);
    QString setupId() const { return m_setupId; }
    // Adds a variable; one whose maximum does not exceed its minimum gets the
    // default range around its current value. Returns false for duplicates.
    bool addVariable(const rf::DesignVariable &variable);
    void addGoal(const rf::Goal &goal);
    // Empty when the setup can run, otherwise the reasons.
    QStringList validate() const;
    // Stores the setup (when it changed) as one undo step; returns its id,
    // or an empty string when it is invalid.
    QString store();

    // ---- Running
    bool start();      // stores, snapshots and runs in the background
    void cancel();
    bool isRunning() const;
    // The last finished run; null when there is none or it was discarded.
    std::shared_ptr<const rf::OptimizationResult> result() const { return m_result; }
    // Writes the best (or snapped) values into the document.
    bool apply(bool snapped);
    void discard();

    // For tests.
    QComboBox *setupCombo() const { return m_setupCombo; }
    QComboBox *analysisCombo() const { return m_analysisCombo; }
    QComboBox *methodCombo() const { return m_methodCombo; }
    QSpinBox *evaluationsSpin() const { return m_evaluations; }
    QTableWidget *variableTable() const { return m_variableTable; }
    QTableWidget *goalTable() const { return m_goalTable; }
    QTableWidget *bestTable() const { return m_bestTable; }
    QTableWidget *goalStatusTable() const { return m_goalStatusTable; }
    QTableWidget *resultsTable() const { return m_resultsTable; }
    PlotWidget *costPlot() const { return m_costPlot; }
    QLabel *progressLabel() const { return m_progressLabel; }
    QPushButton *runButton() const { return m_runButton; }
    QPushButton *stopButton() const { return m_stopButton; }
    QPushButton *applyButton() const { return m_applyButton; }
    QComboBox *applyChoice() const { return m_applyChoice; }
    QTabWidget *tabs() const { return m_tabs; }

Q_SIGNALS:
    void finished(bool ok);
    void progressed();
    // Values were written into the document; `analysisId` is the analysis
    // of the setup, to run again.
    void applied(const QString &analysisId);

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void createWidgets();
    void refreshSetups();
    void refreshAnalyses();
    void refreshCompletions();
    void appendVariableRow(const rf::DesignVariable &variable);
    void appendGoalRow(const rf::Goal &goal);
    void onProgress();
    void onFinished(std::shared_ptr<const rf::OptimizationResult> result);
    void showResult();
    void fillGoalStatus(const QList<rf::GoalEvaluation> &goals);
    void updateButtons();
    void showMessage(const QString &text, bool error);
    QColor goodColor() const;
    QColor badColor() const;
    void fillAddMenu();

    Project *m_project;
    Document *m_document;
    QPointer<Schematic> m_schematic;
    QString m_setupId;
    QList<rf::DesignVariable> m_runVariables; // of the running or finished run
    QString m_runAnalysis;
    std::shared_ptr<const rf::OptimizationResult> m_result;
    bool m_applied = false;
    sim::OptimizationRunner *m_runner;

    QComboBox *m_setupCombo;
    QLineEdit *m_name;
    QToolButton *m_deleteSetup;
    QComboBox *m_analysisCombo;
    QComboBox *m_methodCombo;
    QSpinBox *m_evaluations;
    QTableWidget *m_variableTable;
    QToolButton *m_addVariable;
    QMenu *m_addMenu;
    QTableWidget *m_goalTable;
    QToolButton *m_addGoal;
    QStringListModel *m_completions;

    QTabWidget *m_tabs;
    QLabel *m_progressLabel;
    QProgressBar *m_progressBar;
    PlotWidget *m_costPlot;
    int m_costTrace = -1;
    QTableWidget *m_bestTable;
    QTableWidget *m_goalStatusTable;
    QTableWidget *m_resultsTable;
    QLabel *m_resultSummary;

    QLabel *m_message;
    QPushButton *m_saveButton;
    QPushButton *m_runButton;
    QPushButton *m_stopButton;
    QComboBox *m_applyChoice;
    QPushButton *m_applyButton;
    QPushButton *m_discardButton;
    QPushButton *m_closeButton;
};

} // namespace lvd
