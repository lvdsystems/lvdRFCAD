#pragma once

#include "spice/SpiceAnalysis.h"
#include "spice/SpiceBatch.h"
#include "spice_ui/OperatingPointLabels.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

#include <memory>

class QAction;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QStandardItemModel;
class QTabWidget;
class QTableView;
class QToolBar;

namespace lvd {
class Document;
class Project;
class Schematic;
} // namespace lvd

namespace lvd::spiceui {

class BatchView;
class InstrumentsView;
class SpiceRunner;
class WaveformViewer;
struct WaveformPaneDefinition;

// Results workspace of a SPICE schematic (requirements SP-1 to SP-6, SP-10):
// the analyses stored in the schematic's "simulation" object, runs on a
// worker thread with progress, pause and stop, and the results in tabs:
// waveforms (WaveformViewer), values (operating point, transfer function,
// poles and zeros, sensitivities, Fourier, noise totals), batch results
// (sweeps, Monte Carlo, worst case), the netlist and the simulator log.
//
// Operating-point results are offered to the schematic through
// operatingPointLabelsChanged(); the workspace draws them.
//
// Waveform panes are stored in the simulation object per analysis id under
// "spicePanes": { "<analysis id>": [panes...] }.
class SpiceSimulationView : public QWidget
{
    Q_OBJECT
public:
    SpiceSimulationView(Project *project, Document *document, QWidget *parent = nullptr);
    ~SpiceSimulationView() override;

    // ---- Analyses
    QString currentAnalysisId() const { return m_currentAnalysis; }
    void setCurrentAnalysis(const QString &id);
    QList<spice::SpiceAnalysisSetup> analyses() const;
    std::optional<spice::SpiceAnalysisSetup> analysis(const QString &id) const;
    // Opens the dialog for a new analysis (kind preselected when given).
    void newAnalysis(std::optional<spice::AnalysisKind> kind = std::nullopt);
    void editAnalysis(const QString &id);
    void deleteAnalysis(const QString &id);
    // Stores an analysis without a dialog (scripts, tests); returns its id.
    QString addAnalysis(const spice::SpiceAnalysisSetup &setup);

    // ---- Running
    void run();
    void stop();
    void togglePause();
    bool isRunning() const;
    // Last finished simulation of the current analysis, or null.
    std::shared_ptr<const spice::SpiceSimulation> lastSimulation() const;
    QStringList errors() const { return m_errors; }

    // ---- Batches (SP-3); run on a worker thread on a project snapshot
    void runSweep(const QList<spice::SweepAxis> &axes, const QList<spice::SpiceMeasure> &measures);
    void runMonteCarlo(const spice::MonteCarloSetup &setup);
    void runWorstCase(const QList<spice::Tolerance> &tolerances, const QList<spice::SpiceMeasure> &measures);
    void runOptimization(const spice::SpiceOptimizationSetup &setup);
    std::shared_ptr<const spice::BatchResult> lastBatch() const { return m_batch; }
    // Worker processes for batches (0: one per core; 1: in this process).
    void setBatchWorkers(int workers) { m_batchWorkers = workers; }

    // ---- Parts
    WaveformViewer *waveformViewer() const { return m_viewer; }
    QStandardItemModel *valuesModel() const { return m_valuesModel; }
    BatchView *batchView() const { return m_batchView; }
    InstrumentsView *instrumentsView() const { return m_instruments; }
    QTabWidget *tabs() const { return m_tabs; }
    QString netlistText() const;
    QList<OperatingPointLabel> operatingPointLabels() const { return m_labels; }
    bool isAnnotating() const;
    void setAnnotating(bool annotate);
    QList<QAction *> actions() const;
    QAction *runAction() const { return m_runAction; }
    QAction *optimizeAction() const { return m_optimizeAction; }

Q_SIGNALS:
    void statusMessage(const QString &message);
    void runFinished(bool ok);
    void operatingPointLabelsChanged(const QList<lvd::spiceui::OperatingPointLabel> &labels);

private:
    void createActions();
    void refreshAnalyses();
    void updateActions();
    void showSimulation(std::shared_ptr<const spice::SpiceSimulation> simulation);
    void showMessages(const QStringList &errors, const QStringList &warnings);
    void fillValues(const spice::SpiceSimulation &simulation);
    void storePanes(const QList<WaveformPaneDefinition> &panes);
    QList<WaveformPaneDefinition> storedPanes(const QString &analysisId) const;
    void publishLabels();
    QStringList netNames() const;
    QStringList sourceReferences() const;
    QStringList validate(const spice::SpiceAnalysisSetup &setup) const;
    void startBatch(std::function<std::shared_ptr<spice::BatchResult>(Project &snapshot, const spice::BatchOptions &)> work,
                    const QString &title);
    void openBatchDialog(int mode);
    // Writes values into the schematic as one undo step.
    void applyValues(const QList<spice::SpiceTarget> &targets, const std::vector<double> &values);
    void exportNetlist();

    Project *m_project;
    Document *m_document;
    QPointer<Schematic> m_schematic;
    SpiceRunner *m_runner;
    quint64 m_runTag = 0;
    QString m_currentAnalysis;
    QString m_runningAnalysis;
    QHash<QString, std::shared_ptr<const spice::SpiceSimulation>> m_results;
    std::shared_ptr<const spice::BatchResult> m_batch;
    std::shared_ptr<std::atomic_bool> m_batchCancel;
    bool m_batchRunning = false;
    int m_batchWorkers = 0;
    QStringList m_errors;
    QList<OperatingPointLabel> m_labels;
    bool m_updatingCombo = false;

    QToolBar *m_toolBar;
    QComboBox *m_analysisCombo;
    QLabel *m_banner;
    QTabWidget *m_tabs;
    WaveformViewer *m_viewer;
    QTableView *m_valuesTable;
    QStandardItemModel *m_valuesModel;
    BatchView *m_batchView;
    InstrumentsView *m_instruments;
    QPlainTextEdit *m_netlist;
    QPlainTextEdit *m_log;
    QProgressBar *m_progress;
    QLabel *m_status;

    QAction *m_newAction = nullptr;
    QAction *m_editAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_runAction = nullptr;
    QAction *m_pauseAction = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_annotateAction = nullptr;
    QAction *m_netlistAction = nullptr;
    QAction *m_sweepAction = nullptr;
    QAction *m_monteCarloAction = nullptr;
    QAction *m_worstCaseAction = nullptr;
    QAction *m_optimizeAction = nullptr;
};

} // namespace lvd::spiceui
