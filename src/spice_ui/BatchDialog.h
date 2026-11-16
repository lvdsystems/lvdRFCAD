#pragma once

#include "spice/SpiceBatch.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QSpinBox;
class QTableWidget;

namespace lvd {
class Document;
class Project;
} // namespace lvd

namespace lvd::spiceui {

// Sets up a batch on the current analysis (requirement SP-3): a parameter
// or temperature sweep, Monte Carlo, worst case or an optimisation. The
// setup is stored in the analysis under storageKey() and loaded from there.
class BatchDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Mode { Sweep, MonteCarlo, WorstCase, Optimize };

    BatchDialog(Mode mode, const Project &project, const Document &document, const spice::SpiceAnalysisSetup &analysis,
                QWidget *parent = nullptr);

    Mode mode() const { return m_mode; }
    QString storageKey() const;
    QJsonObject toJson() const;
    void load(const QJsonObject &object);

    QList<spice::SweepAxis> sweepAxes() const;
    QList<spice::SpiceMeasure> measures() const;
    spice::MonteCarloSetup monteCarlo() const; // tolerances, runs, seed and measures
    spice::SpiceOptimizationSetup optimization() const;

    // Values a target can take: variables, numeric parameters ("R1.r"),
    // and "temperature".
    QStringList targetCandidates() const { return m_candidates; }

    QTableWidget *measureTable() const { return m_measures; }
    QTableWidget *toleranceTable() const { return m_tolerances; }
    QTableWidget *sweepTable() const { return m_sweep; }
    QTableWidget *variableTable() const { return m_variables; }
    QTableWidget *goalTable() const { return m_goals; }

    void accept() override;

private:
    QTableWidget *makeTable(const QStringList &headers);
    QWidget *withButtons(QTableWidget *table, const std::function<void()> &addRow);
    void addMeasureRow(const spice::SpiceMeasure &measure = {});
    void addToleranceRow(const spice::Tolerance &tolerance);
    void addSweepRow(const spice::SweepAxis &axis);
    void addVariableRow(const spice::SpiceOptimizationVariable &variable);
    void addGoalRow(const spice::SpiceGoal &goal);
    QComboBox *targetCombo(const QString &text);
    QStringList problems() const;

    Mode m_mode;
    spice::SpiceAnalysisSetup m_analysis;
    QStringList m_candidates;
    QTableWidget *m_measures = nullptr;
    QTableWidget *m_tolerances = nullptr;
    QTableWidget *m_sweep = nullptr;
    QTableWidget *m_variables = nullptr;
    QTableWidget *m_goals = nullptr;
    QSpinBox *m_runs = nullptr;
    QSpinBox *m_seed = nullptr;
    QSpinBox *m_evaluations = nullptr;
    QLabel *m_error = nullptr;
};

} // namespace lvd::spiceui
