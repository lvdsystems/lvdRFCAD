#include "gui/SchematicWorkspace.h"

#include "core/Document.h"
#include "core/Schematic.h"
#include "rfsim/Analysis.h"
#include "schematic_editor/SchematicEditor.h"
#include "schematic_editor/SchematicView.h"
#include "simulation_ui/SimulationView.h"
#include "spice/SpiceAnalysis.h"
#include "spice_ui/SpiceSimulationView.h"

namespace lvd {

SchematicWorkspace::SchematicWorkspace(Project *project, Document *document, QWidget *parent)
    : QSplitter(Qt::Vertical, parent)
{
    m_editor = new SchematicEditor(project, document, this);
    addWidget(m_editor);
    bool hasAnalyses = false;
    if (document->type() == DocumentType::SpiceSchematic) {
        m_spice = new spiceui::SpiceSimulationView(project, document, this);
        addWidget(m_spice);
        hasAnalyses = !spice::spiceAnalyses(document->schematic()->simulation()).isEmpty();
        connect(m_spice, &spiceui::SpiceSimulationView::runFinished, this, [this] { setSimulationVisible(true); });
        // Operating-point values on the drawing (SP-6).
        connect(m_spice, &spiceui::SpiceSimulationView::operatingPointLabelsChanged, this,
                [this](const QList<spiceui::OperatingPointLabel> &labels) {
                    QList<SchematicAnnotation> annotations;
                    for (const spiceui::OperatingPointLabel &label : labels) {
                        SchematicAnnotation annotation;
                        annotation.at = label.at;
                        annotation.text = label.text;
                        annotation.kind = label.kind == spiceui::OperatingPointLabel::Kind::Voltage
                                              ? SchematicAnnotation::Kind::Voltage
                                          : label.kind == spiceui::OperatingPointLabel::Kind::Power
                                              ? SchematicAnnotation::Kind::Power
                                              : SchematicAnnotation::Kind::Current;
                        annotations << annotation;
                    }
                    m_editor->view()->setAnnotations(annotations);
                });
    } else {
        m_simulation = new SimulationView(project, document, this);
        addWidget(m_simulation);
        hasAnalyses = !rf::linearAnalyses(document->schematic()->simulation()).isEmpty();
        connect(m_simulation, &SimulationView::runFinished, this, [this] { setSimulationVisible(true); });
    }
    setStretchFactor(0, 1);
    setStretchFactor(1, 1);
    setCollapsible(0, false);
    setFocusProxy(m_editor);
    setSimulationVisible(hasAnalyses);
}

bool SchematicWorkspace::isSimulationVisible() const
{
    return sizes().value(1) > 0;
}

void SchematicWorkspace::setSimulationVisible(bool visible)
{
    if (visible == isSimulationVisible())
        return;
    const int total = std::max(height(), 600);
    // Plots need room: about half of the tab for the results.
    setSizes(visible ? QList<int>{total * 9 / 20, total * 11 / 20} : QList<int>{total, 0});
}

void SchematicWorkspace::runSimulation()
{
    setSimulationVisible(true);
    if (m_spice) {
        m_spice->run();
        return;
    }
    if (m_simulation->currentAnalysisId().isEmpty()) {
        m_simulation->editAnalysis(QString()); // asks for a new analysis
        if (m_simulation->currentAnalysisId().isEmpty())
            return;
    }
    m_simulation->run();
}

void SchematicWorkspace::newAnalysis()
{
    setSimulationVisible(true);
    if (m_spice)
        m_spice->newAnalysis();
    else
        m_simulation->editAnalysis(QString());
}

} // namespace lvd
