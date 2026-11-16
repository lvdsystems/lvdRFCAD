#pragma once

#include "spice/SpiceAnalysis.h"

#include <QHash>
#include <QWidget>

#include <memory>

class QGridLayout;
class QLabel;
class QScrollArea;

namespace lvd {
class Document;
class PlotWidget;
class Project;
} // namespace lvd

namespace lvd::spiceui {

// Virtual instruments placed on the schematic (requirement SP-5), shown from
// the latest results: the oscilloscope and spectrum analyser read the last
// transient, the Bode plotter the last AC analysis, the voltmeter and ammeter
// the last operating point or transient, and the function generator shows
// its setting and output. Each instrument is a panel with its reference.
class InstrumentsView : public QWidget
{
    Q_OBJECT
public:
    InstrumentsView(Project *project, Document *document, QWidget *parent = nullptr);

    // Keeps the simulation as the latest of its kind and rebuilds the panels.
    void addSimulation(std::shared_ptr<const spice::SpiceSimulation> simulation);
    void clear();

    // For tests and screenshots.
    QStringList instrumentReferences() const;
    PlotWidget *plot(const QString &reference) const;
    QString readout(const QString &reference) const;
    bool hasInstruments() const { return !m_panels.isEmpty(); }

Q_SIGNALS:
    // The Bode plotter needs an AC analysis.
    void acAnalysisRequested();

private:
    struct Panel {
        QWidget *box = nullptr;
        PlotWidget *plot = nullptr;
        QLabel *readout = nullptr;
    };
    void rebuild();
    double parameter(const QString &componentId, const QString &name, double fallback) const;
    QString parameterText(const QString &componentId, const QString &name) const;
    void fillOscilloscope(Panel &panel, const spice::SpiceElement &element);
    void fillMeter(Panel &panel, const spice::SpiceElement &element, bool current);
    void fillGenerator(Panel &panel, const spice::SpiceElement &element);
    void fillSpectrum(Panel &panel, const spice::SpiceElement &element);
    void fillBode(Panel &panel, const spice::SpiceElement &element);

    Project *m_project;
    Document *m_document;
    QHash<QString, std::shared_ptr<const spice::SpiceSimulation>> m_latest; // by plot type: op, tran, ac
    std::shared_ptr<const spice::SpiceSimulation> m_newest;
    QHash<QString, Panel> m_panels; // by reference
    QScrollArea *m_scroll;
    QWidget *m_host;
    QGridLayout *m_grid;
    QLabel *m_placeholder;
};

} // namespace lvd::spiceui
