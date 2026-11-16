#pragma once

#include "rfsim/Optimizer.h"

#include <QList>
#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QScrollArea;
class QSlider;
class QToolButton;
class QVBoxLayout;

namespace lvd {
class Document;
class Project;
} // namespace lvd

namespace lvd::sim {

// The tuning panel of the simulation view (requirement RF-11): a list of
// tunable values (top-sheet variables and component parameters), each with a
// slider between a minimum and a maximum (linear or logarithmic), a numeric
// field and optional stepping through a standard series. The panel only
// holds values; the simulation view simulates them on a snapshot and writes
// them into the document when the user applies them.
class TuningPanel : public QWidget
{
    Q_OBJECT
public:
    static constexpr int SliderSteps = 1000;

    explicit TuningPanel(QWidget *parent = nullptr);
    ~TuningPanel() override;

    // The document whose values are tuned (for the add menu and the
    // document values).
    void setContext(Project *project, Document *document);

    int count() const { return static_cast<int>(m_tuners.size()); }
    // Adds a tunable with the current document value. A variable whose
    // maximum does not exceed its minimum gets the default range around the
    // value. Returns the index, or -1 when it is already listed or has no
    // value.
    int addVariable(const rf::DesignVariable &variable);
    void removeVariable(int index);
    void clear();

    QList<rf::DesignVariable> variables() const;
    std::vector<double> values() const;          // tuned values
    std::vector<double> documentValues() const;  // values in the document
    double value(int index) const;
    // Moves a tunable: the value is clamped into the range and snapped to
    // the series; the slider and the field follow. Emits valuesChanged().
    void setValue(int index, double value);
    // As if the user dragged the slider to `position` (0 .. SliderSteps).
    void setSliderPosition(int index, int position);
    void setRange(int index, double minimum, double maximum, bool logarithmic);
    void setSeries(int index, std::optional<ESeries> series);

    // Re-reads the document values, keeping the tuned values. Tunables that
    // no longer exist are removed (their labels are returned).
    QStringList refreshDocumentValues();
    // Tuned values back to the document values; emits valuesChanged().
    void resetValues();
    // True when a tuned value differs from the document value.
    bool isModified() const;

    void setStatusText(const QString &text);
    QString statusText() const;

    // For tests.
    QSlider *slider(int index) const;
    QLineEdit *valueEdit(int index) const;
    QLineEdit *minimumEdit(int index) const;
    QLineEdit *maximumEdit(int index) const;
    QCheckBox *logCheck(int index) const;
    QComboBox *seriesCombo(int index) const;
    QToolButton *addButton() const { return m_addButton; }
    QPushButton *applyButton() const { return m_applyButton; }
    QPushButton *resetButton() const { return m_resetButton; }

Q_SIGNALS:
    void valuesChanged();
    void variablesChanged();
    void applyRequested();
    void resetRequested();
    void closeRequested();

protected:
    void changeEvent(QEvent *event) override;

private:
    struct Tuner;

    void createRow(Tuner &tuner);
    void showValue(Tuner &tuner);
    void updateButtons();
    void updateStyle();
    void fillAddMenu();
    int indexOf(const QWidget *row) const;
    double fromSlider(const Tuner &tuner, int position) const;
    int toSlider(const Tuner &tuner, double value) const;
    double constrain(const Tuner &tuner, double value) const;
    void editValue(Tuner &tuner);
    void editRange(Tuner &tuner);

    Project *m_project = nullptr;
    Document *m_document = nullptr;
    std::vector<std::unique_ptr<Tuner>> m_tuners;

    QToolButton *m_addButton;
    QMenu *m_addMenu;
    QToolButton *m_closeButton;
    QLabel *m_title;
    QLabel *m_hint;
    QScrollArea *m_scroll;
    QWidget *m_rowsHost;
    QVBoxLayout *m_rows;
    QLabel *m_status;
    QPushButton *m_applyButton;
    QPushButton *m_resetButton;
};

} // namespace lvd::sim
