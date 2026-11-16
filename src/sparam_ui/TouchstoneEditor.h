#pragma once

#include "rfdata/NetworkOps.h"
#include "rfdata/RfTypes.h"
#include "rfdata/Touchstone.h"
#include "sparam_ui/SParamTableModel.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <map>
#include <optional>
#include <utility>
#include <vector>

class QAction;
class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QPlainTextEdit;
class QSplitter;
class QTableView;
class QToolBar;
class QToolButton;
class QUndoStack;

namespace lvd {

class PlotWidget;

// What the rectangular plot shows.
enum class SParamQuantity {
    MagnitudeDb,       // selected parameters
    Magnitude,
    PhaseDeg,
    UnwrappedPhaseDeg,
    GroupDelay,
    Real,
    Imaginary,
    Vswr,              // selected reflection parameters
    ReturnLoss,
    StabilityK,        // two-ports: K and |Delta|
    StabilityMu,       // two-ports: mu and mu'
    MaximumGain,       // two-ports: MAG / MSG in dB
    NoiseFigureMin,    // two-ports with noise data, dB
    NoiseResistance,   // two-ports with noise data, ohm
};

// Result of the passivity and reciprocity check (frequency indices).
struct SParamCheckReport {
    std::vector<int> nonPassive;
    std::vector<int> nonReciprocal;
    QString text; // human-readable summary
    bool passive() const { return nonPassive.empty(); }
    bool reciprocal() const { return nonReciprocal.empty(); }
};

// S-parameter file editor (requirement RF-4), usable as a main-window tab:
// summary, rectangular plot and Smith chart, table with conversions to Z, Y,
// ABCD and T, undoable operations (reference impedances, renumbering,
// resampling, cropping, cascading, de-embedding, cell edits) and Touchstone
// saving. Every operation is available programmatically (no dialog) and
// through actions() (with dialogs).
class TouchstoneEditor : public QWidget
{
    Q_OBJECT
public:
    explicit TouchstoneEditor(QWidget *parent = nullptr);
    ~TouchstoneEditor() override;

    // ---- Document
    bool openFile(const QString &path, QString *error = nullptr);
    // Data from a simulation: no file yet, so it counts as modified (unsaved).
    void setNetwork(const rf::NetworkData &network, const QString &suggestedName);
    // Saves to filePath() with the current write options; without a file
    // path, asks for one (interactive). A version 1 file whose references are
    // no longer equal is written as version 2.
    bool save(QString *error = nullptr);
    // The extension must match the port count (".sNp"; version 2 also ".ts")
    // and version 1 needs equal references; otherwise fails with *error.
    bool saveAs(const QString &path, const rf::TouchstoneWriteOptions &options, QString *error = nullptr);
    // Asks for a file name and the options, offers to fix the extension.
    bool saveAsInteractive();
    // When modified, asks whether to save, discard or cancel; returns false on cancel.
    bool maybeSave();
    bool isModified() const;
    QString filePath() const { return m_filePath; }
    QString title() const; // file or suggested name, "*" when modified
    const rf::NetworkData &network() const { return m_network; }
    QStringList readWarnings() const { return m_readWarnings; }
    rf::TouchstoneWriteOptions writeOptions() const { return m_writeOptions; }

    // ---- Operations (each one undoable; return false with *error on failure)
    bool setReferenceImpedances(const std::vector<double> &z0, QString *error = nullptr);
    bool renumberPorts(const std::vector<int> &order, QString *error = nullptr);
    bool resample(const std::vector<double> &frequencies, rf::Extrapolation extrapolation,
                  QString *error = nullptr, QString *warning = nullptr);
    bool cropFrequencies(double minimumHz, double maximumHz, QString *error = nullptr);
    // otherAfter: this network then `other`; otherwise `other` then this network.
    bool cascadeWith(const rf::NetworkData &other, bool otherAfter, QString *error = nullptr);
    bool deembed(const std::optional<rf::NetworkData> &left, const std::optional<rf::NetworkData> &right,
                 QString *error = nullptr);
    bool setSParameter(int frequencyIndex, int row, int column, rf::Complex value);
    void setComment(const QString &comment);
    // Checks every frequency, shows the failing frequencies on the plots
    // and in the summary. Changes nothing.
    SParamCheckReport checkPassivityAndReciprocity(double tolerance = 1e-9);
    void clearHighlights();
    // Frequency indices highlighted on the plots (failing either check).
    std::vector<int> highlightedFrequencies() const;
    QUndoStack *undoStack() const { return m_undo; }

    // ---- View
    void setSelectedParameters(const std::vector<std::pair<int, int>> &parameters);
    std::vector<std::pair<int, int>> selectedParameters() const;
    void setPlotQuantity(SParamQuantity quantity);
    SParamQuantity plotQuantity() const { return m_quantity; }
    bool isQuantityAvailable(SParamQuantity quantity) const;
    void setGammaOptVisible(bool visible);
    void setTableRepresentation(SParamRepresentation representation);
    SParamRepresentation tableRepresentation() const;
    void setTableForm(SParamTableForm form);
    SParamTableForm tableForm() const;
    QString tableCsv() const;
    bool exportTableCsv(const QString &path, QString *error = nullptr) const;
    // Summary texts by key ("file", "ports", "frequencies", "references",
    // "noise", "checks"), for tests and tool tips.
    QString summaryText(const QString &key) const;

    PlotWidget *rectangularPlot() const { return m_plot; }
    PlotWidget *smithChart() const { return m_smith; }
    SParamTableModel *tableModel() const { return m_model; }
    QTableView *tableView() const { return m_table; }

    // Actions for the main window's menus and tool bars (also on the
    // editor's own tool bar). Hides QWidget::actions().
    QList<QAction *> actions() const;

Q_SIGNALS:
    void modifiedChanged(bool modified);
    void statusMessage(const QString &message);
    void titleChanged(const QString &title);

private:
    struct Series;

    void buildUi();
    void createActions();
    void updateActions();

    // Replaces the whole network (undo/redo of operations).
    void replaceNetwork(const rf::NetworkData &network);
    void setElement(int frequencyIndex, int row, int column, rf::Complex value);
    void pushNetworkChange(const rf::NetworkData &after, const QString &text);
    void loadNetwork(const rf::NetworkData &network, const QString &path, const QString &name,
                     const QStringList &warnings, bool modified);

    void rebuildSelector();
    void updateSelectorState();
    void selectionChangedByUser();
    void updateQuantityItems();
    void updateSummary();
    void updateWarnings();
    void refreshPlots(bool rebuild);
    void refreshSmith(bool rebuild);
    // Shows series on a plot, updating the traces in place when the keys are
    // unchanged (markers survive); returns true when the traces were rebuilt.
    bool showSeries(PlotWidget *plot, const std::vector<Series> &series, bool rebuild, std::vector<int> &keys,
                    std::map<int, int> &traces, std::vector<int> &overlays);
    void updateTableEditability();
    void updateModified();
    void applyComment(const QString &comment);

    std::vector<rf::NoiseParameters> noiseParameters(std::vector<double> *frequencies) const;
    QString directory() const;
    std::optional<rf::NetworkData> readTwoPort(const QString &path, QString *error) const;
    void report(const QString &message);
    void reportError(const QString &title, const QString &message);

    // Interactive actions.
    void onSaveAs();
    void onExportCsv();
    void onReferences();
    void onRenumber();
    void onResample();
    void onCrop();
    void onCascade();
    void onDeembed();
    void onCheck();

    rf::NetworkData m_network;
    QString m_filePath;
    QString m_name;
    QStringList m_readWarnings;
    rf::TouchstoneWriteOptions m_writeOptions;
    bool m_unsavedNew = false;
    bool m_lastModified = false;
    QString m_lastTitle;

    std::vector<std::vector<bool>> m_selected; // [row][column]
    SParamQuantity m_quantity = SParamQuantity::MagnitudeDb;
    bool m_gammaOptVisible = true;
    std::vector<int> m_nonPassive;    // highlighted on the plots
    std::vector<int> m_nonReciprocal;
    QString m_checkText;

    // Plot state: trace keys of the last rebuild, to update data in place.
    std::vector<int> m_plotKeys;
    SParamQuantity m_plotKeysQuantity = SParamQuantity::MagnitudeDb;
    std::map<int, int> m_plotTraces;   // key -> trace id
    std::vector<int> m_smithKeys;
    std::map<int, int> m_smithTraces;
    std::vector<int> m_overlayTraces;
    std::vector<int> m_smithOverlayTraces;

    QUndoStack *m_undo = nullptr;
    bool m_applyingComment = false;

    // Widgets.
    QToolBar *m_toolBar = nullptr;
    QLabel *m_fileLabel = nullptr;
    QLabel *m_portsLabel = nullptr;
    QLabel *m_frequencyLabel = nullptr;
    QLabel *m_referenceLabel = nullptr;
    QLabel *m_noiseLabel = nullptr;
    QLabel *m_checkLabel = nullptr;
    QPlainTextEdit *m_comment = nullptr;
    QToolButton *m_warningsButton = nullptr;
    QPlainTextEdit *m_warnings = nullptr;
    QWidget *m_selectorBox = nullptr;
    QGridLayout *m_selectorGrid = nullptr;
    std::vector<std::vector<QCheckBox *>> m_selectorChecks;
    QComboBox *m_quantityCombo = nullptr;
    QCheckBox *m_gammaOptCheck = nullptr;
    QComboBox *m_representationCombo = nullptr;
    QComboBox *m_formCombo = nullptr;
    QLabel *m_tableNote = nullptr;
    PlotWidget *m_plot = nullptr;
    PlotWidget *m_smith = nullptr;
    SParamTableModel *m_model = nullptr;
    QTableView *m_table = nullptr;
    QSplitter *m_viewSplitter = nullptr;

    // Actions.
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_exportCsvAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_referencesAction = nullptr;
    QAction *m_renumberAction = nullptr;
    QAction *m_resampleAction = nullptr;
    QAction *m_cropAction = nullptr;
    QAction *m_cascadeAction = nullptr;
    QAction *m_deembedAction = nullptr;
    QAction *m_checkAction = nullptr;
    QAction *m_clearHighlightAction = nullptr;
    QAction *m_plotMenuAction = nullptr;
    QAction *m_smithMenuAction = nullptr;
    QList<QAction *> m_actions;
};

} // namespace lvd
