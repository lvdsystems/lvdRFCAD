#pragma once

#include "core/Expression.h"
#include "rfsim/Analysis.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;

namespace lvd {

class Document;
class Project;

// Creates or edits a linear (S-parameter and noise) analysis of a schematic:
// name, sweep type, start, stop and points as expressions with their
// evaluated values (schematic variables apply), a frequency list for list
// sweeps, and noise on or off. OK is disabled while the setup is invalid.
// Accepting stores the analysis with Schematic::setSimulation, as one undo
// step; a new analysis gets the default plots in the same step.
class AnalysisDialog : public QDialog
{
    Q_OBJECT
public:
    // An empty or unknown analysisId creates a new analysis.
    AnalysisDialog(Project *project, Document *document, const QString &analysisId = {}, QWidget *parent = nullptr);

    rf::LinearAnalysisSetup setup() const;
    void setSetup(const rf::LinearAnalysisSetup &setup);
    bool isNew() const { return m_isNew; }
    bool isValid() const { return m_valid; }
    // "201 points, 100 MHz to 3 GHz", or the error.
    QString summary() const;

    // Default plots for a new analysis (on by default).
    void setDefaultPlotsEnabled(bool enabled) { m_defaultPlots = enabled; }

    // Stores the setup (accept() calls it). Returns the analysis id, or an
    // empty string when the setup is invalid.
    QString store();
    QString analysisId() const { return m_id; }

    void accept() override;

    // For tests.
    QLineEdit *nameEdit() const { return m_name; }
    QComboBox *sweepCombo() const { return m_sweep; }
    QLineEdit *startEdit() const { return m_start; }
    QLineEdit *stopEdit() const { return m_stop; }
    QLineEdit *pointsEdit() const { return m_points; }
    QPlainTextEdit *listEdit() const { return m_list; }
    QCheckBox *noiseCheck() const { return m_noise; }
    QPushButton *okButton() const { return m_ok; }
    QLabel *startValue() const { return m_startValue; }
    QLabel *stopValue() const { return m_stopValue; }
    QLabel *pointsValue() const { return m_pointsValue; }

protected:
    void changeEvent(QEvent *event) override;

private:
    void validate();
    void showValue(QLabel *label, const QString &text, const QString &unit, bool integer);
    QString errorStyle() const;

    Project *m_project;
    Document *m_document;
    QString m_id;
    bool m_isNew = true;
    bool m_valid = false;
    bool m_defaultPlots = true;
    QString m_summary;
    Expression::Lookup m_scope;

    QLineEdit *m_name;
    QComboBox *m_sweep;
    QStackedWidget *m_pages;
    QLineEdit *m_start;
    QLineEdit *m_stop;
    QLineEdit *m_points;
    QLabel *m_startValue;
    QLabel *m_stopValue;
    QLabel *m_pointsValue;
    QPlainTextEdit *m_list;
    QLabel *m_listHint;
    QCheckBox *m_noise;
    QLabel *m_summaryLabel;
    QLabel *m_variablesLabel;
    QPushButton *m_ok;
};

} // namespace lvd
