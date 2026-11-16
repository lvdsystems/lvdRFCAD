#include "simulation_ui/BudgetDialog.h"

#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "rfsim/Analysis.h"
#include "rfsim/SignalChain.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lvd {

namespace {

const QString AnalysesKey = QStringLiteral("analyses");

QString reference(const QString &entry)
{
    return entry.section(QLatin1Char('.'), 0, 0).trimmed();
}

QString exitPin(const QString &entry)
{
    return entry.section(QLatin1Char('.'), 1).trimmed();
}

} // namespace

BudgetDialog::BudgetDialog(Project *project, Document *document, const QString &analysisId, QWidget *parent)
    : QDialog(parent), m_project(project), m_document(document)
{
    setObjectName(QStringLiteral("budgetDialog"));
    QStringList variableErrors;
    m_scope = document ? rf::sheetScope(*document, &variableErrors) : Expression::Lookup();
    if (project && document && document->schematic())
        m_netlist = buildNetlist(*project, *document);

    rf::BudgetAnalysisSetup initial;
    initial.name = tr("Budget");
    if (document && document->schematic()) {
        for (const rf::BudgetAnalysisSetup &existing : rf::budgetAnalyses(document->schematic()->simulation())) {
            if (!analysisId.isEmpty() && existing.id == analysisId) {
                initial = existing;
                m_isNew = false;
            }
        }
    }
    m_id = m_isNew ? QString() : initial.id;
    setWindowTitle(m_isNew ? tr("New RF budget analysis") : tr("Edit budget analysis – %1").arg(initial.name));

    const QString expressionTip = tr("A value or an expression with the schematic's variables");
    const auto field = [this](const QString &name, const QString &tip, const QString &placeholder = {}) {
        auto *edit = new QLineEdit(this);
        edit->setObjectName(name);
        edit->setToolTip(tip);
        edit->setPlaceholderText(placeholder);
        return edit;
    };
    const auto valueLabel = [this](const QString &name) {
        auto *label = new QLabel(this);
        label->setObjectName(name);
        label->setMinimumWidth(150);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };
    const auto toolButton = [this](const QString &text, const QString &tip, const QString &name) {
        auto *button = new QToolButton(this);
        button->setText(text);
        button->setToolTip(tip);
        button->setObjectName(name);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        return button;
    };

    m_name = field(QStringLiteral("name"), tr("Name of the analysis"));

    // Start and path.
    m_start = new QComboBox(this);
    m_start->setObjectName(QStringLiteral("start"));
    m_start->addItem(tr("Automatic (source, else port 1, else antenna)"), QString());
    for (const FlatComponent &component : std::as_const(m_netlist.components)) {
        const QString kind = kindOf(component);
        if (kind == QLatin1String("source") || kind == QLatin1String("port") || kind == QLatin1String("antenna"))
            m_start->addItem(QStringLiteral("%1 (%2)").arg(component.path, kind), component.path);
    }

    auto *pathGroup = new QGroupBox(tr("Signal path"), this);
    auto *pathHint = new QLabel(tr("Empty: traced automatically. Otherwise the components in order from the start; "
                                   "choose a branch with the exit pin (REF.pin) at splitters, couplers and switches."),
                                pathGroup);
    pathHint->setWordWrap(true);
    m_path = new QListWidget(pathGroup);
    m_path->setObjectName(QStringLiteral("path"));
    m_path->setMinimumHeight(110);
    m_traceButton = toolButton(tr("Trace"), tr("Fill the path from the automatic trace"), QStringLiteral("trace"));
    m_removeButton = toolButton(tr("Remove"), tr("Remove the selected entry"), QStringLiteral("removeEntry"));
    m_upButton = toolButton(tr("Up"), tr("Move the selected entry up"), QStringLiteral("entryUp"));
    m_downButton = toolButton(tr("Down"), tr("Move the selected entry down"), QStringLiteral("entryDown"));
    m_clearButton = toolButton(tr("Clear"), tr("Remove every entry: trace automatically"), QStringLiteral("clearPath"));
    auto *add = field(QStringLiteral("addEntry"), tr("Type a reference (or REF.pin) and press Enter"),
                      tr("Add: REF or REF.pin"));
    m_exitPin = new QComboBox(pathGroup);
    m_exitPin->setObjectName(QStringLiteral("exitPin"));
    m_chainLabel = new QLabel(pathGroup);
    m_chainLabel->setObjectName(QStringLiteral("chain"));
    m_chainLabel->setWordWrap(true);
    m_chainLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *buttonColumn = new QVBoxLayout;
    for (QToolButton *button : {m_traceButton, m_removeButton, m_upButton, m_downButton, m_clearButton})
        buttonColumn->addWidget(button);
    buttonColumn->addStretch(1);
    auto *listRow = new QHBoxLayout;
    listRow->addWidget(m_path, 1);
    listRow->addLayout(buttonColumn);
    auto *pinRow = new QHBoxLayout;
    pinRow->addWidget(add, 1);
    pinRow->addSpacing(8);
    pinRow->addWidget(new QLabel(tr("Exit pin:"), pathGroup));
    pinRow->addWidget(m_exitPin);
    auto *pathLayout = new QVBoxLayout(pathGroup);
    pathLayout->addWidget(pathHint);
    pathLayout->addLayout(listRow);
    pathLayout->addLayout(pinRow);
    pathLayout->addWidget(m_chainLabel);

    // Signal.
    auto *signalGroup = new QGroupBox(tr("Signal"), this);
    m_frequency = field(QStringLiteral("frequency"), expressionTip, tr("from the start component"));
    m_power = field(QStringLiteral("power"), tr("Input power in dBm; %1").arg(expressionTip), tr("from the start component"));
    m_bandwidth = field(QStringLiteral("bandwidth"), tr("Noise bandwidth; %1").arg(expressionTip));
    m_frequencyValue = valueLabel(QStringLiteral("frequencyValue"));
    m_powerValue = valueLabel(QStringLiteral("powerValue"));
    m_bandwidthValue = valueLabel(QStringLiteral("bandwidthValue"));
    m_mismatch = new QComboBox(signalGroup);
    m_mismatch->setObjectName(QStringLiteral("mismatch"));
    m_mismatch->addItem(tr("Actual S-parameters"), true);
    m_mismatch->addItem(tr("Ideal matched cascade"), false);
    m_mismatchHint = new QLabel(signalGroup);
    m_mismatchHint->setObjectName(QStringLiteral("mismatchHint"));
    m_mismatchHint->setWordWrap(true);
    auto *signalGrid = new QGridLayout(signalGroup);
    signalGrid->addWidget(new QLabel(tr("Frequency:"), signalGroup), 0, 0);
    signalGrid->addWidget(m_frequency, 0, 1);
    signalGrid->addWidget(m_frequencyValue, 0, 2);
    signalGrid->addWidget(new QLabel(tr("Power:"), signalGroup), 1, 0);
    signalGrid->addWidget(m_power, 1, 1);
    signalGrid->addWidget(m_powerValue, 1, 2);
    signalGrid->addWidget(new QLabel(tr("Bandwidth:"), signalGroup), 2, 0);
    signalGrid->addWidget(m_bandwidth, 2, 1);
    signalGrid->addWidget(m_bandwidthValue, 2, 2);
    signalGrid->addWidget(new QLabel(tr("Mismatch:"), signalGroup), 3, 0);
    signalGrid->addWidget(m_mismatch, 3, 1);
    signalGrid->addWidget(m_mismatchHint, 4, 1, 1, 2);
    signalGrid->setColumnStretch(1, 1);

    // Swept budget.
    m_sweepGroup = new QGroupBox(tr("Swept budget (input frequency)"), this);
    m_sweepGroup->setObjectName(QStringLiteral("sweep"));
    m_sweepGroup->setCheckable(true);
    m_sweepStart = field(QStringLiteral("sweepStart"), expressionTip);
    m_sweepStop = field(QStringLiteral("sweepStop"), expressionTip);
    m_sweepPoints = field(QStringLiteral("sweepPoints"), tr("Number of points, 1 to 10,000"));
    m_sweepStartValue = valueLabel(QStringLiteral("sweepStartValue"));
    m_sweepStopValue = valueLabel(QStringLiteral("sweepStopValue"));
    m_sweepPointsValue = valueLabel(QStringLiteral("sweepPointsValue"));
    auto *sweepGrid = new QGridLayout(m_sweepGroup);
    sweepGrid->addWidget(new QLabel(tr("Start:"), m_sweepGroup), 0, 0);
    sweepGrid->addWidget(m_sweepStart, 0, 1);
    sweepGrid->addWidget(m_sweepStartValue, 0, 2);
    sweepGrid->addWidget(new QLabel(tr("Stop:"), m_sweepGroup), 1, 0);
    sweepGrid->addWidget(m_sweepStop, 1, 1);
    sweepGrid->addWidget(m_sweepStopValue, 1, 2);
    sweepGrid->addWidget(new QLabel(tr("Points:"), m_sweepGroup), 2, 0);
    sweepGrid->addWidget(m_sweepPoints, 2, 1);
    sweepGrid->addWidget(m_sweepPointsValue, 2, 2);
    sweepGrid->setColumnStretch(1, 1);

    // Spurs.
    m_spurGroup = new QGroupBox(tr("Mixer spur analysis"), this);
    m_spurGroup->setObjectName(QStringLiteral("spurs"));
    m_spurGroup->setCheckable(true);
    m_spurOrder = field(QStringLiteral("spurOrder"), tr("Largest |m| + |n|, 2 to 15"));
    m_spurOrderValue = valueLabel(QStringLiteral("spurOrderValue"));
    auto *spurGrid = new QGridLayout(m_spurGroup);
    spurGrid->addWidget(new QLabel(tr("Maximum order:"), m_spurGroup), 0, 0);
    spurGrid->addWidget(m_spurOrder, 0, 1);
    spurGrid->addWidget(m_spurOrderValue, 0, 2);
    spurGrid->setColumnStretch(1, 1);

    m_variablesLabel = new QLabel(this);
    m_variablesLabel->setWordWrap(true);
    m_variablesLabel->setVisible(!variableErrors.isEmpty());
    m_variablesLabel->setText(tr("Variable problems: %1").arg(variableErrors.join(QStringLiteral("; "))));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(m_isNew ? tr("Create") : tr("OK"));
    connect(buttons, &QDialogButtonBox::accepted, this, &BudgetDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &BudgetDialog::reject);

    auto *form = new QFormLayout;
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Start component:"), m_start);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(pathGroup);
    layout->addWidget(signalGroup);
    layout->addWidget(m_sweepGroup);
    layout->addWidget(m_spurGroup);
    layout->addWidget(m_variablesLabel);
    layout->addStretch();
    layout->addWidget(buttons);

    setSetup(initial);

    for (QLineEdit *edit : {m_name, m_frequency, m_power, m_bandwidth, m_sweepStart, m_sweepStop, m_sweepPoints, m_spurOrder})
        connect(edit, &QLineEdit::textChanged, this, &BudgetDialog::validate);
    connect(m_start, &QComboBox::currentIndexChanged, this, &BudgetDialog::validate);
    connect(m_mismatch, &QComboBox::currentIndexChanged, this, &BudgetDialog::validate);
    connect(m_sweepGroup, &QGroupBox::toggled, this, &BudgetDialog::validate);
    connect(m_spurGroup, &QGroupBox::toggled, this, &BudgetDialog::validate);
    connect(m_path, &QListWidget::currentRowChanged, this, &BudgetDialog::updatePathControls);
    connect(m_path, &QListWidget::itemChanged, this, [this] {
        if (!m_updating)
            validate();
    });
    connect(m_traceButton, &QToolButton::clicked, this, &BudgetDialog::tracePath);
    connect(m_removeButton, &QToolButton::clicked, this, [this] {
        delete m_path->takeItem(m_path->currentRow());
        validate();
    });
    connect(m_clearButton, &QToolButton::clicked, this, [this] {
        m_path->clear();
        validate();
    });
    const auto move = [this](int delta) {
        const int row = m_path->currentRow();
        const int target = row + delta;
        if (row < 0 || target < 0 || target >= m_path->count())
            return;
        QListWidgetItem *item = m_path->takeItem(row);
        m_path->insertItem(target, item);
        m_path->setCurrentRow(target);
        validate();
    };
    connect(m_upButton, &QToolButton::clicked, this, [move] { move(-1); });
    connect(m_downButton, &QToolButton::clicked, this, [move] { move(1); });
    connect(add, &QLineEdit::returnPressed, this, [this, add] {
        const QString text = add->text().trimmed();
        if (text.isEmpty())
            return;
        addPathEntry(text);
        m_path->setCurrentRow(m_path->count() - 1);
        add->clear();
        validate();
    });
    connect(m_exitPin, &QComboBox::activated, this,
            [this](int index) { setExitPin(m_path->currentRow(), m_exitPin->itemData(index).toString()); });
    resize(600, 720);
}

QString BudgetDialog::kindOf(const FlatComponent &component) const
{
    if (!component.definition)
        return {};
    if (const ModelDefinition *model = component.definition->model(QStringLiteral("rf-behavioural")))
        return model->data.value(QStringLiteral("block")).toString();
    if (const ModelDefinition *model = component.definition->model(QStringLiteral("rf-element")))
        return model->data.value(QStringLiteral("element")).toString();
    return {};
}

QStringList BudgetDialog::startCandidates() const
{
    QStringList list;
    for (int i = 1; i < m_start->count(); ++i)
        list.append(m_start->itemData(i).toString());
    return list;
}

QStringList BudgetDialog::path() const
{
    QStringList entries;
    for (int i = 0; i < m_path->count(); ++i) {
        const QString text = m_path->item(i)->text().trimmed();
        if (!text.isEmpty())
            entries.append(text);
    }
    return entries;
}

void BudgetDialog::addPathEntry(const QString &text)
{
    auto *item = new QListWidgetItem(text, m_path);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
}

rf::BudgetAnalysisSetup BudgetDialog::setup() const
{
    rf::BudgetAnalysisSetup setup;
    setup.id = m_id;
    setup.name = m_name->text().trimmed();
    setup.path = path();
    // With an explicit path, its first entry is the start (see rf::ChainOptions).
    setup.start = setup.path.isEmpty() ? m_start->currentData().toString() : QString();
    setup.frequency = m_frequency->text().trimmed();
    setup.power = m_power->text().trimmed();
    setup.bandwidth = m_bandwidth->text().trimmed();
    setup.mismatch = m_mismatch->currentData().toBool();
    if (m_sweepGroup->isChecked()) {
        setup.sweepStart = m_sweepStart->text().trimmed();
        setup.sweepStop = m_sweepStop->text().trimmed();
    }
    setup.sweepPoints = m_sweepPoints->text().trimmed();
    setup.spurs = m_spurGroup->isChecked();
    setup.spurOrder = m_spurOrder->text().trimmed();
    return setup;
}

void BudgetDialog::setSetup(const rf::BudgetAnalysisSetup &setup)
{
    m_updating = true;
    m_name->setText(setup.name);
    QStringList entries = setup.path;
    // A start given together with a path is not part of the path: show it as its first entry.
    if (!setup.start.isEmpty() && !entries.isEmpty() && reference(entries.first()) != setup.start)
        entries.prepend(setup.start);
    int startIndex = m_start->findData(setup.start);
    if (startIndex < 0 && !setup.start.isEmpty()) {
        m_start->addItem(setup.start, setup.start);
        startIndex = m_start->count() - 1;
    }
    m_start->setCurrentIndex(std::max(0, startIndex));
    m_path->clear();
    for (const QString &entry : std::as_const(entries))
        addPathEntry(entry);
    m_frequency->setText(setup.frequency);
    m_power->setText(setup.power);
    m_bandwidth->setText(setup.bandwidth);
    m_mismatch->setCurrentIndex(setup.mismatch ? 0 : 1);
    const bool sweep = !setup.sweepStart.isEmpty() || !setup.sweepStop.isEmpty();
    m_sweepGroup->setChecked(sweep);
    m_sweepStart->setText(setup.sweepStart);
    m_sweepStop->setText(setup.sweepStop);
    m_sweepPoints->setText(setup.sweepPoints);
    m_spurGroup->setChecked(setup.spurs);
    m_spurOrder->setText(setup.spurOrder);
    m_updating = false;
    validate();
}

void BudgetDialog::tracePath()
{
    rf::ChainOptions options;
    options.start = m_path->count() > 0 ? reference(m_path->item(0)->text()) : m_start->currentData().toString();
    const rf::SignalChain chain = rf::traceChain(m_netlist, options);
    if (chain.start.isEmpty()) {
        m_chainPreview = chain.errors.value(0, tr("no start component"));
        validate();
        return;
    }
    m_updating = true;
    m_path->clear();
    addPathEntry(chain.start);
    for (const rf::ChainStage &stage : chain.stages) {
        QString entry = stage.path;
        // Branching components get their exit pin, so the choice is visible and editable.
        if (stage.component && stage.component->definition && stage.component->definition->pins.size() > 2
            && stage.translation != rf::ChainStage::Translation::Mixer)
            entry += QLatin1Char('.') + stage.component->definition->pins.value(stage.outputPin);
        addPathEntry(entry);
    }
    if (!chain.end.isEmpty())
        addPathEntry(chain.end);
    m_updating = false;
    validate();
}

void BudgetDialog::setExitPin(int entry, const QString &pin)
{
    if (entry < 0 || entry >= m_path->count())
        return;
    QListWidgetItem *item = m_path->item(entry);
    const QString ref = reference(item->text());
    m_updating = true;
    item->setText(pin.isEmpty() ? ref : QStringLiteral("%1.%2").arg(ref, pin));
    m_updating = false;
    validate();
}

void BudgetDialog::updateExitPins()
{
    const QSignalBlocker blocker(m_exitPin);
    m_exitPin->clear();
    const int row = m_path->currentRow();
    const QString entry = row >= 0 && row < m_path->count() ? m_path->item(row)->text() : QString();
    const FlatComponent *component = entry.isEmpty() ? nullptr : m_netlist.component(reference(entry));
    m_exitPin->addItem(tr("(default)"), QString());
    if (component && component->definition && component->definition->pins.size() > 2) {
        for (const QString &pin : component->definition->pins)
            m_exitPin->addItem(pin, pin);
    }
    m_exitPin->setCurrentIndex(std::max(0, m_exitPin->findData(exitPin(entry))));
    m_exitPin->setEnabled(m_exitPin->count() > 1);
}

void BudgetDialog::updatePathControls()
{
    const int row = m_path->currentRow();
    m_removeButton->setEnabled(row >= 0);
    m_upButton->setEnabled(row > 0);
    m_downButton->setEnabled(row >= 0 && row + 1 < m_path->count());
    m_clearButton->setEnabled(m_path->count() > 0);
    updateExitPins();
    // With an explicit path, its first entry is the start.
    const bool explicitPath = m_path->count() > 0;
    m_start->setEnabled(!explicitPath);
    m_start->setToolTip(explicitPath ? tr("The first entry of the path is the start component") : QString());
}

bool BudgetDialog::showValue(QLabel *label, const QString &text, const QString &unit, bool integer,
                             const QString &emptyText, double minimum, double *value)
{
    if (text.trimmed().isEmpty()) {
        label->setText(emptyText);
        label->setStyleSheet(QString());
        label->setToolTip(QString());
        if (value)
            *value = NAN;
        return !emptyText.isNull();
    }
    QString error;
    const auto result = Expression::parse(text).evaluate(m_scope, &error);
    if (result && integer && std::abs(*result - std::round(*result)) > 1e-9)
        error = tr("not an integer");
    else if (result && *result < minimum)
        error = tr("at least %1").arg(minimum);
    if (!result || !error.isEmpty()) {
        label->setText(error.isEmpty() ? tr("invalid") : error);
        label->setStyleSheet(errorStyle());
        label->setToolTip(error);
        if (value)
            *value = NAN;
        return false;
    }
    label->setStyleSheet(QString());
    label->setToolTip(QString());
    if (integer)
        label->setText(QStringLiteral("= %1").arg(QString::number(*result, 'g', 10)));
    else if (unit == QLatin1String("dBm"))
        label->setText(QStringLiteral("= %1 dBm").arg(QString::number(*result, 'g', 6)));
    else
        label->setText(QStringLiteral("= %1").arg(formatEngineering(*result, unit, 6)));
    if (value)
        *value = *result;
    return true;
}

void BudgetDialog::validate()
{
    if (m_updating)
        return;
    updatePathControls();
    const rf::BudgetAnalysisSetup current = setup();

    // Trace the chain with the current options for the preview.
    double frequency = NAN, power = NAN;
    const QString fromStart = tr("from the start component");
    const bool frequencyOk = showValue(m_frequencyValue, current.frequency, QStringLiteral("Hz"), false, fromStart,
                                       1e-300, &frequency);
    const bool powerOk = showValue(m_powerValue, current.power, QStringLiteral("dBm"), false, fromStart, -INFINITY, &power);
    rf::ChainOptions options;
    options.path = current.path;
    options.start = current.start;
    options.frequency = frequency;
    options.powerDbm = power;
    const rf::SignalChain chain = rf::traceChain(m_netlist, options);
    if (current.frequency.isEmpty())
        m_frequencyValue->setText(std::isfinite(chain.inputFrequency)
                                      ? tr("from %1: %2").arg(chain.start, formatEngineering(chain.inputFrequency, QStringLiteral("Hz"), 6))
                                      : tr("from the start component"));
    if (current.power.isEmpty())
        m_powerValue->setText(std::isfinite(chain.inputPowerDbm) && !chain.start.isEmpty()
                                  ? tr("from %1: %2 dBm").arg(chain.start, QString::number(chain.inputPowerDbm, 'g', 6))
                                  : tr("from the start component"));
    QStringList names;
    if (!chain.start.isEmpty())
        names.append(chain.start);
    for (const rf::ChainStage &stage : chain.stages)
        names.append(stage.path);
    if (!chain.end.isEmpty())
        names.append(chain.end);
    if (chain.ok()) {
        m_chainPreview = names.join(QStringLiteral(" → "));
        QString text = tr("Chain: %1").arg(m_chainPreview.toHtmlEscaped());
        if (!chain.warnings.isEmpty())
            text += QStringLiteral("<br><i>%1</i>").arg(chain.warnings.join(QStringLiteral("; ")).toHtmlEscaped());
        m_chainLabel->setText(text);
        m_chainLabel->setStyleSheet(QString());
    } else {
        m_chainPreview = chain.errors.join(QStringLiteral("; "));
        m_chainLabel->setText(tr("Chain: %1").arg(m_chainPreview.toHtmlEscaped()));
        m_chainLabel->setStyleSheet(errorStyle());
    }

    bool valid = frequencyOk && powerOk;
    valid = showValue(m_bandwidthValue, current.bandwidth, QStringLiteral("Hz"), false, QString(), 1e-300) && valid;
    if (current.bandwidth.isEmpty()) {
        m_bandwidthValue->setText(tr("required"));
        m_bandwidthValue->setStyleSheet(errorStyle());
        valid = false;
    }

    m_mismatchHint->setText(current.mismatch
                                ? tr("Stage gains include the mismatch between stages, from each block's S-parameters.")
                                : tr("Every stage is taken as matched: |S21|² of each stage, reflections ignored."));

    if (m_sweepGroup->isChecked()) {
        double start = NAN, stop = NAN;
        const bool startOk = showValue(m_sweepStartValue, m_sweepStart->text(), QStringLiteral("Hz"), false, QString(), 0, &start);
        const bool stopOk = showValue(m_sweepStopValue, m_sweepStop->text(), QStringLiteral("Hz"), false, QString(), 0, &stop);
        double points = NAN;
        const bool pointsOk = showValue(m_sweepPointsValue, current.sweepPoints, QString(), true, QString(), 1, &points)
                              && points <= 10000;
        if (!pointsOk && std::isfinite(points)) {
            m_sweepPointsValue->setText(tr("at most 10,000"));
            m_sweepPointsValue->setStyleSheet(errorStyle());
        }
        valid = valid && startOk && stopOk && pointsOk && !m_sweepStart->text().trimmed().isEmpty()
                && !m_sweepStop->text().trimmed().isEmpty();
        if (startOk && stopOk && stop < start) {
            m_sweepStopValue->setText(tr("below the start"));
            m_sweepStopValue->setStyleSheet(errorStyle());
            valid = false;
        }
        if (m_sweepStart->text().trimmed().isEmpty())
            m_sweepStartValue->setText(tr("required"));
        if (m_sweepStop->text().trimmed().isEmpty())
            m_sweepStopValue->setText(tr("required"));
    }
    if (m_spurGroup->isChecked()) {
        double order = NAN;
        const bool orderOk = showValue(m_spurOrderValue, current.spurOrder, QString(), true, QString(), 2, &order) && order <= 15;
        if (!orderOk && std::isfinite(order)) {
            m_spurOrderValue->setText(tr("2 to 15"));
            m_spurOrderValue->setStyleSheet(errorStyle());
        }
        valid = valid && orderOk;
    }
    m_valid = valid;
    m_ok->setEnabled(m_valid && m_document && m_document->schematic());
}

QString BudgetDialog::chainPreview() const
{
    return m_chainPreview;
}

QString BudgetDialog::store()
{
    validate();
    if (!m_valid || !m_document || !m_document->schematic())
        return {};
    Schematic *schematic = m_document->schematic();
    const QJsonObject before = schematic->simulation();
    QJsonObject previous;
    for (const QJsonValue &value : before.value(AnalysesKey).toArray()) {
        if (!m_id.isEmpty() && value.toObject().value(QStringLiteral("id")).toString() == m_id)
            previous = value.toObject();
    }
    QString id;
    QJsonObject simulation = rf::withBudgetAnalysis(before, setup(), &id);
    // Keep the keys of the stored analysis that the setup does not know.
    if (!previous.isEmpty()) {
        static const QStringList known = {QStringLiteral("id"),        QStringLiteral("type"),      QStringLiteral("name"),
                                          QStringLiteral("bandwidth"), QStringLiteral("mismatch"),  QStringLiteral("path"),
                                          QStringLiteral("start"),     QStringLiteral("frequency"), QStringLiteral("power"),
                                          QStringLiteral("sweep"),     QStringLiteral("spurs")};
        QJsonArray analyses = simulation.value(AnalysesKey).toArray();
        for (qsizetype i = 0; i < analyses.size(); ++i) {
            QJsonObject object = analyses.at(i).toObject();
            if (object.value(QStringLiteral("id")).toString() != id)
                continue;
            for (auto it = previous.begin(); it != previous.end(); ++it) {
                if (!known.contains(it.key()))
                    object.insert(it.key(), it.value());
            }
            analyses.replace(i, object);
        }
        simulation.insert(AnalysesKey, analyses);
    }
    schematic->setSimulation(simulation);
    m_id = id;
    m_isNew = false;
    return id;
}

void BudgetDialog::accept()
{
    if (store().isEmpty())
        return;
    QDialog::accept();
}

void BudgetDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_ok)
        validate();
}

QString BudgetDialog::errorStyle() const
{
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    return QStringLiteral("color: %1;").arg(dark ? QStringLiteral("#f08080") : QStringLiteral("#b02020"));
}

} // namespace lvd
