#pragma once

#include "antenna/ArrayAnalysis.h"
#include "core/Document.h"

#include <QJsonObject>
#include <QPointer>
#include <QSplitter>

#include <memory>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
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

Q_SIGNALS:
    void statusMessage(const QString &text);

private:
    void buildUi();
    void designToForm();
    void formToDesign();
    void commit(const QString &text, int mergeKey = -1);
    void applyContent(const QJsonObject &content);
    void updateVisibility();
    void refreshViews();
    void saveToLibrary();

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

    QComboBox *m_component, *m_copol;
    QDoubleSpinBox *m_range, *m_plotFrequency;
    QCheckBox *m_polar;
    PlotWidget *m_cutPlot;
    Pattern3DView *m_view3d;
    QTableWidget *m_summary;
};

} // namespace lvd::antui
