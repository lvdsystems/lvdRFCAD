#include "simulation_ui/BudgetView.h"

#include "core/Expression.h"
#include "plot/PlotWidget.h"
#include "simulation_ui/BudgetModels.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>

namespace lvd::sim {

namespace {

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

double finiteOrNan(double value)
{
    return std::isfinite(value) ? value : NAN;
}

bool anyFinite(const std::vector<double> &values)
{
    return std::ranges::any_of(values, [](double v) { return std::isfinite(v); });
}

QString hertz(double value)
{
    return std::isfinite(value) ? formatEngineering(value, QStringLiteral("Hz"), 6) : QString();
}

QString dbm(double value)
{
    return std::isfinite(value) ? QStringLiteral("%1 dBm").arg(QString::number(value, 'f', 1)) : QString();
}

QToolButton *textButton(QWidget *parent, const QString &text, const QString &tip, const QString &name)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tip);
    button->setObjectName(name);
    button->setAutoRaise(true);
    return button;
}

QTableView *createTableView(QWidget *parent, const QString &name)
{
    auto *table = new QTableView(parent);
    table->setObjectName(name);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 6);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(false);
    return table;
}

} // namespace

BudgetView::BudgetView(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("budgetView"));
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("budgetSummary"));
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary->setContentsMargins(6, 2, 6, 2);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("budgetTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(createTablePage(), tr("Budget"));
    m_tabs->addTab(createLevelPage(), tr("Level Diagram"));
    m_tabs->addTab(createSweepPage(), tr("Swept Budget"));
    m_tabs->addTab(createSpurPage(), tr("Spurs"));
    m_tabs->addTab(createPlanPage(), tr("Frequency Plan"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_summary);
    layout->addWidget(m_tabs, 1);

    applyTheme();
    setRun(nullptr);
}

BudgetView::~BudgetView() = default;

// ---------------------------------------------------------------- pages

QWidget *BudgetView::createTablePage()
{
    auto *page = new QWidget(this);
    m_tableModel = new BudgetTableModel(this);
    m_table = createTableView(page, QStringLiteral("budgetTable"));
    m_table->setModel(m_tableModel);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setDefaultSectionSize(110);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);

    auto *copyAction = new QAction(tr("Copy"), m_table);
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setShortcutContext(Qt::WidgetShortcut);
    connect(copyAction, &QAction::triggered, this, &BudgetView::copyTable);
    m_table->addAction(copyAction);

    auto *title = new QLabel(tr("Budget cascade"), page);
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    auto *hint = new QLabel(tr("Highlighted: compressed stages. Hover a stage for its notes."), page);
    hint->setObjectName(QStringLiteral("budgetHint"));
    auto *copy = textButton(page, tr("Copy"), tr("Copy the selected rows, or the whole table, to the clipboard"),
                            QStringLiteral("copyBudget"));
    auto *csv = textButton(page, tr("Export CSV…"), tr("Save the budget table as CSV"), QStringLiteral("exportBudgetCsv"));
    connect(copy, &QToolButton::clicked, this, &BudgetView::copyTable);
    connect(csv, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Export Budget"), QStringLiteral("budget.csv"),
                                                          tr("CSV files (*.csv)"));
        if (path.isEmpty())
            return;
        QString error;
        if (!exportCsv(path, &error))
            QMessageBox::warning(this, tr("Export Budget"), error);
        else
            Q_EMIT statusMessage(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
    });

    auto *row = new QHBoxLayout;
    row->setContentsMargins(6, 2, 6, 0);
    row->addWidget(title);
    row->addSpacing(12);
    row->addWidget(hint, 1);
    row->addWidget(copy);
    row->addWidget(csv);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addLayout(row);
    layout->addWidget(m_table, 1);
    return page;
}

QWidget *BudgetView::createLevelPage()
{
    auto *page = new QWidget(this);
    m_levelPlot = new PlotWidget(page);
    m_levelPlot->setObjectName(QStringLiteral("levelDiagram"));
    m_levelPlot->setTitle(tr("Level diagram"));
    m_levelPlot->setXAxis(tr("Stage"), QString());
    m_levelPlot->setYAxis(tr("Level"), QStringLiteral("dBm"));
    m_levelPlot->setYAxis(tr("Cumulative gain, NF"), QStringLiteral("dB"), false, PlotYAxis::Right);
    m_levelPlot->setMarkerTableVisible(false);
    connect(m_levelPlot, &PlotWidget::hoverReadoutChanged, this, &BudgetView::statusMessage);

    m_stageLegend = new QLabel(page);
    m_stageLegend->setObjectName(QStringLiteral("stageLegend"));
    m_stageLegend->setWordWrap(true);
    m_stageLegend->setTextFormat(Qt::RichText);
    m_stageLegend->setContentsMargins(8, 0, 8, 4);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_levelPlot, 1);
    layout->addWidget(m_stageLegend);
    return page;
}

QWidget *BudgetView::createSweepPage()
{
    m_sweepPages = new QStackedWidget(this);
    m_sweepPlaceholder = new QLabel(tr("This analysis has no swept budget. Set a sweep of the input frequency with "
                                       "“Edit…” to see gain, noise figure and IIP3 against frequency."),
                                    m_sweepPages);
    m_sweepPlaceholder->setAlignment(Qt::AlignCenter);
    m_sweepPlaceholder->setWordWrap(true);

    auto *page = new QWidget(m_sweepPages);
    m_sweepStageCombo = new QComboBox(page);
    m_sweepStageCombo->setObjectName(QStringLiteral("sweepStage"));
    m_sweepStageCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_sweepStageCombo, &QComboBox::activated, this,
            [this](int index) { setSweepStage(m_sweepStageCombo->itemData(index).toInt()); });
    m_sweepPlot = new PlotWidget(page);
    m_sweepPlot->setObjectName(QStringLiteral("sweptBudget"));
    m_sweepPlot->setXAxis(tr("Input frequency"), QStringLiteral("Hz"));
    m_sweepPlot->setYAxis(tr("Gain [dB], IIP3 [dBm]"), QString());
    m_sweepPlot->setYAxis(tr("Cumulative NF"), QStringLiteral("dB"), false, PlotYAxis::Right);
    connect(m_sweepPlot, &PlotWidget::hoverReadoutChanged, this, &BudgetView::statusMessage);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(6, 2, 6, 0);
    row->addWidget(new QLabel(tr("Cumulative at:"), page));
    row->addWidget(m_sweepStageCombo);
    row->addStretch(1);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addLayout(row);
    layout->addWidget(m_sweepPlot, 1);

    m_sweepPages->addWidget(m_sweepPlaceholder);
    m_sweepPages->addWidget(page);
    return m_sweepPages;
}

QWidget *BudgetView::createSpurPage()
{
    auto *page = new QWidget(this);
    m_spurModel = new SpurTableModel(this);
    m_spurFilter = new SpurFilterModel(this);
    m_spurFilter->setSourceModel(m_spurModel);
    m_spurTable = createTableView(page, QStringLiteral("spurTable"));
    m_spurTable->setModel(m_spurFilter);
    m_spurTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_spurTable->verticalHeader()->hide();
    m_spurTable->setSortingEnabled(true);
    m_spurTable->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    m_spurFilter->sort(-1);

    m_spurMessage = new QLabel(page);
    m_spurMessage->setObjectName(QStringLiteral("spurMessage"));
    m_spurMessage->setWordWrap(true);
    m_spurMessage->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_spurCount = new QLabel(page);
    m_spurCount->setObjectName(QStringLiteral("spurCount"));
    m_inBandOnly = new QCheckBox(tr("In-band only"), page);
    m_inBandOnly->setObjectName(QStringLiteral("inBandOnly"));
    connect(m_inBandOnly, &QCheckBox::toggled, this, [this](bool on) {
        m_spurFilter->setInBandOnly(on);
        showSpurs();
    });
    auto *copy = textButton(page, tr("Copy"), tr("Copy the shown spurs to the clipboard"), QStringLiteral("copySpurs"));
    connect(copy, &QToolButton::clicked, this, [this] {
        QList<int> rows;
        for (int r = 0; r < m_spurFilter->rowCount(); ++r)
            rows.append(m_spurFilter->mapToSource(m_spurFilter->index(r, 0)).row());
        const QString text = m_spurModel->text(QLatin1Char('\t'), rows);
        if (!text.isEmpty())
            QApplication::clipboard()->setText(text);
    });

    m_spurPlot = new PlotWidget(page);
    m_spurPlot->setObjectName(QStringLiteral("spurChart"));
    m_spurPlot->setTitle(tr("Output spectrum: mixer products and LO leakage"));
    m_spurPlot->setXAxis(tr("Output frequency"), QStringLiteral("Hz"));
    m_spurPlot->setYAxis(tr("Level"), QStringLiteral("dBm"));
    m_spurPlot->setMarkerTableVisible(false);
    connect(m_spurPlot, &PlotWidget::hoverReadoutChanged, this, &BudgetView::statusMessage);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(6, 2, 6, 0);
    row->addWidget(m_spurCount, 1);
    row->addWidget(m_inBandOnly);
    row->addWidget(copy);

    auto *top = new QWidget(page);
    auto *topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(2);
    topLayout->addLayout(row);
    topLayout->addWidget(m_spurTable, 1);
    auto *splitter = new QSplitter(Qt::Vertical, page);
    splitter->addWidget(top);
    splitter->addWidget(m_spurPlot);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_spurMessage);
    layout->addWidget(splitter, 1);
    return page;
}

QWidget *BudgetView::createPlanPage()
{
    auto *page = new QWidget(this);
    m_plan = new QTableWidget(page);
    m_plan->setObjectName(QStringLiteral("frequencyPlan"));
    m_plan->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_plan->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_plan->setAlternatingRowColors(true);
    m_plan->verticalHeader()->hide();
    m_plan->verticalHeader()->setDefaultSectionSize(m_plan->fontMetrics().height() + 6);
    m_plan->setColumnCount(9);
    m_plan->setHorizontalHeaderLabels({tr("#"), tr("Component"), tr("Type"), tr("Input frequency"),
                                       tr("Output frequency"), tr("Conversion"), tr("LO source"), tr("LO frequency"),
                                       tr("LO drive")});
    m_plan->horizontalHeader()->setStretchLastSection(true);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_plan);
    return page;
}

// ---------------------------------------------------------------- content

void BudgetView::setRun(std::shared_ptr<const rf::BudgetRun> run)
{
    m_run = std::move(run);
    const bool haveBudget = m_run && !m_run->budget.stages.empty();
    m_tableModel->setBudget(haveBudget ? m_run : nullptr, haveBudget ? &m_run->budget : nullptr);
    m_table->resizeColumnsToContents();
    for (int c = 0; c < m_tableModel->columnCount(); ++c)
        m_table->setColumnWidth(c, std::max(m_table->columnWidth(c), 84));
    m_spurModel->setSpurs(m_run, m_run ? &m_run->spurs : nullptr);
    m_spurTable->resizeColumnsToContents();

    {
        const QSignalBlocker blocker(m_sweepStageCombo);
        m_sweepStageCombo->clear();
        m_sweepStageCombo->addItem(tr("Chain output"), -1);
        if (m_run) {
            for (size_t i = 0; i < m_run->budget.stages.size(); ++i)
                m_sweepStageCombo->addItem(QStringLiteral("%1 · %2").arg(i + 1).arg(m_run->budget.stages[i].path),
                                           static_cast<int>(i));
        }
        const int index = m_sweepStageCombo->findData(m_sweepStage);
        if (index < 0)
            m_sweepStage = -1;
        m_sweepStageCombo->setCurrentIndex(std::max(0, index));
    }

    showSummary();
    showLevels();
    showSweep();
    showSpurs();
    showPlan();
}

QString BudgetView::summary() const
{
    return m_summary->text();
}

void BudgetView::showSummary()
{
    if (!m_run || m_run->budget.stages.empty()) {
        m_summary->setText(m_run ? tr("No budget: see the errors above.") : tr("Not run yet. Press F5 to run."));
        return;
    }
    const rf::BudgetResult &b = m_run->budget;
    const rf::BudgetStage &out = b.stages.back();
    const QString chain = m_run->chain.end.isEmpty() ? b.start : QStringLiteral("%1 → %2").arg(b.start, m_run->chain.end);
    const QString mode = b.mismatch ? tr("actual S-parameters (mismatch between stages included)")
                                    : tr("ideal matched cascade (mismatch ignored)");
    QString text = tr("<b>%1</b> · %2 at %3 · bandwidth %4 · mismatch: <b>%5</b><br>"
                      "Output: %6, gain %7 dB, NF %8 dB, SNR %9 dB")
                       .arg(chain.toHtmlEscaped(), dbm(b.inputPowerDbm), hertz(b.inputFrequency), hertz(b.bandwidth),
                            mode, hertz(out.outputFrequency), QString::number(out.cumulativeGainDb, 'f', 2),
                            QString::number(out.cumulativeNfDb, 'f', 2), QString::number(out.snrDb, 'f', 1));
    const auto db = [](double value) {
        return std::isfinite(value) ? QString::number(value, 'f', 1) : QString(QChar(0x221E));
    };
    if (b.hasPhaseNoise || b.hasBlocker) {
        QStringList parts{tr("thermal %1 dB").arg(db(out.thermalSnrDb))};
        if (b.hasPhaseNoise)
            parts << tr("LO phase noise %1 dB (integrated %2 to %3)")
                         .arg(db(out.phaseNoiseSnrDb), hertz(b.phaseNoiseStart), hertz(b.phaseNoiseStop));
        if (b.hasBlocker)
            parts << tr("reciprocal mixing %1 dB (blocker %2 dBm at +%3)")
                         .arg(db(out.reciprocalMixingSnrDb))
                         .arg(b.blockerPowerDbm, 0, 'g', 4)
                         .arg(hertz(b.blockerOffset));
        text += QStringLiteral("<br>") + tr("SNR limits: %1").arg(parts.join(QStringLiteral(", ")));
    }
    if (b.hasDac)
        text += QStringLiteral("<br>")
                + tr("DAC: sinc roll-off %1 dB, noise density %2 dBm/Hz%3, first image at %4: %5 dBm at the output")
                      .arg(QString::number(b.dacSincDb, 'f', 2), QString::number(b.dacNsdDbmHz, 'f', 1),
                           b.dacNsdFromQuantisation ? tr(" (quantisation)") : QString(), hertz(b.dacImageFrequency),
                           db(out.dacImageDbm));
    if (b.hasDetector)
        text += QStringLiteral("<br>")
                + tr("Detector: %1 dB above its sensitivity of %2 dBm, %3 dB below the top of its range")
                      .arg(db(b.detectorMarginDb), QString::number(b.detectorSensitivityDbm, 'g', 4), db(b.detectorHeadroomDb));
    m_summary->setText(text);
}

int BudgetView::levelTrace(LevelTrace trace) const
{
    return m_levelTraces[static_cast<int>(trace)];
}

QString BudgetView::stageLegend() const
{
    QString text = m_stageLegend->text();
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    text.remove(tags);
    return text.simplified();
}

void BudgetView::showLevels()
{
    m_levelPlot->clear();
    for (int &id : m_levelTraces)
        id = -1;
    if (!m_run || m_run->budget.stages.empty()) {
        m_stageLegend->clear();
        return;
    }
    const rf::BudgetResult &b = m_run->budget;
    const size_t n = b.stages.size();
    std::vector<double> x, signal, noise, op1db, gain, nf, totalNoise;
    x.push_back(0);
    signal.push_back(b.inputPowerDbm);
    noise.push_back(b.inputNoiseDbm);
    totalNoise.push_back(b.inputNoiseDbm);
    op1db.push_back(NAN);
    gain.push_back(0);
    nf.push_back(0);
    for (size_t i = 0; i < n; ++i) {
        const rf::BudgetStage &s = b.stages[i];
        x.push_back(static_cast<double>(i + 1));
        signal.push_back(finiteOrNan(s.signalDbm));
        noise.push_back(finiteOrNan(s.noiseDbm));
        totalNoise.push_back(finiteOrNan(s.totalNoiseDbm));
        op1db.push_back(finiteOrNan(s.op1dbCumulativeDbm));
        gain.push_back(finiteOrNan(s.cumulativeGainDb));
        nf.push_back(finiteOrNan(s.cumulativeNfDb));
    }
    const auto add = [&](LevelTrace which, const QString &name, const QString &unit, const std::vector<double> &y,
                         Qt::PenStyle pen, PlotYAxis axis) {
        TraceStyle style;
        style.lineStyle = pen;
        style.showPoints = true;
        style.yAxis = axis;
        const int id = m_levelPlot->addTrace(name, unit, style);
        m_levelPlot->setTraceData(id, x, y);
        m_levelTraces[static_cast<int>(which)] = id;
    };
    add(LevelTrace::Signal, tr("Signal"), QStringLiteral("dBm"), signal, Qt::SolidLine, PlotYAxis::Left);
    add(LevelTrace::Noise, tr("Noise in %1").arg(hertz(b.bandwidth)), QStringLiteral("dBm"), noise, Qt::SolidLine,
        PlotYAxis::Left);
    if (b.hasPhaseNoise || b.hasBlocker)
        add(LevelTrace::TotalNoise, tr("Total noise (with phase noise, reciprocal mixing)"), QStringLiteral("dBm"), totalNoise,
            Qt::DashLine, PlotYAxis::Left);
    if (anyFinite(op1db))
        add(LevelTrace::Op1db, tr("Cumulative OP1dB"), QStringLiteral("dBm"), op1db, Qt::DashLine, PlotYAxis::Left);
    add(LevelTrace::Gain, tr("Cumulative gain"), QStringLiteral("dB"), gain, Qt::DotLine, PlotYAxis::Right);
    add(LevelTrace::Nf, tr("Cumulative NF"), QStringLiteral("dB"), nf, Qt::DashDotLine, PlotYAxis::Right);
    m_levelPlot->autoscale();
    m_levelPlot->setXRange(-0.25, static_cast<double>(n) + 0.25);

    QStringList items{tr("<b>0</b>&nbsp;Input&nbsp;(%1)").arg(b.start.toHtmlEscaped())};
    for (size_t i = 0; i < n; ++i) {
        const rf::BudgetStage &s = b.stages[i];
        QString item = QStringLiteral("<b>%1</b>&nbsp;%2").arg(i + 1).arg(s.path.toHtmlEscaped());
        if (s.compressed)
            item += QStringLiteral("&nbsp;(%1)").arg(tr("compressed"));
        items.append(item);
    }
    m_stageLegend->setText(tr("Stages: %1").arg(items.join(QStringLiteral(" &nbsp;·&nbsp; "))));
}

bool BudgetView::hasSweep() const
{
    return m_run && !m_run->sweep.empty() && m_run->sweep.size() == m_run->sweepFrequencies.size();
}

int BudgetView::sweepTrace(SweepTrace trace) const
{
    return m_sweepTraces[static_cast<int>(trace)];
}

void BudgetView::setSweepStage(int stage)
{
    m_sweepStage = stage;
    const int index = m_sweepStageCombo->findData(stage);
    if (index >= 0) {
        const QSignalBlocker blocker(m_sweepStageCombo);
        m_sweepStageCombo->setCurrentIndex(index);
    }
    showSweep();
}

void BudgetView::showSweep()
{
    m_sweepPlot->clear();
    for (int &id : m_sweepTraces)
        id = -1;
    m_sweepPages->setCurrentIndex(hasSweep() ? 1 : 0);
    if (!hasSweep())
        return;
    const std::vector<double> &x = m_run->sweepFrequencies;
    std::vector<double> gain, nf, iip3, snr;
    const bool showSnr = m_run->budget.hasPhaseNoise || m_run->budget.hasBlocker;
    for (const rf::BudgetResult &point : m_run->sweep) {
        const rf::BudgetStage *stage = nullptr;
        if (!point.stages.empty()) {
            const bool valid = m_sweepStage >= 0 && m_sweepStage < static_cast<int>(point.stages.size());
            stage = valid ? &point.stages[static_cast<size_t>(m_sweepStage)] : &point.stages.back();
        }
        gain.push_back(stage ? finiteOrNan(stage->cumulativeGainDb) : NAN);
        nf.push_back(stage ? finiteOrNan(stage->cumulativeNfDb) : NAN);
        iip3.push_back(stage ? finiteOrNan(stage->iip3Dbm) : NAN);
        snr.push_back(stage ? finiteOrNan(stage->snrDb) : NAN);
    }
    const QString where = m_sweepStage < 0 ? tr("chain output") : m_sweepStageCombo->currentText();
    m_sweepPlot->setTitle(tr("Swept budget at the %1").arg(where));
    const auto add = [&](SweepTrace which, const QString &name, const QString &unit, const std::vector<double> &y,
                         Qt::PenStyle pen, PlotYAxis axis) {
        TraceStyle style;
        style.lineStyle = pen;
        style.yAxis = axis;
        const int id = m_sweepPlot->addTrace(name, unit, style);
        m_sweepPlot->setTraceData(id, x, y);
        m_sweepTraces[static_cast<int>(which)] = id;
    };
    add(SweepTrace::Gain, tr("Cumulative gain"), QStringLiteral("dB"), gain, Qt::SolidLine, PlotYAxis::Left);
    add(SweepTrace::Nf, tr("Cumulative NF"), QStringLiteral("dB"), nf, Qt::DashLine, PlotYAxis::Right);
    if (anyFinite(iip3))
        add(SweepTrace::Iip3, tr("IIP3"), QStringLiteral("dBm"), iip3, Qt::DotLine, PlotYAxis::Left);
    if (showSnr && anyFinite(snr))
        add(SweepTrace::Snr, tr("SNR (total)"), QStringLiteral("dB"), snr, Qt::DashDotLine, PlotYAxis::Right);
    m_sweepPlot->autoscale();
    const double wanted = m_run->budget.inputFrequency;
    if (std::isfinite(wanted) && wanted >= x.front() && wanted <= x.back())
        m_sweepPlot->addMarker(m_sweepTraces[static_cast<int>(SweepTrace::Gain)], wanted);
}

QString BudgetView::spurMessage() const
{
    return m_spurMessage->isHidden() ? QString() : m_spurMessage->text();
}

void BudgetView::showSpurs()
{
    QString message;
    bool failure = false;
    const auto setMessage = [&] {
        m_spurMessage->setText(message);
        m_spurMessage->setHidden(message.isEmpty());
        m_spurMessage->setProperty("failure", failure);
        applyTheme();
    };
    m_spurPlot->clear();
    if (!m_run || m_run->budget.stages.empty()) {
        m_spurCount->clear();
        setMessage();
        return;
    }
    const rf::SpurResult &spurs = m_run->spurs;
    const bool hasMixer = std::ranges::any_of(m_run->chain.stages, [](const rf::ChainStage &stage) {
        return stage.translation == rf::ChainStage::Translation::Mixer;
    });
    if (!spurs.errors.isEmpty()) {
        message = tr("The spur analysis failed: %1").arg(spurs.errors.join(QStringLiteral("; ")));
        failure = true;
    } else if (!hasMixer) {
        message = tr("The chain has no mixer, so there are no mixer spurs.");
    } else if (spurs.spurs.empty() && std::isnan(spurs.wantedOutputFrequency)) {
        message = tr("The spur analysis is off for this analysis; turn it on with “Edit…”.");
    } else if (spurs.spurs.empty()) {
        message = tr("No spurs above the listing threshold.");
    }
    setMessage();

    const int total = static_cast<int>(spurs.spurs.size());
    const int inBand = static_cast<int>(std::ranges::count_if(spurs.spurs, [](const rf::Spur &s) { return s.inBand; }));
    m_spurCount->setText(total == 0 ? QString()
                                    : tr("%1 spurs, %2 in band; %3 shown").arg(total).arg(inBand).arg(m_spurFilter->rowCount()));
    if (total == 0)
        return;

    // Chart: what the wanted signal generates at the output.
    const rf::BudgetStage &out = m_run->budget.stages.back();
    const double wantedF = std::isfinite(spurs.wantedOutputFrequency) ? spurs.wantedOutputFrequency : out.outputFrequency;
    const double wantedP = std::isfinite(spurs.wantedOutputDbm) ? spurs.wantedOutputDbm : out.signalDbm;
    std::vector<std::pair<double, double>> outBand, inBandPoints;
    double lowest = wantedP;
    for (const rf::Spur &s : spurs.spurs) {
        if (s.kind != rf::Spur::Kind::Product && s.kind != rf::Spur::Kind::LoLeakage)
            continue;
        if (!std::isfinite(s.frequency) || !std::isfinite(s.levelDbm))
            continue;
        (s.inBand ? inBandPoints : outBand).emplace_back(s.frequency, s.levelDbm);
        lowest = std::min(lowest, s.levelDbm);
    }
    const auto addPoints = [&](const QString &name, std::vector<std::pair<double, double>> points, double width) {
        if (points.empty())
            return;
        std::ranges::sort(points);
        std::vector<double> x, y;
        for (const auto &[f, p] : points) {
            x.push_back(f);
            y.push_back(p);
        }
        TraceStyle style;
        style.lineStyle = Qt::NoPen;
        style.showPoints = true;
        style.lineWidth = width;
        const int id = m_spurPlot->addTrace(name, QStringLiteral("dBm"), style);
        m_spurPlot->setTraceData(id, x, y);
    };
    if (std::isfinite(wantedF) && std::isfinite(wantedP)) {
        const double half = std::isfinite(m_run->budget.bandwidth) ? m_run->budget.bandwidth / 2 : 0;
        const double floor = lowest - 10;
        TraceStyle band;
        band.lineStyle = Qt::DashLine;
        band.lineWidth = 1.5;
        const int bandId = m_spurPlot->addTrace(tr("Wanted band"), QStringLiteral("dBm"), band);
        const std::vector<double> bx{wantedF - half, wantedF - half, wantedF + half, wantedF + half};
        const std::vector<double> by{floor, wantedP + 5, wantedP + 5, floor};
        m_spurPlot->setTraceData(bandId, bx, by);
        addPoints(tr("Wanted output"), {{wantedF, wantedP}}, 3.0);
    }
    addPoints(tr("Spurs"), outBand, 2.0);
    addPoints(tr("In-band spurs"), inBandPoints, 2.5);
    m_spurPlot->autoscale();
    // Keep the wanted output (often near 0 Hz at an IF) clear of the frame.
    double low = std::isfinite(wantedF) ? wantedF : INFINITY, high = low;
    for (const auto &points : {outBand, inBandPoints}) {
        for (const auto &point : points) {
            low = std::min(low, point.first);
            high = std::max(high, point.first);
        }
    }
    if (std::isfinite(low) && std::isfinite(high)) {
        const double margin = std::max(0.03 * (high - low), 0.05 * std::abs(high));
        m_spurPlot->setXRange(low - margin, high + margin);
    }
}

void BudgetView::showPlan()
{
    m_plan->setRowCount(0);
    if (!m_run || !m_run->chain.ok() || m_run->chain.stages.empty())
        return;
    const rf::SignalChain &chain = m_run->chain;
    const auto addRow = [this](const QStringList &cells, const QString &tip = {}) {
        const int row = m_plan->rowCount();
        m_plan->insertRow(row);
        for (int c = 0; c < cells.size(); ++c) {
            auto *item = new QTableWidgetItem(cells.at(c));
            if (c == 0 || (c >= 3 && c != 5 && c != 6))
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (!tip.isEmpty())
                item->setToolTip(tip);
            m_plan->setItem(row, c, item);
        }
    };
    const rf::BudgetResult &budget = m_run->budget;
    addRow({tr("0"), chain.start, chain.startKind, QString(), hertz(chain.inputFrequency), tr("start"), QString(), QString(),
            dbm(chain.inputPowerDbm)},
           budget.hasDac ? tr("First DAC image at %1").arg(hertz(budget.dacImageFrequency)) : QString());
    for (size_t i = 0; i < chain.stages.size(); ++i) {
        const rf::ChainStage &s = chain.stages[i];
        QString conversion;
        QString tip;
        switch (s.translation) {
        case rf::ChainStage::Translation::None:
            break;
        case rf::ChainStage::Translation::Mixer:
            conversion = QStringLiteral("%1, %2")
                             .arg(s.upConversion ? tr("up-conversion") : tr("down-conversion"),
                                  s.sumProduct ? tr("f_in + f_LO") : tr("|f_in − f_LO|"));
            tip = tr("The LO level is the LO block's power plus the gain of the LO path.");
            if (s.loMultiplication > 1)
                tip += QLatin1Char(' ') + tr("The LO is multiplied by %1 on its way (phase noise + %2 dB).")
                                              .arg(s.loMultiplication)
                                              .arg(20.0 * std::log10(s.loMultiplication), 0, 'f', 1);
            break;
        case rf::ChainStage::Translation::Multiplier:
            conversion = tr("× %1").arg(s.factor);
            break;
        }
        addRow({QString::number(i + 1), s.path, s.kind.isEmpty() ? s.name : s.kind, hertz(s.inputFrequency),
                hertz(s.outputFrequency), conversion,
                s.loMultiplication > 1 ? QStringLiteral("%1 (×%2)").arg(s.loSource).arg(s.loMultiplication) : s.loSource,
                hertz(s.loFrequency), dbm(s.loPowerDbm)},
               tip);
    }
    if (!chain.end.isEmpty())
        addRow({QString::number(chain.stages.size() + 1), chain.end, chain.endKind, hertz(chain.stages.back().outputFrequency),
                QString(), tr("end"), QString(), QString(), QString()});
    m_plan->resizeColumnsToContents();
}

// ---------------------------------------------------------------- export

QString BudgetView::tableText(QChar separator) const
{
    return m_tableModel->text(separator);
}

void BudgetView::copyTable() const
{
    std::set<int> unique;
    if (m_table->selectionModel()) {
        for (const QModelIndex &index : m_table->selectionModel()->selectedIndexes())
            unique.insert(index.row());
    }
    const QList<int> rows(unique.begin(), unique.end());
    const QString text = m_tableModel->text(QLatin1Char('\t'), rows);
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

bool BudgetView::exportCsv(const QString &path, QString *error) const
{
    const QString text = m_tableModel->text(QLatin1Char(','));
    if (text.isEmpty()) {
        if (error)
            *error = tr("There is no budget to export.");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    file.write(text.toUtf8());
    if (!file.commit()) {
        if (error)
            *error = tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- theme

void BudgetView::applyTheme()
{
    const bool dark = isDark(palette());
    m_tableModel->setDarkTheme(dark);
    m_spurModel->setDarkTheme(dark);
    const QColor muted = palette().color(QPalette::PlaceholderText);
    const QColor warning = dark ? QColor(0xE8, 0xC0, 0x60) : QColor(0x8A, 0x5A, 0x00);
    const bool failure = m_spurMessage->property("failure").toBool();
    m_spurMessage->setStyleSheet(QStringLiteral("QLabel { color: %1; padding: 4px 6px; }")
                                     .arg((failure ? warning : muted).name()));
    m_stageLegend->setStyleSheet(QStringLiteral("QLabel { color: %1; }").arg(palette().color(QPalette::WindowText).name()));
    if (auto *hint = findChild<QLabel *>(QStringLiteral("budgetHint")))
        hint->setStyleSheet(QStringLiteral("QLabel { color: %1; }").arg(muted.name()));
}

void BudgetView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_tableModel)
        applyTheme();
}

} // namespace lvd::sim
