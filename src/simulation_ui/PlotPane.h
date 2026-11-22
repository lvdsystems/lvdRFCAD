#pragma once

#include "simulation_ui/PlotDefinitions.h"

#include <QHash>
#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

class QComboBox;
class QCompleter;
class QLabel;
class QLineEdit;
class QStringListModel;
class QToolButton;
class QTreeWidget;

namespace lvd {
class PlotWidget;
namespace rf {
struct LinearResult;
}
} // namespace lvd

namespace lvd::sim {

// One plot of the simulation view: a PlotWidget bound to a plot definition,
// with a trace editor (add by expression with completion, remove, rename,
// change the kind). The pane never stores anything itself: every change is
// handed to definitionEdited() and comes back through setDefinition() once
// the schematic has stored it, which keeps undo in one place.
class PlotPane : public QWidget
{
    Q_OBJECT
public:
    explicit PlotPane(const PlotDefinition &definition, QWidget *parent = nullptr);
    ~PlotPane() override;

    const PlotDefinition &definition() const { return m_definition; }
    // Syncs traces in place: existing traces keep their id, markers and the
    // zoom; added expressions get new traces; removed ones go.
    void setDefinition(const PlotDefinition &definition);
    // Evaluates every trace on the result (null: clears the data).
    void setResult(std::shared_ptr<const rf::LinearResult> result);
    void setCompletions(const QStringList &completions);

    PlotWidget *plot() const { return m_plot; }
    QTreeWidget *traceList() const { return m_traces; }
    QLineEdit *expressionEdit() const { return m_expression; }
    QString inputError() const;

    // Trace id of an expression, -1 if none (traces appear with the first
    // successful evaluation).
    int traceId(const QString &expression) const;
    // Evaluation error of a trace, empty when it is fine.
    QString traceError(const QString &expression) const;

    // Edits, reported through definitionEdited(). addTrace() refuses
    // duplicates and, when a result is shown, expressions that fail on it;
    // inputError() then holds the reason.
    bool addTrace(const QString &expression);
    void removeTrace(const QString &expression);
    void renameTrace(const QString &expression, const QString &name);
    void setKind(const QString &kind);
    void rename(const QString &name);

    bool isEditorVisible() const;
    void setEditorVisible(bool visible);

    // Source and load stability circles at the first marker's frequency, or
    // where K is lowest (two-ports with K < 1 somewhere; Smith charts only).
    void setStabilityCirclesEnabled(bool enabled);
    bool stabilityCirclesEnabled() const { return m_stabilityCircles; }
    int stabilityCircleCount() const { return static_cast<int>(m_circleIds.size()); }

    // Design circles of a two-port at the first marker's frequency (the
    // middle of the sweep without markers), Smith charts only:
    // gain circles, constant available gain GA in the source plane and
    // constant operating power gain GP in the load plane, at levels in dB
    // below MAG (MSG when K < 1), default 1, 2, 3; noise circles, constant
    // noise figure in the source plane, at levels in dB above NFmin,
    // default 0.25, 0.5, 1, 2 (needs a result with noise). Levels are
    // changed in the buttons' menus. Every circle is labelled with its
    // value, plane and frequency.
    void setGainCirclesEnabled(bool enabled);
    bool gainCirclesEnabled() const { return m_gainCircles; }
    void setNoiseCirclesEnabled(bool enabled);
    bool noiseCirclesEnabled() const { return m_noiseCircles; }
    void setGainCircleLevels(const QList<double> &dbBelowMaximum);
    QList<double> gainCircleLevels() const { return m_gainLevels; }
    void setNoiseCircleLevels(const QList<double> &dbAboveMinimum);
    QList<double> noiseCircleLevels() const { return m_noiseLevels; }
    int designCircleCount() const { return static_cast<int>(m_designCircleIds.size()); }
    QStringList designCircleNames() const { return m_designCircleNames; }
    // "1, 2, 3" -> {1, 2, 3}: 1 to 12 non-negative numbers separated by
    // commas, semicolons or spaces; nullopt otherwise.
    static std::optional<QList<double>> parseLevels(const QString &text);

Q_SIGNALS:
    void definitionEdited(const lvd::sim::PlotDefinition &definition);
    void removeRequested(const QString &plotId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    struct TraceState {
        int id = -1;
        QString error;
        QString unit; // unit of the shown values
        bool noise = false;
    };

    void applyKind();
    void syncTraces();
    void evaluate();
    void evaluateTrace(const QString &expression, TraceState &state);
    void layoutAxes();
    void refreshList();
    void updateStabilityCircles();
    void updateDesignCircles();
    int circleFrequencyIndex() const;
    void editCircleLevels(bool gain);
    void setInputError(const QString &error);
    QColor errorColor() const;

    PlotDefinition m_definition;
    std::shared_ptr<const rf::LinearResult> m_result;
    QHash<QString, TraceState> m_state;

    PlotWidget *m_plot;
    QWidget *m_editor;
    QLineEdit *m_name;
    QComboBox *m_kind;
    QLineEdit *m_expression;
    QToolButton *m_addButton;
    QToolButton *m_removeButton;
    QToolButton *m_stabilityButton;
    QToolButton *m_gainButton;
    QToolButton *m_noiseButton;
    QToolButton *m_editorButton;
    QToolButton *m_closeButton;
    QTreeWidget *m_traces;
    QLabel *m_inputError;
    QCompleter *m_completer;
    QStringListModel *m_completionModel;

    bool m_stabilityCircles = false;
    std::vector<int> m_circleIds;
    bool m_gainCircles = false;
    bool m_noiseCircles = false;
    QList<double> m_gainLevels{1.0, 2.0, 3.0};
    QList<double> m_noiseLevels{0.25, 0.5, 1.0, 2.0};
    std::vector<int> m_designCircleIds;
    QStringList m_designCircleNames;
    bool m_updating = false;
};

} // namespace lvd::sim
