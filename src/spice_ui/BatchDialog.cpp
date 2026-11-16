#include "spice_ui/BatchDialog.h"

#include "core/Connectivity.h"
#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace lvd::spiceui {

namespace {

QString cellText(const QTableWidget *table, int row, int column)
{
    if (auto *combo = qobject_cast<QComboBox *>(table->cellWidget(row, column)))
        return combo->currentText().trimmed();
    if (auto *check = qobject_cast<QCheckBox *>(table->cellWidget(row, column)))
        return check->isChecked() ? QStringLiteral("1") : QString();
    const QTableWidgetItem *item = table->item(row, column);
    return item ? item->text().trimmed() : QString();
}

QComboBox *choiceCombo(const QStringList &choices, const QString &current, QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->addItems(choices);
    combo->setCurrentIndex(std::max(0, int(choices.indexOf(current))));
    return combo;
}

double numberOf(const QString &text, double fallback)
{
    if (const auto value = Expression::parse(text).evaluate())
        return *value;
    return fallback;
}

QString percent(double relative)
{
    return QString::number(relative * 100.0, 'g', 6) + QLatin1Char('%');
}

double relativeOf(const QString &text)
{
    QString t = text.trimmed();
    const bool isPercent = t.endsWith(QLatin1Char('%'));
    if (isPercent)
        t.chop(1);
    const double value = numberOf(t, NAN);
    return isPercent ? value / 100.0 : value;
}

} // namespace

BatchDialog::BatchDialog(Mode mode, const Project &project, const Document &document, const spice::SpiceAnalysisSetup &analysis,
                         QWidget *parent)
    : QDialog(parent), m_mode(mode), m_analysis(analysis)
{
    static const char *titles[] = {QT_TR_NOOP("Parameter Sweep"), QT_TR_NOOP("Monte Carlo"), QT_TR_NOOP("Worst Case"),
                                   QT_TR_NOOP("Optimize")};
    setWindowTitle(tr("%1 of \"%2\"").arg(tr(titles[int(mode)]), analysis.name));
    resize(760, 560);

    // Candidates: variables, numeric parameters of this sheet's parts.
    if (const Schematic *schematic = document.schematic()) {
        for (const Variable &variable : schematic->variables())
            m_candidates << variable.name;
        QStringList parameters;
        for (const PlacedComponent &component : schematic->components()) {
            const DefinitionPtr definition = project.resolve(*schematic, component.definition).definition;
            if (!definition || definition->isSheetPort() || !definition->netName.isEmpty())
                continue;
            for (const ParameterDefinition &parameter : definition->parameters)
                if (parameter.isExpression())
                    parameters << component.reference + QLatin1Char('.') + parameter.name;
        }
        parameters.sort();
        m_candidates << parameters;
    }
    m_candidates << QStringLiteral("temperature");

    auto *layout = new QVBoxLayout(this);
    auto *note = new QLabel(this);
    note->setWordWrap(true);
    layout->addWidget(note);

    switch (mode) {
    case Mode::Sweep: {
        note->setText(tr("Runs the analysis once per value; nested sweeps run every combination (the first row is the "
                         "outer loop). Values are expressions; log sweeps take points per decade."));
        m_sweep = makeTable({tr("Value swept"), tr("Mode"), tr("Start"), tr("Stop"), tr("Points"), tr("List (lists only)")});
        auto *group = new QGroupBox(tr("Swept values (up to three)"), this);
        (new QVBoxLayout(group))->addWidget(withButtons(m_sweep, [this] {
            spice::SweepAxis axis;
            axis.target.text = m_candidates.value(0);
            axis.start = QStringLiteral("1");
            axis.stop = QStringLiteral("10");
            axis.points = QStringLiteral("5");
            addSweepRow(axis);
        }));
        layout->addWidget(group);
        break;
    }
    case Mode::MonteCarlo:
    case Mode::WorstCase: {
        note->setText(mode == Mode::MonteCarlo
                          ? tr("Each run draws every toleranced value independently: uniform within the tolerance, or "
                               "Gaussian with the tolerance at 3 sigma. Run 0 is the nominal circuit. A target such as "
                               "\"R*.r\" applies to every part whose reference matches. Runs go to parallel worker processes.")
                          : tr("Finds the sensitivity of every measure to every toleranced value, then simulates the two "
                               "corners that push each measure lowest and highest. A target such as \"R*.r\" applies to "
                               "every matching part; a temperature tolerance is in °C."));
        m_tolerances = makeTable({tr("Value"), tr("Tolerance"), tr("Distribution")});
        auto *group = new QGroupBox(tr("Tolerances"), this);
        (new QVBoxLayout(group))->addWidget(withButtons(m_tolerances, [this] {
            spice::Tolerance tolerance;
            tolerance.target.text = QStringLiteral("R*.r");
            tolerance.relative = 0.01;
            addToleranceRow(tolerance);
        }));
        layout->addWidget(group, 1);
        if (mode == Mode::MonteCarlo) {
            auto *form = new QFormLayout;
            m_runs = new QSpinBox(this);
            m_runs->setRange(1, 100000);
            m_runs->setValue(100);
            m_seed = new QSpinBox(this);
            m_seed->setRange(1, 1 << 30);
            m_seed->setValue(1);
            m_seed->setToolTip(tr("The same seed gives the same runs."));
            form->addRow(tr("Runs:"), m_runs);
            form->addRow(tr("Random seed:"), m_seed);
            layout->addLayout(form);
        }
        // Defaults for a new setup.
        for (const auto &[target, relative] : {std::pair{"R*.r", 0.01}, std::pair{"C*.c", 0.05}, std::pair{"L*.l", 0.05}}) {
            spice::Tolerance tolerance;
            tolerance.target.text = QString::fromLatin1(target);
            tolerance.relative = relative;
            addToleranceRow(tolerance);
        }
        break;
    }
    case Mode::Optimize: {
        note->setText(tr("Changes the variables within their ranges (Nelder-Mead, logarithmic for positive ranges) until "
                         "the goals are met; values can be rounded to a standard series at the end. Applying writes them "
                         "into the schematic."));
        m_variables = makeTable({tr("Value"), tr("Minimum"), tr("Maximum"), tr("Log"), tr("Series")});
        auto *variables = new QGroupBox(tr("Variables"), this);
        (new QVBoxLayout(variables))->addWidget(withButtons(m_variables, [this] {
            spice::SpiceOptimizationVariable v;
            v.target.text = m_candidates.value(0);
            v.minimum = 1;
            v.maximum = 1e6;
            addVariableRow(v);
        }));
        layout->addWidget(variables, 1);
        m_goals = makeTable({tr("Expression"), tr("Function"), tr("At"), tr("Goal"), tr("Target"), tr("Weight")});
        auto *goals = new QGroupBox(tr("Goals"), this);
        (new QVBoxLayout(goals))->addWidget(withButtons(m_goals, [this] {
            spice::SpiceGoal goal;
            goal.measure.expression = QStringLiteral("v(out)");
            addGoalRow(goal);
        }));
        layout->addWidget(goals, 1);
        auto *form = new QFormLayout;
        m_evaluations = new QSpinBox(this);
        m_evaluations->setRange(5, 10000);
        m_evaluations->setValue(200);
        form->addRow(tr("Maximum simulations:"), m_evaluations);
        layout->addLayout(form);
        break;
    }
    }

    if (mode != Mode::Optimize) {
        m_measures = makeTable({tr("Expression"), tr("Function"), tr("At / level"), tr("Name")});
        auto *group = new QGroupBox(tr("Measures (numbers taken from each run)"), this);
        (new QVBoxLayout(group))->addWidget(withButtons(m_measures, [this] { addMeasureRow(); }));
        layout->addWidget(group, 1);
    }

    m_error = new QLabel(this);
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("QLabel { background: #c62828; color: white; padding: 4px; }"));
    m_error->hide();
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Run"));
    connect(buttons, &QDialogButtonBox::accepted, this, &BatchDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    const QJsonObject stored = analysis.data.value(storageKey()).toObject();
    if (!stored.isEmpty()) {
        load(stored);
    } else if (m_measures && m_measures->rowCount() == 0) {
        spice::SpiceMeasure m;
        m.expression = QStringLiteral("v(out)");
        m.function = analysis.kind == spice::AnalysisKind::OperatingPoint ? QStringLiteral("value") : QStringLiteral("max");
        addMeasureRow(m);
    }
    if (m_sweep && m_sweep->rowCount() == 0) {
        spice::SweepAxis axis;
        axis.target.text = m_candidates.value(0);
        axis.start = QStringLiteral("1");
        axis.stop = QStringLiteral("10");
        axis.points = QStringLiteral("5");
        addSweepRow(axis);
    }
}

QTableWidget *BatchDialog::makeTable(const QStringList &headers)
{
    auto *table = new QTableWidget(0, int(headers.size()), this);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setMinimumHeight(110);
    return table;
}

QWidget *BatchDialog::withButtons(QTableWidget *table, const std::function<void()> &addRow)
{
    auto *box = new QWidget(this);
    auto *layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(table, 1);
    auto *buttons = new QVBoxLayout;
    auto *add = new QPushButton(tr("Add"), box);
    auto *remove = new QPushButton(tr("Remove"), box);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    layout->addLayout(buttons);
    connect(add, &QPushButton::clicked, this, addRow);
    connect(remove, &QPushButton::clicked, this, [table] {
        const int row = table->currentRow();
        if (row >= 0)
            table->removeRow(row);
    });
    return box;
}

QComboBox *BatchDialog::targetCombo(const QString &text)
{
    auto *combo = new QComboBox(this);
    combo->setEditable(true);
    combo->addItems(m_candidates);
    combo->setEditText(text);
    return combo;
}

void BatchDialog::addMeasureRow(const spice::SpiceMeasure &measure)
{
    const int row = m_measures->rowCount();
    m_measures->insertRow(row);
    m_measures->setItem(row, 0, new QTableWidgetItem(measure.expression.isEmpty() ? QStringLiteral("v(out)") : measure.expression));
    m_measures->setCellWidget(row, 1, choiceCombo(spice::measureFunctions(), measure.function, this));
    m_measures->setItem(row, 2, new QTableWidgetItem(measure.function == QLatin1String("crossing") ? measure.level : measure.at));
    m_measures->setItem(row, 3, new QTableWidgetItem(measure.name));
}

void BatchDialog::addToleranceRow(const spice::Tolerance &tolerance)
{
    const int row = m_tolerances->rowCount();
    m_tolerances->insertRow(row);
    m_tolerances->setCellWidget(row, 0, targetCombo(tolerance.target.text));
    m_tolerances->setItem(row, 1, new QTableWidgetItem(tolerance.target.isTemperature() ? QString::number(tolerance.relative)
                                                                                         : percent(tolerance.relative)));
    m_tolerances->setCellWidget(row, 2, choiceCombo({QStringLiteral("gaussian"), QStringLiteral("uniform")},
                                                    tolerance.distribution == spice::Tolerance::Distribution::Uniform
                                                        ? QStringLiteral("uniform")
                                                        : QStringLiteral("gaussian"),
                                                    this));
}

void BatchDialog::addSweepRow(const spice::SweepAxis &axis)
{
    const int row = m_sweep->rowCount();
    m_sweep->insertRow(row);
    m_sweep->setCellWidget(row, 0, targetCombo(axis.target.text));
    m_sweep->setCellWidget(row, 1, choiceCombo({QStringLiteral("lin"), QStringLiteral("log"), QStringLiteral("list")}, axis.mode, this));
    m_sweep->setItem(row, 2, new QTableWidgetItem(axis.start));
    m_sweep->setItem(row, 3, new QTableWidgetItem(axis.stop));
    m_sweep->setItem(row, 4, new QTableWidgetItem(axis.points));
    m_sweep->setItem(row, 5, new QTableWidgetItem(axis.list.join(QStringLiteral(", "))));
}

void BatchDialog::addVariableRow(const spice::SpiceOptimizationVariable &variable)
{
    const int row = m_variables->rowCount();
    m_variables->insertRow(row);
    m_variables->setCellWidget(row, 0, targetCombo(variable.target.text));
    m_variables->setItem(row, 1, new QTableWidgetItem(QString::number(variable.minimum, 'g', 6)));
    m_variables->setItem(row, 2, new QTableWidgetItem(QString::number(variable.maximum, 'g', 6)));
    auto *log = new QCheckBox(this);
    log->setChecked(variable.logarithmic);
    m_variables->setCellWidget(row, 3, log);
    QStringList series{tr("none")};
    for (ESeries s : allSeries())
        series << seriesName(s);
    m_variables->setCellWidget(row, 4, choiceCombo(series, variable.series ? seriesName(*variable.series) : tr("none"), this));
}

void BatchDialog::addGoalRow(const spice::SpiceGoal &goal)
{
    const int row = m_goals->rowCount();
    m_goals->insertRow(row);
    m_goals->setItem(row, 0, new QTableWidgetItem(goal.measure.expression));
    m_goals->setCellWidget(row, 1, choiceCombo(spice::measureFunctions(), goal.measure.function, this));
    m_goals->setItem(row, 2, new QTableWidgetItem(goal.measure.at));
    static const QStringList types{QStringLiteral("below"), QStringLiteral("above"), QStringLiteral("equal"),
                                   QStringLiteral("minimize"), QStringLiteral("maximize")};
    m_goals->setCellWidget(row, 3, choiceCombo(types, types.value(int(goal.type)), this));
    m_goals->setItem(row, 4, new QTableWidgetItem(QString::number(goal.target, 'g', 6)));
    m_goals->setItem(row, 5, new QTableWidgetItem(QString::number(goal.weight, 'g', 6)));
}

QString BatchDialog::storageKey() const
{
    switch (m_mode) {
    case Mode::Sweep: return QStringLiteral("sweep");
    case Mode::MonteCarlo: return QStringLiteral("monteCarlo");
    case Mode::WorstCase: return QStringLiteral("worstCase");
    case Mode::Optimize: return QStringLiteral("optimization");
    }
    return {};
}

QList<spice::SweepAxis> BatchDialog::sweepAxes() const
{
    QList<spice::SweepAxis> axes;
    for (int row = 0; m_sweep && row < m_sweep->rowCount(); ++row) {
        spice::SweepAxis axis;
        axis.target.text = cellText(m_sweep, row, 0);
        axis.mode = cellText(m_sweep, row, 1);
        axis.start = cellText(m_sweep, row, 2);
        axis.stop = cellText(m_sweep, row, 3);
        axis.points = cellText(m_sweep, row, 4);
        for (const QString &item : cellText(m_sweep, row, 5).split(QLatin1Char(','), Qt::SkipEmptyParts))
            axis.list << item.trimmed();
        axes << axis;
    }
    return axes;
}

QList<spice::SpiceMeasure> BatchDialog::measures() const
{
    QList<spice::SpiceMeasure> list;
    for (int row = 0; m_measures && row < m_measures->rowCount(); ++row) {
        spice::SpiceMeasure m;
        m.expression = cellText(m_measures, row, 0);
        m.function = cellText(m_measures, row, 1);
        if (m.function == QLatin1String("crossing"))
            m.level = cellText(m_measures, row, 2);
        else
            m.at = cellText(m_measures, row, 2);
        m.name = cellText(m_measures, row, 3);
        if (!m.expression.isEmpty())
            list << m;
    }
    return list;
}

spice::MonteCarloSetup BatchDialog::monteCarlo() const
{
    spice::MonteCarloSetup setup;
    for (int row = 0; m_tolerances && row < m_tolerances->rowCount(); ++row) {
        spice::Tolerance t;
        t.target.text = cellText(m_tolerances, row, 0);
        t.relative = relativeOf(cellText(m_tolerances, row, 1));
        t.distribution = cellText(m_tolerances, row, 2) == QLatin1String("uniform") ? spice::Tolerance::Distribution::Uniform
                                                                                    : spice::Tolerance::Distribution::Gaussian;
        if (!t.target.text.isEmpty())
            setup.tolerances << t;
    }
    if (m_runs)
        setup.runs = m_runs->value();
    if (m_seed)
        setup.seed = quint32(m_seed->value());
    setup.measures = measures();
    return setup;
}

spice::SpiceOptimizationSetup BatchDialog::optimization() const
{
    spice::SpiceOptimizationSetup setup;
    for (int row = 0; m_variables && row < m_variables->rowCount(); ++row) {
        spice::SpiceOptimizationVariable v;
        v.target.text = cellText(m_variables, row, 0);
        v.minimum = numberOf(cellText(m_variables, row, 1), NAN);
        v.maximum = numberOf(cellText(m_variables, row, 2), NAN);
        v.logarithmic = !cellText(m_variables, row, 3).isEmpty();
        v.series = seriesFromName(cellText(m_variables, row, 4));
        setup.variables << v;
    }
    for (int row = 0; m_goals && row < m_goals->rowCount(); ++row) {
        spice::SpiceGoal goal;
        goal.measure.expression = cellText(m_goals, row, 0);
        goal.measure.function = cellText(m_goals, row, 1);
        if (goal.measure.function == QLatin1String("crossing"))
            goal.measure.level = cellText(m_goals, row, 2);
        else
            goal.measure.at = cellText(m_goals, row, 2);
        const QString type = cellText(m_goals, row, 3);
        goal.type = type == QLatin1String("above")      ? spice::SpiceGoal::Type::Above
                    : type == QLatin1String("equal")    ? spice::SpiceGoal::Type::Equal
                    : type == QLatin1String("minimize") ? spice::SpiceGoal::Type::Minimize
                    : type == QLatin1String("maximize") ? spice::SpiceGoal::Type::Maximize
                                                        : spice::SpiceGoal::Type::Below;
        goal.target = numberOf(cellText(m_goals, row, 4), 0);
        goal.weight = numberOf(cellText(m_goals, row, 5), 1);
        setup.goals << goal;
    }
    if (m_evaluations)
        setup.maxEvaluations = m_evaluations->value();
    return setup;
}

QJsonObject BatchDialog::toJson() const
{
    switch (m_mode) {
    case Mode::Sweep: {
        QJsonArray axes, measureArray;
        for (const spice::SweepAxis &axis : sweepAxes())
            axes.append(axis.toJson());
        for (const spice::SpiceMeasure &m : measures())
            measureArray.append(m.toJson());
        return {{QStringLiteral("axes"), axes}, {QStringLiteral("measures"), measureArray}};
    }
    case Mode::MonteCarlo:
    case Mode::WorstCase:
        return monteCarlo().toJson();
    case Mode::Optimize:
        return optimization().toJson();
    }
    return {};
}

void BatchDialog::load(const QJsonObject &object)
{
    switch (m_mode) {
    case Mode::Sweep:
        m_sweep->setRowCount(0);
        for (const QJsonValue &value : object.value(QStringLiteral("axes")).toArray())
            addSweepRow(spice::SweepAxis::fromJson(value.toObject()));
        m_measures->setRowCount(0);
        for (const QJsonValue &value : object.value(QStringLiteral("measures")).toArray())
            addMeasureRow(spice::SpiceMeasure::fromJson(value.toObject()));
        break;
    case Mode::MonteCarlo:
    case Mode::WorstCase: {
        const spice::MonteCarloSetup setup = spice::MonteCarloSetup::fromJson(object);
        m_tolerances->setRowCount(0);
        for (const spice::Tolerance &t : setup.tolerances)
            addToleranceRow(t);
        if (m_runs)
            m_runs->setValue(setup.runs);
        if (m_seed)
            m_seed->setValue(int(setup.seed));
        m_measures->setRowCount(0);
        for (const spice::SpiceMeasure &m : setup.measures)
            addMeasureRow(m);
        break;
    }
    case Mode::Optimize: {
        const spice::SpiceOptimizationSetup setup = spice::SpiceOptimizationSetup::fromJson(object);
        m_variables->setRowCount(0);
        for (const spice::SpiceOptimizationVariable &v : setup.variables)
            addVariableRow(v);
        m_goals->setRowCount(0);
        for (const spice::SpiceGoal &g : setup.goals)
            addGoalRow(g);
        m_evaluations->setValue(setup.maxEvaluations);
        break;
    }
    }
}

QStringList BatchDialog::problems() const
{
    QStringList list;
    switch (m_mode) {
    case Mode::Sweep: {
        const QList<spice::SweepAxis> axes = sweepAxes();
        if (axes.isEmpty() || axes.size() > 3)
            list << tr("Sweep one to three values.");
        // Values may use schematic variables; they are checked when the sweep runs.
        break;
    }
    case Mode::MonteCarlo:
    case Mode::WorstCase:
        if (monteCarlo().tolerances.isEmpty())
            list << tr("Give at least one tolerance.");
        for (const spice::Tolerance &t : monteCarlo().tolerances)
            if (!std::isfinite(t.relative) || t.relative < 0)
                list << tr("%1: the tolerance is not a number.").arg(t.target.text);
        if (measures().isEmpty())
            list << tr("Give at least one measure.");
        break;
    case Mode::Optimize:
        if (optimization().variables.isEmpty() || optimization().goals.isEmpty())
            list << tr("Give at least one variable and one goal.");
        for (const spice::SpiceOptimizationVariable &v : optimization().variables)
            if (!(v.maximum > v.minimum))
                list << tr("%1: the maximum must exceed the minimum.").arg(v.target.text);
        break;
    }
    return list;
}

void BatchDialog::accept()
{
    const QStringList list = problems();
    if (!list.isEmpty()) {
        m_error->setText(list.join(QLatin1Char('\n')));
        m_error->show();
        return;
    }
    QDialog::accept();
}

} // namespace lvd::spiceui
