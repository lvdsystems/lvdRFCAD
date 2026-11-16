#include "spice_ui/SpiceSimulationView.h"

#include "core/Connectivity.h"
#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "spice/SpiceImport.h"
#include "spice/SpiceProcessPool.h"
#include "spice/SpiceWaveforms.h"
#include "spice_ui/BatchDialog.h"
#include "spice_ui/BatchView.h"
#include "spice_ui/InstrumentsView.h"
#include "spice_ui/SpiceAnalysisDialog.h"
#include "spice_ui/SpiceRunner.h"
#include "spice_ui/WaveformPane.h"
#include "spice_ui/WaveformViewer.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableView>
#include <QThread>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <numbers>
#include <thread>

namespace lvd::spiceui {

using spice::AnalysisKind;

namespace {

const QString PanesKey = QStringLiteral("spicePanes");

// The plot a result is shown from in the waveform viewer.
int waveformPlotIndex(const spice::SpiceSimulation &simulation)
{
    QString wanted;
    switch (simulation.analysis.kind) {
    case AnalysisKind::DcSweep: wanted = QStringLiteral("dc"); break;
    case AnalysisKind::Ac:
    case AnalysisKind::Sensitivity: wanted = QStringLiteral("ac"); break;
    case AnalysisKind::Transient:
    case AnalysisKind::Fourier: wanted = QStringLiteral("tran"); break;
    case AnalysisKind::Noise: wanted = QStringLiteral("noise"); break;
    default: return -1;
    }
    for (int i = 0; i < simulation.run.plots.size(); ++i)
        if (simulation.run.plots.at(i).type == wanted && simulation.run.plots.at(i).scale())
            return i;
    return -1;
}

QString formatValue(double value, const QString &unit)
{
    return formatEngineering(value, unit, 5);
}

} // namespace

SpiceSimulationView::SpiceSimulationView(Project *project, Document *document, QWidget *parent)
    : QWidget(parent), m_project(project), m_document(document), m_schematic(document->schematic())
{
    m_runner = new SpiceRunner(this);
    createActions();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_toolBar = new QToolBar(this);
    m_toolBar->setIconSize(QSize(16, 16));
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_analysisCombo = new QComboBox(this);
    m_analysisCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_analysisCombo->setMinimumContentsLength(24);
    m_analysisCombo->setToolTip(tr("SPICE analysis"));
    m_toolBar->addWidget(m_analysisCombo);
    m_toolBar->addAction(m_newAction);
    m_toolBar->addAction(m_editAction);
    m_toolBar->addAction(m_deleteAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_runAction);
    m_toolBar->addAction(m_pauseAction);
    m_toolBar->addAction(m_stopAction);
    m_toolBar->addSeparator();
    auto *batchButton = new QToolButton(this);
    batchButton->setText(tr("Sweeps && Statistics"));
    batchButton->setPopupMode(QToolButton::InstantPopup);
    auto *batchMenu = new QMenu(batchButton);
    batchMenu->addAction(m_sweepAction);
    batchMenu->addAction(m_monteCarloAction);
    batchMenu->addAction(m_worstCaseAction);
    batchMenu->addSeparator();
    batchMenu->addAction(m_optimizeAction);
    batchButton->setMenu(batchMenu);
    m_toolBar->addWidget(batchButton);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_annotateAction);
    m_toolBar->addAction(m_netlistAction);
    layout->addWidget(m_toolBar);

    m_banner = new QLabel(this);
    m_banner->setWordWrap(true);
    m_banner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_banner->setContentsMargins(8, 4, 8, 4);
    m_banner->hide();
    layout->addWidget(m_banner);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_viewer = new WaveformViewer(this);
    m_tabs->addTab(m_viewer, tr("Waveforms"));

    m_valuesModel = new QStandardItemModel(this);
    m_valuesTable = new QTableView(this);
    m_valuesTable->setModel(m_valuesModel);
    m_valuesTable->verticalHeader()->hide();
    m_valuesTable->horizontalHeader()->setStretchLastSection(true);
    m_valuesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_valuesTable->setAlternatingRowColors(true);
    m_valuesTable->setSortingEnabled(true);
    m_tabs->addTab(m_valuesTable, tr("Values"));

    m_instruments = new InstrumentsView(project, document, this);
    m_tabs->addTab(m_instruments, tr("Instruments"));
    connect(m_instruments, &InstrumentsView::acAnalysisRequested, this, [this] { newAnalysis(AnalysisKind::Ac); });

    m_batchView = new BatchView(this);
    m_tabs->addTab(m_batchView, tr("Sweeps && Statistics"));

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_netlist = new QPlainTextEdit(this);
    m_netlist->setReadOnly(true);
    m_netlist->setFont(mono);
    m_netlist->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_tabs->addTab(m_netlist, tr("Netlist"));
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setFont(mono);
    m_log->setMaximumBlockCount(20000);
    m_tabs->addTab(m_log, tr("Log"));
    layout->addWidget(m_tabs, 1);

    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(6, 0, 6, 2);
    m_status = new QLabel(this);
    m_progress = new QProgressBar(this);
    m_progress->setMaximumWidth(220);
    m_progress->setMaximumHeight(14);
    m_progress->setTextVisible(false);
    m_progress->hide();
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_progress);
    layout->addLayout(statusRow);

    connect(m_analysisCombo, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_updatingCombo)
            setCurrentAnalysis(m_analysisCombo->currentData().toString());
    });
    connect(m_viewer, &WaveformViewer::panesEdited, this, &SpiceSimulationView::storePanes);
    connect(m_viewer, &WaveformViewer::statusMessage, this, &SpiceSimulationView::statusMessage);
    connect(m_runner, &SpiceRunner::progress, this, [this](const QString &analysis, double fraction) {
        m_progress->show();
        if (fraction < 0) {
            m_progress->setRange(0, 0);
        } else {
            m_progress->setRange(0, 1000);
            m_progress->setValue(int(fraction * 1000));
        }
        m_status->setText(fraction < 0 ? tr("Running %1…").arg(analysis)
                                       : tr("Running %1: %2 %").arg(analysis).arg(fraction * 100, 0, 'f', 1));
    });
    connect(m_runner, &SpiceRunner::output, this, [this](const QString &line) { m_log->appendPlainText(line); });
    connect(m_runner, &SpiceRunner::finished, this,
            [this](std::shared_ptr<const spice::SpiceSimulation> simulation, quint64 tag) {
                if (tag != m_runTag)
                    return;
                m_progress->hide();
                m_results.insert(m_runningAnalysis, simulation);
                const bool ok = simulation->ok();
                m_status->setText(simulation->run.cancelled
                                      ? tr("Stopped.")
                                      : ok ? tr("Finished in %1 s.").arg(simulation->run.seconds, 0, 'f', 2)
                                           : tr("The simulation failed."));
                if (m_runningAnalysis == m_currentAnalysis)
                    showSimulation(simulation);
                updateActions();
                Q_EMIT runFinished(ok);
            });
    connect(m_batchView, &BatchView::showRunRequested, this, [this](int index) {
        if (!m_batch || index < 0 || index >= m_batch->runs.size())
            return;
        showSimulation(m_batch->runs.at(index).simulation);
        m_tabs->setCurrentWidget(m_viewer);
    });
    if (m_schematic) {
        connect(m_schematic, &Schematic::simulationChanged, this, [this] { refreshAnalyses(); });
        // Labels describe the circuit as simulated; edits move parts away.
        const auto dropLabels = [this] {
            if (!m_labels.isEmpty()) {
                m_labels.clear();
                publishLabels();
            }
        };
        connect(m_schematic, &Schematic::itemAdded, this, dropLabels);
        connect(m_schematic, &Schematic::itemChanged, this, dropLabels);
        connect(m_schematic, &Schematic::itemRemoved, this, dropLabels);
    }

    refreshAnalyses();
    updateActions();
}

SpiceSimulationView::~SpiceSimulationView()
{
    if (m_batchCancel)
        m_batchCancel->store(true);
}

void SpiceSimulationView::createActions()
{
    m_newAction = new QAction(tr("New…"), this);
    m_newAction->setToolTip(tr("New SPICE analysis"));
    connect(m_newAction, &QAction::triggered, this, [this] { newAnalysis(); });
    m_editAction = new QAction(tr("Edit…"), this);
    connect(m_editAction, &QAction::triggered, this, [this] { editAnalysis(m_currentAnalysis); });
    m_deleteAction = new QAction(tr("Delete"), this);
    connect(m_deleteAction, &QAction::triggered, this, [this] {
        if (QMessageBox::question(this, tr("Delete Analysis"), tr("Delete the analysis \"%1\"?").arg(m_analysisCombo->currentText()))
            == QMessageBox::Yes)
            deleteAnalysis(m_currentAnalysis);
    });
    m_runAction = new QAction(tr("Run"), this);
    m_runAction->setShortcut(QKeySequence(Qt::Key_F5));
    m_runAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_runAction, &QAction::triggered, this, &SpiceSimulationView::run);
    m_pauseAction = new QAction(tr("Pause"), this);
    m_pauseAction->setCheckable(true);
    connect(m_pauseAction, &QAction::triggered, this, &SpiceSimulationView::togglePause);
    m_stopAction = new QAction(tr("Stop"), this);
    m_stopAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    m_stopAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_stopAction, &QAction::triggered, this, &SpiceSimulationView::stop);
    m_annotateAction = new QAction(tr("Show on Schematic"), this);
    m_annotateAction->setCheckable(true);
    m_annotateAction->setChecked(true);
    m_annotateAction->setToolTip(tr("Show operating-point voltages, currents and powers on the schematic"));
    connect(m_annotateAction, &QAction::toggled, this, [this] { publishLabels(); });
    m_netlistAction = new QAction(tr("Export Netlist…"), this);
    connect(m_netlistAction, &QAction::triggered, this, &SpiceSimulationView::exportNetlist);
    m_sweepAction = new QAction(tr("Parameter Sweep…"), this);
    connect(m_sweepAction, &QAction::triggered, this, [this] { openBatchDialog(int(BatchDialog::Mode::Sweep)); });
    m_monteCarloAction = new QAction(tr("Monte Carlo…"), this);
    connect(m_monteCarloAction, &QAction::triggered, this, [this] { openBatchDialog(int(BatchDialog::Mode::MonteCarlo)); });
    m_worstCaseAction = new QAction(tr("Worst Case…"), this);
    connect(m_worstCaseAction, &QAction::triggered, this, [this] { openBatchDialog(int(BatchDialog::Mode::WorstCase)); });
    m_optimizeAction = new QAction(tr("Optimize…"), this);
    connect(m_optimizeAction, &QAction::triggered, this, [this] { openBatchDialog(int(BatchDialog::Mode::Optimize)); });
}

QList<QAction *> SpiceSimulationView::actions() const
{
    return {m_newAction, m_editAction, m_deleteAction, m_runAction, m_pauseAction, m_stopAction, m_annotateAction,
            m_netlistAction, m_sweepAction, m_monteCarloAction, m_worstCaseAction, m_optimizeAction};
}

// ---------------------------------------------------------------- analyses

QList<spice::SpiceAnalysisSetup> SpiceSimulationView::analyses() const
{
    return m_schematic ? spice::spiceAnalyses(m_schematic->simulation()) : QList<spice::SpiceAnalysisSetup>{};
}

std::optional<spice::SpiceAnalysisSetup> SpiceSimulationView::analysis(const QString &id) const
{
    for (const spice::SpiceAnalysisSetup &setup : analyses())
        if (setup.id == id)
            return setup;
    return std::nullopt;
}

void SpiceSimulationView::refreshAnalyses()
{
    const QList<spice::SpiceAnalysisSetup> list = analyses();
    m_updatingCombo = true;
    m_analysisCombo->clear();
    for (const spice::SpiceAnalysisSetup &setup : list)
        m_analysisCombo->addItem(QStringLiteral("%1  (%2)").arg(setup.name, spice::analysisDisplayName(setup.kind)), setup.id);
    if (list.isEmpty())
        m_analysisCombo->addItem(tr("No analysis — click New…"), QString());
    bool found = false;
    for (const spice::SpiceAnalysisSetup &setup : list)
        found = found || setup.id == m_currentAnalysis;
    if (!found)
        m_currentAnalysis = list.isEmpty() ? QString() : list.first().id;
    m_analysisCombo->setCurrentIndex(std::max(0, m_analysisCombo->findData(m_currentAnalysis)));
    m_updatingCombo = false;
    updateActions();
}

void SpiceSimulationView::setCurrentAnalysis(const QString &id)
{
    if (id == m_currentAnalysis && m_viewer->result())
        return;
    m_currentAnalysis = id;
    m_updatingCombo = true;
    m_analysisCombo->setCurrentIndex(std::max(0, m_analysisCombo->findData(id)));
    m_updatingCombo = false;
    if (const auto result = m_results.value(id))
        showSimulation(result);
    else {
        m_viewer->clearData();
        m_valuesModel->clear();
        m_banner->hide();
    }
    updateActions();
}

QString SpiceSimulationView::addAnalysis(const spice::SpiceAnalysisSetup &setup)
{
    if (!m_schematic)
        return {};
    QString id;
    m_schematic->setSimulation(spice::withSpiceAnalysis(m_schematic->simulation(), setup, &id));
    refreshAnalyses();
    setCurrentAnalysis(id);
    return id;
}

QStringList SpiceSimulationView::netNames() const
{
    QStringList names;
    if (!m_schematic)
        return names;
    const QString imported = m_schematic->simulation().value(QStringLiteral("netlist")).toString();
    const spice::SpiceDeck deck = imported.isEmpty() ? spice::buildSpiceDeck(*m_project, *m_document)
                                                     : spice::deckFromNetlist(imported, QString());
    // Labelled nets first, then the generated ones.
    QStringList generated;
    for (auto it = deck.nodeOfNet.begin(); it != deck.nodeOfNet.end(); ++it) {
        if (it.key() == QLatin1String("0"))
            continue;
        (it.key().startsWith(QLatin1String("Net-(")) || it.key().contains(QLatin1Char('/')) ? generated : names) << it.key();
    }
    names << QStringLiteral("0") << generated;
    return names;
}

QStringList SpiceSimulationView::sourceReferences() const
{
    QStringList references;
    if (!m_schematic)
        return references;
    const QString imported = m_schematic->simulation().value(QStringLiteral("netlist")).toString();
    const spice::SpiceDeck deck = imported.isEmpty() ? spice::buildSpiceDeck(*m_project, *m_document)
                                                     : spice::deckFromNetlist(imported, QString());
    for (const spice::SpiceElement &element : deck.elements)
        for (const QString &name : element.elementNames)
            if (name.startsWith(QLatin1Char('v')) || name.startsWith(QLatin1Char('i'))) {
                references << element.path;
                break;
            }
    return references;
}

QStringList SpiceSimulationView::validate(const spice::SpiceAnalysisSetup &setup) const
{
    const spice::SpiceSimulation prepared = spice::prepareSpiceAnalysis(*m_project, *m_document, setup);
    // Only problems of the setup itself keep the dialog open; circuit
    // problems show when the analysis runs.
    QStringList problems;
    for (const QString &error : prepared.errors)
        if (error.startsWith(setup.name + QLatin1Char(':')))
            problems << error.mid(setup.name.size() + 1).trimmed();
    return problems;
}

void SpiceSimulationView::newAnalysis(std::optional<AnalysisKind> kind)
{
    spice::SpiceAnalysisSetup setup = spice::SpiceAnalysisSetup::create(kind.value_or(AnalysisKind::Transient));
    // Sensible sources and nets for the defaults.
    const QStringList sources = sourceReferences();
    const QStringList nets = netNames();
    if (!sources.isEmpty())
        setup.set(QStringLiteral("source"), sources.first());
    SpiceAnalysisDialog dialog(setup, true, this);
    dialog.setNets(nets);
    dialog.setSources(sources);
    dialog.setValidator([this](const spice::SpiceAnalysisSetup &s) { return validate(s); });
    if (dialog.exec() != QDialog::Accepted)
        return;
    addAnalysis(dialog.setup());
    run();
}

void SpiceSimulationView::editAnalysis(const QString &id)
{
    const auto setup = analysis(id);
    if (!setup) {
        newAnalysis();
        return;
    }
    SpiceAnalysisDialog dialog(*setup, false, this);
    dialog.setNets(netNames());
    dialog.setSources(sourceReferences());
    dialog.setValidator([this](const spice::SpiceAnalysisSetup &s) { return validate(s); });
    if (dialog.exec() != QDialog::Accepted)
        return;
    addAnalysis(dialog.setup());
    m_results.remove(id);
    run();
}

void SpiceSimulationView::deleteAnalysis(const QString &id)
{
    if (!m_schematic || id.isEmpty())
        return;
    QJsonObject simulation = m_schematic->simulation();
    QJsonArray list = simulation.value(QStringLiteral("analyses")).toArray();
    for (qsizetype i = 0; i < list.size(); ++i)
        if (list.at(i).toObject().value(QStringLiteral("id")).toString() == id) {
            list.removeAt(i);
            break;
        }
    simulation.insert(QStringLiteral("analyses"), list);
    QJsonObject panes = simulation.value(PanesKey).toObject();
    panes.remove(id);
    simulation.insert(PanesKey, panes);
    m_schematic->setSimulation(simulation);
    m_results.remove(id);
    refreshAnalyses();
    setCurrentAnalysis(m_currentAnalysis);
}

// ---------------------------------------------------------------- running

bool SpiceSimulationView::isRunning() const
{
    return m_runner->isRunning() || m_batchRunning;
}

void SpiceSimulationView::run()
{
    if (isRunning())
        return;
    const auto setup = analysis(m_currentAnalysis);
    if (!setup) {
        newAnalysis();
        return;
    }
    spice::SpiceSimulation prepared = spice::prepareSpiceAnalysis(*m_project, *m_document, *setup);
    m_netlist->setPlainText(prepared.netlist);
    m_log->clear();
    if (!prepared.errors.isEmpty()) {
        showMessages(prepared.errors, prepared.warnings);
        m_status->setText(tr("The circuit cannot be simulated."));
        Q_EMIT runFinished(false);
        return;
    }
    m_runningAnalysis = setup->id;
    m_runner->start(std::move(prepared), ++m_runTag);
    m_status->setText(tr("Running %1…").arg(setup->name));
    m_progress->setRange(0, 0);
    m_progress->show();
    updateActions();
}

void SpiceSimulationView::stop()
{
    if (m_runner->isRunning())
        m_runner->abort();
    if (m_batchCancel)
        m_batchCancel->store(true);
}

void SpiceSimulationView::togglePause()
{
    if (!m_runner->isRunning())
        return;
    if (m_runner->isPaused()) {
        m_runner->resume();
        m_status->setText(tr("Running…"));
    } else {
        m_runner->pause();
        m_status->setText(tr("Paused."));
    }
    updateActions();
}

std::shared_ptr<const spice::SpiceSimulation> SpiceSimulationView::lastSimulation() const
{
    return m_results.value(m_currentAnalysis);
}

void SpiceSimulationView::updateActions()
{
    const bool running = isRunning();
    const bool hasAnalysis = !m_currentAnalysis.isEmpty();
    m_runAction->setEnabled(!running);
    m_stopAction->setEnabled(running);
    m_pauseAction->setEnabled(m_runner->isRunning());
    m_pauseAction->setChecked(m_runner->isPaused());
    m_editAction->setEnabled(hasAnalysis && !running);
    m_deleteAction->setEnabled(hasAnalysis && !running);
    m_newAction->setEnabled(!running);
    for (QAction *action : {m_sweepAction, m_monteCarloAction, m_worstCaseAction, m_optimizeAction})
        action->setEnabled(hasAnalysis && !running);
    m_analysisCombo->setEnabled(!running);
}

void SpiceSimulationView::showMessages(const QStringList &errors, const QStringList &warnings)
{
    m_errors = errors;
    if (errors.isEmpty() && warnings.isEmpty()) {
        m_banner->hide();
        return;
    }
    const bool error = !errors.isEmpty();
    QStringList lines = error ? errors : warnings;
    if (lines.size() > 8) {
        const qsizetype more = lines.size() - 8;
        lines = lines.mid(0, 8);
        lines << tr("… and %n more (see the Log tab)", nullptr, int(more));
    }
    m_banner->setText(lines.join(QLatin1Char('\n')));
    m_banner->setStyleSheet(error ? QStringLiteral("QLabel { background: #c62828; color: white; }")
                                  : QStringLiteral("QLabel { background: #f9a825; color: black; }"));
    m_banner->show();
}

void SpiceSimulationView::showSimulation(std::shared_ptr<const spice::SpiceSimulation> simulation)
{
    if (!simulation)
        return;
    showMessages(simulation->errors, simulation->warnings);
    if (m_netlist->toPlainText().isEmpty() || simulation->analysis.id == m_currentAnalysis)
        m_netlist->setPlainText(simulation->netlist);
    if (!simulation->run.log.isEmpty())
        m_log->setPlainText(simulation->run.log.join(QLatin1Char('\n')));

    // Names as on the schematic: v(<label>), i(<reference>).
    const auto deck = std::make_shared<spice::SpiceDeck>(simulation->deck);
    m_viewer->setNameResolver([deck](const spice::SpicePlot &plot, const QString &name) {
        return spice::plotResolver(plot, deck.get())(name);
    });
    const int plotIndex = waveformPlotIndex(*simulation);
    if (plotIndex >= 0) {
        m_viewer->setData(std::shared_ptr<const spice::SpiceRunResult>(simulation, &simulation->run), plotIndex);
        QList<WaveformPaneDefinition> panes = storedPanes(simulation->analysis.id);
        if (panes.isEmpty()) {
            // Probes on the schematic, else the first node voltages.
            WaveformPaneDefinition pane = WaveformViewer::defaultPane(&simulation->run.plots.at(plotIndex));
            QStringList traces;
            for (const spice::SpiceElement &element : simulation->deck.elements) {
                switch (element.probe) {
                case spice::ProbeKind::Voltage:
                    traces << QStringLiteral("v(%1)").arg(element.pinNodes.value(QStringLiteral("p")));
                    break;
                case spice::ProbeKind::Differential:
                    traces << QStringLiteral("v(%1,%2)").arg(element.pinNodes.value(QStringLiteral("p")),
                                                             element.pinNodes.value(QStringLiteral("n")));
                    break;
                case spice::ProbeKind::Current:
                    traces << QStringLiteral("i(%1)").arg(element.name);
                    break;
                case spice::ProbeKind::None:
                    break;
                }
            }
            if (traces.isEmpty()) {
                if (simulation->analysis.kind == AnalysisKind::Noise) {
                    traces << QStringLiteral("onoise_spectrum");
                } else {
                    for (auto it = simulation->deck.netOfNode.begin(); it != simulation->deck.netOfNode.end() && traces.size() < 3; ++it) {
                        const QString net = it.value();
                        // Labelled nets of this sheet; not ground, generated names or supplies.
                        static const QStringList supplies{QStringLiteral("vcc"), QStringLiteral("vee"), QStringLiteral("vdd"),
                                                          QStringLiteral("vss")};
                        if (!net.isEmpty() && it.key() != QLatin1String("0") && !net.startsWith(QLatin1String("Net-("))
                            && !net.contains(QLatin1Char('/')) && !supplies.contains(it.key()))
                            traces << QStringLiteral("v(%1)").arg(it.key());
                    }
                }
            }
            pane.traces = traces;
            panes = {pane};
        }
        m_viewer->setPanes(panes);
    } else {
        m_viewer->setData(std::shared_ptr<const spice::SpiceRunResult>(simulation, &simulation->run),
                          simulation->run.plots.isEmpty() ? -1 : 0);
    }
    fillValues(*simulation);
    m_instruments->addSimulation(simulation);

    if (simulation->analysis.kind == AnalysisKind::OperatingPoint) {
        m_labels = spiceui::operatingPointLabels(*m_project, *m_document, *simulation);
        publishLabels();
    }
    // The tab that shows something; an open Instruments tab stays.
    const bool keepInstruments = m_instruments->hasInstruments() && m_tabs->currentWidget() == m_instruments;
    if (keepInstruments)
        m_tabs->setCurrentWidget(m_instruments);
    else if (plotIndex >= 0)
        m_tabs->setCurrentWidget(m_viewer);
    else if (m_valuesModel->rowCount() > 0)
        m_tabs->setCurrentWidget(m_valuesTable);
    else if (!simulation->errors.isEmpty())
        m_tabs->setCurrentWidget(m_log);
}

void SpiceSimulationView::fillValues(const spice::SpiceSimulation &simulation)
{
    m_valuesModel->clear();
    const auto addRow = [&](const QString &name, const QString &value, const QString &extra = {}, double sortValue = NAN) {
        auto *nameItem = new QStandardItem(name);
        auto *valueItem = new QStandardItem(value);
        valueItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (std::isfinite(sortValue))
            valueItem->setData(sortValue, Qt::UserRole);
        QList<QStandardItem *> row{nameItem, valueItem};
        if (m_valuesModel->columnCount() > 2 || !extra.isEmpty())
            row << new QStandardItem(extra);
        m_valuesModel->appendRow(row);
    };
    const auto headers = [&](const QStringList &labels) { m_valuesModel->setHorizontalHeaderLabels(labels); };

    switch (simulation.analysis.kind) {
    case AnalysisKind::OperatingPoint: {
        const spice::SpicePlot *op = simulation.run.plot(QStringLiteral("op"));
        if (!op)
            break;
        headers({tr("Quantity"), tr("Value"), tr("Part or net")});
        // Node voltages with their net names, then currents and powers.
        for (const spice::SpiceVector &v : op->vectors) {
            if (v.size() == 0)
                continue;
            const double value = v.at(0).real();
            // Readable names as on the schematic: V(out), I(R1), Ic(Q1),
            // P(Q1); the SPICE vector name is the tooltip.
            QString unit = spice::vectorKindUnit(v.kind);
            QString where;
            QString label = v.name;
            if (v.name.startsWith(QLatin1String("v("))) {
                const QString node = v.name.mid(2).chopped(1);
                where = simulation.deck.netOfNode.value(node);
                label = QStringLiteral("V(%1)").arg(where.isEmpty() ? node : where);
                unit = QStringLiteral("V");
            } else if (v.name.startsWith(QLatin1String("i("))) {
                const QString element = v.name.mid(2).chopped(1);
                if (const spice::SpiceElement *e = simulation.deck.elementByName(element))
                    where = e->path;
                label = QStringLiteral("I(%1)").arg(where.isEmpty() ? element : where);
                unit = QStringLiteral("A");
            } else if (v.name.startsWith(QLatin1Char('@'))) {
                const QString element = v.name.mid(1).section(QLatin1Char('['), 0, 0);
                const QString quantity = v.name.section(QLatin1Char('['), 1).chopped(1);
                if (const spice::SpiceElement *e = simulation.deck.elementByName(element))
                    where = e->path;
                unit = quantity == QLatin1String("p") ? QStringLiteral("W") : QStringLiteral("A");
                QString prefix = quantity == QLatin1String("p") ? QStringLiteral("P") : QStringLiteral("I");
                if (quantity.size() == 2 && quantity.at(0) == QLatin1Char('i'))
                    prefix = QStringLiteral("I") + quantity.at(1); // ic, ib, ie
                else if (quantity == QLatin1String("id"))
                    prefix = QStringLiteral("Id");
                label = QStringLiteral("%1(%2)").arg(prefix, where.isEmpty() ? element : where);
            }
            addRow(label, formatValue(value, unit), where, value);
            m_valuesModel->item(m_valuesModel->rowCount() - 1, 0)->setToolTip(v.name);
        }
        break;
    }
    case AnalysisKind::TransferFunction: {
        headers({tr("Quantity"), tr("Value")});
        if (const spice::SpicePlot *tf = simulation.run.plot(QStringLiteral("tf"))) {
            const auto row = [&](const char *name, const QString &label, const QString &unit) {
                if (const spice::SpiceVector *v = tf->vector(QLatin1String(name)); v && v->size() > 0)
                    addRow(label, formatValue(v->at(0).real(), unit), {}, v->at(0).real());
            };
            row("transfer_function", tr("Transfer function"), QString());
            row("input_impedance", tr("Input impedance"), QStringLiteral("Ω"));
            row("output_impedance", tr("Output impedance"), QStringLiteral("Ω"));
        }
        break;
    }
    case AnalysisKind::PoleZero: {
        headers({tr("Root"), tr("s (rad/s)"), tr("Frequency")});
        if (const spice::SpicePlot *pz = simulation.run.plot(QStringLiteral("pz"))) {
            for (const spice::SpiceVector &v : pz->vectors) {
                for (std::size_t i = 0; i < v.size(); ++i) {
                    const std::complex<double> s = v.at(i);
                    const QString text = QStringLiteral("%1 %2 j%3")
                                             .arg(s.real(), 0, 'g', 6)
                                             .arg(s.imag() < 0 ? QLatin1Char('-') : QLatin1Char('+'))
                                             .arg(std::abs(s.imag()), 0, 'g', 6);
                    addRow(v.name, text, formatValue(std::abs(s) / (2 * std::numbers::pi), QStringLiteral("Hz")), std::abs(s));
                }
            }
        }
        break;
    }
    case AnalysisKind::Sensitivity: {
        headers({tr("Parameter"), tr("Sensitivity")});
        if (const spice::SpicePlot *sens = simulation.run.plot(QStringLiteral("sens"))) {
            for (const spice::SpiceVector &v : sens->vectors)
                if (v.size() == 1 && !v.isComplex)
                    addRow(v.name, QString::number(v.real.front(), 'g', 6), {}, v.real.front());
        }
        break;
    }
    case AnalysisKind::Noise: {
        headers({tr("Quantity"), tr("Value")});
        for (const spice::SpicePlot &plot : simulation.run.plots)
            if (plot.type == QLatin1String("noise") && !plot.scale())
                for (const spice::SpiceVector &v : plot.vectors)
                    if (v.size() > 0)
                        addRow(v.name, formatValue(v.at(0).real(), spice::vectorKindUnit(v.kind)), {}, v.at(0).real());
        break;
    }
    case AnalysisKind::Fourier: {
        headers({tr("Signal / harmonic"), tr("Magnitude"), tr("Frequency, phase")});
        for (const spice::FourierSignal &signal : simulation.fourier) {
            if (!signal.result) {
                addRow(signal.expression, signal.error);
                continue;
            }
            const spice::FourierResult &r = *signal.result;
            addRow(signal.expression, tr("THD %1 %").arg(r.thdPercent, 0, 'f', 4),
                   tr("fundamental %1, DC %2").arg(formatValue(r.fundamental, QStringLiteral("Hz")), formatValue(r.dc, QString())));
            for (const spice::Harmonic &h : r.harmonics)
                addRow(QStringLiteral("   %1").arg(h.n), formatValue(h.magnitude, QString()),
                       QStringLiteral("%1, %2°").arg(formatValue(h.frequency, QStringLiteral("Hz"))).arg(h.phaseDeg, 0, 'f', 2),
                       h.magnitude);
        }
        break;
    }
    default:
        break;
    }
    m_valuesTable->resizeColumnsToContents();
}

// ---------------------------------------------------------------- panes

QList<WaveformPaneDefinition> SpiceSimulationView::storedPanes(const QString &analysisId) const
{
    if (!m_schematic)
        return {};
    return WaveformViewer::panesFromJson(
        m_schematic->simulation().value(PanesKey).toObject().value(analysisId).toArray());
}

void SpiceSimulationView::storePanes(const QList<WaveformPaneDefinition> &panes)
{
    const std::shared_ptr<const spice::SpiceSimulation> shown = m_results.value(m_currentAnalysis);
    if (!m_schematic || !shown)
        return;
    QJsonObject simulation = m_schematic->simulation();
    QJsonObject all = simulation.value(PanesKey).toObject();
    all.insert(shown->analysis.id, WaveformViewer::panesToJson(panes));
    simulation.insert(PanesKey, all);
    m_schematic->setSimulation(simulation);
}

// ---------------------------------------------------------------- annotation

bool SpiceSimulationView::isAnnotating() const
{
    return m_annotateAction->isChecked();
}

void SpiceSimulationView::setAnnotating(bool annotate)
{
    m_annotateAction->setChecked(annotate);
}

void SpiceSimulationView::publishLabels()
{
    Q_EMIT operatingPointLabelsChanged(isAnnotating() ? m_labels : QList<OperatingPointLabel>{});
}

// ---------------------------------------------------------------- netlist

QString SpiceSimulationView::netlistText() const
{
    const auto setup = analysis(m_currentAnalysis);
    if (setup)
        return spice::prepareSpiceAnalysis(*m_project, *m_document, *setup).netlist;
    return spice::buildSpiceDeck(*m_project, *m_document).text;
}

void SpiceSimulationView::exportNetlist()
{
    const QString text = netlistText();
    m_netlist->setPlainText(text);
    m_tabs->setCurrentWidget(m_netlist);
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Netlist"),
                                                      QStringLiteral("%1/%2.cir").arg(m_project->directory(), m_document->name()),
                                                      tr("SPICE netlists (*.cir *.sp *.net);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(text.toUtf8()) < 0) {
        QMessageBox::warning(this, tr("Export Netlist"), file.errorString());
        return;
    }
    Q_EMIT statusMessage(tr("Netlist written to %1").arg(QDir::toNativeSeparators(path)));
}

// ---------------------------------------------------------------- batches

void SpiceSimulationView::startBatch(
    std::function<std::shared_ptr<spice::BatchResult>(Project &snapshot, const spice::BatchOptions &)> work, const QString &title)
{
    if (isRunning())
        return;
    std::shared_ptr<Project> snapshot(m_project->snapshot());
    m_batchCancel = std::make_shared<std::atomic_bool>(false);
    m_batchRunning = true;
    updateActions();
    m_status->setText(tr("%1…").arg(title));
    m_progress->setRange(0, 0);
    m_progress->show();
    const int workers = m_batchWorkers;
    auto cancel = m_batchCancel;
    QPointer<SpiceSimulationView> self(this);
    std::thread([self, snapshot, work, workers, cancel, title] {
        spice::BatchOptions options;
        options.cancel = cancel.get();
        std::shared_ptr<spice::SpiceProcessPool> pool;
        if (workers != 1) {
            pool = std::make_shared<spice::SpiceProcessPool>(workers);
            if (QFileInfo::exists(pool->program()))
                options.execute = [pool](QList<spice::SpiceSimulation *> &simulations, const std::atomic_bool *c,
                                         const std::function<void(int)> &progress) { pool->execute(simulations, c, progress); };
        }
        options.progress = [self](const spice::BatchProgress &p) {
            QMetaObject::invokeMethod(qApp, [self, p] {
                if (!self)
                    return;
                self->m_progress->setRange(0, std::max(1, p.total));
                self->m_progress->setValue(p.done);
                self->m_status->setText(p.message);
            });
        };
        std::shared_ptr<spice::BatchResult> result = work(*snapshot, options);
        QMetaObject::invokeMethod(qApp, [self, result, title] {
            if (!self)
                return;
            self->m_batchRunning = false;
            self->m_progress->hide();
            self->m_batch = result;
            self->m_batchView->setResult(result);
            self->m_tabs->setCurrentWidget(self->m_batchView);
            self->showMessages(result->errors, result->warnings);
            self->m_status->setText(result->cancelled ? tr("%1 stopped.").arg(title)
                                    : result->ok()    ? tr("%1: %n run(s).", nullptr, int(result->runs.size())).arg(title)
                                                      : tr("%1 failed.").arg(title));
            self->updateActions();
            Q_EMIT self->runFinished(result->ok());
        });
    }).detach();
}

void SpiceSimulationView::runSweep(const QList<spice::SweepAxis> &axes, const QList<spice::SpiceMeasure> &measures)
{
    const auto setup = analysis(m_currentAnalysis);
    if (!setup)
        return;
    const QString document = m_document->name();
    startBatch(
        [setup = *setup, axes, measures, document](Project &snapshot, const spice::BatchOptions &options) {
            return std::make_shared<spice::BatchResult>(spice::runParameterSweep(snapshot, document, setup, axes, measures, options));
        },
        tr("Parameter sweep"));
}

void SpiceSimulationView::runMonteCarlo(const spice::MonteCarloSetup &monteCarlo)
{
    const auto setup = analysis(m_currentAnalysis);
    if (!setup)
        return;
    const QString document = m_document->name();
    m_batchView->setMode(BatchView::Mode::MonteCarlo);
    startBatch(
        [setup = *setup, monteCarlo, document](Project &snapshot, const spice::BatchOptions &options) {
            return std::make_shared<spice::BatchResult>(spice::runMonteCarlo(snapshot, document, setup, monteCarlo, options));
        },
        tr("Monte Carlo"));
}

void SpiceSimulationView::runWorstCase(const QList<spice::Tolerance> &tolerances, const QList<spice::SpiceMeasure> &measures)
{
    const auto setup = analysis(m_currentAnalysis);
    if (!setup)
        return;
    const QString document = m_document->name();
    auto summary = std::make_shared<QList<spice::WorstCaseMeasure>>();
    m_batchView->setMode(BatchView::Mode::WorstCase);
    startBatch(
        [setup = *setup, tolerances, measures, document, summary, view = QPointer<BatchView>(m_batchView)](Project &snapshot, const spice::BatchOptions &options) {
            auto result = std::make_shared<spice::BatchResult>(
                spice::runWorstCase(snapshot, document, setup, tolerances, measures, summary.get(), options));
            QMetaObject::invokeMethod(qApp, [view, summary] {
                if (view)
                    view->setWorstCase(*summary);
            });
            return result;
        },
        tr("Worst case"));
}

void SpiceSimulationView::runOptimization(const spice::SpiceOptimizationSetup &optimization)
{
    const auto setup = analysis(m_currentAnalysis);
    if (!setup)
        return;
    const QString document = m_document->name();
    m_batchView->setMode(BatchView::Mode::Optimization);
    QPointer<SpiceSimulationView> self(this);
    startBatch(
        [setup = *setup, optimization, document, self](Project &snapshot, const spice::BatchOptions &options) {
            const spice::SpiceOptimizationResult optimized = spice::optimizeSpice(snapshot, document, setup, optimization, options);
            auto result = std::make_shared<spice::BatchResult>();
            result->errors = optimized.errors;
            result->warnings = optimized.warnings;
            result->cancelled = optimized.cancelled;
            for (const spice::SpiceOptimizationVariable &v : optimization.variables)
                result->targets << v.target;
            for (const spice::SpiceGoal &g : optimization.goals)
                result->measures << g.measure;
            if (optimized.ok()) {
                const auto addRun = [&](const QString &label, const std::vector<double> &values, const std::vector<double> &measures) {
                    spice::BatchRun run;
                    run.label = label;
                    run.values = values;
                    run.measures = measures;
                    result->runs << run;
                };
                addRun(tr("initial"), optimized.initial, {});
                addRun(tr("optimum"), optimized.best, {});
                addRun(tr("standard values"), optimized.snapped, optimized.bestMeasures);
            }
            QMetaObject::invokeMethod(qApp, [self, optimized, optimization] {
                if (self)
                    self->m_batchView->setOptimization(optimization, optimized);
            });
            return result;
        },
        tr("Optimisation"));
}

void SpiceSimulationView::applyValues(const QList<spice::SpiceTarget> &targets, const std::vector<double> &values)
{
    if (!m_schematic)
        return;
    Schematic::Edit edit(m_schematic, tr("Apply optimised values"));
    QList<Variable> variables = m_schematic->variables();
    bool variablesChanged = false;
    for (qsizetype i = 0; i < targets.size() && std::size_t(i) < values.size(); ++i) {
        const QString text = QString::number(values[std::size_t(i)], 'g', 6);
        if (targets.at(i).isParameter()) {
            if (const PlacedComponent *c = m_schematic->componentByReference(targets.at(i).component()))
                m_schematic->setParameter(c->id, targets.at(i).parameter(), text);
        } else if (!targets.at(i).isTemperature()) {
            for (Variable &v : variables)
                if (v.name == targets.at(i).text) {
                    v.value = text;
                    variablesChanged = true;
                }
        }
    }
    if (variablesChanged)
        m_schematic->setVariables(variables);
    Q_EMIT statusMessage(tr("Optimised values written to the schematic."));
}

void SpiceSimulationView::openBatchDialog(int mode)
{
    const auto setup = analysis(m_currentAnalysis);
    if (!setup)
        return;
    BatchDialog dialog(BatchDialog::Mode(mode), *m_project, *m_document, *setup, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    // The setup is stored with the analysis so it is there next time.
    spice::SpiceAnalysisSetup stored = *setup;
    stored.set(dialog.storageKey(), dialog.toJson());
    addAnalysis(stored);
    switch (BatchDialog::Mode(mode)) {
    case BatchDialog::Mode::Sweep:
        m_batchView->setMode(BatchView::Mode::Sweep);
        runSweep(dialog.sweepAxes(), dialog.measures());
        break;
    case BatchDialog::Mode::MonteCarlo:
        runMonteCarlo(dialog.monteCarlo());
        break;
    case BatchDialog::Mode::WorstCase:
        runWorstCase(dialog.monteCarlo().tolerances, dialog.measures());
        break;
    case BatchDialog::Mode::Optimize:
        m_batchView->setApplyHandler([this, variables = dialog.optimization().variables](const std::vector<double> &values) {
            QList<spice::SpiceTarget> targets;
            for (const spice::SpiceOptimizationVariable &v : variables)
                targets << v.target;
            applyValues(targets, values);
        });
        runOptimization(dialog.optimization());
        break;
    }
}

} // namespace lvd::spiceui
