#pragma once

#include "antenna/ArrayAnalysis.h"
#include "core/Document.h"
#include "fullwave/Simulation.h"

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
class QPlainTextEdit;
class QProgressBar;
class QTabWidget;
class QSpinBox;
class QPushButton;
class QGroupBox;
class QFormLayout;
class QTableWidget;
class QTimer;
class QUndoStack;

namespace lvd {
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

struct ElementResults {
    std::shared_ptr<const ant::ElementPattern> pattern;
    ant::PatternCut cutE, cutH;   // phi = 0 and phi = 90
    ant::SphericalGrid sphere;
    double directivityDbi = NAN, gainDbi = NAN;
    double peakThetaDeg = NAN, peakPhiDeg = NAN;
    double beamwidthEDeg = NAN, beamwidthHDeg = NAN;
    double frontToBackDb = NAN;
    QString error;
    QStringList warnings;
};

// Workspace of an antenna element document: the model and its parameters,
// pattern import, cuts, 3D pattern and figures of merit (requirements AN-5).
class ElementWorkspace : public QSplitter
{
    Q_OBJECT
public:
    ElementWorkspace(Project *project, Document *document, ant::AntennaLibraryManager *library, QWidget *parent = nullptr);

    Document *document() const { return m_document; }
    const ant::ElementDesign &design() const { return m_design; }
    const ElementResults &results() const { return m_results; }
    QList<QAction *> editActions() const;

    void applyLibraryEntry(const QString &key, const ant::AntennaLibraryEntry &entry);
    // Copies a pattern file into the project's "patterns" folder and uses it.
    bool importPattern(const QString &path, QString *error = nullptr);
    void recompute(); // synchronous

    // Full-wave model (requirements section 10): starts the openEMS run in
    // the background; waitForSimulation() is for tests and scripts.
    bool startSimulation(QString *error = nullptr);
    void abortSimulation();
    // Shows a view tab: "cuts", "3d", "geometry", "port" or "summary".
    void showView(const QString &key);
    bool isSimulating() const { return m_simulating; }
    bool waitForSimulation(int timeoutMs = 600000);
    const fw::FwResults *simulationResults() const { return m_fwResults ? &*m_fwResults : nullptr; }
    QString simulationFolder() const;
    // Template "stl": copies an STL file into the project's "cad" folder and
    // adds it as a body (scale: file units to millimetres).
    bool addStlBody(const QString &path, double scale, bool metal, QString *error = nullptr);
    // Substrate picker of the full-wave templates: fills the template's
    // permittivity, loss tangent and height from a material of
    // libraries/substrates.json (fw::applySubstrate) as one undoable edit.
    // False for an unknown material or no template.
    bool applySubstrateMaterial(const QString &materialId);

Q_SIGNALS:
    void statusMessage(const QString &text);
    void simulationFinished(bool ok);

private:
    void buildUi();
    void designToForm();
    void formToDesign();
    void commit(const QString &text, int mergeKey = -1);
    void applyContent(const QJsonObject &content);
    void updateVisibility();
    void refreshViews();
    void saveToLibrary();
    void buildFullWavePanel(QFormLayout *form);
    void rebuildParameterForm();
    void fullWaveToForm();
    void formToFullWave();
    void refreshGeometry();
    void refreshPortPlot();
    void loadSimulationResults();
    void onSimulationFinished(const fw::FwResults &results);

    Project *m_project;
    QPointer<Document> m_document;
    ant::AntennaLibraryManager *m_library;
    ant::ElementDesign m_design;
    QJsonObject m_content;
    QUndoStack *m_undo;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    bool m_updatingForm = false;
    bool m_writing = false;
    QTimer *m_debounce;
    ElementResults m_results;

    QComboBox *m_model, *m_polarisation, *m_distribution;
    QDoubleSpinBox *m_frequency, *m_efficiency, *m_exponent, *m_backLobe, *m_length, *m_height;
    QDoubleSpinBox *m_patchW, *m_patchL, *m_substrateH, *m_permittivity, *m_apA, *m_apB;
    QLineEdit *m_patternFile;
    QLabel *m_derived;
    QPlainTextEdit *m_notes;

    // Full-wave
    QGroupBox *m_fwGroup = nullptr;
    QComboBox *m_fwTemplate = nullptr;
    QFormLayout *m_fwParams = nullptr;
    QComboBox *m_fwSubstrate = nullptr;
    std::vector<std::pair<QString, QDoubleSpinBox *>> m_fwParamSpins;
    QDoubleSpinBox *m_fwStart = nullptr, *m_fwStop = nullptr, *m_fwCells = nullptr, *m_fwEnd = nullptr, *m_fwStep = nullptr;
    QComboBox *m_fwBoundary = nullptr;
    QPushButton *m_fwRun = nullptr, *m_fwAbort = nullptr;
    QTabWidget *m_tabs = nullptr;
    QSpinBox *m_fwNx = nullptr, *m_fwNy = nullptr;
    QDoubleSpinBox *m_fwDx = nullptr, *m_fwDy = nullptr;
    QWidget *m_fwBodiesBox = nullptr;
    QTableWidget *m_fwBodies = nullptr;
    QProgressBar *m_fwProgress = nullptr;
    QLabel *m_fwStatus = nullptr;
    std::optional<fw::FwResults> m_fwResults;
    std::shared_ptr<std::atomic_bool> m_fwCancel;
    bool m_simulating = false;
    Pattern3DView *m_geometryView = nullptr;
    PlotWidget *m_portPlot = nullptr;

    QComboBox *m_component, *m_copol;
    QDoubleSpinBox *m_range, *m_plotFrequency;
    QCheckBox *m_polar;
    PlotWidget *m_cutPlot;
    Pattern3DView *m_view3d;
    QTableWidget *m_summary;
};

} // namespace lvd::antui
