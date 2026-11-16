#pragma once

#include "spice/SpiceData.h"
#include "spice_ui/WaveformPane.h"

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <memory>

class QAction;
class QCompleter;
class QStringListModel;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QToolBar;

namespace lvd::spiceui {

class MeasurementPanel;

// Waveform viewer for SPICE results (requirement SP-4): one plot of a run
// (tran, ac, dc, noise) in vertically stacked panes that share the x range,
// traces given as expressions, cursors A and B, measurements and FFT.
//
// The owner keeps the pane definitions in its document: it calls setPanes()
// when it opens or switches the analysis (no signal), and stores what
// panesEdited() hands it after every user change.
class WaveformViewer : public QWidget
{
    Q_OBJECT
public:
    explicit WaveformViewer(QWidget *parent = nullptr);
    ~WaveformViewer() override;

    // ---- Data
    // Shows result->plots[plotIndex]. A new run of the same analysis type
    // keeps the panes, the zoom (when still inside the data) and the cursors.
    // Plots without waveforms (op, tf, pz, sens, a single point) show a
    // placeholder text instead.
    void setData(std::shared_ptr<const spice::SpiceRunResult> result, int plotIndex);
    void clearData();
    std::shared_ptr<const spice::SpiceRunResult> result() const { return m_result; }
    int plotIndex() const { return m_plotIndex; }
    const spice::SpicePlot *shownPlot() const; // null without data
    bool hasWaveforms() const;                 // false: the placeholder is shown
    QString placeholderText() const;
    // Vector names of the shown plot, the scale first.
    QStringList availableVectors() const;
    // Optional: resolves names the plot does not have (schematic net labels,
    // part references), such as spice::plotResolver(plot, deck).
    void setNameResolver(WaveformNameResolver resolver);

    // ---- Panes (the persisted state)
    void setPanes(const QList<WaveformPaneDefinition> &panes); // does not emit panesEdited
    QList<WaveformPaneDefinition> panes() const;
    static QJsonArray panesToJson(const QList<WaveformPaneDefinition> &panes);
    static QList<WaveformPaneDefinition> panesFromJson(const QJsonArray &array);
    // Default panes for a plot: one pane, log x for ac and noise.
    static WaveformPaneDefinition defaultPane(const spice::SpicePlot *plot, const QString &id = QStringLiteral("p1"));

    // The user-level edits below emit panesEdited.
    QString addPane(const QString &name = QString()); // returns the new id; becomes current
    bool removePane(const QString &paneId);
    // Adds a trace to a pane (empty id: the current pane; created when there
    // is none). Fails with a message for syntax errors, unknown vectors and
    // duplicates.
    bool addTrace(const QString &expression, const QString &paneId = QString(), QString *error = nullptr);
    bool removeTrace(const QString &paneId, int index);
    // A new pane with the amplitude spectrum of the expression (tran plots);
    // returns its id, or empty with *error set.
    QString addFftPane(const QString &expression, QString *error = nullptr);
    QString fftOfSelectedTrace(QString *error = nullptr);

    // ---- Current pane (clicking a pane selects it)
    QString currentPaneId() const { return m_currentId; }
    void setCurrentPane(const QString &paneId);
    WaveformPane *currentPane() const;
    WaveformPane *pane(const QString &paneId) const;
    QList<WaveformPane *> paneWidgets() const;

    // Cursors A (0) and B (1) of the current pane.
    void setCursorShown(int which, bool shown);

    MeasurementPanel *measurementPanel() const { return m_measurements; }
    QToolBar *toolBar() const { return m_toolBar; }

Q_SIGNALS:
    void panesEdited(const QList<WaveformPaneDefinition> &panes);
    void statusMessage(const QString &message);
    void currentPaneChanged(const QString &paneId);

private:
    WaveformPane *createPane(const WaveformPaneDefinition &definition);
    void deletePanes();
    QString nextPaneId() const;
    void emitEdited();
    void syncXRange(WaveformPane *source, double min, double max);
    void updateMeasurements();
    void updateActions();
    void updateVectorList();
    void updateCompletions();
    void updatePlaceholder();
    void submitExpression();
    void setInputError(const QString &message);

    std::shared_ptr<const spice::SpiceRunResult> m_result;
    int m_plotIndex = -1;
    WaveformNameResolver m_nameResolver;
    QList<WaveformPane *> m_panes;
    QString m_currentId;
    bool m_syncing = false;

    QStackedWidget *m_stack = nullptr;
    QLabel *m_placeholder = nullptr;
    QToolBar *m_toolBar = nullptr;
    QSplitter *m_mainSplitter = nullptr;
    QSplitter *m_paneSplitter = nullptr;
    QLineEdit *m_expression = nullptr;
    QPushButton *m_addButton = nullptr;
    QLabel *m_inputError = nullptr;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_vectors = nullptr;
    MeasurementPanel *m_measurements = nullptr;
    QCompleter *m_completer = nullptr;
    QStringListModel *m_completionModel = nullptr;

    QAction *m_addPaneAction = nullptr;
    QAction *m_removePaneAction = nullptr;
    QAction *m_cursorActions[2] = {nullptr, nullptr};
    QAction *m_fftAction = nullptr;
    QAction *m_autoscaleAction = nullptr;
    QAction *m_logXAction = nullptr;
    QAction *m_exportPngAction = nullptr;
    QAction *m_exportSvgAction = nullptr;
    QAction *m_exportCsvAction = nullptr;
    QAction *m_copyAction = nullptr;
};

} // namespace lvd::spiceui
