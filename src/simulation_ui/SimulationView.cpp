#include "simulation_ui/SimulationView.h"

#include "core/Document.h"
#include "core/Expression.h"
#include "core/Netlist.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "plot/PlotWidget.h"
#include "rfsim/Analysis.h"
#include "rfsim/Budget.h"
#include "simulation_ui/AnalysisDialog.h"
#include "simulation_ui/BudgetDialog.h"
#include "simulation_ui/BudgetModels.h"
#include "simulation_ui/BudgetRunner.h"
#include "simulation_ui/BudgetView.h"
#include "simulation_ui/DesignVariables.h"
#include "simulation_ui/OptimizationDialog.h"
#include "simulation_ui/PlotDefinitions.h"
#include "simulation_ui/PlotPane.h"
#include "simulation_ui/ResultsTable.h"
#include "simulation_ui/SimulationRunner.h"
#include "simulation_ui/TuningPanel.h"
#include "simulation_ui/TuningRunner.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QSaveFile>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringListModel>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <set>

namespace lvd {

namespace {

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

enum class Glyph { Run, Stop, AutoRun, Plot, Export, New, Edit, Delete, Tune, Optimize };

// Small toolbar icons drawn for the current palette.
QIcon glyphIcon(Glyph glyph, const QPalette &palette)
{
    const bool dark = isDark(palette);
    const QColor text = palette.color(QPalette::WindowText);
    const QColor green = dark ? QColor(0x5F, 0xC8, 0x7A) : QColor(0x1E, 0x8E, 0x3E);
    const QColor red = dark ? QColor(0xEE, 0x77, 0x77) : QColor(0xC6, 0x2E, 0x2E);
    QIcon icon;
    for (int size : {16, 24, 32}) {
        for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled}) {
            QPixmap pixmap(size, size);
            pixmap.fill(Qt::transparent);
            QPainter p(&pixmap);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(size / 16.0, size / 16.0);
            const bool disabled = mode == QIcon::Disabled;
            const auto tint = [&](QColor c) {
                if (disabled)
                    c.setAlphaF(0.35f);
                return c;
            };
            QPen pen(tint(text), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            switch (glyph) {
            case Glyph::Run: {
                QPainterPath path;
                path.moveTo(4, 2.5);
                path.lineTo(13, 8);
                path.lineTo(4, 13.5);
                path.closeSubpath();
                p.fillPath(path, tint(green));
                break;
            }
            case Glyph::Stop:
                p.setPen(Qt::NoPen);
                p.setBrush(tint(red));
                p.drawRoundedRect(QRectF(3.5, 3.5, 9, 9), 1.5, 1.5);
                break;
            case Glyph::AutoRun: {
                p.setPen(QPen(tint(green), 1.6, Qt::SolidLine, Qt::RoundCap));
                p.drawArc(QRectF(2.5, 2.5, 11, 11), 60 * 16, 270 * 16);
                QPainterPath head;
                head.moveTo(10.5, 1.2);
                head.lineTo(13.8, 3.6);
                head.lineTo(10.0, 5.6);
                head.closeSubpath();
                p.fillPath(head, tint(green));
                break;
            }
            case Glyph::Plot: {
                p.setPen(pen);
                p.drawLine(QPointF(2.5, 2.5), QPointF(2.5, 13.5));
                p.drawLine(QPointF(2.5, 13.5), QPointF(13.5, 13.5));
                QPainterPath curve;
                curve.moveTo(3.5, 11);
                curve.cubicTo(6, 2, 9, 2, 13, 9);
                p.setPen(QPen(tint(green), 1.5));
                p.drawPath(curve);
                break;
            }
            case Glyph::Export:
                p.setPen(pen);
                p.drawLine(QPointF(8, 2), QPointF(8, 10));
                p.drawLine(QPointF(5, 7), QPointF(8, 10));
                p.drawLine(QPointF(11, 7), QPointF(8, 10));
                p.drawLine(QPointF(3, 11), QPointF(3, 14));
                p.drawLine(QPointF(3, 14), QPointF(13, 14));
                p.drawLine(QPointF(13, 14), QPointF(13, 11));
                break;
            case Glyph::New:
                p.setPen(QPen(tint(text), 1.6, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(8, 3), QPointF(8, 13));
                p.drawLine(QPointF(3, 8), QPointF(13, 8));
                break;
            case Glyph::Edit: {
                p.setPen(pen);
                QPainterPath pencil;
                pencil.moveTo(3, 13);
                pencil.lineTo(4, 10);
                pencil.lineTo(11, 3);
                pencil.lineTo(13, 5);
                pencil.lineTo(6, 12);
                pencil.closeSubpath();
                p.drawPath(pencil);
                break;
            }
            case Glyph::Delete:
                p.setPen(QPen(tint(red), 1.6, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(4, 4), QPointF(12, 12));
                p.drawLine(QPointF(12, 4), QPointF(4, 12));
                break;
            case Glyph::Tune: {
                // Three slider tracks with knobs.
                p.setPen(pen);
                for (int i = 0; i < 3; ++i)
                    p.drawLine(QPointF(2.5, 4 + 4 * i), QPointF(13.5, 4 + 4 * i));
                p.setPen(Qt::NoPen);
                p.setBrush(tint(green));
                const double knobs[] = {10, 5, 8.5};
                for (int i = 0; i < 3; ++i)
                    p.drawEllipse(QPointF(knobs[i], 4 + 4 * i), 2, 2);
                break;
            }
            case Glyph::Optimize:
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(QPointF(8, 8), 5.5, 5.5);
                p.drawEllipse(QPointF(8, 8), 2.8, 2.8);
                p.setPen(Qt::NoPen);
                p.setBrush(tint(green));
                p.drawEllipse(QPointF(8, 8), 1.3, 1.3);
                break;
            }
            p.end();
            icon.addPixmap(pixmap, mode);
        }
    }
    return icon;
}

QString elapsedText(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    return QStringLiteral("%1 s").arg(QString::number(ms / 1000.0, 'f', 1));
}

const QString LinearType = QStringLiteral("rf-linear");
const QString BudgetType = QStringLiteral("rf-budget");

// Errors of a budget run and its parts, without repeats.
QStringList budgetErrors(const rf::BudgetRun &run)
{
    QStringList errors = run.errors;
    for (const QString &error : run.chain.errors + run.budget.errors) {
        if (!errors.contains(error))
            errors.append(error);
    }
    return errors;
}

QStringList withoutRepeats(const QStringList &list)
{
    QStringList result;
    for (const QString &item : list) {
        if (!result.contains(item))
            result.append(item);
    }
    return result;
}

} // namespace

SimulationView::SimulationView(Project *project, Document *document, QWidget *parent)
    : QWidget(parent), m_project(project), m_document(document),
      m_schematic(document ? document->schematic() : nullptr)
{
    setObjectName(QStringLiteral("simulationView"));
    m_runner = new sim::SimulationRunner(this);
    connect(m_runner, &sim::SimulationRunner::finished, this, &SimulationView::onRunFinished);
    m_budgetRunner = new sim::BudgetRunner(this);
    connect(m_budgetRunner, &sim::BudgetRunner::finished, this, &SimulationView::onBudgetFinished);

    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(100);
    connect(m_elapsedTimer, &QTimer::timeout, this, &SimulationView::updateStatus);
    m_autoRunTimer = new QTimer(this);
    m_autoRunTimer->setSingleShot(true);
    m_autoRunTimer->setInterval(300);
    connect(m_autoRunTimer, &QTimer::timeout, this, &SimulationView::onAutoRunTimer);

    createActions();
    createToolBar();

    // Errors are shown prominently above the plots.
    m_errorBanner = new QFrame(this);
    m_errorBanner->setObjectName(QStringLiteral("errorBanner"));
    m_errorBanner->setFrameShape(QFrame::StyledPanel);
    m_errorTitle = new QLabel(m_errorBanner);
    m_errorTitle->setObjectName(QStringLiteral("errorTitle"));
    QFont bold = m_errorTitle->font();
    bold.setBold(true);
    m_errorTitle->setFont(bold);
    m_errorText = new QLabel(m_errorBanner);
    m_errorText->setObjectName(QStringLiteral("errorText"));
    m_errorText->setWordWrap(true);
    m_errorText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *bannerLayout = new QVBoxLayout(m_errorBanner);
    bannerLayout->setContentsMargins(10, 6, 10, 6);
    bannerLayout->setSpacing(2);
    bannerLayout->addWidget(m_errorTitle);
    bannerLayout->addWidget(m_errorText);
    m_errorBanner->hide();

    // Plots: tiled in a grid, or as tabs.
    m_plotHost = new QWidget(this);
    m_tabs = new QTabWidget(m_plotHost);
    m_tabs->setObjectName(QStringLiteral("plotTabs"));
    m_tabs->setDocumentMode(true);
    m_gridHost = new QWidget(m_plotHost);
    m_grid = new QGridLayout(m_gridHost);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(4);
    m_placeholder = new QLabel(m_plotHost);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    auto *hostLayout = new QVBoxLayout(m_plotHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->addWidget(m_tabs);
    hostLayout->addWidget(m_gridHost);
    hostLayout->addWidget(m_placeholder);

    auto *tablePanel = new QWidget(this);
    createTable(tablePanel);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(true);
    m_splitter->addWidget(m_plotHost);
    m_splitter->addWidget(tablePanel);
    m_splitter->setStretchFactor(0, 4);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({600, 150});

    // Budget analyses show their own view instead of the plots and the table.
    m_budgetView = new sim::BudgetView(this);
    connect(m_budgetView, &sim::BudgetView::statusMessage, this, &SimulationView::statusMessage);
    m_resultStack = new QStackedWidget(this);
    m_resultStack->setObjectName(QStringLiteral("resultStack"));
    m_resultStack->addWidget(m_splitter);
    m_resultStack->addWidget(m_budgetView);

    // Tuning: the panel to the right of the results, a worker for the
    // snapshot simulations.
    m_tuningRunner = new sim::TuningRunner(this);
    connect(m_tuningRunner, &sim::TuningRunner::evaluated, this, &SimulationView::onTuningEvaluated);
    m_tuningPanel = new sim::TuningPanel(this);
    m_tuningPanel->setContext(m_project, m_document);
    m_tuningPanel->hide();
    connect(m_tuningPanel, &sim::TuningPanel::valuesChanged, this, &SimulationView::requestTuning);
    connect(m_tuningPanel, &sim::TuningPanel::variablesChanged, this, &SimulationView::requestTuning);
    connect(m_tuningPanel, &sim::TuningPanel::applyRequested, this, [this] {
        QString error;
        if (!applyTuning(&error))
            QMessageBox::warning(this, tr("Apply Tuned Values"), error);
    });
    connect(m_tuningPanel, &sim::TuningPanel::resetRequested, this, &SimulationView::resetTuning);
    connect(m_tuningPanel, &sim::TuningPanel::closeRequested, this, &SimulationView::stopTuning);
    m_tuningRefreshTimer = new QTimer(this);
    m_tuningRefreshTimer->setSingleShot(true);
    m_tuningRefreshTimer->setInterval(150);
    connect(m_tuningRefreshTimer, &QTimer::timeout, this, &SimulationView::refreshTuningSnapshot);
    m_tuningClock.start();

    m_mainSplitter = new QSplitter(Qt::Horizontal, this);
    m_mainSplitter->setObjectName(QStringLiteral("tuningSplitter"));
    m_mainSplitter->setChildrenCollapsible(false);
    m_mainSplitter->addWidget(m_resultStack);
    m_mainSplitter->addWidget(m_tuningPanel);
    m_mainSplitter->setStretchFactor(0, 1);
    m_mainSplitter->setStretchFactor(1, 0);

    // Warnings, collapsed below the plots.
    m_warningList = new QListWidget(this);
    m_warningList->setObjectName(QStringLiteral("warnings"));
    m_warningList->setMaximumHeight(110);
    m_warningList->setWordWrap(true);
    m_warningList->hide();

    // Status row: busy indicator, elapsed time, warnings toggle.
    m_busy = new QProgressBar(this);
    m_busy->setObjectName(QStringLiteral("busy"));
    m_busy->setRange(0, 0);
    m_busy->setMaximumWidth(120);
    m_busy->setMaximumHeight(14);
    m_busy->setTextVisible(false);
    m_busy->hide();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("status"));
    m_warningButton = new QToolButton(this);
    m_warningButton->setObjectName(QStringLiteral("warningsToggle"));
    m_warningButton->setCheckable(true);
    m_warningButton->setAutoRaise(true);
    m_warningButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_warningButton->setArrowType(Qt::RightArrow);
    m_warningButton->hide();
    connect(m_warningButton, &QToolButton::toggled, this, [this](bool on) {
        m_warningList->setVisible(on);
        m_warningButton->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    auto *status = new QHBoxLayout;
    status->setContentsMargins(6, 0, 6, 2);
    status->addWidget(m_busy);
    status->addWidget(m_statusLabel, 1);
    status->addWidget(m_warningButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_toolBar);
    layout->addWidget(m_errorBanner);
    layout->addWidget(m_mainSplitter, 1);
    layout->addWidget(m_warningList);
    layout->addLayout(status);

    if (m_schematic) {
        connect(m_schematic, &Schematic::simulationChanged, this, &SimulationView::onSimulationChanged);
    }
    connectDocuments();
    if (m_project) {
        connect(m_project, &Project::documentAdded, this, &SimulationView::watchDocument);
        if (m_project && m_document && m_document->schematic())
            m_ports = sim::netlistPortNumbers(buildNetlist(*m_project, *m_document));
    }

    refreshAnalyses();
    relayoutPlots();
    m_lastFingerprint = fingerprint();
    updateIcons();
    showMessages({}, {});
    updateActions();
    updateStatus();
}

SimulationView::~SimulationView()
{
    // The runner cancels and joins its worker in its destructor; disconnect
    // first so that nothing is delivered into a half-destroyed view.
    disconnect(m_runner, nullptr, this, nullptr);
    disconnect(m_budgetRunner, nullptr, this, nullptr);
    disconnect(m_tuningRunner, nullptr, this, nullptr);
    disconnect(m_tuningPanel, nullptr, this, nullptr);
    if (m_optimizationDialog)
        disconnect(m_optimizationDialog, nullptr, this, nullptr);
    m_runner->cancel();
    m_budgetRunner->cancel();
}

// ---------------------------------------------------------------- set-up

void SimulationView::createActions()
{
    const auto action = [this](const QString &text, const QString &tip, const QString &name) {
        auto *a = new QAction(text, this);
        a->setToolTip(tip);
        a->setStatusTip(tip);
        a->setObjectName(name);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(a);
        return a;
    };
    m_newAnalysisAction = action(tr("New Analysis…"), tr("Create a new analysis: linear (S-parameters) or RF budget"),
                                 QStringLiteral("newAnalysis"));
    m_newLinearAction = action(tr("Linear Analysis (S-parameters, noise)…"),
                               tr("Create a new linear analysis: S-parameters and noise over a frequency sweep"),
                               QStringLiteral("newLinearAnalysis"));
    m_newBudgetAction = action(tr("RF Budget Analysis…"),
                               tr("Create a new RF budget analysis: cascade along the signal path, level diagram, spurs"),
                               QStringLiteral("newBudgetAnalysis"));
    m_editAnalysisAction = action(tr("Edit…"), tr("Edit the sweep and options of the analysis"), QStringLiteral("editAnalysis"));
    m_deleteAnalysisAction = action(tr("Delete"), tr("Delete the analysis and its plots"), QStringLiteral("deleteAnalysis"));
    m_runAction = action(tr("Run"), tr("Run the analysis (F5)"), QStringLiteral("run"));
    m_runAction->setShortcut(QKeySequence(Qt::Key_F5));
    m_stopAction = action(tr("Stop"), tr("Stop the running simulation (Shift+F5)"), QStringLiteral("stop"));
    m_stopAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    m_autoRunAction = action(tr("Auto-run"), tr("Run again after every change of the circuit"), QStringLiteral("autoRun"));
    m_autoRunAction->setCheckable(true);
    m_addPlotAction = action(tr("Add Plot"), tr("Add a plot to the analysis"), QStringLiteral("addPlot"));
    m_tileAction = action(tr("Tile Plots"), tr("Show all plots side by side instead of as tabs"), QStringLiteral("tilePlots"));
    m_tileAction->setCheckable(true);
    m_tileAction->setChecked(m_tiled);
    m_exportAction = action(tr("Export"), tr("Export the results"), QStringLiteral("export"));
    m_exportTouchstoneAction = action(tr("Touchstone File…"), tr("Save the S-parameters as a Touchstone file"),
                                      QStringLiteral("exportTouchstone"));
    m_exportCsvAction = action(tr("Table as CSV…"), tr("Save the results table as CSV"), QStringLiteral("exportCsv"));
    m_copyTableAction = action(tr("Copy Table"), tr("Copy the selected rows of the results table, or all rows"),
                               QStringLiteral("copyTable"));
    m_tuneAction = action(tr("Tune"), tr("Tune values with sliders; the plots follow live, the schematic changes when you apply"),
                          QStringLiteral("tune"));
    m_tuneAction->setCheckable(true);
    m_optimizeAction = action(tr("Optimize…"), tr("Optimise values of the circuit toward goals on the results"),
                              QStringLiteral("optimize"));

    auto *newMenu = new QMenu(this);
    newMenu->addAction(m_newLinearAction);
    newMenu->addAction(m_newBudgetAction);
    m_newAnalysisAction->setMenu(newMenu);

    auto *plotMenu = new QMenu(this);
    plotMenu->addAction(tr("Rectangular Plot"), this, [this] { addPlot(QStringLiteral("rectangular")); });
    plotMenu->addAction(tr("Smith Chart"), this, [this] { addPlot(QStringLiteral("smith")); });
    plotMenu->addAction(tr("Polar Plot"), this, [this] { addPlot(QStringLiteral("polar")); });
    plotMenu->addSeparator();
    plotMenu->addAction(tr("Default Plots"), this, &SimulationView::addDefaultPlots);
    m_addPlotAction->setMenu(plotMenu);
    auto *exportMenu = new QMenu(this);
    exportMenu->addAction(m_exportTouchstoneAction);
    exportMenu->addAction(m_exportCsvAction);
    exportMenu->addSeparator();
    exportMenu->addAction(m_copyTableAction);
    m_exportAction->setMenu(exportMenu);

    connect(m_newAnalysisAction, &QAction::triggered, this, [this] { newAnalysis({}); });
    connect(m_newLinearAction, &QAction::triggered, this, [this] { newAnalysis(LinearType); });
    connect(m_newBudgetAction, &QAction::triggered, this, [this] { newAnalysis(BudgetType); });
    connect(m_editAnalysisAction, &QAction::triggered, this, [this] { editAnalysis(m_currentAnalysis); });
    connect(m_deleteAnalysisAction, &QAction::triggered, this, [this] {
        if (m_currentAnalysis.isEmpty())
            return;
        const QString name = m_analysisCombo->currentText();
        if (QMessageBox::question(this, tr("Delete Analysis"), tr("Delete the analysis “%1” and its plots?").arg(name))
            == QMessageBox::Yes)
            deleteAnalysis(m_currentAnalysis);
    });
    connect(m_runAction, &QAction::triggered, this, &SimulationView::run);
    connect(m_stopAction, &QAction::triggered, this, &SimulationView::cancel);
    connect(m_autoRunAction, &QAction::toggled, this, &SimulationView::setAutoRun);
    connect(m_addPlotAction, &QAction::triggered, this, [this] { addPlot(QStringLiteral("rectangular")); });
    connect(m_tileAction, &QAction::toggled, this, &SimulationView::setTiled);
    connect(m_exportTouchstoneAction, &QAction::triggered, this, &SimulationView::exportTouchstoneInteractive);
    connect(m_exportCsvAction, &QAction::triggered, this, &SimulationView::exportCsvInteractive);
    connect(m_copyTableAction, &QAction::triggered, this, &SimulationView::copyTable);
    connect(m_tuneAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            startTuning();
        else
            stopTuning();
    });
    connect(m_optimizeAction, &QAction::triggered, this, [this] { openOptimization(); });
}

void SimulationView::createToolBar()
{
    m_toolBar = new QToolBar(this);
    m_toolBar->setObjectName(QStringLiteral("simulationToolBar"));
    m_toolBar->setIconSize(QSize(16, 16));
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto *label = new QLabel(tr("Analysis:"), m_toolBar);
    label->setContentsMargins(4, 0, 4, 0);
    m_toolBar->addWidget(label);
    m_analysisCombo = new QComboBox(m_toolBar);
    m_analysisCombo->setObjectName(QStringLiteral("analysis"));
    m_analysisCombo->setMinimumContentsLength(24); // "Receiver budget (budget)" fits
    m_analysisCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_toolBar->addWidget(m_analysisCombo);
    connect(m_analysisCombo, &QComboBox::activated, this,
            [this](int index) { setCurrentAnalysis(m_analysisCombo->itemData(index).toString()); });
    m_toolBar->addAction(m_newAnalysisAction);
    if (auto *button = qobject_cast<QToolButton *>(m_toolBar->widgetForAction(m_newAnalysisAction)))
        button->setPopupMode(QToolButton::InstantPopup);
    m_toolBar->addAction(m_editAnalysisAction);
    m_toolBar->addAction(m_deleteAnalysisAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_runAction);
    m_toolBar->addAction(m_stopAction);
    m_toolBar->addAction(m_autoRunAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_tuneAction);
    m_toolBar->addAction(m_optimizeAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_addPlotAction);
    if (auto *button = qobject_cast<QToolButton *>(m_toolBar->widgetForAction(m_addPlotAction)))
        button->setPopupMode(QToolButton::MenuButtonPopup);
    m_toolBar->addAction(m_tileAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_exportAction);
    if (auto *button = qobject_cast<QToolButton *>(m_toolBar->widgetForAction(m_exportAction)))
        button->setPopupMode(QToolButton::InstantPopup);
}

void SimulationView::createTable(QWidget *panel)
{
    m_tableModel = new sim::ResultsTableModel(this);
    m_table = new QTableView(panel);
    m_table->setObjectName(QStringLiteral("resultsTable"));
    m_table->setModel(m_tableModel);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setDefaultSectionSize(120);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->addAction(m_copyTableAction);
    m_copyTableAction->setShortcut(QKeySequence::Copy);

    auto *title = new QLabel(tr("Table"), panel);
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);

    m_columnEdit = new QLineEdit(panel);
    m_columnEdit->setObjectName(QStringLiteral("tableColumnExpression"));
    m_columnEdit->setPlaceholderText(tr("Add a column: dB(S21), VSWR(1), K..."));
    m_columnEdit->setClearButtonEnabled(true);
    m_completionModel = new QStringListModel(this);
    auto *completer = new QCompleter(m_completionModel, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    m_columnEdit->setCompleter(completer);
    connect(m_columnEdit, &QLineEdit::returnPressed, this, [this] {
        const QString expression = m_columnEdit->text().trimmed();
        if (expression.isEmpty())
            return;
        QStringList columns = tableColumns();
        if (!columns.contains(expression))
            columns.append(expression);
        setTableColumns(columns);
        m_columnEdit->clear();
    });

    const auto button = [panel](const QString &text, const QString &tip) {
        auto *b = new QToolButton(panel);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    auto *remove = button(tr("Remove Column"), tr("Remove the column of the current cell"));
    auto *reset = button(tr("Reset"), tr("Show the traces of the plots"));
    auto *copy = button(tr("Copy"), tr("Copy the selected rows, or all rows, to the clipboard"));
    connect(remove, &QToolButton::clicked, this, [this] {
        const int column = m_table->currentIndex().column();
        QStringList columns = tableColumns();
        if (column >= 1 && column - 1 < columns.size()) {
            columns.removeAt(column - 1);
            setTableColumns(columns.isEmpty() ? QStringList{QString()} : columns);
        }
    });
    connect(reset, &QToolButton::clicked, this, [this] { setTableColumns({}); });
    connect(copy, &QToolButton::clicked, this, &SimulationView::copyTable);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(6, 2, 6, 0);
    row->addWidget(title);
    row->addSpacing(8);
    row->addWidget(m_columnEdit, 1);
    row->addWidget(remove);
    row->addWidget(reset);
    row->addWidget(copy);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addLayout(row);
    layout->addWidget(m_table, 1);
}

void SimulationView::connectDocuments()
{
    if (m_project) {
        for (Document *document : m_project->documents())
            watchDocument(document);
    } else if (m_document) {
        watchDocument(m_document);
    }
}

void SimulationView::watchDocument(Document *document)
{
    // Every schematic of the project can be a sheet of this one.
    if (document && document->schematic())
        connect(document->schematic(), &Schematic::changed, this, &SimulationView::onCircuitEdited, Qt::UniqueConnection);
}

// ---------------------------------------------------------------- running

void SimulationView::run()
{
    if (!m_schematic || !m_document) {
        finishWithError(tr("This document is not a schematic."));
        return;
    }
    if (m_currentAnalysis.isEmpty()) {
        Q_EMIT statusMessage(tr("There is no analysis to run; create one first."));
        return;
    }
    if (m_tuning) {
        // While tuning, the plots show the tuned copy: simulate that.
        requestTuning();
        return;
    }
    if (isRunning()) {
        // The running simulation is outdated: stop it and start again once
        // it has stopped, with the circuit as it is then.
        m_pendingRun = true;
        m_runner->cancel();
        m_budgetRunner->cancel();
        updateStatus();
        return;
    }
    m_autoRunTimer->stop();
    m_pendingRun = false;

    if (isBudget(m_currentAnalysis)) {
        rf::BudgetAnalysisSetup setup;
        bool found = false;
        for (const rf::BudgetAnalysisSetup &analysis : rf::budgetAnalyses(m_schematic->simulation())) {
            if (analysis.id == m_currentAnalysis) {
                setup = analysis;
                found = true;
            }
        }
        if (!found) {
            finishWithError(tr("The analysis no longer exists."));
            return;
        }
        // Netlist and scope read the documents: GUI thread.
        m_lastFingerprint = fingerprint();
        m_outdated = false;
        QStringList variableErrors;
        Expression::Lookup scope = rf::sheetScope(*m_document, &variableErrors);
        Netlist netlist = m_project ? buildNetlist(*m_project, *m_document) : Netlist{};
        if (!m_project)
            netlist.errors.append(tr("The document does not belong to a project."));
        m_runningAnalysis = setup.id;
        ++m_runTag;
        m_budgetRunner->start(std::move(netlist), setup, std::move(scope), m_project ? m_project->directory() : QString(),
                              m_runTag);
        m_elapsed.start();
        m_elapsedTimer->start();
        Q_EMIT statusMessage(tr("Computing the budget %1…").arg(setup.name));
        updateActions();
        updateStatus();
        return;
    }

    rf::LinearAnalysisSetup setup;
    bool found = false;
    for (const rf::LinearAnalysisSetup &analysis : rf::linearAnalyses(m_schematic->simulation())) {
        if (analysis.id == m_currentAnalysis) {
            setup = analysis;
            found = true;
        }
    }
    if (!found) {
        finishWithError(tr("The analysis no longer exists."));
        return;
    }

    // Netlist and sweep read the documents: GUI thread.
    m_lastFingerprint = fingerprint();
    m_outdated = false;
    QString sweepError;
    QStringList variableErrors;
    const auto frequencies = rf::sweepFrequencies(setup, rf::sheetScope(*m_document, &variableErrors), &sweepError);
    if (!frequencies) {
        QString message = tr("%1: %2").arg(setup.name, sweepError);
        if (!variableErrors.isEmpty())
            message += QLatin1Char('\n') + variableErrors.join(QLatin1Char('\n'));
        m_runningAnalysis = setup.id;
        finishWithError(message);
        return;
    }
    Netlist netlist = m_project ? buildNetlist(*m_project, *m_document) : Netlist{};
    if (!m_project)
        netlist.errors.append(tr("The document does not belong to a project."));
    m_ports = sim::netlistPortNumbers(netlist);
    m_noise = setup.noise;
    refreshCompletions();

    rf::LinearOptions options;
    options.noise = setup.noise;
    options.projectDirectory = m_project ? m_project->directory() : QString();
    m_runningAnalysis = setup.id;
    ++m_runTag;
    m_runner->start(std::move(netlist), *frequencies, options, m_runTag);
    m_elapsed.start();
    m_elapsedTimer->start();
    Q_EMIT statusMessage(tr("Simulating %1 (%2 points)…").arg(setup.name).arg(frequencies->size()));
    updateActions();
    updateStatus();
}

void SimulationView::cancel()
{
    m_pendingRun = false;
    m_autoRunTimer->stop();
    m_runner->cancel();
    m_budgetRunner->cancel();
    updateStatus();
}

bool SimulationView::isRunning() const
{
    return m_runner->isRunning() || m_budgetRunner->isRunning();
}

const rf::LinearResult *SimulationView::lastResult() const
{
    return m_results.value(m_currentAnalysis).get();
}

std::shared_ptr<const rf::LinearResult> SimulationView::lastResultShared() const
{
    return m_results.value(m_currentAnalysis);
}

const rf::BudgetRun *SimulationView::lastBudgetRun() const
{
    return m_budgetResults.value(m_currentAnalysis).get();
}

std::shared_ptr<const rf::BudgetRun> SimulationView::lastBudgetRunShared() const
{
    return m_budgetResults.value(m_currentAnalysis);
}

bool SimulationView::hasResult(const QString &id) const
{
    return m_results.contains(id) || m_budgetResults.contains(id);
}

void SimulationView::setAutoRun(bool enabled)
{
    {
        const QSignalBlocker blocker(m_autoRunAction);
        m_autoRunAction->setChecked(enabled);
    }
    if (enabled && (m_outdated || !hasResult(m_currentAnalysis)))
        m_autoRunTimer->start();
    if (!enabled)
        m_autoRunTimer->stop();
}

bool SimulationView::isAutoRun() const
{
    return m_autoRunAction->isChecked();
}

void SimulationView::setAutoRunDelay(int milliseconds)
{
    m_autoRunTimer->setInterval(milliseconds);
}

void SimulationView::onCircuitEdited()
{
    // Debounced: the check (and an auto-run) happens after a quiet period.
    m_autoRunTimer->start();
    // While tuning, the snapshot is retaken (keeping the tuned values).
    if (m_tuning)
        m_tuningRefreshTimer->start();
}

void SimulationView::onAutoRunTimer()
{
    if (m_tuning)
        return; // the tuning snapshot follows the edits instead
    const QByteArray current = fingerprint();
    const bool changed = current != m_lastFingerprint;
    if (changed && !m_outdated && hasResult(m_currentAnalysis)) {
        m_outdated = true;
        updateStatus();
    }
    if (isAutoRun() && (changed || !hasResult(m_currentAnalysis)) && !m_currentAnalysis.isEmpty())
        run();
}

void SimulationView::onBudgetFinished(std::shared_ptr<const rf::BudgetRun> budget, quint64 tag)
{
    m_elapsedTimer->stop();
    m_lastElapsedMs = m_elapsed.isValid() ? m_elapsed.elapsed() : -1;
    if (m_pendingRun) {
        // The result belongs to an outdated circuit: drop it and run again.
        updateActions();
        run();
        return;
    }
    if (tag != m_runTag || budget->cancelled) {
        Q_EMIT statusMessage(tr("Simulation stopped."));
        updateActions();
        updateStatus();
        Q_EMIT runFinished(false);
        return;
    }
    m_budgetResults.insert(m_runningAnalysis, budget);
    if (m_runningAnalysis == m_currentAnalysis)
        showResult();
    updateActions();
    updateStatus();
    if (budget->ok())
        Q_EMIT statusMessage(tr("Budget finished in %1.").arg(elapsedText(m_lastElapsedMs)));
    else
        Q_EMIT statusMessage(tr("Budget failed: %1").arg(budgetErrors(*budget).value(0)));
    Q_EMIT runFinished(budget->ok());
}

void SimulationView::onRunFinished(std::shared_ptr<const rf::LinearResult> result, quint64 tag)
{
    m_elapsedTimer->stop();
    m_lastElapsedMs = m_elapsed.isValid() ? m_elapsed.elapsed() : -1;
    if (m_pendingRun) {
        // The result belongs to an outdated circuit: drop it and run again.
        updateActions();
        run();
        return;
    }
    if (tag != m_runTag || result->cancelled) {
        Q_EMIT statusMessage(tr("Simulation stopped."));
        updateActions();
        updateStatus();
        Q_EMIT runFinished(false);
        return;
    }
    if (m_tuning && m_runningAnalysis == m_tuningAnalysis) {
        // Finished just as tuning started: keep it for when tuning ends.
        m_resultBeforeTuning = result;
        updateActions();
        updateStatus();
        Q_EMIT runFinished(result->ok());
        return;
    }
    m_results.insert(m_runningAnalysis, result);
    if (m_runningAnalysis == m_currentAnalysis)
        showResult();
    updateActions();
    updateStatus();
    if (result->ok())
        Q_EMIT statusMessage(tr("Simulation finished in %1.").arg(elapsedText(m_lastElapsedMs)));
    else
        Q_EMIT statusMessage(tr("Simulation failed: %1").arg(result->errors.value(0)));
    Q_EMIT runFinished(result->ok());
}

void SimulationView::finishWithError(const QString &message)
{
    if (isBudget(m_runningAnalysis)) {
        auto run = std::make_shared<rf::BudgetRun>();
        run->errors.append(message);
        m_budgetResults.insert(m_runningAnalysis, run);
    } else {
        auto result = std::make_shared<rf::LinearResult>();
        result->errors.append(message);
        if (!m_runningAnalysis.isEmpty())
            m_results.insert(m_runningAnalysis, result);
    }
    if (m_runningAnalysis == m_currentAnalysis || m_runningAnalysis.isEmpty())
        showMessages({message}, {});
    if (m_runningAnalysis == m_currentAnalysis)
        showResult();
    updateActions();
    updateStatus();
    Q_EMIT statusMessage(tr("Simulation failed: %1").arg(message));
    Q_EMIT runFinished(false);
}

QByteArray SimulationView::fingerprint() const
{
    // Everything a run depends on: the schematics of the project (without
    // their plots) and the current analysis.
    QByteArray bytes;
    const auto add = [&bytes](const Document *document) {
        if (!document || !document->schematic())
            return;
        QJsonObject content = document->schematic()->toJson();
        content.remove(QStringLiteral("simulation"));
        bytes += document->name().toUtf8();
        bytes += QJsonDocument(content).toJson(QJsonDocument::Compact);
    };
    if (m_project) {
        for (const Document *document : m_project->documents())
            add(document);
    } else {
        add(m_document);
    }
    if (m_schematic) {
        for (const rf::LinearAnalysisSetup &analysis : rf::linearAnalyses(m_schematic->simulation())) {
            if (analysis.id == m_currentAnalysis)
                bytes += QJsonDocument(analysis.toJson()).toJson(QJsonDocument::Compact);
        }
        for (const rf::BudgetAnalysisSetup &analysis : rf::budgetAnalyses(m_schematic->simulation())) {
            if (analysis.id == m_currentAnalysis)
                bytes += QJsonDocument(analysis.toJson()).toJson(QJsonDocument::Compact);
        }
    }
    return bytes;
}

// ---------------------------------------------------------------- analyses

QString SimulationView::currentAnalysisId() const
{
    return m_currentAnalysis;
}

void SimulationView::setCurrentAnalysis(const QString &id)
{
    if (id == m_currentAnalysis && m_analysisCombo->currentData().toString() == id)
        return;
    if (m_tuning && id != m_tuningAnalysis)
        stopTuning();
    m_currentAnalysis = id;
    const int index = m_analysisCombo->findData(id);
    if (index >= 0)
        m_analysisCombo->setCurrentIndex(index);
    // A shown result is taken as current for the circuit as it is now.
    m_lastFingerprint = fingerprint();
    m_outdated = false;
    syncPlots();
    showResult();
    updateActions();
    updateStatus();
}

QString SimulationView::analysisType(const QString &id) const
{
    return m_analysisTypes.value(id);
}

void SimulationView::editAnalysis(const QString &id)
{
    if (id.isEmpty()) {
        newAnalysis({});
        return;
    }
    QString stored;
    if (isBudget(id)) {
        BudgetDialog dialog(m_project, m_document, id, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        stored = dialog.analysisId();
    } else {
        AnalysisDialog dialog(m_project, m_document, id, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        stored = dialog.analysisId();
    }
    const bool created = id != stored;
    setCurrentAnalysis(stored);
    if (created || isAutoRun())
        run();
}

void SimulationView::newAnalysis(const QString &type)
{
    QString chosen = type;
    if (chosen.isEmpty()) {
        const QStringList items{tr("Linear analysis (S-parameters, noise)"), tr("RF budget analysis")};
        bool ok = false;
        const QString item = QInputDialog::getItem(this, tr("New Analysis"), tr("Type of the analysis:"), items, 0, false, &ok);
        if (!ok)
            return;
        chosen = item == items.at(1) ? BudgetType : LinearType;
    }
    QString stored;
    if (chosen == BudgetType) {
        BudgetDialog dialog(m_project, m_document, {}, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        stored = dialog.analysisId();
    } else {
        AnalysisDialog dialog(m_project, m_document, {}, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        stored = dialog.analysisId();
    }
    setCurrentAnalysis(stored);
    run();
}

void SimulationView::deleteAnalysis(const QString &id)
{
    if (!m_schematic || id.isEmpty())
        return;
    if (id == m_runningAnalysis && isRunning())
        cancel();
    m_results.remove(id);
    m_budgetResults.remove(id);
    m_schematic->setSimulation(sim::withoutAnalysis(m_schematic->simulation(), id));
}

void SimulationView::refreshAnalyses()
{
    // Linear and budget analyses, in the order of the file.
    struct Entry {
        QString id;
        QString name;
        QString type;
    };
    QList<Entry> analyses;
    const QJsonObject simulation = m_schematic ? m_schematic->simulation() : QJsonObject();
    for (const QJsonValue &value : simulation.value(QStringLiteral("analyses")).toArray()) {
        const QJsonObject object = value.toObject();
        const QString type = object.value(QStringLiteral("type")).toString();
        const QString id = object.value(QStringLiteral("id")).toString();
        if ((type == LinearType || type == BudgetType) && !id.isEmpty())
            analyses.append({id, object.value(QStringLiteral("name")).toString(), type});
    }
    m_analysisTypes.clear();
    {
        const QSignalBlocker blocker(m_analysisCombo);
        m_analysisCombo->clear();
        for (const Entry &analysis : std::as_const(analyses)) {
            const QString name = analysis.name.isEmpty() ? analysis.id : analysis.name;
            const bool budget = analysis.type == BudgetType;
            m_analysisCombo->addItem(budget ? tr("%1 (budget)").arg(name) : tr("%1 (linear)").arg(name), analysis.id);
            m_analysisCombo->setItemData(m_analysisCombo->count() - 1,
                                         budget ? tr("RF budget analysis") : tr("Linear analysis: S-parameters and noise"),
                                         Qt::ToolTipRole);
            m_analysisTypes.insert(analysis.id, analysis.type);
        }
    }
    QString current = m_currentAnalysis;
    if (!m_analysisTypes.contains(current))
        current = analyses.isEmpty() ? QString() : analyses.first().id;
    for (auto it = m_results.begin(); it != m_results.end();)
        it = m_analysisTypes.value(it.key()) == LinearType ? std::next(it) : m_results.erase(it);
    for (auto it = m_budgetResults.begin(); it != m_budgetResults.end();)
        it = m_analysisTypes.value(it.key()) == BudgetType ? std::next(it) : m_budgetResults.erase(it);
    for (const rf::LinearAnalysisSetup &analysis : rf::linearAnalyses(simulation)) {
        if (analysis.id == current)
            m_noise = analysis.noise;
    }
    const bool changed = current != m_currentAnalysis;
    m_currentAnalysis = current;
    {
        const QSignalBlocker blocker(m_analysisCombo);
        m_analysisCombo->setCurrentIndex(m_analysisCombo->findData(current));
    }
    syncPlots();
    if (changed)
        showResult();
    updateActions();
}

void SimulationView::onSimulationChanged()
{
    refreshAnalyses();
    updateTableColumns();
    if (m_tuning && analysisType(m_tuningAnalysis) != LinearType)
        stopTuning();
    else if (m_tuning)
        m_tuningRefreshTimer->start(); // the sweep may have changed
    // A changed analysis setup is a reason to run again; plot changes are not
    // (the fingerprint leaves them out).
    m_autoRunTimer->start();
}

// ---------------------------------------------------------------- plots

void SimulationView::syncPlots()
{
    const QList<sim::PlotDefinition> definitions = m_schematic && !m_currentAnalysis.isEmpty()
        ? sim::plotDefinitions(m_schematic->simulation(), m_currentAnalysis)
        : QList<sim::PlotDefinition>();
    const std::shared_ptr<const rf::LinearResult> result = m_results.value(m_currentAnalysis);
    const QStringList completionList = completions();

    QList<sim::PlotPane *> panes;
    for (const sim::PlotDefinition &definition : definitions) {
        sim::PlotPane *pane = plotPane(definition.id);
        if (pane) {
            pane->setDefinition(definition);
        } else {
            pane = new sim::PlotPane(definition, m_plotHost);
            connect(pane, &sim::PlotPane::definitionEdited, this, &SimulationView::onPlotEdited);
            connect(pane, &sim::PlotPane::removeRequested, this, &SimulationView::removePlot);
            connect(pane->plot(), &PlotWidget::hoverReadoutChanged, this, &SimulationView::statusMessage);
            pane->setCompletions(completionList);
            pane->setResult(result);
        }
        panes.append(pane);
    }
    const bool sameSet = panes == m_panes;
    for (sim::PlotPane *pane : std::as_const(m_panes)) {
        if (!panes.contains(pane))
            pane->deleteLater();
    }
    for (sim::PlotPane *pane : std::as_const(m_panes)) {
        if (!panes.contains(pane))
            pane->hide();
    }
    m_panes = panes;
    if (!sameSet)
        relayoutPlots();
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *pane = qobject_cast<sim::PlotPane *>(m_tabs->widget(i)))
            m_tabs->setTabText(i, pane->definition().name.isEmpty() ? tr("Plot") : pane->definition().name);
    }
    updateTableColumns();
}

void SimulationView::relayoutPlots()
{
    while (m_tabs->count() > 0)
        m_tabs->removeTab(0);
    while (QLayoutItem *item = m_grid->takeAt(0))
        delete item;
    const auto count = static_cast<int>(m_panes.size());
    const int columns = count <= 1 ? 1 : (count <= 4 ? 2 : 3);
    for (int i = 0; i < count; ++i) {
        sim::PlotPane *pane = m_panes.at(i);
        if (m_tiled) {
            pane->setParent(m_gridHost);
            m_grid->addWidget(pane, i / columns, i % columns);
            pane->show();
        } else {
            m_tabs->addTab(pane, pane->definition().name.isEmpty() ? tr("Plot") : pane->definition().name);
        }
    }
    for (int c = 0; c < 3; ++c)
        m_grid->setColumnStretch(c, c < columns ? 1 : 0);
    for (int r = 0; r < 4; ++r)
        m_grid->setRowStretch(r, r < (count + columns - 1) / std::max(1, columns) ? 1 : 0);
    m_tabs->setVisible(!m_tiled && count > 0);
    m_gridHost->setVisible(m_tiled && count > 0);
    m_placeholder->setVisible(count == 0);
    m_placeholder->setText(m_currentAnalysis.isEmpty()
                               ? tr("No analysis yet. Create one with “New Analysis…”.")
                               : tr("No plots for this analysis. Add one with “Add Plot”."));
}

void SimulationView::setTiled(bool tiled)
{
    {
        const QSignalBlocker blocker(m_tileAction);
        m_tileAction->setChecked(tiled);
    }
    if (tiled == m_tiled)
        return;
    m_tiled = tiled;
    relayoutPlots();
}

sim::PlotPane *SimulationView::plotPane(const QString &plotId) const
{
    for (sim::PlotPane *pane : m_panes) {
        if (pane->definition().id == plotId)
            return pane;
    }
    return nullptr;
}

QString SimulationView::addPlot(const QString &kind)
{
    if (!m_schematic || m_currentAnalysis.isEmpty() || isBudget(m_currentAnalysis))
        return {};
    const std::vector<int> ports = m_ports.empty() ? std::vector<int>{1, 2} : m_ports;
    sim::PlotDefinition plot;
    plot.kind = kind;
    plot.analysis = m_currentAnalysis;
    if (kind == QLatin1String("smith")) {
        plot.name = tr("Smith chart");
        plot.traces = {sim::sParameterName(ports.front(), ports.front())};
    } else if (kind == QLatin1String("polar")) {
        plot.name = tr("Polar");
        plot.traces = {ports.size() > 1 ? sim::sParameterName(ports[1], ports[0]) : sim::sParameterName(ports[0], ports[0])};
    } else {
        plot.name = tr("Plot");
        plot.traces = {QStringLiteral("dB(%1)").arg(ports.size() > 1 ? sim::sParameterName(ports[1], ports[0])
                                                                     : sim::sParameterName(ports[0], ports[0]))};
    }
    QString id;
    m_schematic->setSimulation(sim::withPlot(m_schematic->simulation(), plot, &id));
    if (sim::PlotPane *pane = plotPane(id)) {
        pane->setEditorVisible(true);
        if (!m_tiled)
            m_tabs->setCurrentWidget(pane);
    }
    return id;
}

void SimulationView::addDefaultPlots()
{
    if (!m_schematic || m_currentAnalysis.isEmpty() || isBudget(m_currentAnalysis))
        return;
    QJsonObject simulation = m_schematic->simulation();
    for (const sim::PlotDefinition &plot : sim::defaultPlots(m_currentAnalysis, m_ports, m_noise))
        simulation = sim::withPlot(simulation, plot);
    m_schematic->setSimulation(simulation);
}

void SimulationView::removePlot(const QString &plotId)
{
    if (m_schematic)
        m_schematic->setSimulation(sim::withoutPlot(m_schematic->simulation(), plotId));
}

void SimulationView::onPlotEdited(const sim::PlotDefinition &definition)
{
    if (m_schematic)
        m_schematic->setSimulation(sim::withPlot(m_schematic->simulation(), definition));
}

QStringList SimulationView::completions() const
{
    return sim::measurementCompletions(m_ports, m_noise);
}

void SimulationView::refreshCompletions()
{
    const QStringList list = completions();
    for (sim::PlotPane *pane : std::as_const(m_panes))
        pane->setCompletions(list);
    if (m_completionModel->stringList() != list)
        m_completionModel->setStringList(list);
}

// ---------------------------------------------------------------- results

void SimulationView::showBudgetResult()
{
    m_resultStack->setCurrentWidget(m_budgetView);
    const std::shared_ptr<const rf::BudgetRun> budget = m_budgetResults.value(m_currentAnalysis);
    m_budgetView->setRun(budget);
    if (budget)
        showMessages(budgetErrors(*budget), withoutRepeats(budget->warnings));
    else
        showMessages({}, {});
}

void SimulationView::showResult()
{
    if (isBudget(m_currentAnalysis)) {
        showBudgetResult();
        return;
    }
    m_resultStack->setCurrentWidget(m_splitter);
    m_budgetView->setRun(nullptr);
    const std::shared_ptr<const rf::LinearResult> result = m_results.value(m_currentAnalysis);
    for (sim::PlotPane *pane : std::as_const(m_panes))
        pane->setResult(result);
    m_tableModel->setResult(result);
    m_table->resizeColumnsToContents();
    if (result) {
        if (result->ok() && result->portNumbers != m_ports) {
            m_ports = result->portNumbers;
        }
        m_noise = result->network.hasNoiseCorrelation() || m_noise;
        showMessages(result->errors, result->warnings);
        // An analysis created without the dialog (from Python, for example)
        // has no plots yet: give it the default ones now that the ports are known.
        if (result->ok() && m_panes.isEmpty()) {
            addDefaultPlots();
            for (sim::PlotPane *pane : std::as_const(m_panes))
                pane->setResult(result);
        }
    } else {
        showMessages({}, {});
    }
    refreshCompletions();
}

void SimulationView::showMessages(const QStringList &errors, const QStringList &warnings)
{
    m_errors = errors;
    m_warnings = warnings;
    m_errorBanner->setVisible(!errors.isEmpty());
    m_errorTitle->setText(errors.size() == 1 ? tr("The simulation failed") : tr("The simulation failed with %1 errors").arg(errors.size()));
    QStringList lines;
    for (const QString &error : errors)
        lines.append(QStringLiteral("• %1").arg(error.toHtmlEscaped()));
    m_errorText->setText(lines.join(QStringLiteral("<br>")));

    m_warningList->clear();
    m_warningList->addItems(warnings);
    m_warningButton->setVisible(!warnings.isEmpty());
    m_warningButton->setText(warnings.size() == 1 ? tr("1 warning") : tr("%1 warnings").arg(warnings.size()));
    if (warnings.isEmpty())
        m_warningButton->setChecked(false);
    updateIcons();
}

void SimulationView::updateActions()
{
    const bool running = isRunning();
    const bool haveAnalysis = !m_currentAnalysis.isEmpty();
    const bool budget = isBudget(m_currentAnalysis);
    const auto result = m_results.value(m_currentAnalysis);
    const bool haveResult = !budget && result && result->ok();
    const auto budgetRun = m_budgetResults.value(m_currentAnalysis);
    const bool haveBudget = budget && budgetRun && !budgetRun->budget.stages.empty();
    m_runAction->setEnabled(haveAnalysis);
    m_stopAction->setEnabled(running);
    m_editAnalysisAction->setEnabled(haveAnalysis);
    m_deleteAnalysisAction->setEnabled(haveAnalysis);
    m_addPlotAction->setEnabled(haveAnalysis && !budget);
    m_tileAction->setEnabled(!budget);
    m_exportTouchstoneAction->setEnabled(haveResult);
    m_exportCsvAction->setEnabled(haveResult || haveBudget);
    m_copyTableAction->setEnabled(haveResult || haveBudget);
    m_analysisCombo->setEnabled(m_analysisCombo->count() > 0);
    const bool tunable = m_project && m_schematic && haveAnalysis && !budget;
    m_tuneAction->setEnabled(tunable || m_tuning);
    {
        const QSignalBlocker blocker(m_tuneAction);
        m_tuneAction->setChecked(m_tuning);
    }
    bool anyLinear = false;
    for (const QString &type : std::as_const(m_analysisTypes))
        anyLinear = anyLinear || type == LinearType;
    m_optimizeAction->setEnabled(m_project && m_schematic && anyLinear);
}

void SimulationView::updateStatus()
{
    const bool running = isRunning();
    m_busy->setVisible(running);
    QString text;
    if (m_tuning) {
        const auto result = m_results.value(m_tuningAnalysis);
        if (result && !result->ok())
            text = tr("Tuning – the simulation failed");
        else if (m_tuningResults == 0)
            text = tr("Tuning – simulating…");
        else
            text = tr("Tuning – %1 points, %2 ms per simulation")
                       .arg(result ? result->network.frequencyCount() : 0)
                       .arg(QString::number(m_lastTuningMs, 'f', 1));
        text += QStringLiteral(" – ")
                + (m_tuningPanel->isModified() ? tr("tuned values not applied") : tr("values as in the schematic"));
    } else if (running) {
        text = m_pendingRun ? tr("Restarting with the latest changes…")
                            : tr("Running… %1").arg(elapsedText(m_elapsed.isValid() ? m_elapsed.elapsed() : 0));
    } else if (const auto budget = isBudget(m_currentAnalysis) ? m_budgetResults.value(m_currentAnalysis) : nullptr) {
        if (budget->ok() || !budget->budget.stages.empty()) {
            const size_t stages = budget->budget.stages.size();
            text = tr("Budget, %1 at %2")
                       .arg(stages == 1 ? tr("1 stage") : tr("%1 stages").arg(stages),
                            formatEngineering(budget->budget.inputFrequency, QStringLiteral("Hz")));
            if (!budget->sweep.empty())
                text += QStringLiteral(", ") + tr("swept over %1 points").arg(budget->sweep.size());
            if (!budget->spurs.spurs.empty())
                text += QStringLiteral(", ") + tr("%1 spurs").arg(budget->spurs.spurs.size());
            if (m_lastElapsedMs >= 0)
                text += QStringLiteral(" – ") + tr("finished in %1").arg(elapsedText(m_lastElapsedMs));
        } else {
            text = tr("Failed");
        }
        if (m_outdated)
            text += QStringLiteral(" – ") + tr("outdated, the circuit has changed");
    } else if (const auto result = isBudget(m_currentAnalysis) ? nullptr : m_results.value(m_currentAnalysis)) {
        if (result->ok()) {
            const rf::NetworkData &n = result->network;
            const QString points = n.frequencyCount() == 1 ? tr("1 point") : tr("%1 points").arg(n.frequencyCount());
            const QString ports = n.portCount() == 1 ? tr("1 port") : tr("%1 ports").arg(n.portCount());
            text = tr("%1, %2 to %3, %4")
                       .arg(points, formatEngineering(n.frequencies.front(), QStringLiteral("Hz")),
                            formatEngineering(n.frequencies.back(), QStringLiteral("Hz")), ports);
            if (m_lastElapsedMs >= 0)
                text += QStringLiteral(" – ") + tr("finished in %1").arg(elapsedText(m_lastElapsedMs));
        } else {
            text = tr("Failed");
        }
        if (m_outdated)
            text += QStringLiteral(" – ") + tr("outdated, the circuit has changed");
    } else if (!m_currentAnalysis.isEmpty()) {
        text = tr("Not run yet. Press F5 to run.");
    }
    m_statusLabel->setText(text);
}

void SimulationView::updateIcons()
{
    const QPalette pal = palette();
    m_newAnalysisAction->setIcon(glyphIcon(Glyph::New, pal));
    m_editAnalysisAction->setIcon(glyphIcon(Glyph::Edit, pal));
    m_deleteAnalysisAction->setIcon(glyphIcon(Glyph::Delete, pal));
    m_runAction->setIcon(glyphIcon(Glyph::Run, pal));
    m_stopAction->setIcon(glyphIcon(Glyph::Stop, pal));
    m_autoRunAction->setIcon(glyphIcon(Glyph::AutoRun, pal));
    m_addPlotAction->setIcon(glyphIcon(Glyph::Plot, pal));
    m_exportAction->setIcon(glyphIcon(Glyph::Export, pal));
    m_tuneAction->setIcon(glyphIcon(Glyph::Tune, pal));
    m_optimizeAction->setIcon(glyphIcon(Glyph::Optimize, pal));

    const bool dark = isDark(pal);
    const QColor background = dark ? QColor(0x4A, 0x22, 0x22) : QColor(0xFD, 0xEC, 0xEC);
    const QColor border = dark ? QColor(0xA0, 0x40, 0x40) : QColor(0xE0, 0x90, 0x90);
    const QColor text = dark ? QColor(0xFF, 0xD0, 0xD0) : QColor(0x8A, 0x10, 0x10);
    m_errorBanner->setStyleSheet(
        QStringLiteral("QFrame#errorBanner { background: %1; border: 1px solid %2; border-radius: 4px; }"
                       "QFrame#errorBanner QLabel { color: %3; background: transparent; border: none; }")
            .arg(background.name(), border.name(), text.name()));
    const QColor warning = dark ? QColor(0xE8, 0xC0, 0x60) : QColor(0x8A, 0x5A, 0x00);
    m_warningButton->setStyleSheet(QStringLiteral("QToolButton { color: %1; }").arg(warning.name()));
}

void SimulationView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_runAction)
        updateIcons();
}

// ---------------------------------------------------------------- table

void SimulationView::setTableColumns(const QStringList &expressions)
{
    m_tableColumns.clear();
    for (const QString &expression : expressions) {
        if (!expression.trimmed().isEmpty())
            m_tableColumns.append(expression.trimmed());
    }
    // A list with only an empty entry means "no columns" (kept apart from
    // "default columns").
    if (m_tableColumns.isEmpty() && !expressions.isEmpty())
        m_tableColumns.append(QString());
    updateTableColumns();
}

QStringList SimulationView::tableColumns() const
{
    if (m_tableColumns.isEmpty()) {
        QStringList columns;
        for (sim::PlotPane *pane : m_panes) {
            for (const QString &expression : pane->definition().traces) {
                if (!columns.contains(expression))
                    columns.append(expression);
            }
        }
        return columns;
    }
    QStringList columns = m_tableColumns;
    columns.removeAll(QString());
    return columns;
}

void SimulationView::updateTableColumns()
{
    m_tableModel->setExpressions(tableColumns());
    m_table->resizeColumnsToContents();
}

void SimulationView::copyTable() const
{
    if (isBudget(m_currentAnalysis)) {
        m_budgetView->copyTable();
        return;
    }
    QList<int> rows;
    if (m_table->selectionModel()) {
        for (const QModelIndex &index : m_table->selectionModel()->selectedRows())
            rows.append(index.row());
    }
    std::sort(rows.begin(), rows.end());
    const QString text = m_tableModel->text(QLatin1Char('\t'), rows);
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

bool SimulationView::exportTouchstone(const QString &path, const rf::TouchstoneWriteOptions &options, QString *error) const
{
    const auto result = m_results.value(m_currentAnalysis);
    if (!result || !result->ok()) {
        if (error)
            *error = tr("There is no result to export.");
        return false;
    }
    return sim::exportTouchstone(path, *result, options, error);
}

bool SimulationView::exportCsv(const QString &path, QString *error) const
{
    if (isBudget(m_currentAnalysis))
        return m_budgetView->exportCsv(path, error);
    const auto result = m_results.value(m_currentAnalysis);
    if (!result || !result->ok()) {
        if (error)
            *error = tr("There is no result to export.");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    file.write(sim::resultsCsv(*result, tableColumns()).toUtf8());
    if (!file.commit()) {
        if (error)
            *error = tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

void SimulationView::exportTouchstoneInteractive()
{
    const auto result = m_results.value(m_currentAnalysis);
    if (!result || !result->ok())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Export Touchstone"));
    auto *format = new QComboBox(&dialog);
    format->addItem(tr("Real / imaginary (RI)"), static_cast<int>(rf::TouchstoneFormat::RealImaginary));
    format->addItem(tr("Magnitude / angle (MA)"), static_cast<int>(rf::TouchstoneFormat::MagnitudeAngle));
    format->addItem(tr("dB / angle (DB)"), static_cast<int>(rf::TouchstoneFormat::DecibelAngle));
    auto *unit = new QComboBox(&dialog);
    for (const char *u : {"Hz", "kHz", "MHz", "GHz"})
        unit->addItem(QString::fromLatin1(u));
    unit->setCurrentText(QStringLiteral("GHz"));
    auto *version = new QComboBox(&dialog);
    version->addItem(tr("1.1"), 1);
    version->addItem(tr("2.0"), 2);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *form = new QFormLayout(&dialog);
    form->addRow(tr("Format:"), format);
    form->addRow(tr("Frequency unit:"), unit);
    form->addRow(tr("Version:"), version);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const int ports = result->network.portCount();
    const QString suggested = QFileInfo(m_document->filePath()).absoluteDir().filePath(
        QStringLiteral("%1.s%2p").arg(m_document->name()).arg(ports));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Touchstone"), suggested,
                                                      tr("Touchstone files (*.s%1p *.ts)").arg(ports));
    if (path.isEmpty())
        return;
    rf::TouchstoneWriteOptions options;
    options.format = static_cast<rf::TouchstoneFormat>(format->currentData().toInt());
    options.frequencyUnit = unit->currentText();
    options.version = version->currentData().toInt();
    QString error;
    if (!exportTouchstone(path, options, &error))
        QMessageBox::warning(this, tr("Export Touchstone"), error);
    else
        Q_EMIT statusMessage(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
}

void SimulationView::exportCsvInteractive()
{
    const QString suggested = QFileInfo(m_document->filePath()).absoluteDir().filePath(m_document->name() + QStringLiteral(".csv"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Table"), suggested, tr("CSV files (*.csv)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!exportCsv(path, &error))
        QMessageBox::warning(this, tr("Export Table"), error);
    else
        Q_EMIT statusMessage(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
}

QList<QAction *> SimulationView::actions() const
{
    return {m_newAnalysisAction, m_editAnalysisAction, m_deleteAnalysisAction, m_runAction,
            m_stopAction,        m_autoRunAction,      m_addPlotAction,        m_tileAction,
            m_exportTouchstoneAction, m_exportCsvAction, m_copyTableAction,   m_tuneAction,
            m_optimizeAction};
}

// ---------------------------------------------------------------- tuning

void SimulationView::startTuning()
{
    if (m_tuning)
        return;
    if (!m_project || !m_schematic || !m_document || m_currentAnalysis.isEmpty() || isBudget(m_currentAnalysis)) {
        Q_EMIT statusMessage(tr("Tuning needs a linear analysis of a schematic in a project."));
        updateActions();
        return;
    }
    // A run of the real circuit would overwrite the tuned plots.
    if (isRunning())
        cancel();
    m_autoRunTimer->stop();
    m_tuning = true;
    m_tuningAnalysis = m_currentAnalysis;
    m_resultBeforeTuning = m_results.value(m_currentAnalysis);
    m_tuningFirstSerial = m_tuningRunner->lastRequested() + 1;
    m_tuningResults = 0;
    m_lastTuningMs = -1;
    m_lastTunedValues.clear();
    m_tuningTimes.clear();

    takeTuningSnapshot();
    m_tuningPanel->setContext(m_project, m_document);
    {
        // Values kept from an earlier session start at the document values.
        const QSignalBlocker blocker(m_tuningPanel);
        m_tuningPanel->refreshDocumentValues();
        m_tuningPanel->resetValues();
    }
    m_tuningPanel->show();
    if (m_mainSplitter->sizes().value(1) < m_tuningPanel->minimumWidth()) {
        const int total = std::max(m_mainSplitter->width(), 800);
        const int panel = std::clamp(total / 4, m_tuningPanel->minimumWidth(), 360);
        m_mainSplitter->setSizes({total - panel, panel});
    }
    requestTuning();
    updateActions();
    updateStatus();
    Q_EMIT statusMessage(tr("Tuning a copy of the circuit; apply to write the values into the schematic."));
}

void SimulationView::stopTuning()
{
    if (!m_tuning)
        return;
    m_tuning = false;
    m_tuningRunner->dropPending();
    m_tuningRefreshTimer->stop();
    m_tuningPanel->hide();
    {
        const QSignalBlocker blocker(m_tuningPanel);
        m_tuningPanel->refreshDocumentValues();
        m_tuningPanel->resetValues();
    }
    // Back to the result of the document (updated by applyTuning()).
    if (m_resultBeforeTuning)
        m_results.insert(m_tuningAnalysis, m_resultBeforeTuning);
    else
        m_results.remove(m_tuningAnalysis);
    m_resultBeforeTuning.reset();
    const bool changed = fingerprint() != m_lastFingerprint;
    if (changed && hasResult(m_currentAnalysis))
        m_outdated = true;
    showResult();
    updateActions();
    updateStatus();
    if (isAutoRun() && (changed || !hasResult(m_currentAnalysis)))
        m_autoRunTimer->start();
}

bool SimulationView::applyTuning(QString *error)
{
    if (!m_tuning || !m_project || !m_document) {
        if (error)
            *error = tr("Tuning is not active.");
        return false;
    }
    if (!m_tuningPanel->isModified())
        return true;
    const QList<rf::DesignVariable> variables = m_tuningPanel->variables();
    const std::vector<double> values = m_tuningPanel->values();
    QString message;
    const bool ok = sim::applyDesignValuesUndoable(*m_project, *m_document, variables, values, tr("Apply tuned values"),
                                                   &message);
    {
        const QSignalBlocker blocker(m_tuningPanel);
        m_tuningPanel->refreshDocumentValues();
    }
    if (!ok) {
        if (error)
            *error = message;
        updateStatus();
        return false;
    }
    // The shown tuned result is now the result of the document, if it is
    // the result of exactly these values.
    if (!m_tuningRunner->isBusy() && m_lastTunedValues == values) {
        m_resultBeforeTuning = m_results.value(m_tuningAnalysis);
        m_lastFingerprint = fingerprint();
        m_outdated = false;
    } else {
        m_resultBeforeTuning.reset();
    }
    updateStatus();
    Q_EMIT statusMessage(tr("Tuned values written into the schematic."));
    return true;
}

void SimulationView::resetTuning()
{
    if (m_tuning)
        m_tuningPanel->resetValues();
}

bool SimulationView::isTuningBusy() const
{
    return m_tuning && m_tuningRunner->isBusy();
}

double SimulationView::tuningUpdateRate() const
{
    if (m_tuningTimes.size() < 2)
        return 0;
    const double span = static_cast<double>(m_tuningTimes.back() - m_tuningTimes.front());
    return span > 0 ? 1000.0 * static_cast<double>(m_tuningTimes.size() - 1) / span : 0.0;
}

void SimulationView::takeTuningSnapshot()
{
    // Project::snapshot() reads the documents: GUI thread. The worker is its
    // only user from here on.
    m_tuningRunner->setSnapshot(std::shared_ptr<Project>(m_project->snapshot().release()), m_document->name());
}

void SimulationView::refreshTuningSnapshot()
{
    if (!m_tuning)
        return;
    takeTuningSnapshot();
    QStringList removed;
    {
        const QSignalBlocker blocker(m_tuningPanel);
        removed = m_tuningPanel->refreshDocumentValues();
    }
    if (!removed.isEmpty())
        Q_EMIT statusMessage(tr("No longer tuned (removed from the schematic): %1").arg(removed.join(QStringLiteral(", "))));
    requestTuning();
}

void SimulationView::requestTuning()
{
    if (!m_tuning || !m_schematic)
        return;
    for (const rf::LinearAnalysisSetup &analysis : rf::linearAnalyses(m_schematic->simulation())) {
        if (analysis.id == m_tuningAnalysis) {
            m_tuningRunner->request(analysis, m_tuningPanel->variables(), m_tuningPanel->values());
            updateStatus();
            return;
        }
    }
    stopTuning();
}

void SimulationView::onTuningEvaluated(std::shared_ptr<const rf::LinearResult> result, quint64 serial,
                                       std::vector<double> values, double milliseconds)
{
    if (!m_tuning || serial < m_tuningFirstSerial)
        return; // from an earlier session
    ++m_tuningResults;
    m_lastTuningMs = milliseconds;
    m_lastTunedValues = std::move(values);
    m_tuningTimes.append(m_tuningClock.elapsed());
    while (m_tuningTimes.size() > 30)
        m_tuningTimes.removeFirst();

    m_results.insert(m_tuningAnalysis, result);
    if (m_currentAnalysis == m_tuningAnalysis) {
        // A light version of showResult(): traces update in place.
        for (sim::PlotPane *pane : std::as_const(m_panes))
            pane->setResult(result);
        m_tableModel->setResult(result);
        if (result->errors != m_errors || result->warnings != m_warnings)
            showMessages(result->errors, result->warnings);
    }
    const double rate = tuningUpdateRate();
    QString status = result->ok() ? tr("%1 ms per simulation").arg(QString::number(milliseconds, 'f', 1))
                                  : tr("The simulation failed: %1").arg(result->errors.value(0));
    if (rate > 0 && m_tuningRunner->isBusy())
        status += QStringLiteral(", ") + tr("%1 updates/s").arg(QString::number(rate, 'f', 0));
    m_tuningPanel->setStatusText(status);
    updateStatus();
    Q_EMIT tuningResultShown();
}

// ---------------------------------------------------------------- optimisation

OptimizationDialog *SimulationView::optimizationDialog() const
{
    return m_optimizationDialog;
}

OptimizationDialog *SimulationView::openOptimization(const QString &setupId)
{
    if (!m_project || !m_schematic || !m_document)
        return nullptr;
    if (m_optimizationDialog) {
        if (!setupId.isEmpty() && !m_optimizationDialog->isRunning())
            m_optimizationDialog->selectSetup(setupId);
        m_optimizationDialog->show();
        m_optimizationDialog->raise();
        m_optimizationDialog->activateWindow();
        return m_optimizationDialog;
    }
    auto *dialog = new OptimizationDialog(m_project, m_document, setupId, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &OptimizationDialog::applied, this, [this](const QString &analysisId) {
        stopTuning();
        if (m_analysisTypes.value(analysisId) == LinearType)
            setCurrentAnalysis(analysisId);
        run();
    });
    m_optimizationDialog = dialog;
    dialog->show();
    return dialog;
}

} // namespace lvd
