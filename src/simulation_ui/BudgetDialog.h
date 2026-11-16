#pragma once

#include "core/Expression.h"
#include "core/Netlist.h"
#include "rfsim/Budget.h"

#include <QDialog>

#include <cmath>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;

namespace lvd {

class Document;
class Project;

// Creates or edits an RF budget analysis of a schematic (RF-6 to RF-9):
// start component, explicit signal path, frequency and power overrides,
// bandwidth, mismatch mode, an optional swept budget and the spur analysis.
// Expressions show their evaluated value; the traced chain is previewed.
// OK is disabled while a value is invalid (an incomplete circuit is not a
// reason: the run reports it). Accepting stores the analysis with
// Schematic::setSimulation, as one undo step; keys of the stored analysis
// that this dialog does not know are kept.
//
// The explicit path is a list of "REF" or "REF.pin" entries starting with
// the start component; "Trace" fills it from the automatic trace, and the
// exit pin of the selected entry can be chosen to take another branch at
// splitters, couplers and switches.
class BudgetDialog : public QDialog
{
    Q_OBJECT
public:
    // An empty or unknown analysisId creates a new analysis.
    BudgetDialog(Project *project, Document *document, const QString &analysisId = {}, QWidget *parent = nullptr);

    rf::BudgetAnalysisSetup setup() const;
    void setSetup(const rf::BudgetAnalysisSetup &setup);
    bool isNew() const { return m_isNew; }
    bool isValid() const { return m_valid; }

    // Stores the setup (accept() calls it). Returns the analysis id, or an
    // empty string when the setup is invalid.
    QString store();
    QString analysisId() const { return m_id; }

    // Start candidates: sources, ports and antennas of the netlist.
    QStringList startCandidates() const;
    // Fills the explicit path from the automatic trace of the current start.
    void tracePath();
    // Sets the exit pin of a path entry (empty: the default exit).
    void setExitPin(int entry, const QString &pin);
    QStringList path() const;
    // The previewed chain: "SRC1 → A1 → TERM1", or the tracing error.
    QString chainPreview() const;

    void accept() override;

    // For tests.
    QLineEdit *nameEdit() const { return m_name; }
    QComboBox *startCombo() const { return m_start; }
    QListWidget *pathList() const { return m_path; }
    QComboBox *exitPinCombo() const { return m_exitPin; }
    QLineEdit *frequencyEdit() const { return m_frequency; }
    QLineEdit *powerEdit() const { return m_power; }
    QLineEdit *bandwidthEdit() const { return m_bandwidth; }
    QComboBox *mismatchCombo() const { return m_mismatch; }
    QGroupBox *sweepGroup() const { return m_sweepGroup; }
    QLineEdit *sweepStartEdit() const { return m_sweepStart; }
    QLineEdit *sweepStopEdit() const { return m_sweepStop; }
    QLineEdit *sweepPointsEdit() const { return m_sweepPoints; }
    QGroupBox *spurGroup() const { return m_spurGroup; }
    QLineEdit *spurOrderEdit() const { return m_spurOrder; }
    QLabel *frequencyValue() const { return m_frequencyValue; }
    QLabel *powerValue() const { return m_powerValue; }
    QLabel *bandwidthValue() const { return m_bandwidthValue; }
    QLabel *mismatchHint() const { return m_mismatchHint; }
    QPushButton *okButton() const { return m_ok; }

protected:
    void changeEvent(QEvent *event) override;

private:
    void validate();
    void updatePathControls();
    void updateExitPins();
    void addPathEntry(const QString &text);
    // Evaluates an expression into the label; empty text is valid only with
    // a non-null `emptyText`, which is then shown. Returns false when the
    // text is invalid (or below the minimum).
    bool showValue(QLabel *label, const QString &text, const QString &unit, bool integer, const QString &emptyText,
                   double minimum = -INFINITY, double *value = nullptr);
    QString errorStyle() const;
    QString kindOf(const FlatComponent &component) const;

    Project *m_project;
    Document *m_document;
    QString m_id;
    bool m_isNew = true;
    bool m_valid = false;
    bool m_updating = false;
    Expression::Lookup m_scope;
    Netlist m_netlist;
    QString m_chainPreview;

    QLineEdit *m_name;
    QComboBox *m_start;
    QListWidget *m_path;
    QComboBox *m_exitPin;
    QToolButton *m_traceButton;
    QToolButton *m_removeButton;
    QToolButton *m_upButton;
    QToolButton *m_downButton;
    QToolButton *m_clearButton;
    QLabel *m_chainLabel;
    QLineEdit *m_frequency;
    QLineEdit *m_power;
    QLineEdit *m_bandwidth;
    QLabel *m_frequencyValue;
    QLabel *m_powerValue;
    QLabel *m_bandwidthValue;
    QComboBox *m_mismatch;
    QLabel *m_mismatchHint;
    QGroupBox *m_sweepGroup;
    QLineEdit *m_sweepStart;
    QLineEdit *m_sweepStop;
    QLineEdit *m_sweepPoints;
    QLabel *m_sweepStartValue;
    QLabel *m_sweepStopValue;
    QLabel *m_sweepPointsValue;
    QGroupBox *m_spurGroup;
    QLineEdit *m_spurOrder;
    QLabel *m_spurOrderValue;
    QLabel *m_variablesLabel;
    QPushButton *m_ok;
};

} // namespace lvd
