#pragma once

#include <QSplitter>

namespace lvd {

class Document;
class Project;
class SchematicEditor;
class SimulationView;

namespace spiceui {
class SpiceSimulationView;
}

// The tab of a schematic document: the editor above, its simulation results
// below. The results area starts collapsed and opens on the first run. RF
// schematics get the RF SimulationView, SPICE schematics the
// SpiceSimulationView; the other accessor returns null.
class SchematicWorkspace : public QSplitter
{
    Q_OBJECT
public:
    SchematicWorkspace(Project *project, Document *document, QWidget *parent = nullptr);

    SchematicEditor *editor() const { return m_editor; }
    SimulationView *simulation() const { return m_simulation; }
    spiceui::SpiceSimulationView *spiceSimulation() const { return m_spice; }
    bool isSpice() const { return m_spice != nullptr; }

    bool isSimulationVisible() const;
    void setSimulationVisible(bool visible);
    // Opens the results area and runs the current analysis (creating one if needed).
    void runSimulation();
    // Opens the dialog for a new analysis.
    void newAnalysis();

private:
    SchematicEditor *m_editor;
    SimulationView *m_simulation = nullptr;
    spiceui::SpiceSimulationView *m_spice = nullptr;
};

} // namespace lvd
