#pragma once

#include "antenna/ArrayAnalysis.h"
#include "core/Document.h"

#include <QJsonObject>
#include <QPointer>
#include <QSplitter>

#include <atomic>
#include <memory>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QTimer;
class QToolBox;
class QUndoStack;

namespace lvd {
class ColorMapPlot;
class Document;
class Pattern3DView;
class PlotWidget;
class Project;
} // namespace lvd

namespace lvd::ant {
class AntennaLibraryManager;
struct AntennaLibraryEntry;
} // namespace lvd::ant

namespace lvd::antui {

// Everything computed for one state of an array design (in the background).
struct ArrayResults {
    ant::ArrayDesign design;
    ant::ArrayEvaluation evaluation;
    ant::PatternCut cutX, cutY, cutSteer;    // phi = 0, phi = 90 and the steering plane
    ant::PatternCut idealX, idealY, idealSteer;
    ant::PatternCut horizon, idealHorizon;   // theta = 90 (azimuth pattern)
    ant::UvGrid uv;
    ant::SphericalGrid sphere;
    ant::BeamMetrics metrics;
    std::vector<ant::ScanPoint> scan;
    std::vector<ant::BandPoint> band;
    ant::BudgetValues budget;
    ant::TrValues tx, rx;
    std::optional<ant::GratingLobeInfo> lobes;
    double gratingFreeScan0 = NAN, gratingFreeScan90 = NAN;
    QStringList warnings;
    QString error;
    double seconds = 0;
    bool cancelled = false;
};

// Workspace of an antenna array document (requirements AN-1 to AN-6): a
// settings panel on the left, pattern views on the right. Every edit is
// written to the document at once and recomputed in the background.
class ArrayWorkspace : public QSplitter
{
    Q_OBJECT
public:
    ArrayWorkspace(Project *project, Document *document, ant::AntennaLibraryManager *library, QWidget *parent = nullptr);
    ~ArrayWorkspace() override;

    Document *document() const { return m_document; }
    const ant::ArrayDesign &design() const { return m_design; }
    QList<QAction *> editActions() const;
    // The latest finished results, or null before the first run.
    const ArrayResults *results() const { return m_results.get(); }
    bool isComputing() const { return m_running; }

    // Applies an antenna library entry (element, pattern, T/R module or array preset).
    void applyLibraryEntry(const QString &key, const ant::AntennaLibraryEntry &entry);
    // Runs pattern synthesis now (blocking with a progress dialog in the GUI; direct in tests).
    void synthesise();
    // Opens the element position editor; any geometry becomes a custom table.
    void editPositions();

    // For tests and screenshots: wait until the background run is done.
    bool waitForResults(int timeoutMs = 60000);
    QTabWidget *views() const { return m_views; }
    void showView(const QString &name); // "cuts", "uv", "3d", "layout", "scan", "summary"
    // Settings pages: 0 frequency and element, 1 geometry, 2 excitation, 3 AESA,
    // 4 T/R module, 5 synthesis, 6 sweeps.
    void showSettingsPage(int index);
    // View options of the top bar.
    void setViewOptions(const QString &component, bool polar, double rangeDb);

Q_SIGNALS:
    void statusMessage(const QString &text);
    void resultsReady();

private:
    void buildPanel();
    void buildViews();
    void designToForm();
    void formToDesign();
    void commit(const QString &text, int mergeKey = -1);
    void applyContent(const QJsonObject &content);
    void scheduleCompute();
    void startCompute();
    void onComputed(std::shared_ptr<ArrayResults> results);
    void refreshViews();
    void refreshCuts();
    void refreshUv();
    void refresh3d();
    void refreshLayout();
    void refreshSweeps();
    void refreshSummary();
    void refreshElementChoices();
    void refreshTrChoices();
    void updateVisibility();
    void toggleFailure(int index);

    Project *m_project;
    QPointer<Document> m_document;
    ant::AntennaLibraryManager *m_library;
    ant::ArrayDesign m_design;
    QJsonObject m_content;
    QUndoStack *m_undo;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    bool m_updatingForm = false;
    bool m_writing = false;

    std::shared_ptr<ArrayResults> m_results;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QTimer *m_debounce;
    bool m_running = false;
    bool m_pending = false;

    // Panel
    QToolBox *m_toolBox = nullptr;
    QDoubleSpinBox *m_frequency, *m_designFrequency;
    QComboBox *m_elementSource;
    QComboBox *m_elementModel, *m_elementPolarisation;
    QDoubleSpinBox *m_elementExponent, *m_elementEfficiency;
    QLabel *m_elementNote;
    QComboBox *m_lattice, *m_unit, *m_aperture;
    QSpinBox *m_nx, *m_ny;
    QDoubleSpinBox *m_dx, *m_dy, *m_apertureX, *m_apertureY;
    QLineEdit *m_rings;
    QCheckBox *m_centreElement;
    QDoubleSpinBox *m_cylRadius, *m_arc, *m_dz;
    QSpinBox *m_columns, *m_rows;
    QLabel *m_geometryNote;
    QPushButton *m_importElements;
    QComboBox *m_taperX, *m_taperY;
    QDoubleSpinBox *m_sllX, *m_sllY, *m_paramX, *m_paramY;
    QSpinBox *m_nbarX, *m_nbarY;
    QCheckBox *m_radial;
    QDoubleSpinBox *m_steerTheta, *m_steerPhi;
    QComboBox *m_steering, *m_pattern;
    QSpinBox *m_subX, *m_subY;
    QCheckBox *m_subTaper;
    QTableWidget *m_beams, *m_nulls, *m_weights;
    QSpinBox *m_phaseBits, *m_attBits, *m_seed;
    QDoubleSpinBox *m_attStep, *m_delayLsb, *m_ampError, *m_phaseError, *m_failedFraction;
    QLineEdit *m_failedList;
    QComboBox *m_txSource, *m_rxSource, *m_txLink, *m_rxLink;
    QDoubleSpinBox *m_txPower, *m_txGain, *m_rxGain, *m_rxNf;
    QDoubleSpinBox *m_frontLoss, *m_bfLoss, *m_antennaTemp, *m_ambientTemp;
    QCheckBox *m_txTaper;
    QLabel *m_budgetLabel;
    QDoubleSpinBox *m_synMainBeam, *m_synSll;
    QLineEdit *m_synSegments;
    QCheckBox *m_synPhaseOnly;
    QSpinBox *m_synIterations;
    QLabel *m_synResult;
    QDoubleSpinBox *m_scanPlane, *m_scanMax, *m_scanStep, *m_bandStart, *m_bandStop;
    QSpinBox *m_bandPoints;

    // Views
    QTabWidget *m_views = nullptr;
    QComboBox *m_component, *m_copol, *m_cutMode;
    QDoubleSpinBox *m_range;
    QCheckBox *m_polar, *m_showIdeal;
    QComboBox *m_layoutValue;
    QLabel *m_statusLabel;
    PlotWidget *m_cutPlot;
    ColorMapPlot *m_uvPlot;
    Pattern3DView *m_view3d;
    ColorMapPlot *m_layoutPlot;
    PlotWidget *m_scanPlot, *m_bandPlot;
    QTableWidget *m_summary;
};

} // namespace lvd::antui
