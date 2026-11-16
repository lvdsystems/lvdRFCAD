#pragma once

#include "rfsim/Budget.h"

#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QStackedWidget;
class QTabWidget;
class QTableView;
class QTableWidget;
class QToolButton;

namespace lvd {
class PlotWidget;
}

namespace lvd::sim {

class BudgetTableModel;
class SpurFilterModel;
class SpurTableModel;

// Results of a budget analysis (RF-6 to RF-9), shown by the simulation view
// instead of the linear plots: the budget table, the level diagram, the swept
// budget, the spur table and chart, and the frequency plan. Errors and
// warnings are shown by the simulation view; this widget only shows why a
// part is empty (no sweep, spur analysis off or failed).
class BudgetView : public QWidget
{
    Q_OBJECT
public:
    enum class LevelTrace { Signal, Noise, Op1db, Gain, Nf };
    enum class SweepTrace { Gain, Nf, Iip3 };
    enum Tab { TableTab, LevelTab, SweepTab, SpurTab, PlanTab };

    explicit BudgetView(QWidget *parent = nullptr);
    ~BudgetView() override;

    // Null clears every view.
    void setRun(std::shared_ptr<const rf::BudgetRun> run);
    std::shared_ptr<const rf::BudgetRun> run() const { return m_run; }

    // Swept budget at the output of this stage (index into budget stages);
    // -1: the chain output.
    void setSweepStage(int stage);
    int sweepStage() const { return m_sweepStage; }

    // ---- Table and export
    BudgetTableModel *tableModel() const { return m_tableModel; }
    QTableView *tableView() const { return m_table; }
    // The budget as text with full precision (tab: clipboard, comma: CSV).
    QString tableText(QChar separator) const;
    void copyTable() const; // the selected rows, or all
    bool exportCsv(const QString &path, QString *error = nullptr) const;

    // ---- Plots (trace ids are -1 when the trace is not shown)
    PlotWidget *levelPlot() const { return m_levelPlot; }
    int levelTrace(LevelTrace trace) const;
    QString stageLegend() const;
    PlotWidget *sweepPlot() const { return m_sweepPlot; }
    int sweepTrace(SweepTrace trace) const;
    QComboBox *sweepStageCombo() const { return m_sweepStageCombo; }
    bool hasSweep() const;

    // ---- Spurs
    SpurTableModel *spurModel() const { return m_spurModel; }
    SpurFilterModel *spurFilter() const { return m_spurFilter; }
    QTableView *spurView() const { return m_spurTable; }
    QCheckBox *inBandOnlyCheck() const { return m_inBandOnly; }
    PlotWidget *spurPlot() const { return m_spurPlot; }
    // Why the spur table is empty ("" when it shows spurs).
    QString spurMessage() const;

    // ---- Frequency plan
    QTableWidget *frequencyPlan() const { return m_plan; }

    QTabWidget *tabs() const { return m_tabs; }
    QString summary() const;

Q_SIGNALS:
    void statusMessage(const QString &message);

protected:
    void changeEvent(QEvent *event) override;

private:
    QWidget *createTablePage();
    QWidget *createLevelPage();
    QWidget *createSweepPage();
    QWidget *createSpurPage();
    QWidget *createPlanPage();

    void showSummary();
    void showLevels();
    void showSweep();
    void showSpurs();
    void showPlan();
    void applyTheme();

    std::shared_ptr<const rf::BudgetRun> m_run;
    int m_sweepStage = -1;

    QLabel *m_summary;
    QTabWidget *m_tabs;

    BudgetTableModel *m_tableModel;
    QTableView *m_table;

    PlotWidget *m_levelPlot;
    QLabel *m_stageLegend;
    int m_levelTraces[5] = {-1, -1, -1, -1, -1};

    QStackedWidget *m_sweepPages;
    QLabel *m_sweepPlaceholder;
    QComboBox *m_sweepStageCombo;
    PlotWidget *m_sweepPlot;
    int m_sweepTraces[3] = {-1, -1, -1};

    SpurTableModel *m_spurModel;
    SpurFilterModel *m_spurFilter;
    QTableView *m_spurTable;
    QCheckBox *m_inBandOnly;
    QLabel *m_spurMessage;
    QLabel *m_spurCount;
    PlotWidget *m_spurPlot;

    QTableWidget *m_plan;
};

} // namespace lvd::sim
