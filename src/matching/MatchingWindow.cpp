#include "matching/MatchingWindow.h"

#include "core/Document.h"
#include "core/Expression.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "matching/MatchLadderView.h"
#include "matching/MatchingInsert.h"
#include "plot/PlotWidget.h"
#include "rfdata/NetworkMath.h"
#include "rfdata/NetworkOps.h"
#include "rfdata/Touchstone.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lvd {

using namespace lvd::rf;

namespace {

constexpr double kPi = 3.14159265358979323846;

QString formatNumber(double value, int digits = 6)
{
    return formatEngineering(value, QString(), digits);
}

std::optional<double> readNumber(const QLineEdit *edit)
{
    return parseNumber(edit->text());
}

const std::vector<std::pair<QString, std::optional<ESeries>>> &seriesChoices()
{
    static const std::vector<std::pair<QString, std::optional<ESeries>>> choices = {
        {QString(), std::nullopt},
        {QStringLiteral("E12"), ESeries::E12},
        {QStringLiteral("E24"), ESeries::E24},
        {QStringLiteral("E48"), ESeries::E48},
        {QStringLiteral("E96"), ESeries::E96},
    };
    return choices;
}

QString formatFrequency(double hz)
{
    if (!std::isfinite(hz))
        return QStringLiteral("—");
    return formatEngineering(hz, QStringLiteral("Hz"), 4);
}

} // namespace

MatchingWindow::MatchingWindow(QWidget *parent) : QWidget(parent)
{
    setWindowTitle(tr("Matching network synthesis"));

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    auto *inputScroll = new QScrollArea(splitter);
    inputScroll->setWidgetResizable(true);
    inputScroll->setFrameShape(QFrame::NoFrame);
    inputScroll->setWidget(createInputs());
    inputScroll->setMinimumWidth(inputScroll->widget()->minimumSizeHint().width() + 24);

    auto *right = new QSplitter(Qt::Vertical, splitter);
    m_solutions = new QTreeWidget(right);
    m_solutions->setRootIsDecorated(false);
    m_solutions->setUniformRowHeights(true);
    m_solutions->setAlternatingRowColors(true);
    m_solutions->setHeaderLabels({tr("#"), tr("Network"), tr("Elements (source to load)"), tr("Q"), tr("BW (3 dB)"),
                                  tr("|Γ| at f0")});
    m_solutions->header()->setStretchLastSection(false);
    m_solutions->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_solutions->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_solutions->setMinimumHeight(110);
    connect(m_solutions, &QTreeWidget::currentItemChanged, this, [this] { showSelection(); });

    m_ladder = new MatchLadderView(right);

    auto *plots = new QSplitter(Qt::Horizontal, right);
    m_smith = new PlotWidget(plots);
    m_smith->setKind(PlotKind::Smith);
    m_smith->setTitle(tr("Impedance path at f0"));
    m_smith->setMarkerTableVisible(false);
    m_response = new PlotWidget(plots);
    m_response->setTitle(tr("Input reflection"));
    m_response->setXAxis(tr("Frequency"), QStringLiteral("Hz"));
    m_response->setYAxis(tr("|Γ|"), QStringLiteral("dB"));
    m_response->setMarkerTableVisible(false);
    plots->setStretchFactor(0, 1);
    plots->setStretchFactor(1, 1);

    right->addWidget(m_solutions);
    right->addWidget(m_ladder);
    right->addWidget(plots);
    right->setStretchFactor(0, 2);
    right->setStretchFactor(1, 2);
    right->setStretchFactor(2, 5);
    splitter->addWidget(inputScroll);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    m_insertButton = new QPushButton(tr("Insert into schematic"), this);
    m_insertButton->setToolTip(tr("Place the selected network in the schematic, between net labels"));
    connect(m_insertButton, &QPushButton::clicked, this, &MatchingWindow::insertClicked);

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_message, 1);
    bottom->addWidget(m_insertButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(splitter, 1);
    layout->addLayout(bottom);

    updateTopologyFields();
    updateInsertButton();
    recompute();
}

MatchingWindow::~MatchingWindow() = default;

QSize MatchingWindow::sizeHint() const { return QSize(1280, 860); }

QWidget *MatchingWindow::createInputs()
{
    auto *panel = new QWidget;
    auto *column = new QVBoxLayout(panel);

    // Design frequency and reference impedance.
    auto *design = new QGroupBox(tr("Design"), panel);
    auto *designForm = new QFormLayout(design);
    m_frequency = new QLineEdit(QStringLiteral("1 GHz"), design);
    m_frequency->setObjectName(QStringLiteral("frequency"));
    m_frequency->setToolTip(tr("Design frequency, with SI prefix: 2.4GHz, 868 MHz, 1e9"));
    designForm->addRow(tr("Frequency:"), m_frequency);
    m_z0 = new QLineEdit(QStringLiteral("50"), design);
    m_z0->setToolTip(tr("Reference impedance for reflection-coefficient entry and the Smith chart"));
    designForm->addRow(tr("Reference Z0 (Ω):"), m_z0);
    column->addWidget(design);

    const auto impedanceGroup = [&](const QString &title, ImpedanceEditor &editor) {
        auto *group = new QGroupBox(title, panel);
        auto *form = new QFormLayout(group);
        createImpedanceEditor(editor);
        form->addRow(tr("Entry:"), editor.mode);
        auto *a = new QHBoxLayout;
        a->addWidget(editor.first, 1);
        a->addWidget(editor.firstUnit);
        auto *b = new QHBoxLayout;
        b->addWidget(editor.second, 1);
        b->addWidget(editor.secondUnit);
        form->addRow(QString(), a);
        form->addRow(QString(), b);
        column->addWidget(group);
        return form;
    };
    impedanceGroup(tr("Source"), m_source);
    QFormLayout *loadForm = impedanceGroup(tr("Load"), m_load);
    m_touchstoneButton = new QPushButton(tr("From Touchstone file…"), panel);
    m_touchstoneButton->setToolTip(tr("Read the load at the design frequency: Γ of a one-port or S11 of a two-port"));
    connect(m_touchstoneButton, &QPushButton::clicked, this, &MatchingWindow::browseTouchstone);
    loadForm->addRow(QString(), m_touchstoneButton);
    writeImpedance(m_source, {50, 0});
    writeImpedance(m_load, {50, 0});

    auto *network = new QGroupBox(tr("Network"), panel);
    auto *form = new QFormLayout(network);
    m_topology = new QComboBox(network);
    for (const MatchTopology t : {MatchTopology::LNetwork, MatchTopology::PiNetwork, MatchTopology::TNetwork,
                                  MatchTopology::MultiSection, MatchTopology::QuarterWave, MatchTopology::SingleStub,
                                  MatchTopology::DoubleStub})
        m_topology->addItem(matchTopologyName(t), static_cast<int>(t));
    form->addRow(tr("Topology:"), m_topology);
    m_q = new QDoubleSpinBox(network);
    m_q->setRange(0.1, 1000);
    m_q->setDecimals(2);
    m_q->setValue(5);
    m_q->setToolTip(tr("Loaded Q of the Pi or T network; at least the Q of the L-network"));
    m_qLabel = new QLabel(tr("Loaded Q:"), network);
    form->addRow(m_qLabel, m_q);
    m_sections = new QSpinBox(network);
    m_sections->setRange(1, 20);
    m_sections->setValue(2);
    m_sectionsLabel = new QLabel(tr("Sections:"), network);
    form->addRow(m_sectionsLabel, m_sections);
    m_lineZ0 = new QLineEdit(network);
    m_lineZ0->setPlaceholderText(tr("Re(Zs)"));
    m_lineZ0->setToolTip(tr("Characteristic impedance of the lines and stubs; empty uses Re(Zs)"));
    m_lineZ0Label = new QLabel(tr("Line Z0 (Ω):"), network);
    form->addRow(m_lineZ0Label, m_lineZ0);
    m_spacing = new QDoubleSpinBox(network);
    m_spacing->setRange(1, 179);
    m_spacing->setDecimals(1);
    m_spacing->setValue(45);
    m_spacing->setSuffix(QStringLiteral("°"));
    m_spacingLabel = new QLabel(tr("Stub spacing:"), network);
    form->addRow(m_spacingLabel, m_spacing);
    m_series = new QComboBox(network);
    for (const auto &[name, series] : seriesChoices())
        m_series->addItem(name.isEmpty() ? tr("Exact values") : name);
    m_series->setToolTip(tr("Snap capacitors and inductors to a standard series (RF-13)"));
    form->addRow(tr("Standard values:"), m_series);
    column->addWidget(network);
    column->addStretch(1);

    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);

    const auto changed = [this] { recompute(); };
    for (QLineEdit *edit : {m_frequency, m_z0, m_lineZ0, m_source.first, m_source.second, m_load.first, m_load.second})
        connect(edit, &QLineEdit::editingFinished, this, changed);
    connect(m_topology, &QComboBox::currentIndexChanged, this, [this] {
        updateTopologyFields();
        recompute();
    });
    connect(m_q, &QDoubleSpinBox::valueChanged, this, changed);
    connect(m_sections, &QSpinBox::valueChanged, this, changed);
    connect(m_spacing, &QDoubleSpinBox::valueChanged, this, changed);
    connect(m_series, &QComboBox::currentIndexChanged, this, changed);
    connect(m_source.mode, &QComboBox::currentIndexChanged, this, [this] { impedanceModeChanged(m_source); });
    connect(m_load.mode, &QComboBox::currentIndexChanged, this, [this] { impedanceModeChanged(m_load); });
    return panel;
}

void MatchingWindow::createImpedanceEditor(ImpedanceEditor &editor)
{
    editor.mode = new QComboBox;
    editor.mode->addItem(tr("R + jX"));
    editor.mode->addItem(tr("Reflection |Γ| ∠ φ"));
    editor.mode->setProperty("lvdPrevious", 0);
    editor.first = new QLineEdit;
    editor.second = new QLineEdit;
    editor.firstUnit = new QLabel;
    editor.secondUnit = new QLabel;
    for (QLabel *label : {editor.firstUnit, editor.secondUnit})
        label->setMinimumWidth(label->fontMetrics().horizontalAdvance(QStringLiteral("jX Ω ")));
}

double MatchingWindow::referenceZ0() const
{
    const auto z0 = readNumber(m_z0);
    return z0 && *z0 > 0 ? *z0 : 50.0;
}

std::optional<std::complex<double>> MatchingWindow::readImpedance(const ImpedanceEditor &editor, QString *error) const
{
    const auto a = readNumber(editor.first), b = readNumber(editor.second);
    if (!a || !b) {
        *error = tr("Enter numbers for both impedance fields.");
        return std::nullopt;
    }
    if (editor.mode->currentIndex() == 0)
        return std::complex<double>(*a, *b);
    if (*a < 0 || *a >= 1) {
        *error = tr("|Γ| must be at least 0 and below 1.");
        return std::nullopt;
    }
    return impedanceFromReflection(std::polar(*a, *b * kPi / 180), referenceZ0());
}

void MatchingWindow::writeImpedance(ImpedanceEditor &editor, std::complex<double> z)
{
    const QSignalBlocker b1(editor.first), b2(editor.second);
    if (editor.mode->currentIndex() == 0) {
        editor.first->setText(formatNumber(z.real()));
        editor.second->setText(formatNumber(z.imag()));
        editor.firstUnit->setText(tr("R Ω"));
        editor.secondUnit->setText(tr("jX Ω"));
    } else {
        const std::complex<double> g = reflectionFromImpedance(z, referenceZ0());
        editor.first->setText(QString::number(std::abs(g), 'g', 6));
        editor.second->setText(QString::number(std::arg(g) * 180 / kPi, 'g', 6));
        editor.firstUnit->setText(tr("|Γ|"));
        editor.secondUnit->setText(tr("φ °"));
    }
}

void MatchingWindow::impedanceModeChanged(ImpedanceEditor &editor)
{
    // Convert the value entered in the previous representation.
    const int now = editor.mode->currentIndex();
    const int before = editor.mode->property("lvdPrevious").toInt();
    editor.mode->setProperty("lvdPrevious", now);
    QString error;
    {
        const QSignalBlocker blocker(editor.mode);
        editor.mode->setCurrentIndex(before);
        const auto z = readImpedance(editor, &error);
        editor.mode->setCurrentIndex(now);
        if (z) {
            writeImpedance(editor, *z);
            return;
        }
    }
    writeImpedance(editor, {50, 0});
}

void MatchingWindow::updateTopologyFields()
{
    const MatchTopology t = topology();
    const bool piT = t == MatchTopology::PiNetwork || t == MatchTopology::TNetwork;
    const bool distributed = t == MatchTopology::QuarterWave || t == MatchTopology::SingleStub || t == MatchTopology::DoubleStub;
    m_qLabel->setVisible(piT);
    m_q->setVisible(piT);
    m_sectionsLabel->setVisible(t == MatchTopology::MultiSection);
    m_sections->setVisible(t == MatchTopology::MultiSection);
    m_lineZ0Label->setVisible(distributed);
    m_lineZ0->setVisible(distributed);
    m_spacingLabel->setVisible(t == MatchTopology::DoubleStub);
    m_spacing->setVisible(t == MatchTopology::DoubleStub);
    m_series->setEnabled(!distributed);
}

MatchTopology MatchingWindow::topology() const
{
    return static_cast<MatchTopology>(m_topology->currentData().toInt());
}

void MatchingWindow::setTopology(MatchTopology t)
{
    const int index = m_topology->findData(static_cast<int>(t));
    if (index >= 0)
        m_topology->setCurrentIndex(index);
}

void MatchingWindow::setLoadedQ(double q) { m_q->setValue(q); }
void MatchingWindow::setSections(int sections) { m_sections->setValue(sections); }
void MatchingWindow::setStubSpacing(double degrees) { m_spacing->setValue(degrees); }

void MatchingWindow::setLineImpedance(double z0)
{
    m_lineZ0->setText(z0 > 0 ? formatNumber(z0) : QString());
    recompute();
}

void MatchingWindow::setStandardSeries(std::optional<ESeries> series)
{
    const auto &choices = seriesChoices();
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].second == series)
            m_series->setCurrentIndex(static_cast<int>(i));
    }
}

void MatchingWindow::setImpedances(std::complex<double> zs, std::complex<double> zl, double frequency)
{
    m_frequency->setText(formatEngineering(frequency, QStringLiteral("Hz"), 8));
    writeImpedance(m_source, zs);
    writeImpedance(m_load, zl);
    recompute();
}

void MatchingWindow::setTarget(Project *project, Document *document)
{
    m_project = project;
    m_document = document && document->schematic() ? document : nullptr;
    updateInsertButton();
}

bool MatchingWindow::loadFromTouchstone(const QString &path, QString *error)
{
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        setMessage(message, true);
        return false;
    };
    const auto f0 = readNumber(m_frequency);
    if (!f0 || *f0 <= 0)
        return fail(tr("Enter the design frequency first."));
    const TouchstoneReadResult read = readTouchstone(path);
    if (!read.network)
        return fail(tr("Cannot read %1: %2").arg(QFileInfo(path).fileName(), read.error));
    const NetworkData &network = *read.network;
    if (network.portCount() < 1 || network.portCount() > 2)
        return fail(tr("%1 has %2 ports; a one-port or two-port file is needed.").arg(QFileInfo(path).fileName()).arg(network.portCount()));
    QString interpolationError;
    const auto at = interpolate(network, {*f0}, Extrapolation::Error, &interpolationError);
    if (!at)
        return fail(tr("%1 does not cover %2: %3")
                        .arg(QFileInfo(path).fileName(), formatEngineering(*f0, QStringLiteral("Hz")), interpolationError));
    const std::complex<double> gamma = at->s.front()(0, 0);
    if (std::abs(gamma) >= 1)
        return fail(tr("The load in %1 has |Γ| ≥ 1 at the design frequency; it cannot be matched.").arg(QFileInfo(path).fileName()));
    writeImpedance(m_load, impedanceFromReflection(gamma, network.z0.front()));
    recompute();
    Q_EMIT statusMessage(tr("Load read from %1 at %2").arg(QFileInfo(path).fileName(), formatEngineering(*f0, QStringLiteral("Hz"))));
    return true;
}

void MatchingWindow::browseTouchstone()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Load from Touchstone file"), m_project ? m_project->directory() : QString(),
                                                      tr("Touchstone files (*.s1p *.s2p *.ts);;All files (*)"));
    if (!path.isEmpty())
        loadFromTouchstone(path);
}

void MatchingWindow::setMessage(const QString &text, bool error)
{
    m_message->setText(text);
    QPalette pal = m_message->palette();
    pal.setColor(QPalette::WindowText, error ? QColor(palette().color(QPalette::Window).lightness() < 128 ? 0xFF7B72 : 0xC62828)
                                             : palette().color(QPalette::WindowText));
    m_message->setPalette(pal);
}

QString MatchingWindow::message() const { return m_message->text(); }

void MatchingWindow::recompute()
{
    if (m_updating)
        return;
    m_updating = true;
    const int previous = selectedSolution();
    m_result = MatchResult{};
    m_ideal.clear();

    QString error;
    const auto f0 = readNumber(m_frequency);
    MatchRequest request;
    if (!f0 || *f0 <= 0) {
        error = tr("Enter a positive design frequency.");
    } else {
        request.frequency = *f0;
        const auto zs = readImpedance(m_source, &error);
        const auto zl = zs ? readImpedance(m_load, &error) : std::nullopt;
        if (zs && zl) {
            request.zs = *zs;
            request.zl = *zl;
            request.topology = topology();
            request.q = m_q->value();
            request.sections = m_sections->value();
            request.stubSpacingDeg = m_spacing->value();
            if (!m_lineZ0->text().trimmed().isEmpty()) {
                const auto z0 = readNumber(m_lineZ0);
                if (!z0 || *z0 <= 0)
                    error = tr("The line impedance must be a positive number.");
                else
                    request.lineZ0 = *z0;
            }
        }
    }
    if (error.isEmpty()) {
        MatchResult ideal = synthesizeMatch(request);
        if (ideal.ok()) {
            m_ideal = ideal.solutions;
            const auto series = seriesChoices()[static_cast<size_t>(std::max(0, m_series->currentIndex()))].second;
            if (series) {
                for (MatchSolution &s : ideal.solutions)
                    s = snapSolution(s, *series);
            }
        }
        m_result = ideal;
        if (!m_result.ok())
            error = m_result.error;
    } else {
        m_result.error = error;
    }

    fillResults();
    if (!error.isEmpty()) {
        setMessage(error, true);
        Q_EMIT statusMessage(error);
    } else {
        QString text = tr("%n solution(s).", nullptr, static_cast<int>(m_result.solutions.size()));
        if (!m_result.warnings.isEmpty())
            text += QLatin1Char(' ') + m_result.warnings.join(QLatin1Char(' '));
        setMessage(text, false);
    }
    m_updating = false;
    selectSolution(previous >= 0 && previous < static_cast<int>(m_result.solutions.size()) ? previous : 0);
}

void MatchingWindow::fillResults()
{
    const QSignalBlocker blocker(m_solutions);
    m_solutions->clear();
    for (size_t i = 0; i < m_result.solutions.size(); ++i) {
        const MatchSolution &s = m_result.solutions[i];
        QStringList elements;
        QStringList ideal;
        for (const MatchElement &e : s.elements) {
            elements << describeElement(e);
            MatchElement exact = e;
            exact.value = e.idealValue;
            ideal << describeElement(exact);
        }
        auto *item = new QTreeWidgetItem(m_solutions);
        item->setText(0, QString::number(i + 1));
        item->setText(1, s.description);
        item->setText(2, elements.isEmpty() ? tr("(none)") : elements.join(QStringLiteral("; ")));
        item->setText(3, QString::number(s.loadedQ, 'g', 3));
        item->setText(4, formatFrequency(s.bandwidth3dB));
        item->setText(5, s.mismatch < 1e-9 ? tr("0") : QStringLiteral("%1 (%2 dB)").arg(s.mismatch, 0, 'g', 3).arg(db20(s.mismatch), 0, 'f', 1));
        item->setToolTip(1, s.description);
        item->setToolTip(2, s.snapped ? tr("Snapped: %1\nExact: %2").arg(elements.join(QStringLiteral("; ")), ideal.join(QStringLiteral("; ")))
                                      : elements.join(QLatin1Char('\n')));
        item->setToolTip(3, tr("Largest nodal Q (|X|/R between elements); estimated bandwidth f0/Q = %1")
                                .arg(formatFrequency(s.bandwidthEstimate)));
        item->setToolTip(4, tr("Band around f0 where |Γ|² ≤ 1/2 (half the power reflected); "
                               "— when the band is not bounded between 0 and 3·f0"));
        item->setData(0, Qt::UserRole, static_cast<int>(i));
    }
}

int MatchingWindow::selectedSolution() const
{
    const QTreeWidgetItem *item = m_solutions->currentItem();
    return item ? item->data(0, Qt::UserRole).toInt() : -1;
}

void MatchingWindow::selectSolution(int index)
{
    if (index < 0 || index >= m_solutions->topLevelItemCount()) {
        m_solutions->setCurrentItem(nullptr);
        showSelection();
        return;
    }
    m_solutions->setCurrentItem(m_solutions->topLevelItem(index));
    showSelection();
}

void MatchingWindow::showSelection()
{
    if (m_updating)
        return;
    const int index = selectedSolution();
    m_smith->clear();
    m_response->clear();
    updateInsertButton();
    if (index < 0 || index >= static_cast<int>(m_result.solutions.size())) {
        m_ladder->setSolution(std::nullopt);
        return;
    }
    const MatchSolution &s = m_result.solutions[static_cast<size_t>(index)];
    m_ladder->setSolution(s);

    // Smith chart: the path from the load to the matched point.
    const double z0 = referenceZ0();
    m_smith->setSmithZ0(z0);
    const auto gammaOf = [z0](std::complex<double> z) {
        if (std::abs(z) > 1e200)
            return std::complex<double>(1, 0);
        return (z - z0) / (z + z0);
    };
    const std::vector<TrajectorySegment> path = impedanceTrajectory(s, 96);
    const bool perElement = path.size() <= 4;
    std::vector<double> x;
    std::vector<std::complex<double>> g;
    int number = 0;
    for (const TrajectorySegment &segment : path) {
        if (perElement) {
            x.clear();
            g.clear();
        }
        for (const std::complex<double> z : segment.impedances) {
            x.push_back(static_cast<double>(x.size()));
            g.push_back(gammaOf(z));
        }
        ++number;
        if (perElement) {
            const MatchElement &e = s.elements[static_cast<size_t>(segment.elementIndex)];
            const int trace = m_smith->addTrace(QStringLiteral("%1. %2").arg(number).arg(describeElement(e)));
            m_smith->setTraceData(trace, x, g);
        }
    }
    if (!perElement && !g.empty()) {
        const int trace = m_smith->addTrace(tr("Impedance path"));
        m_smith->setTraceData(trace, x, g);
    }
    const auto point = [&](const QString &name, std::complex<double> z, QColor color) {
        TraceStyle style;
        style.color = color;
        style.lineStyle = Qt::NoPen;
        style.showPoints = true;
        style.lineWidth = 3;
        const int trace = m_smith->addTrace(name, QString(), style);
        const std::vector<double> px{0.0};
        const std::vector<std::complex<double>> pg{gammaOf(z)};
        m_smith->setTraceData(trace, px, pg);
    };
    const bool dark = m_smith->theme().dark;
    point(tr("ZL"), s.zl, dark ? QColor(0xF2, 0xB8, 0x4B) : QColor(0xB2, 0x6B, 0x00));
    point(tr("Zs*"), std::conj(s.zs), dark ? QColor(0xE6, 0x67, 0x67) : QColor(0xC6, 0x28, 0x28));
    if (s.snapped)
        point(tr("Zin (snapped)"), inputImpedance(s, s.frequency), dark ? QColor(0xB0, 0x9C, 0xFF) : QColor(0x5E, 0x35, 0xB1));

    // |Gamma| in dB versus frequency, f0 +- 50 %.
    const int points = 401;
    std::vector<double> f(points);
    for (int i = 0; i < points; ++i)
        f[static_cast<size_t>(i)] = s.frequency * (0.5 + double(i) / (points - 1));
    const auto toDb = [](const std::vector<double> &magnitudes) {
        std::vector<double> db;
        db.reserve(magnitudes.size());
        for (const double m : magnitudes)
            db.push_back(std::max(-80.0, 20 * std::log10(std::max(m, 1e-12))));
        return db;
    };
    double lowest = 0;
    if (s.snapped && index < static_cast<int>(m_ideal.size())) {
        TraceStyle dashed;
        dashed.lineStyle = Qt::DashLine;
        const int ideal = m_response->addTrace(tr("Exact values"), QStringLiteral("dB"), dashed);
        const std::vector<double> db = toDb(reflectionMagnitude(m_ideal[static_cast<size_t>(index)], f));
        m_response->setTraceData(ideal, f, db);
        lowest = std::min(lowest, *std::min_element(db.begin(), db.end()));
    }
    const int trace = m_response->addTrace(s.snapped ? tr("|Γ| (%1)").arg(seriesName(*s.series)) : tr("|Γ|"), QStringLiteral("dB"));
    const std::vector<double> db = toDb(reflectionMagnitude(s, f));
    m_response->setTraceData(trace, f, db);
    lowest = std::min(lowest, *std::min_element(db.begin(), db.end()));
    m_response->setXRange(f.front(), f.back());
    m_response->setYRange(std::min(-30.0, std::floor(lowest / 10) * 10), 0);
}

bool MatchingWindow::canInsert() const
{
    return m_project && m_document && m_document->schematic() && selectedSolution() >= 0;
}

void MatchingWindow::updateInsertButton()
{
    m_insertButton->setEnabled(canInsert());
    if (!m_project || !m_document)
        m_insertButton->setToolTip(tr("Open a schematic to insert the network"));
    else
        m_insertButton->setToolTip(tr("Place the selected network in %1, between net labels").arg(m_document->name()));
}

bool MatchingWindow::insertSelected(QString *error)
{
    QString message;
    bool ok = false;
    if (!canInsert()) {
        message = tr("Select a solution and open a schematic first.");
    } else {
        // Below everything already on the sheet.
        const Schematic &sheet = *m_document->schematic();
        int minX = std::numeric_limits<int>::max(), maxY = std::numeric_limits<int>::min();
        const auto take = [&](QPoint p) {
            minX = std::min(minX, p.x());
            maxY = std::max(maxY, p.y());
        };
        for (const PlacedComponent &c : sheet.components())
            take(c.at);
        for (const Wire &w : sheet.wires()) {
            for (const QPoint &p : w.points)
                take(p);
        }
        for (const NetLabel &l : sheet.labels())
            take(l.at);
        for (const TextNote &t : sheet.texts())
            take(t.at);
        const QPoint origin = maxY == std::numeric_limits<int>::min() ? QPoint(0, 0) : QPoint(minX, maxY + 150);
        const MatchSolution &s = m_result.solutions[static_cast<size_t>(selectedSolution())];
        const auto inserted = insertMatchingNetwork(*m_project, *m_document, s, origin, &message);
        if (inserted) {
            ok = true;
            message = tr("Inserted the matching network into %1 between %2 and %3.")
                          .arg(m_document->name(), inserted->inputLabel, inserted->outputLabel);
            Q_EMIT networkInserted(inserted->inputLabel, inserted->outputLabel);
        }
    }
    if (error && !ok)
        *error = message;
    setMessage(message, !ok);
    Q_EMIT statusMessage(message);
    return ok;
}

void MatchingWindow::insertClicked() { insertSelected(); }

void MatchingWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        if (m_ladder)
            m_ladder->update();
        if (!m_updating && m_smith && m_solutions)
            showSelection();
    }
}

} // namespace lvd
