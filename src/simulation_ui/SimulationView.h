#pragma once

#include "rfdata/Touchstone.h"
#include "rfsim/LinearSolver.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QWidget>

#include <memory>
#include <vector>

class QAction;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QSplitter;
class QStackedWidget;
class QStringListModel;
class QTabWidget;
class QTableView;
class QTimer;
class QToolBar;
class QToolButton;

namespace lvd {

class Document;
class OptimizationDialog;
class Project;
class Schematic;

namespace rf {
struct BudgetRun;
}

namespace sim {
class BudgetRunner;
class BudgetView;
class PlotPane;
class ResultsTableModel;
class SimulationRunner;
class TuningPanel;
class TuningRunner;
struct PlotDefinition;
} // namespace sim

// Results workspace of one schematic (requirement RF-5, PL-7): analysis
// selection and editing, running in the background (NF-3) with optional
// auto-run after edits, plot panes bound to the analysis, a results table and
// export.
//
// Two analysis types are handled: linear analyses ("rf-linear": plots and
// the results table) and RF budget analyses ("rf-budget", RF-6 to RF-9: a
// sim::BudgetView with the budget table, level diagram, swept budget, spurs
// and frequency plan, shown instead of the plots). Budget runs use the same
// pattern on their own runner (rf::runBudget on a worker thread).
//
// Threading: the netlist and the sweep are built on the GUI thread (they read
// the documents), then simulateLinear runs on a worker thread owned by a
// SimulationRunner, with a cancel flag. Only one run is active at a time; a
// run requested while another is active cancels it and starts once it has
// stopped, with a fresh netlist. The result comes back through a queued call.
//
// Analyses and plots live in the schematic's "simulation" object; every
// change goes through Schematic::setSimulation, so it is undoable and saved
// with the document. Unknown keys in that object are preserved.
//
// Tuning (RF-11): for a linear analysis, a sim::TuningPanel next to the
// plots lists tunable values with sliders. Starting to tune takes a project
// snapshot on the GUI thread and hands it to a sim::TuningRunner, whose
// worker thread re-simulates the snapshot for every slider move; while it
// is busy only the newest values are kept, and each result updates the plots
// in place. The document is unchanged until applyTuning() writes the values
// (one undo step per touched schematic); resetTuning() and stopTuning()
// discard them. Edits of the real circuit while tuning retake the snapshot
// and keep the tuned values. Optimisation (RF-12) runs in an
// OptimizationDialog opened with openOptimization().
class SimulationView : public QWidget
{
    Q_OBJECT
public:
    SimulationView(Project *project, Document *document, QWidget *parent = nullptr);
    ~SimulationView() override;

    // ---- Running
    void run();
    void cancel();
    bool isRunning() const;
    // Last finished result of the current analysis; null when there is none
    // (or when the current analysis is of the other type).
    const rf::LinearResult *lastResult() const;
    std::shared_ptr<const rf::LinearResult> lastResultShared() const;
    const rf::BudgetRun *lastBudgetRun() const;
    std::shared_ptr<const rf::BudgetRun> lastBudgetRunShared() const;
    void setAutoRun(bool enabled);
    bool isAutoRun() const;
    // True when the circuit or the analysis changed after the shown result.
    bool isOutdated() const { return m_outdated; }
    qint64 lastElapsedMs() const { return m_lastElapsedMs; }
    // Milliseconds of quiet after an edit before an auto-run starts.
    void setAutoRunDelay(int milliseconds);

    // ---- Analyses
    QString currentAnalysisId() const;
    void setCurrentAnalysis(const QString &id);
    // "rf-linear", "rf-budget", or empty when there is no such analysis.
    QString analysisType(const QString &id) const;
    QString currentAnalysisType() const { return analysisType(m_currentAnalysis); }
    // Opens the dialog of the analysis' type; an empty id creates a new
    // analysis after asking for its type.
    void editAnalysis(const QString &id);
    // Opens the dialog for a new analysis of the type ("rf-linear" or
    // "rf-budget"; empty: ask).
    void newAnalysis(const QString &type = {});
    void deleteAnalysis(const QString &id);

    // ---- Budget analyses
    sim::BudgetView *budgetView() const { return m_budgetView; }

    // ---- Plots
    // Adds a plot of the given kind ("rectangular", "smith", "polar") to the
    // current analysis; returns its id.
    QString addPlot(const QString &kind);
    void addDefaultPlots();
    void removePlot(const QString &plotId);
    QList<sim::PlotPane *> plotPanes() const { return m_panes; }
    sim::PlotPane *plotPane(const QString &plotId) const;
    void setTiled(bool tiled);
    bool isTiled() const { return m_tiled; }
    // Completion candidates for trace expressions (from the port numbers).
    QStringList completions() const;

    // ---- Table and export
    sim::ResultsTableModel *tableModel() const { return m_tableModel; }
    QTableView *tableView() const { return m_table; }
    void setTableColumns(const QStringList &expressions);
    QStringList tableColumns() const;
    void copyTable() const; // selected rows, or all (budget analyses: the budget table)
    bool exportTouchstone(const QString &path, const rf::TouchstoneWriteOptions &options, QString *error = nullptr) const;
    // The results table, or for a budget analysis the budget table.
    bool exportCsv(const QString &path, QString *error = nullptr) const;

    // ---- Tuning (RF-11)
    // Opens the tuning panel for the current (linear) analysis.
    void startTuning();
    // Closes it; tuned values not applied are discarded.
    void stopTuning();
    bool isTuning() const { return m_tuning; }
    sim::TuningPanel *tuningPanel() const { return m_tuningPanel; }
    // Writes the tuned values into the document as one undo step per
    // touched schematic; tuning stays open.
    bool applyTuning(QString *error = nullptr);
    // Tuned values back to the document values.
    void resetTuning();
    // True while tuned values wait for their simulation.
    bool isTuningBusy() const;
    // Tuning results shown per second, over the recent results.
    double tuningUpdateRate() const;
    // Worker time of the last tuning simulation, in ms (-1: none yet).
    double lastTuningMs() const { return m_lastTuningMs; }
    int tuningResultCount() const { return m_tuningResults; }
    // The values of the shown tuning result.
    std::vector<double> lastTunedValues() const { return m_lastTunedValues; }
    QAction *tuneAction() const { return m_tuneAction; }

    // ---- Optimisation (RF-12)
    // Shows the (non-modal) optimisation dialog, with a stored setup or a new
    // one. Applying its result runs the analysis again.
    OptimizationDialog *openOptimization(const QString &setupId = {});
    OptimizationDialog *optimizationDialog() const;
    QAction *optimizeAction() const { return m_optimizeAction; }

    // ---- Messages of the shown result
    QStringList errors() const { return m_errors; }
    QStringList warnings() const { return m_warnings; }

    // Run, Stop, Auto-run, New/Edit/Delete analysis, Add plot, Export...;
    // shortcuts (F5, Shift+F5) are active while the view has focus.
    QList<QAction *> actions() const;
    QAction *runAction() const { return m_runAction; }
    QAction *stopAction() const { return m_stopAction; }
    QAction *autoRunAction() const { return m_autoRunAction; }

Q_SIGNALS:
    void statusMessage(const QString &message);
    void runFinished(bool ok);
    // A tuning result was shown in the plots.
    void tuningResultShown();

protected:
    void changeEvent(QEvent *event) override;

private:
    void createActions();
    void createToolBar();
    void createTable(QWidget *panel);
    void connectDocuments();
    void watchDocument(Document *document);

    void onSimulationChanged();
    void onCircuitEdited();
    void onAutoRunTimer();
    void onRunFinished(std::shared_ptr<const rf::LinearResult> result, quint64 tag);
    void onBudgetFinished(std::shared_ptr<const rf::BudgetRun> run, quint64 tag);
    void onPlotEdited(const sim::PlotDefinition &definition);
    bool isBudget(const QString &id) const { return analysisType(id) == QLatin1String("rf-budget"); }
    bool hasResult(const QString &id) const;
    void showBudgetResult();

    void refreshAnalyses();
    void syncPlots();
    void relayoutPlots();
    void showResult();
    void showMessages(const QStringList &errors, const QStringList &warnings);
    void updateActions();
    void updateStatus();
    void updateIcons();
    void updateTableColumns();
    void refreshCompletions();
    QByteArray fingerprint() const;
    void finishWithError(const QString &message);
    void exportTouchstoneInteractive();
    void exportCsvInteractive();
    void requestTuning();
    void takeTuningSnapshot();
    void refreshTuningSnapshot();
    void onTuningEvaluated(std::shared_ptr<const rf::LinearResult> result, quint64 serial, std::vector<double> values,
                           double milliseconds);

    Project *m_project;
    Document *m_document;
    QPointer<Schematic> m_schematic;
    sim::SimulationRunner *m_runner;
    sim::BudgetRunner *m_budgetRunner;

    // Results, per analysis id.
    QHash<QString, std::shared_ptr<const rf::LinearResult>> m_results;
    QHash<QString, std::shared_ptr<const rf::BudgetRun>> m_budgetResults;
    QHash<QString, QString> m_analysisTypes; // id -> type, of the analyses in the combo
    QString m_currentAnalysis;
    QString m_runningAnalysis;
    quint64 m_runTag = 0;
    bool m_pendingRun = false;
    QByteArray m_lastFingerprint;
    bool m_outdated = false;
    std::vector<int> m_ports;
    bool m_noise = true;
    QStringList m_errors;
    QStringList m_warnings;
    QStringList m_tableColumns; // empty: the traces of the analysis' plots

    QElapsedTimer m_elapsed;
    qint64 m_lastElapsedMs = -1;
    QTimer *m_elapsedTimer;
    QTimer *m_autoRunTimer;

    // Widgets.
    QToolBar *m_toolBar;
    QComboBox *m_analysisCombo;
    QFrame *m_errorBanner;
    QLabel *m_errorTitle;
    QLabel *m_errorText;
    QStackedWidget *m_resultStack; // the linear plots and table, or the budget view
    QSplitter *m_splitter;
    sim::BudgetView *m_budgetView;
    QWidget *m_plotHost;
    QTabWidget *m_tabs;
    QWidget *m_gridHost;
    QGridLayout *m_grid;
    QLabel *m_placeholder;
    QList<sim::PlotPane *> m_panes;
    bool m_tiled = true;

    sim::ResultsTableModel *m_tableModel;
    QTableView *m_table;
    QLineEdit *m_columnEdit;
    QStringListModel *m_completionModel;

    QListWidget *m_warningList;
    QToolButton *m_warningButton;
    QProgressBar *m_busy;
    QLabel *m_statusLabel;

    QAction *m_newAnalysisAction = nullptr;
    QAction *m_newLinearAction = nullptr;
    QAction *m_newBudgetAction = nullptr;
    QAction *m_editAnalysisAction = nullptr;
    QAction *m_deleteAnalysisAction = nullptr;
    QAction *m_runAction = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_autoRunAction = nullptr;
    QAction *m_addPlotAction = nullptr;
    QAction *m_tileAction = nullptr;
    QAction *m_exportAction = nullptr;
    QAction *m_exportTouchstoneAction = nullptr;
    QAction *m_exportCsvAction = nullptr;
    QAction *m_copyTableAction = nullptr;
    QAction *m_tuneAction = nullptr;
    QAction *m_optimizeAction = nullptr;

    // Tuning.
    QSplitter *m_mainSplitter;
    sim::TuningPanel *m_tuningPanel;
    sim::TuningRunner *m_tuningRunner;
    QTimer *m_tuningRefreshTimer;
    bool m_tuning = false;
    QString m_tuningAnalysis;
    std::shared_ptr<const rf::LinearResult> m_resultBeforeTuning;
    quint64 m_tuningFirstSerial = 0;
    int m_tuningResults = 0;
    double m_lastTuningMs = -1;
    std::vector<double> m_lastTunedValues;
    QElapsedTimer m_tuningClock;
    QList<qint64> m_tuningTimes; // ms on m_tuningClock of the recent results

    QPointer<OptimizationDialog> m_optimizationDialog;
};

} // namespace lvd
