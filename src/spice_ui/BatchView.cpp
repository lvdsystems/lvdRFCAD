#include "spice_ui/BatchView.h"

#include "plot/PlotWidget.h"
#include "spice/SpiceWaveforms.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lvd::spiceui {

namespace {

QStandardItem *numberItem(double value, const QString &unit = {})
{
    auto *item = new QStandardItem(std::isfinite(value) ? formatEngineering(value, unit, 5) : QStringLiteral("—"));
    item->setData(value, Qt::UserRole);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

QTableView *makeTable(QStandardItemModel *model, QWidget *parent)
{
    auto *table = new QTableView(parent);
    table->setModel(model);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    return table;
}

} // namespace

BatchView::BatchView(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto *top = new QHBoxLayout;
    m_heading = new QLabel(tr("Run a parameter sweep, Monte Carlo, worst case or optimisation from \"Sweeps & Statistics\"."), this);
    m_heading->setWordWrap(true);
    top->addWidget(m_heading, 1);
    m_applyButton = new QPushButton(tr("Apply to Schematic"), this);
    m_applyButton->hide();
    top->addWidget(m_applyButton);
    layout->addLayout(top);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    auto *tables = new QSplitter(Qt::Vertical, splitter);
    m_summary = new QStandardItemModel(this);
    m_summaryTable = makeTable(m_summary, tables);
    m_runs = new QStandardItemModel(this);
    m_runsTable = makeTable(m_runs, tables);
    m_runsTable->setSortingEnabled(true);
    m_runsTable->setToolTip(tr("Double-click a run to show its waveforms."));
    tables->addWidget(m_summaryTable);
    tables->addWidget(m_runsTable);
    tables->setStretchFactor(1, 2);

    auto *plotBox = new QWidget(splitter);
    auto *plotLayout = new QVBoxLayout(plotBox);
    plotLayout->setContentsMargins(0, 0, 0, 0);
    auto *controls = new QHBoxLayout;
    controls->addWidget(new QLabel(tr("Plot:"), plotBox));
    m_measureCombo = new QComboBox(plotBox);
    controls->addWidget(m_measureCombo, 1);
    controls->addWidget(new QLabel(tr("Overlay:"), plotBox));
    m_overlay = new QLineEdit(plotBox);
    m_overlay->setPlaceholderText(tr("trace of every run, e.g. v(out)"));
    controls->addWidget(m_overlay, 1);
    plotLayout->addLayout(controls);
    m_plot = new PlotWidget(plotBox);
    m_plot->setMarkerTableVisible(false);
    plotLayout->addWidget(m_plot, 1);
    splitter->addWidget(tables);
    splitter->addWidget(plotBox);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    connect(m_runsTable, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        const int run = m_runs->item(index.row(), 0)->data(Qt::UserRole + 1).toInt();
        if (m_result && run >= 0 && run < m_result->runs.size() && m_result->runs.at(run).simulation)
            Q_EMIT showRunRequested(run);
    });
    connect(m_measureCombo, &QComboBox::activated, this, [this](int index) {
        m_overlay->clear();
        showMeasure(index);
    });
    connect(m_overlay, &QLineEdit::returnPressed, this, [this] {
        QString error;
        if (!setOverlay(m_overlay->text(), &error))
            m_heading->setText(error);
    });
    connect(m_applyButton, &QPushButton::clicked, this, [this] {
        if (m_apply && !m_applyValues.empty())
            m_apply(m_applyValues);
    });
}

void BatchView::setResult(std::shared_ptr<const spice::BatchResult> result)
{
    m_result = std::move(result);
    // Worst-case and optimisation summaries arrive just before the result.
    if (m_mode == Mode::Sweep || m_mode == Mode::MonteCarlo) {
        m_summary->clear();
        m_applyValues.clear();
    }
    m_applyButton->setVisible(m_mode == Mode::Optimization && !m_applyValues.empty() && m_apply);
    fillRuns();
    if (m_mode == Mode::MonteCarlo || m_mode == Mode::Sweep)
        fillStatistics();
    m_measureCombo->clear();
    if (m_result)
        for (const spice::SpiceMeasure &measure : m_result->measures)
            m_measureCombo->addItem(measure.label());
    static const char *names[] = {QT_TR_NOOP("Parameter sweep"), QT_TR_NOOP("Monte Carlo"), QT_TR_NOOP("Worst case"),
                                  QT_TR_NOOP("Optimisation")};
    if (m_result && m_mode != Mode::Optimization)
        m_heading->setText(tr("%1: %n run(s)%2", nullptr, int(m_result->runs.size()))
                               .arg(tr(names[int(m_mode)]),
                                    m_result->warnings.isEmpty() ? QString()
                                                                 : tr(", %n warning(s)", nullptr, int(m_result->warnings.size()))));
    m_plot->clear();
    if (m_result && !m_result->measures.isEmpty() && m_mode != Mode::Optimization)
        showMeasure(0);
}

void BatchView::fillRuns()
{
    m_runs->clear();
    if (!m_result)
        return;
    QStringList headers{tr("Run")};
    for (const spice::SpiceTarget &target : m_result->targets)
        headers << target.text;
    for (const spice::SpiceMeasure &measure : m_result->measures)
        headers << measure.label();
    m_runs->setHorizontalHeaderLabels(headers);
    for (int r = 0; r < m_result->runs.size(); ++r) {
        const spice::BatchRun &run = m_result->runs.at(r);
        auto *label = new QStandardItem(run.label);
        label->setData(r, Qt::UserRole + 1);
        QList<QStandardItem *> row{label};
        for (std::size_t i = 0; i < std::size_t(m_result->targets.size()); ++i)
            row << numberItem(i < run.values.size() ? run.values[i] : NAN,
                              m_result->targets.at(qsizetype(i)).isTemperature() ? QStringLiteral("°C") : QString());
        for (qsizetype m = 0; m < m_result->measures.size(); ++m) {
            QStandardItem *item = numberItem(std::size_t(m) < run.measures.size() ? run.measures[std::size_t(m)] : NAN);
            if (m < run.measureErrors.size() && !run.measureErrors.at(m).isEmpty())
                item->setToolTip(run.measureErrors.at(m));
            row << item;
        }
        m_runs->appendRow(row);
    }
    m_runsTable->resizeColumnsToContents();
}

void BatchView::fillStatistics()
{
    if (!m_result || m_result->measures.isEmpty())
        return;
    const QList<spice::MeasureStatistics> stats = spice::statistics(*m_result);
    if (m_mode == Mode::MonteCarlo)
        m_summary->setHorizontalHeaderLabels(
            {tr("Measure"), tr("Nominal"), tr("Mean"), tr("Sigma"), tr("Minimum"), tr("Maximum"), tr("Runs")});
    else
        m_summary->setHorizontalHeaderLabels({tr("Measure"), tr("First"), tr("Mean"), tr("Sigma"), tr("Minimum"), tr("Maximum"), tr("Runs")});
    for (const spice::MeasureStatistics &s : stats) {
        auto *count = new QStandardItem(QString::number(s.count));
        count->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_summary->appendRow({new QStandardItem(s.name), numberItem(s.nominal), numberItem(s.mean), numberItem(s.sigma),
                              numberItem(s.minimum), numberItem(s.maximum), count});
    }
    m_summaryTable->resizeColumnsToContents();
}

void BatchView::setWorstCase(const QList<spice::WorstCaseMeasure> &summary)
{
    m_summary->clear();
    m_summary->setHorizontalHeaderLabels({tr("Measure"), tr("Minimum"), tr("Nominal"), tr("Maximum"), tr("Spread")});
    for (const spice::WorstCaseMeasure &w : summary)
        m_summary->appendRow({new QStandardItem(w.name), numberItem(w.minimum), numberItem(w.nominal), numberItem(w.maximum),
                              numberItem(w.maximum - w.minimum)});
    m_summaryTable->resizeColumnsToContents();
}

void BatchView::setOptimization(const spice::SpiceOptimizationSetup &setup, const spice::SpiceOptimizationResult &result)
{
    m_summary->clear();
    m_summary->setHorizontalHeaderLabels({tr("Goal"), tr("Value"), tr("Met")});
    for (qsizetype i = 0; i < setup.goals.size(); ++i) {
        const double value = std::size_t(i) < result.bestMeasures.size() ? result.bestMeasures[std::size_t(i)] : NAN;
        const bool met = std::size_t(i) < result.goalsMet.size() && result.goalsMet[std::size_t(i)];
        m_summary->appendRow({new QStandardItem(setup.goals.at(i).label()), numberItem(value),
                              new QStandardItem(met ? tr("yes") : tr("no"))});
    }
    m_summaryTable->resizeColumnsToContents();
    m_applyValues = result.snapped;
    m_applyButton->setVisible(result.ok() && !result.snapped.empty() && bool(m_apply));
    if (result.ok())
        m_heading->setText(tr("Optimisation: %1 simulations, cost %2 → %3%4")
                               .arg(result.evaluations)
                               .arg(result.initialCost, 0, 'g', 4)
                               .arg(result.snappedCost, 0, 'g', 4)
                               .arg(result.converged ? tr(", converged") : QString()));
}

void BatchView::showMeasure(int index)
{
    m_plot->clear();
    if (!m_result || index < 0 || index >= m_result->measures.size())
        return;
    const QString name = m_result->measures.at(index).label();
    std::vector<double> values;
    for (const spice::BatchRun &run : m_result->runs)
        values.push_back(std::size_t(index) < run.measures.size() ? run.measures[std::size_t(index)] : NAN);

    if (m_mode == Mode::MonteCarlo) {
        // Histogram as a stepped outline.
        std::vector<double> finite;
        for (double v : values)
            if (std::isfinite(v))
                finite.push_back(v);
        if (finite.size() < 2)
            return;
        const auto [lo, hi] = std::minmax_element(finite.begin(), finite.end());
        const int bins = std::clamp(int(std::sqrt(double(finite.size()))) + 1, 5, 50);
        const double width = (*hi - *lo) / bins > 0 ? (*hi - *lo) / bins : 1.0;
        std::vector<int> counts(std::size_t(bins), 0);
        for (double v : finite)
            ++counts[std::size_t(std::clamp(int((v - *lo) / width), 0, bins - 1))];
        std::vector<double> x, y;
        for (int b = 0; b < bins; ++b) {
            const double left = *lo + b * width;
            x.insert(x.end(), {left, left, left + width, left + width});
            y.insert(y.end(), {0.0, double(counts[std::size_t(b)]), double(counts[std::size_t(b)]), 0.0});
        }
        m_plot->setXAxis(name, QString());
        m_plot->setYAxis(tr("Runs"), QString());
        const int trace = m_plot->addTrace(name);
        m_plot->setTraceData(trace, x, y);
        m_plot->autoscale();
        return;
    }
    // Against the first swept value; nested sweeps give one curve per outer value.
    if (m_result->targets.isEmpty())
        return;
    const qsizetype inner = m_result->targets.size() - 1;
    m_plot->setXAxis(m_result->targets.at(inner).text, QString());
    m_plot->setYAxis(name, QString());
    QMap<QString, QPair<std::vector<double>, std::vector<double>>> curves;
    QStringList order;
    for (qsizetype r = 0; r < m_result->runs.size(); ++r) {
        const spice::BatchRun &run = m_result->runs.at(r);
        QStringList outer;
        for (qsizetype t = 0; t < inner; ++t)
            outer << QStringLiteral("%1 = %2").arg(m_result->targets.at(t).text, formatEngineering(run.values[std::size_t(t)], QString()));
        const QString key = outer.isEmpty() ? name : outer.join(QStringLiteral(", "));
        if (!curves.contains(key))
            order << key;
        curves[key].first.push_back(run.values[std::size_t(inner)]);
        curves[key].second.push_back(values[std::size_t(r)]);
    }
    for (const QString &key : std::as_const(order)) {
        TraceStyle style;
        style.showPoints = true;
        const int trace = m_plot->addTrace(key, QString(), style);
        m_plot->setTraceData(trace, curves[key].first, curves[key].second);
    }
    m_plot->autoscale();
}

bool BatchView::setOverlay(const QString &expression, QString *error)
{
    if (!m_result || expression.trimmed().isEmpty())
        return false;
    m_plot->clear();
    int drawn = 0;
    QString firstError;
    QString xLabel, xUnit;
    const int limit = 200; // more traces only cost time and colours
    for (const spice::BatchRun &run : m_result->runs) {
        if (!run.simulation || drawn >= limit)
            continue;
        const spice::SpicePlot *plot = nullptr;
        for (const spice::SpicePlot &candidate : run.simulation->run.plots)
            if (candidate.scale()) {
                plot = &candidate;
                break;
            }
        if (!plot)
            continue;
        QString e;
        const auto w = spice::evaluateOnPlot(expression, *plot, &run.simulation->deck, &e);
        if (!w) {
            if (firstError.isEmpty())
                firstError = e;
            continue;
        }
        if (xLabel.isEmpty()) {
            xLabel = plot->scaleName;
            xUnit = spice::vectorKindUnit(plot->scale()->kind);
            m_plot->setXAxis(xLabel, xUnit, plot->type == QLatin1String("ac") || plot->type == QLatin1String("noise"));
            m_plot->setYAxis(expression, w->unit);
        }
        TraceStyle style;
        style.lineWidth = 1.2;
        const int trace = m_plot->addTrace(run.label, w->unit, style);
        if (w->isComplex) {
            m_plot->setTraceData(trace, w->x, w->y);
            m_plot->setTraceComplexPart(trace, ComplexPart::MagnitudeDb);
        } else {
            m_plot->setTraceData(trace, w->x, w->real());
        }
        ++drawn;
    }
    m_plot->setLegendVisible(drawn <= 12);
    m_plot->autoscale();
    if (drawn == 0 && error)
        *error = firstError.isEmpty() ? tr("No run has waveforms.") : firstError;
    return drawn > 0;
}

} // namespace lvd::spiceui
