#include "simulation_ui/OptimizationDialog.h"

#include "core/Document.h"
#include "core/Expression.h"
#include "core/Netlist.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "plot/PlotWidget.h"
#include "simulation_ui/DesignVariables.h"
#include "simulation_ui/OptimizationRunner.h"
#include "simulation_ui/PlotDefinitions.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lvd {

namespace {

enum VariableColumn { VarName, VarCurrent, VarMin, VarMax, VarLog, VarSeries, VarColumns };
enum GoalColumn { GoalMeasurement, GoalType, GoalTarget, GoalWeight, GoalBandStart, GoalBandStop, GoalColumns };

constexpr int VariableRole = Qt::UserRole + 1;

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

QString numberText(double value, int digits = 4)
{
    if (!std::isfinite(value))
        return QStringLiteral("–");
    return QString::number(value, 'g', digits);
}

QString costText(double value)
{
    if (!std::isfinite(value))
        return QStringLiteral("–");
    return QString::number(value, 'g', 4);
}

QTableWidgetItem *readOnly(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}

QString textOf(const QTableWidget *table, int row, int column)
{
    const QTableWidgetItem *item = table->item(row, column);
    return item ? item->text().trimmed() : QString();
}

constexpr int ExactValueRole = Qt::UserRole + 2;
constexpr int ShownTextRole = Qt::UserRole + 3;

// An editable cell showing a number; the exact value is kept as long as the
// text is not edited (the text has only 4 significant digits).
QTableWidgetItem *numberItem(double value, const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    if (std::isfinite(value)) {
        item->setData(ExactValueRole, value);
        item->setData(ShownTextRole, text);
    }
    return item;
}

// The number of a cell: the exact value while the text is unchanged,
// otherwise the parsed text (rf::NotSet when it is not a number); `empty` for
// an empty cell.
double numberOf(const QTableWidget *table, int row, int column, double empty)
{
    const QTableWidgetItem *item = table->item(row, column);
    if (!item || item->text().trimmed().isEmpty())
        return empty;
    const QVariant exact = item->data(ExactValueRole);
    if (exact.isValid() && item->text() == item->data(ShownTextRole).toString())
        return exact.toDouble();
    return sim::parseValue(item->text()).value_or(rf::NotSet);
}

// Line edits with measurement completion in the measurement column.
class MeasurementDelegate : public QStyledItemDelegate
{
public:
    MeasurementDelegate(QStringListModel *completions, QObject *parent)
        : QStyledItemDelegate(parent), m_completions(completions)
    {
    }

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QWidget *editor = QStyledItemDelegate::createEditor(parent, option, index);
        if (auto *line = qobject_cast<QLineEdit *>(editor); line && index.column() == GoalMeasurement) {
            auto *completer = new QCompleter(m_completions, line);
            completer->setCaseSensitivity(Qt::CaseInsensitive);
            completer->setFilterMode(Qt::MatchContains);
            line->setCompleter(completer);
        }
        return editor;
    }

private:
    QStringListModel *m_completions;
};

const char *const MethodNames[] = {QT_TRANSLATE_NOOP("lvd::OptimizationDialog", "Nelder–Mead (gradient-free)"),
                                   QT_TRANSLATE_NOOP("lvd::OptimizationDialog", "Levenberg–Marquardt (gradient)")};

} // namespace

OptimizationDialog::OptimizationDialog(Project *project, Document *document, const QString &setupId, QWidget *parent)
    : QDialog(parent), m_project(project), m_document(document),
      m_schematic(document ? document->schematic() : nullptr)
{
    setObjectName(QStringLiteral("optimizationDialog"));
    setWindowTitle(tr("Optimisation"));
    m_runner = new sim::OptimizationRunner(this);
    connect(m_runner, &sim::OptimizationRunner::progressed, this, &OptimizationDialog::onProgress);
    connect(m_runner, &sim::OptimizationRunner::finished, this, &OptimizationDialog::onFinished);

    createWidgets();
    refreshAnalyses();
    refreshSetups();
    refreshCompletions();
    selectSetup(setupId);
    if (m_schematic) {
        connect(m_schematic, &Schematic::simulationChanged, this, [this] {
            refreshAnalyses();
            refreshSetups();
        });
    }
    resize(1180, 720);
}

OptimizationDialog::~OptimizationDialog()
{
    disconnect(m_runner, nullptr, this, nullptr);
    m_runner->cancel();
}

// ---------------------------------------------------------------- widgets

void OptimizationDialog::createWidgets()
{
    // Setup row.
    m_setupCombo = new QComboBox(this);
    m_setupCombo->setObjectName(QStringLiteral("optimizationSetup"));
    m_setupCombo->setMinimumContentsLength(16);
    m_setupCombo->setToolTip(tr("Optimisation setups stored with the schematic"));
    connect(m_setupCombo, &QComboBox::activated, this,
            [this](int index) { selectSetup(m_setupCombo->itemData(index).toString()); });
    m_deleteSetup = new QToolButton(this);
    m_deleteSetup->setText(tr("Delete"));
    m_deleteSetup->setToolTip(tr("Delete the stored setup"));
    connect(m_deleteSetup, &QToolButton::clicked, this, [this] {
        if (!m_schematic || m_setupId.isEmpty() || isRunning())
            return;
        QJsonObject simulation = m_schematic->simulation();
        QJsonArray setups;
        for (const QJsonValue &value : simulation.value(QStringLiteral("optimizations")).toArray()) {
            if (value.toObject().value(QStringLiteral("id")).toString() != m_setupId)
                setups.append(value);
        }
        simulation.insert(QStringLiteral("optimizations"), setups);
        m_setupId.clear();
        m_schematic->setSimulation(simulation);
        selectSetup({});
    });
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("optimizationName"));
    m_analysisCombo = new QComboBox(this);
    m_analysisCombo->setObjectName(QStringLiteral("optimizationAnalysis"));
    m_analysisCombo->setToolTip(tr("The linear analysis whose sweep the goals are evaluated on"));
    connect(m_analysisCombo, &QComboBox::activated, this, [this] { refreshCompletions(); });
    m_methodCombo = new QComboBox(this);
    m_methodCombo->setObjectName(QStringLiteral("optimizationMethod"));
    m_methodCombo->addItem(tr(MethodNames[0]), static_cast<int>(rf::OptimizationSetup::Method::NelderMead));
    m_methodCombo->addItem(tr(MethodNames[1]), static_cast<int>(rf::OptimizationSetup::Method::LevenbergMarquardt));
    m_methodCombo->setItemData(0, tr("Simplex search; robust, handles minimise and maximise goals"), Qt::ToolTipRole);
    m_methodCombo->setItemData(1, tr("Least squares with a numerical Jacobian; fast near a solution"), Qt::ToolTipRole);
    m_evaluations = new QSpinBox(this);
    m_evaluations->setObjectName(QStringLiteral("maxEvaluations"));
    m_evaluations->setRange(10, 100000);
    m_evaluations->setSingleStep(50);
    m_evaluations->setValue(500);
    m_evaluations->setToolTip(tr("The run stops after this many simulations"));

    auto *top = new QGridLayout;
    top->addWidget(new QLabel(tr("Setup:"), this), 0, 0);
    auto *setupRow = new QHBoxLayout;
    setupRow->addWidget(m_setupCombo, 1);
    setupRow->addWidget(m_deleteSetup);
    top->addLayout(setupRow, 0, 1);
    top->addWidget(new QLabel(tr("Name:"), this), 0, 2);
    top->addWidget(m_name, 0, 3);
    top->addWidget(new QLabel(tr("Analysis:"), this), 1, 0);
    top->addWidget(m_analysisCombo, 1, 1);
    top->addWidget(new QLabel(tr("Method:"), this), 1, 2);
    auto *methodRow = new QHBoxLayout;
    methodRow->addWidget(m_methodCombo, 1);
    methodRow->addSpacing(12);
    methodRow->addWidget(new QLabel(tr("Max. evaluations:"), this));
    methodRow->addWidget(m_evaluations);
    top->addLayout(methodRow, 1, 3);
    top->setColumnStretch(1, 1);
    top->setColumnStretch(3, 1);

    const auto title = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        QFont bold = label->font();
        bold.setBold(true);
        label->setFont(bold);
        return label;
    };
    const auto tool = [this](const QString &text, const QString &tip) {
        auto *button = new QToolButton(this);
        button->setText(text);
        button->setToolTip(tip);
        button->setAutoRaise(true);
        return button;
    };

    // Variables.
    m_variableTable = new QTableWidget(0, VarColumns, this);
    m_variableTable->setObjectName(QStringLiteral("optimizationVariables"));
    m_variableTable->setHorizontalHeaderLabels({tr("Variable"), tr("Now"), tr("Minimum"), tr("Maximum"), tr("Log"), tr("Series")});
    m_variableTable->verticalHeader()->hide();
    m_variableTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_variableTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_variableTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_variableTable->horizontalHeader()->setSectionResizeMode(VarName, QHeaderView::Stretch);
    m_variableTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                                     | QAbstractItemView::AnyKeyPressed);
    m_addMenu = new QMenu(this);
    connect(m_addMenu, &QMenu::aboutToShow, this, &OptimizationDialog::fillAddMenu);
    m_addVariable = tool(tr("Add…"), tr("Add a variable or a component parameter"));
    m_addVariable->setObjectName(QStringLiteral("addOptimizationVariable"));
    m_addVariable->setMenu(m_addMenu);
    m_addVariable->setPopupMode(QToolButton::InstantPopup);
    auto *removeVariable = tool(tr("Remove"), tr("Remove the selected variable"));
    connect(removeVariable, &QToolButton::clicked, this, [this] {
        const int row = m_variableTable->currentRow();
        if (row >= 0)
            m_variableTable->removeRow(row);
        updateButtons();
    });
    auto *variableHeader = new QHBoxLayout;
    variableHeader->addWidget(title(tr("Variables")));
    variableHeader->addStretch(1);
    variableHeader->addWidget(m_addVariable);
    variableHeader->addWidget(removeVariable);

    // Goals.
    m_completions = new QStringListModel(this);
    m_goalTable = new QTableWidget(0, GoalColumns, this);
    m_goalTable->setObjectName(QStringLiteral("optimizationGoals"));
    m_goalTable->setHorizontalHeaderLabels(
        {tr("Measurement"), tr("Goal"), tr("Target"), tr("Weight"), tr("From"), tr("To")});
    m_goalTable->verticalHeader()->hide();
    m_goalTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_goalTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_goalTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_goalTable->horizontalHeader()->setSectionResizeMode(GoalMeasurement, QHeaderView::Stretch);
    m_goalTable->setItemDelegate(new MeasurementDelegate(m_completions, m_goalTable));
    m_goalTable->setToolTip(tr("From and To are the band in which the goal applies (such as 2.4GHz); empty: the whole sweep"));
    m_addGoal = tool(tr("Add Goal"), tr("Add a goal on a measurement"));
    m_addGoal->setObjectName(QStringLiteral("addGoal"));
    connect(m_addGoal, &QToolButton::clicked, this, [this] {
        rf::Goal goal;
        goal.measurement = QStringLiteral("dB(S11)");
        goal.target = -15;
        addGoal(goal);
        m_goalTable->setCurrentCell(m_goalTable->rowCount() - 1, GoalMeasurement);
        m_goalTable->editItem(m_goalTable->item(m_goalTable->rowCount() - 1, GoalMeasurement));
    });
    auto *removeGoal = tool(tr("Remove"), tr("Remove the selected goal"));
    connect(removeGoal, &QToolButton::clicked, this, [this] {
        const int row = m_goalTable->currentRow();
        if (row >= 0)
            m_goalTable->removeRow(row);
        updateButtons();
    });
    auto *goalHeader = new QHBoxLayout;
    goalHeader->addWidget(title(tr("Goals")));
    goalHeader->addStretch(1);
    goalHeader->addWidget(m_addGoal);
    goalHeader->addWidget(removeGoal);

    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addLayout(variableHeader);
    leftLayout->addWidget(m_variableTable, 1);
    leftLayout->addLayout(goalHeader);
    leftLayout->addWidget(m_goalTable, 1);

    // Progress.
    m_progressLabel = new QLabel(tr("Not run yet."), this);
    m_progressLabel->setObjectName(QStringLiteral("optimizationProgress"));
    m_progressBar = new QProgressBar(this);
    m_progressBar->setTextVisible(false);
    m_progressBar->setMaximumHeight(12);
    m_costPlot = new PlotWidget(this);
    m_costPlot->setObjectName(QStringLiteral("costPlot"));
    m_costPlot->setTitle(tr("Best cost"));
    m_costPlot->setXAxis(tr("Evaluation"), {});
    m_costPlot->setYAxis(tr("Cost"), {}, true);
    m_costPlot->setLegendVisible(false);
    m_costPlot->setMarkerTableVisible(false);
    m_costPlot->setMinimumHeight(170);
    TraceStyle style;
    style.lineWidth = 2;
    m_costTrace = m_costPlot->addTrace(tr("Best cost"), {}, style);
    m_bestTable = new QTableWidget(0, 2, this);
    m_bestTable->setObjectName(QStringLiteral("bestValues"));
    m_bestTable->setHorizontalHeaderLabels({tr("Variable"), tr("Best so far")});
    m_goalStatusTable = new QTableWidget(0, 3, this);
    m_goalStatusTable->setObjectName(QStringLiteral("goalStatus"));
    m_goalStatusTable->setHorizontalHeaderLabels({tr("Goal"), tr("Worst value"), tr("Status")});
    for (QTableWidget *table : {m_bestTable, m_goalStatusTable}) {
        table->verticalHeader()->hide();
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    }
    auto *progressPage = new QWidget(this);
    auto *progressLayout = new QVBoxLayout(progressPage);
    progressLayout->setContentsMargins(4, 4, 4, 4);
    progressLayout->addWidget(m_progressLabel);
    progressLayout->addWidget(m_progressBar);
    progressLayout->addWidget(m_costPlot, 3);
    auto *tables = new QHBoxLayout;
    tables->addWidget(m_bestTable, 2);
    tables->addWidget(m_goalStatusTable, 3);
    progressLayout->addLayout(tables, 2);

    // Results.
    m_resultSummary = new QLabel(this);
    m_resultSummary->setObjectName(QStringLiteral("optimizationSummary"));
    m_resultSummary->setWordWrap(true);
    m_resultsTable = new QTableWidget(0, 4, this);
    m_resultsTable->setObjectName(QStringLiteral("optimizationResults"));
    m_resultsTable->setHorizontalHeaderLabels({QString(), tr("Initial"), tr("Best"), tr("Snapped")});
    m_resultsTable->verticalHeader()->hide();
    m_resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto *resultsPage = new QWidget(this);
    auto *resultsLayout = new QVBoxLayout(resultsPage);
    resultsLayout->setContentsMargins(4, 4, 4, 4);
    resultsLayout->addWidget(m_resultSummary);
    resultsLayout->addWidget(m_resultsTable, 1);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("optimizationTabs"));
    m_tabs->addTab(progressPage, tr("Progress"));
    m_tabs->addTab(resultsPage, tr("Results"));

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(left);
    splitter->addWidget(m_tabs);
    splitter->setStretchFactor(0, 5);
    splitter->setStretchFactor(1, 6);
    splitter->setChildrenCollapsible(false);
    splitter->setSizes({580, 600});

    // Buttons.
    m_message = new QLabel(this);
    m_message->setObjectName(QStringLiteral("optimizationMessage"));
    m_message->setWordWrap(true);
    m_saveButton = new QPushButton(tr("Save Setup"), this);
    m_saveButton->setToolTip(tr("Store the setup with the schematic (undoable)"));
    m_runButton = new QPushButton(tr("Run"), this);
    m_runButton->setObjectName(QStringLiteral("runOptimization"));
    m_runButton->setToolTip(tr("Store the setup and optimise a copy of the circuit in the background"));
    m_stopButton = new QPushButton(tr("Stop"), this);
    m_stopButton->setObjectName(QStringLiteral("stopOptimization"));
    m_applyChoice = new QComboBox(this);
    m_applyChoice->setObjectName(QStringLiteral("applyChoice"));
    m_applyChoice->addItem(tr("Best values"), false);
    m_applyChoice->addItem(tr("Snapped to standard values"), true);
    m_applyButton = new QPushButton(tr("Apply"), this);
    m_applyButton->setObjectName(QStringLiteral("applyOptimization"));
    m_applyButton->setToolTip(tr("Write the chosen values into the schematic (one undo step) and run the analysis"));
    m_discardButton = new QPushButton(tr("Discard"), this);
    m_discardButton->setToolTip(tr("Forget the result; the schematic stays as it is"));
    m_closeButton = new QPushButton(tr("Close"), this);
    connect(m_saveButton, &QPushButton::clicked, this, [this] {
        if (!store().isEmpty())
            showMessage(tr("Setup stored with the schematic."), false);
    });
    connect(m_runButton, &QPushButton::clicked, this, &OptimizationDialog::start);
    connect(m_stopButton, &QPushButton::clicked, this, &OptimizationDialog::cancel);
    connect(m_applyButton, &QPushButton::clicked, this, [this] { apply(m_applyChoice->currentData().toBool()); });
    connect(m_discardButton, &QPushButton::clicked, this, &OptimizationDialog::discard);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::close);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_saveButton);
    buttons->addWidget(m_runButton);
    buttons->addWidget(m_stopButton);
    buttons->addStretch(1);
    buttons->addWidget(new QLabel(tr("Apply:"), this));
    buttons->addWidget(m_applyChoice);
    buttons->addWidget(m_applyButton);
    buttons->addWidget(m_discardButton);
    buttons->addSpacing(12);
    buttons->addWidget(m_closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(splitter, 1);
    layout->addWidget(m_message);
    layout->addLayout(buttons);
    updateButtons();
}

void OptimizationDialog::refreshAnalyses()
{
    const QString current = m_analysisCombo->currentData().toString();
    const QSignalBlocker blocker(m_analysisCombo);
    m_analysisCombo->clear();
    if (!m_schematic)
        return;
    for (const rf::LinearAnalysisSetup &analysis : rf::linearAnalyses(m_schematic->simulation()))
        m_analysisCombo->addItem(analysis.name.isEmpty() ? analysis.id : analysis.name, analysis.id);
    const int index = m_analysisCombo->findData(current);
    m_analysisCombo->setCurrentIndex(index >= 0 ? index : 0);
}

void OptimizationDialog::refreshSetups()
{
    const QSignalBlocker blocker(m_setupCombo);
    m_setupCombo->clear();
    if (m_schematic) {
        for (const rf::OptimizationSetup &setup : rf::optimizationSetups(m_schematic->simulation()))
            m_setupCombo->addItem(setup.name.isEmpty() ? setup.id : setup.name, setup.id);
    }
    m_setupCombo->addItem(tr("New setup"), QString());
    m_setupCombo->setCurrentIndex(std::max(0, m_setupCombo->findData(m_setupId)));
}

void OptimizationDialog::refreshCompletions()
{
    std::vector<int> ports;
    bool noise = false;
    if (m_project && m_document && m_document->schematic())
        ports = sim::netlistPortNumbers(buildNetlist(*m_project, *m_document));
    if (m_schematic) {
        for (const rf::LinearAnalysisSetup &analysis : rf::linearAnalyses(m_schematic->simulation())) {
            if (analysis.id == m_analysisCombo->currentData().toString())
                noise = analysis.noise;
        }
    }
    m_completions->setStringList(sim::measurementCompletions(ports.empty() ? std::vector<int>{1, 2} : ports, noise));
}

void OptimizationDialog::fillAddMenu()
{
    sim::fillDesignVariableMenu(m_addMenu, m_project, m_document, setup().variables,
                                [this](const rf::DesignVariable &variable) { addVariable(variable); });
}

// ---------------------------------------------------------------- setup

void OptimizationDialog::selectSetup(const QString &id)
{
    if (isRunning())
        return;
    rf::OptimizationSetup chosen;
    bool found = false;
    if (m_schematic && !id.isEmpty()) {
        for (const rf::OptimizationSetup &setup : rf::optimizationSetups(m_schematic->simulation())) {
            if (setup.id == id) {
                chosen = setup;
                found = true;
            }
        }
    }
    if (!found) {
        const int count = m_schematic ? int(rf::optimizationSetups(m_schematic->simulation()).size()) : 0;
        chosen.name = tr("Optimisation %1").arg(count + 1);
        chosen.analysis = m_analysisCombo->currentData().toString();
    }
    m_setupId = found ? id : QString();
    setSetup(chosen);
    {
        const QSignalBlocker blocker(m_setupCombo);
        m_setupCombo->setCurrentIndex(std::max(0, m_setupCombo->findData(m_setupId)));
    }
    if (found && m_result)
        discard();
    updateButtons();
}

void OptimizationDialog::setSetup(const rf::OptimizationSetup &setup)
{
    m_name->setText(setup.name);
    const int analysis = m_analysisCombo->findData(setup.analysis);
    if (analysis >= 0)
        m_analysisCombo->setCurrentIndex(analysis);
    m_methodCombo->setCurrentIndex(std::max(0, m_methodCombo->findData(static_cast<int>(setup.method))));
    m_evaluations->setValue(setup.maxEvaluations);
    m_variableTable->setRowCount(0);
    for (const rf::DesignVariable &variable : setup.variables)
        appendVariableRow(variable);
    m_goalTable->setRowCount(0);
    for (const rf::Goal &goal : setup.goals)
        appendGoalRow(goal);
    refreshCompletions();
    updateButtons();
}

bool OptimizationDialog::addVariable(const rf::DesignVariable &variable)
{
    for (const rf::DesignVariable &existing : setup().variables) {
        if (sim::sameDesignVariable(existing, variable))
            return false;
    }
    rf::DesignVariable added = variable;
    if (!(added.maximum > added.minimum) && m_project && m_document) {
        if (const auto value = rf::currentValue(*m_project, *m_document, added))
            sim::setDefaultRange(added, *value);
    }
    if (added.unit.isEmpty() && added.kind == rf::DesignVariable::Kind::Parameter && m_project && m_document) {
        for (const sim::DesignCandidate &candidate : sim::designCandidates(*m_project, *m_document)) {
            if (sim::sameDesignVariable(candidate.variable, added))
                added.unit = candidate.variable.unit;
        }
    }
    appendVariableRow(added);
    updateButtons();
    return true;
}

void OptimizationDialog::addGoal(const rf::Goal &goal)
{
    appendGoalRow(goal);
    updateButtons();
}

void OptimizationDialog::appendVariableRow(const rf::DesignVariable &variable)
{
    const int row = m_variableTable->rowCount();
    m_variableTable->insertRow(row);
    auto *name = readOnly(variable.label());
    name->setData(VariableRole, QJsonDocument(variable.toJson()).toJson(QJsonDocument::Compact));
    m_variableTable->setItem(row, VarName, name);
    QString now = QStringLiteral("–");
    if (m_project && m_document) {
        QString error;
        if (const auto value = rf::currentValue(*m_project, *m_document, variable, &error))
            now = sim::formatValue(*value, variable.unit);
        else
            name->setToolTip(error);
    }
    m_variableTable->setItem(row, VarCurrent, readOnly(now));
    m_variableTable->setItem(row, VarMin, numberItem(variable.minimum, sim::formatValue(variable.minimum, variable.unit)));
    m_variableTable->setItem(row, VarMax, numberItem(variable.maximum, sim::formatValue(variable.maximum, variable.unit)));
    auto *log = new QTableWidgetItem;
    log->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
    log->setCheckState(variable.logarithmic ? Qt::Checked : Qt::Unchecked);
    log->setToolTip(tr("Search on a logarithmic scale (needs a positive minimum)"));
    m_variableTable->setItem(row, VarLog, log);
    auto *series = new QComboBox;
    series->setFrame(false);
    series->addItem(sim::seriesLabel(std::nullopt), QString());
    for (ESeries s : sim::offeredSeries())
        series->addItem(seriesName(s), seriesName(s));
    if (variable.series) {
        if (series->findData(seriesName(*variable.series)) < 0)
            series->addItem(seriesName(*variable.series), seriesName(*variable.series));
        series->setCurrentIndex(series->findData(seriesName(*variable.series)));
    }
    series->setToolTip(tr("Snap the result to a standard series"));
    m_variableTable->setCellWidget(row, VarSeries, series);
}

void OptimizationDialog::appendGoalRow(const rf::Goal &goal)
{
    const int row = m_goalTable->rowCount();
    m_goalTable->insertRow(row);
    m_goalTable->setItem(row, GoalMeasurement, new QTableWidgetItem(goal.measurement));
    auto *type = new QComboBox;
    type->setFrame(false);
    type->addItem(tr("below"), static_cast<int>(rf::Goal::Type::Below));
    type->addItem(tr("above"), static_cast<int>(rf::Goal::Type::Above));
    type->addItem(tr("equal to"), static_cast<int>(rf::Goal::Type::Equal));
    type->addItem(tr("minimise"), static_cast<int>(rf::Goal::Type::Minimize));
    type->addItem(tr("maximise"), static_cast<int>(rf::Goal::Type::Maximize));
    type->setCurrentIndex(std::max(0, type->findData(static_cast<int>(goal.type))));
    m_goalTable->setCellWidget(row, GoalType, type);
    m_goalTable->setItem(row, GoalTarget, numberItem(goal.target, numberText(goal.target, 8)));
    m_goalTable->setItem(row, GoalWeight, numberItem(goal.weight, numberText(goal.weight, 8)));
    const auto band = [](double frequency) {
        return numberItem(frequency, std::isfinite(frequency) ? formatEngineering(frequency, QStringLiteral("Hz")) : QString());
    };
    m_goalTable->setItem(row, GoalBandStart, band(goal.bandStart));
    m_goalTable->setItem(row, GoalBandStop, band(goal.bandStop));
    const auto targetEnabled = [this, type] {
        const auto t = static_cast<rf::Goal::Type>(type->currentData().toInt());
        const bool enabled = t != rf::Goal::Type::Minimize && t != rf::Goal::Type::Maximize;
        int at = -1; // rows move when others are removed
        for (int r = 0; r < m_goalTable->rowCount() && at < 0; ++r) {
            if (m_goalTable->cellWidget(r, GoalType) == type)
                at = r;
        }
        if (QTableWidgetItem *target = at >= 0 ? m_goalTable->item(at, GoalTarget) : nullptr) {
            target->setFlags(enabled ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable
                                     : Qt::ItemIsSelectable);
        }
    };
    targetEnabled();
    connect(type, &QComboBox::currentIndexChanged, this, targetEnabled);
}

rf::OptimizationSetup OptimizationDialog::setup() const
{
    rf::OptimizationSetup setup;
    setup.id = m_setupId;
    setup.name = m_name->text().trimmed();
    setup.analysis = m_analysisCombo->currentData().toString();
    setup.method = static_cast<rf::OptimizationSetup::Method>(m_methodCombo->currentData().toInt());
    setup.maxEvaluations = m_evaluations->value();
    if (m_schematic) {
        for (const rf::OptimizationSetup &stored : rf::optimizationSetups(m_schematic->simulation())) {
            if (stored.id == m_setupId)
                setup.tolerance = stored.tolerance;
        }
    }
    for (int row = 0; row < m_variableTable->rowCount(); ++row) {
        const QTableWidgetItem *name = m_variableTable->item(row, VarName);
        if (!name)
            continue;
        rf::DesignVariable variable =
            rf::DesignVariable::fromJson(QJsonDocument::fromJson(name->data(VariableRole).toByteArray()).object());
        variable.minimum = numberOf(m_variableTable, row, VarMin, rf::NotSet);
        variable.maximum = numberOf(m_variableTable, row, VarMax, rf::NotSet);
        variable.logarithmic = m_variableTable->item(row, VarLog)->checkState() == Qt::Checked;
        if (auto *series = qobject_cast<QComboBox *>(m_variableTable->cellWidget(row, VarSeries)))
            variable.series = seriesFromName(series->currentData().toString());
        setup.variables.append(variable);
    }
    for (int row = 0; row < m_goalTable->rowCount(); ++row) {
        rf::Goal goal;
        goal.measurement = textOf(m_goalTable, row, GoalMeasurement);
        if (auto *type = qobject_cast<QComboBox *>(m_goalTable->cellWidget(row, GoalType)))
            goal.type = static_cast<rf::Goal::Type>(type->currentData().toInt());
        goal.target = numberOf(m_goalTable, row, GoalTarget, rf::NotSet);
        if (goal.type == rf::Goal::Type::Minimize || goal.type == rf::Goal::Type::Maximize)
            goal.target = std::isfinite(goal.target) ? goal.target : 0.0;
        goal.weight = numberOf(m_goalTable, row, GoalWeight, rf::NotSet);
        // Empty: the whole sweep. Text that is not a number becomes -1,
        // which validate() reports.
        const auto band = [&](int column) {
            const double value = numberOf(m_goalTable, row, column, INFINITY);
            return std::isinf(value) ? rf::NotSet : (std::isfinite(value) ? value : -1.0);
        };
        goal.bandStart = band(GoalBandStart);
        goal.bandStop = band(GoalBandStop);
        setup.goals.append(goal);
    }
    return setup;
}

QStringList OptimizationDialog::validate() const
{
    QStringList errors;
    const rf::OptimizationSetup s = setup();
    if (s.analysis.isEmpty())
        errors.append(tr("The schematic has no linear analysis; create one first."));
    if (s.variables.isEmpty())
        errors.append(tr("Add at least one variable."));
    if (s.goals.isEmpty())
        errors.append(tr("Add at least one goal."));
    for (const rf::DesignVariable &variable : s.variables) {
        if (!std::isfinite(variable.minimum) || !std::isfinite(variable.maximum))
            errors.append(tr("%1: the minimum and the maximum must be numbers.").arg(variable.label()));
        else if (!(variable.maximum > variable.minimum))
            errors.append(tr("%1: the maximum must exceed the minimum.").arg(variable.label()));
    }
    for (const rf::Goal &goal : s.goals) {
        const QString name = goal.measurement.isEmpty() ? tr("A goal") : goal.measurement;
        if (goal.measurement.isEmpty())
            errors.append(tr("A goal has no measurement."));
        if (!std::isfinite(goal.target))
            errors.append(tr("%1: the target must be a number.").arg(name));
        if (!std::isfinite(goal.weight) || goal.weight <= 0)
            errors.append(tr("%1: the weight must be a positive number.").arg(name));
        if (goal.bandStart < 0 || goal.bandStop < 0)
            errors.append(tr("%1: the band limits must be frequencies, such as 2.4GHz.").arg(name));
        else if (std::isfinite(goal.bandStart) && std::isfinite(goal.bandStop) && goal.bandStop < goal.bandStart)
            errors.append(tr("%1: the band ends before it starts.").arg(name));
    }
    return errors;
}

QString OptimizationDialog::store()
{
    if (!m_schematic)
        return {};
    const QStringList errors = validate();
    if (!errors.isEmpty()) {
        showMessage(errors.join(QLatin1Char('\n')), true);
        return {};
    }
    const rf::OptimizationSetup s = setup();
    for (const rf::OptimizationSetup &stored : rf::optimizationSetups(m_schematic->simulation())) {
        if (stored.id == s.id && !s.id.isEmpty() && stored.toJson() == s.toJson())
            return s.id; // unchanged: no undo step
    }
    QString id;
    m_schematic->setSimulation(rf::withOptimizationSetup(m_schematic->simulation(), s, &id));
    m_setupId = id;
    refreshSetups();
    return id;
}

// ---------------------------------------------------------------- running

bool OptimizationDialog::start()
{
    if (isRunning() || !m_project || !m_document)
        return false;
    const QString id = store();
    if (id.isEmpty())
        return false;
    const rf::OptimizationSetup s = setup();
    m_runVariables = s.variables;
    m_runAnalysis = s.analysis;
    m_result.reset();
    m_applied = false;
    // The snapshot reads the documents: GUI thread. From here on only the
    // worker touches it.
    std::shared_ptr<Project> snapshot(m_project->snapshot().release());
    if (!m_runner->start(std::move(snapshot), m_document->name(), s))
        return false;
    m_costPlot->setTraceData(m_costTrace, std::span<const double>(), std::span<const double>());
    m_bestTable->setRowCount(0);
    m_goalStatusTable->setRowCount(0);
    m_resultsTable->setRowCount(0);
    m_resultSummary->clear();
    m_progressBar->setRange(0, s.maxEvaluations);
    m_progressBar->setValue(0);
    m_progressLabel->setText(tr("Starting…"));
    m_tabs->setCurrentIndex(0);
    showMessage(tr("Optimising a copy of the circuit; the schematic is not changed until you apply."), false);
    updateButtons();
    return true;
}

void OptimizationDialog::cancel()
{
    if (!isRunning())
        return;
    m_runner->cancel();
    m_progressLabel->setText(tr("Stopping…"));
}

bool OptimizationDialog::isRunning() const
{
    return m_runner->isRunning();
}

void OptimizationDialog::onProgress()
{
    const sim::OptimizationRunner::Progress p = m_runner->progress();
    m_progressBar->setRange(0, std::max(1, p.maxEvaluations));
    m_progressBar->setValue(std::min(p.evaluation, p.maxEvaluations));
    if (isRunning()) {
        m_progressLabel->setText(tr("Evaluation %1 of at most %2 — best cost %3")
                                     .arg(p.evaluation)
                                     .arg(p.maxEvaluations)
                                     .arg(costText(p.bestCost)));
    }
    std::vector<double> x, y;
    for (const QPointF &point : p.history) {
        // A step curve; a zero cost is drawn at the bottom of the log axis.
        if (!y.empty()) {
            x.push_back(point.x());
            y.push_back(y.back());
        }
        x.push_back(point.x());
        y.push_back(std::max(point.y(), 1e-12));
    }
    if (!x.empty() && p.evaluation > x.back()) {
        x.push_back(p.evaluation);
        y.push_back(y.back());
    }
    m_costPlot->setTraceData(m_costTrace, x, y);
    m_costPlot->autoscale();

    m_bestTable->setRowCount(int(m_runVariables.size()));
    for (int i = 0; i < m_runVariables.size(); ++i) {
        const rf::DesignVariable &variable = m_runVariables.at(i);
        m_bestTable->setItem(i, 0, readOnly(variable.label()));
        m_bestTable->setItem(i, 1, readOnly(size_t(i) < p.best.size() ? sim::formatValue(p.best[size_t(i)], variable.unit)
                                                                       : QStringLiteral("–")));
    }
    fillGoalStatus(p.goals);
    Q_EMIT progressed();
}

void OptimizationDialog::fillGoalStatus(const QList<rf::GoalEvaluation> &goals)
{
    m_goalStatusTable->setRowCount(int(goals.size()));
    for (int i = 0; i < goals.size(); ++i) {
        const rf::GoalEvaluation &goal = goals.at(i);
        m_goalStatusTable->setItem(i, 0, readOnly(goal.goal.label()));
        m_goalStatusTable->setItem(i, 1, readOnly(numberText(goal.worst)));
        auto *status = readOnly(goal.error.isEmpty() ? (goal.met ? tr("met") : tr("not met")) : tr("error"));
        status->setForeground(goal.met && goal.error.isEmpty() ? goodColor() : badColor());
        if (!goal.error.isEmpty())
            status->setToolTip(goal.error);
        m_goalStatusTable->setItem(i, 2, status);
    }
}

void OptimizationDialog::onFinished(std::shared_ptr<const rf::OptimizationResult> result)
{
    m_result = std::move(result);
    onProgress();
    m_progressBar->setValue(m_progressBar->maximum()); // finished, whatever the budget
    showResult();
    if (m_result->ok())
        m_tabs->setCurrentIndex(1);
    updateButtons();
    Q_EMIT finished(m_result->ok());
}

void OptimizationDialog::showResult()
{
    m_resultsTable->setRowCount(0);
    if (!m_result)
        return;
    const rf::OptimizationResult &r = *m_result;
    QString summary;
    if (!r.ok()) {
        summary = tr("The optimisation failed: %1").arg(r.errors.join(QStringLiteral("; ")));
        m_progressLabel->setText(tr("Failed."));
        showMessage(summary, true);
    } else {
        if (r.cancelled)
            summary = tr("Stopped after %1 evaluations.").arg(r.evaluations);
        else if (r.converged)
            summary = tr("Converged after %1 evaluations.").arg(r.evaluations);
        else
            summary = tr("Reached the limit of %1 evaluations.").arg(r.evaluations);
        bool allMet = !r.goalsSnapped.isEmpty();
        for (const rf::GoalEvaluation &goal : r.goalsSnapped)
            allMet = allMet && goal.met && goal.error.isEmpty();
        summary += QLatin1Char(' ') + (allMet ? tr("All goals are met with the snapped values.")
                                              : tr("Not every goal is met with the snapped values."));
        m_progressLabel->setText(summary);
        showMessage(r.warnings.join(QLatin1Char('\n')), false);
    }
    m_resultSummary->setText(summary);
    if (!r.ok())
        return;

    const auto addRow = [this](const QString &label, const QStringList &cells, const QList<int> &state = {}) {
        const int row = m_resultsTable->rowCount();
        m_resultsTable->insertRow(row);
        m_resultsTable->setItem(row, 0, readOnly(label));
        for (int c = 0; c < cells.size(); ++c) {
            auto *item = readOnly(cells.at(c));
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (c < state.size() && state.at(c) != 0)
                item->setForeground(state.at(c) > 0 ? goodColor() : badColor());
            m_resultsTable->setItem(row, c + 1, item);
        }
    };
    for (int i = 0; i < m_runVariables.size(); ++i) {
        const rf::DesignVariable &variable = m_runVariables.at(i);
        const auto cell = [&](const std::vector<double> &values) {
            return size_t(i) < values.size() ? sim::formatValue(values[size_t(i)], variable.unit) : QStringLiteral("–");
        };
        QString label = variable.label();
        if (variable.series)
            label += QStringLiteral(" (%1)").arg(seriesName(*variable.series));
        addRow(label, {cell(r.initial), cell(r.best), cell(r.snapped)});
    }
    addRow(tr("Cost"), {costText(r.initialCost), costText(r.bestCost), costText(r.snappedCost)});
    for (int g = 0; g < r.goalsBest.size(); ++g) {
        QStringList cells;
        QList<int> state;
        for (const QList<rf::GoalEvaluation> *goals : {&r.goalsInitial, &r.goalsBest, &r.goalsSnapped}) {
            if (g >= goals->size()) {
                cells.append(QStringLiteral("–"));
                state.append(0);
                continue;
            }
            const rf::GoalEvaluation &goal = goals->at(g);
            const bool met = goal.met && goal.error.isEmpty();
            cells.append(goal.error.isEmpty() ? QStringLiteral("%1 %2").arg(numberText(goal.worst), met ? QStringLiteral("✓")
                                                                                                         : QStringLiteral("✗"))
                                              : tr("error"));
            state.append(met ? 1 : -1);
        }
        addRow(r.goalsBest.at(g).goal.label(), cells, state);
    }
    bool anySeries = false;
    for (const rf::DesignVariable &variable : std::as_const(m_runVariables))
        anySeries = anySeries || variable.series.has_value();
    m_applyChoice->setCurrentIndex(anySeries ? 1 : 0);
}

bool OptimizationDialog::apply(bool snapped)
{
    if (!m_result || !m_result->ok() || !m_project || !m_document || isRunning())
        return false;
    const std::vector<double> &values = snapped ? m_result->snapped : m_result->best;
    QString error;
    if (!sim::applyDesignValuesUndoable(*m_project, *m_document, m_runVariables, values,
                                        tr("Apply optimised values"), &error)) {
        showMessage(tr("The values could not be applied: %1").arg(error), true);
        return false;
    }
    m_applied = true;
    showMessage(snapped ? tr("The snapped values were written into the schematic.")
                        : tr("The best values were written into the schematic."),
                false);
    // The "Now" column follows the document.
    for (int row = 0; row < m_variableTable->rowCount(); ++row) {
        const QTableWidgetItem *name = m_variableTable->item(row, VarName);
        const rf::DesignVariable variable =
            rf::DesignVariable::fromJson(QJsonDocument::fromJson(name->data(VariableRole).toByteArray()).object());
        if (const auto value = rf::currentValue(*m_project, *m_document, variable))
            m_variableTable->item(row, VarCurrent)->setText(sim::formatValue(*value, variable.unit));
    }
    updateButtons();
    Q_EMIT applied(m_runAnalysis);
    return true;
}

void OptimizationDialog::discard()
{
    m_result.reset();
    m_applied = false;
    m_resultsTable->setRowCount(0);
    m_resultSummary->clear();
    showMessage(QString(), false);
    updateButtons();
}

void OptimizationDialog::updateButtons()
{
    const bool running = isRunning();
    const bool haveResult = m_result && m_result->ok();
    m_runButton->setEnabled(!running && !m_schematic.isNull());
    m_saveButton->setEnabled(!running && !m_schematic.isNull());
    m_stopButton->setEnabled(running);
    m_applyButton->setEnabled(haveResult && !m_applied && !running);
    m_applyChoice->setEnabled(haveResult && !running);
    m_discardButton->setEnabled(haveResult && !running);
    m_setupCombo->setEnabled(!running);
    m_deleteSetup->setEnabled(!running && !m_setupId.isEmpty());
    m_variableTable->setEnabled(!running);
    m_goalTable->setEnabled(!running);
    m_addVariable->setEnabled(!running);
    m_addGoal->setEnabled(!running);
}

void OptimizationDialog::showMessage(const QString &text, bool error)
{
    m_message->setText(text);
    m_message->setVisible(!text.isEmpty());
    QPalette pal = m_message->palette();
    pal.setColor(QPalette::WindowText, error ? badColor() : palette().color(QPalette::WindowText));
    m_message->setPalette(pal);
}

QColor OptimizationDialog::goodColor() const
{
    return isDark(palette()) ? QColor(0x6F, 0xD0, 0x86) : QColor(0x1E, 0x7E, 0x34);
}

QColor OptimizationDialog::badColor() const
{
    return isDark(palette()) ? QColor(0xF0, 0x80, 0x80) : QColor(0xB0, 0x20, 0x20);
}

void OptimizationDialog::closeEvent(QCloseEvent *event)
{
    if (isRunning()) {
        m_runner->cancel();
        m_runner->wait();
    }
    QDialog::closeEvent(event);
}

void OptimizationDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_resultsTable) {
        showResult();
        if (m_result)
            fillGoalStatus(m_runner->progress().goals);
    }
}

} // namespace lvd
