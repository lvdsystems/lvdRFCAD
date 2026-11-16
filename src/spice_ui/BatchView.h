#pragma once

#include "spice/SpiceBatch.h"

#include <QWidget>

#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStandardItemModel;
class QTableView;

namespace lvd {
class PlotWidget;
}

namespace lvd::spiceui {

// Results of a batch (requirement SP-3): a table of the runs with their
// values and measures; a summary (Monte Carlo statistics, worst-case
// limits, the optimum); and a plot: a measure against the swept value, a
// histogram of a measure, or one trace expression of every run overlaid.
// Double-clicking a run asks to show it in the waveform viewer.
class BatchView : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { Sweep, MonteCarlo, WorstCase, Optimization };

    explicit BatchView(QWidget *parent = nullptr);

    void setMode(Mode mode) { m_mode = mode; }
    Mode mode() const { return m_mode; }
    void setResult(std::shared_ptr<const spice::BatchResult> result);
    std::shared_ptr<const spice::BatchResult> result() const { return m_result; }
    void setWorstCase(const QList<spice::WorstCaseMeasure> &summary);
    void setOptimization(const spice::SpiceOptimizationSetup &setup, const spice::SpiceOptimizationResult &result);
    // Called with the values to write into the schematic (optimisation).
    void setApplyHandler(std::function<void(const std::vector<double> &)> handler) { m_apply = std::move(handler); }

    QStandardItemModel *runsModel() const { return m_runs; }
    QStandardItemModel *summaryModel() const { return m_summary; }
    PlotWidget *plot() const { return m_plot; }
    // Overlays an expression of every run on the plot; false with *error.
    bool setOverlay(const QString &expression, QString *error = nullptr);
    void showMeasure(int index); // curve (sweeps) or histogram (Monte Carlo)

Q_SIGNALS:
    void showRunRequested(int runIndex);

private:
    void fillRuns();
    void fillStatistics();

    Mode m_mode = Mode::Sweep;
    std::shared_ptr<const spice::BatchResult> m_result;
    std::function<void(const std::vector<double> &)> m_apply;
    std::vector<double> m_applyValues;
    QLabel *m_heading;
    QStandardItemModel *m_summary;
    QTableView *m_summaryTable;
    QStandardItemModel *m_runs;
    QTableView *m_runsTable;
    QComboBox *m_measureCombo;
    QLineEdit *m_overlay;
    QPushButton *m_applyButton;
    PlotWidget *m_plot;
};

} // namespace lvd::spiceui
