#include "simulation_ui/TuningPanel.h"

#include "core/Document.h"
#include "core/Project.h"
#include "simulation_ui/DesignVariables.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lvd::sim {

struct TuningPanel::Tuner {
    rf::DesignVariable variable;
    double documentValue = 0;
    double value = 0;

    QFrame *row = nullptr;
    QLabel *name = nullptr;
    QLabel *documentLabel = nullptr;
    QToolButton *remove = nullptr;
    QSlider *slider = nullptr;
    QLineEdit *valueEdit = nullptr;
    QLineEdit *minimumEdit = nullptr;
    QLineEdit *maximumEdit = nullptr;
    QCheckBox *log = nullptr;
    QComboBox *series = nullptr;

    bool logarithmic() const { return variable.logarithmic && variable.minimum > 0; }
};

namespace {

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

bool differs(double a, double b)
{
    return std::abs(a - b) > 1e-12 * std::max(std::abs(a), std::abs(b));
}

} // namespace

TuningPanel::TuningPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("tuningPanel"));

    m_title = new QLabel(tr("Tune"), this);
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);

    m_addMenu = new QMenu(this);
    connect(m_addMenu, &QMenu::aboutToShow, this, &TuningPanel::fillAddMenu);
    m_addButton = new QToolButton(this);
    m_addButton->setObjectName(QStringLiteral("addTunable"));
    m_addButton->setText(tr("Add…"));
    m_addButton->setToolTip(tr("Add a variable or a component parameter to tune"));
    m_addButton->setPopupMode(QToolButton::InstantPopup);
    m_addButton->setMenu(m_addMenu);
    m_addButton->setAutoRaise(true);

    m_closeButton = new QToolButton(this);
    m_closeButton->setObjectName(QStringLiteral("closeTuning"));
    m_closeButton->setText(QStringLiteral("✕"));
    m_closeButton->setToolTip(tr("Close tuning; values not applied are discarded"));
    m_closeButton->setAutoRaise(true);
    connect(m_closeButton, &QToolButton::clicked, this, &TuningPanel::closeRequested);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(6, 4, 2, 0);
    header->addWidget(m_title);
    header->addStretch(1);
    header->addWidget(m_addButton);
    header->addWidget(m_closeButton);

    m_hint = new QLabel(tr("Add variables or component parameters with “Add…”. Moving a slider simulates a copy "
                           "of the circuit; the schematic changes only when you apply."),
                        this);
    m_hint->setWordWrap(true);
    m_hint->setForegroundRole(QPalette::PlaceholderText);
    m_hint->setContentsMargins(6, 4, 6, 4);

    m_rowsHost = new QWidget;
    m_rows = new QVBoxLayout(m_rowsHost);
    m_rows->setContentsMargins(4, 4, 4, 4);
    m_rows->setSpacing(6);
    m_rows->addStretch(1);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("tunables"));
    m_scroll->setWidget(m_rowsHost);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("tuningStatus"));
    m_status->setContentsMargins(6, 0, 6, 0);
    m_status->setWordWrap(true);

    m_applyButton = new QPushButton(tr("Apply"), this);
    m_applyButton->setObjectName(QStringLiteral("applyTuning"));
    m_applyButton->setToolTip(tr("Write the tuned values into the schematic (one undo step)"));
    m_resetButton = new QPushButton(tr("Reset"), this);
    m_resetButton->setObjectName(QStringLiteral("resetTuning"));
    m_resetButton->setToolTip(tr("Discard the tuned values and go back to the values in the schematic"));
    connect(m_applyButton, &QPushButton::clicked, this, &TuningPanel::applyRequested);
    connect(m_resetButton, &QPushButton::clicked, this, &TuningPanel::resetRequested);
    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(6, 0, 6, 6);
    buttons->addStretch(1);
    buttons->addWidget(m_resetButton);
    buttons->addWidget(m_applyButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addLayout(header);
    layout->addWidget(m_hint);
    layout->addWidget(m_scroll, 1);
    layout->addWidget(m_status);
    layout->addLayout(buttons);

    setMinimumWidth(260);
    updateButtons();
    updateStyle();
}

TuningPanel::~TuningPanel() = default;

void TuningPanel::setContext(Project *project, Document *document)
{
    m_project = project;
    m_document = document;
}

// ---------------------------------------------------------------- tunables

int TuningPanel::addVariable(const rf::DesignVariable &variable)
{
    for (const auto &tuner : m_tuners) {
        if (sameDesignVariable(tuner->variable, variable))
            return -1;
    }
    if (!m_project || !m_document)
        return -1;
    const auto current = rf::currentValue(*m_project, *m_document, variable);
    if (!current || !std::isfinite(*current))
        return -1;
    auto tuner = std::make_unique<Tuner>();
    tuner->variable = variable;
    if (!(variable.maximum > variable.minimum))
        setDefaultRange(tuner->variable, *current);
    // The document value is always inside the range.
    tuner->variable.minimum = std::min(tuner->variable.minimum, *current);
    tuner->variable.maximum = std::max(tuner->variable.maximum, *current);
    if (tuner->variable.unit.isEmpty() && variable.kind == rf::DesignVariable::Kind::Parameter) {
        for (const DesignCandidate &candidate : designCandidates(*m_project, *m_document)) {
            if (sameDesignVariable(candidate.variable, variable))
                tuner->variable.unit = candidate.variable.unit;
        }
    }
    tuner->documentValue = *current;
    tuner->value = *current;
    createRow(*tuner);
    m_rows->insertWidget(m_rows->count() - 1, tuner->row);
    m_tuners.push_back(std::move(tuner));
    showValue(*m_tuners.back());
    updateButtons();
    Q_EMIT variablesChanged();
    return count() - 1;
}

void TuningPanel::removeVariable(int index)
{
    if (index < 0 || index >= count())
        return;
    const bool modified = differs(m_tuners[size_t(index)]->value, m_tuners[size_t(index)]->documentValue);
    m_tuners[size_t(index)]->row->deleteLater();
    m_tuners[size_t(index)]->row->hide();
    m_tuners.erase(m_tuners.begin() + index);
    updateButtons();
    Q_EMIT variablesChanged();
    if (modified)
        Q_EMIT valuesChanged();
}

void TuningPanel::clear()
{
    while (count() > 0)
        removeVariable(count() - 1);
}

QList<rf::DesignVariable> TuningPanel::variables() const
{
    QList<rf::DesignVariable> result;
    for (const auto &tuner : m_tuners)
        result.append(tuner->variable);
    return result;
}

std::vector<double> TuningPanel::values() const
{
    std::vector<double> result;
    for (const auto &tuner : m_tuners)
        result.push_back(tuner->value);
    return result;
}

std::vector<double> TuningPanel::documentValues() const
{
    std::vector<double> result;
    for (const auto &tuner : m_tuners)
        result.push_back(tuner->documentValue);
    return result;
}

double TuningPanel::value(int index) const
{
    return index >= 0 && index < count() ? m_tuners[size_t(index)]->value : 0.0;
}

double TuningPanel::fromSlider(const Tuner &tuner, int position) const
{
    const double t = std::clamp(position / double(SliderSteps), 0.0, 1.0);
    const double lo = tuner.variable.minimum, hi = tuner.variable.maximum;
    return tuner.logarithmic() ? lo * std::pow(hi / lo, t) : lo + (hi - lo) * t;
}

int TuningPanel::toSlider(const Tuner &tuner, double value) const
{
    const double lo = tuner.variable.minimum, hi = tuner.variable.maximum;
    if (!(hi > lo))
        return 0;
    double t = tuner.logarithmic() ? std::log(value / lo) / std::log(hi / lo) : (value - lo) / (hi - lo);
    t = std::isfinite(t) ? std::clamp(t, 0.0, 1.0) : 0.0;
    return static_cast<int>(std::lround(t * SliderSteps));
}

double TuningPanel::constrain(const Tuner &tuner, double value) const
{
    const double lo = tuner.variable.minimum, hi = tuner.variable.maximum;
    value = std::clamp(value, lo, hi);
    if (const auto &series = tuner.variable.series) {
        double snapped = snapToSeries(value, *series);
        if (snapped > hi)
            snapped = snapToSeries(value, *series, Rounding::Down);
        if (snapped < lo)
            snapped = snapToSeries(value, *series, Rounding::Up);
        if (snapped >= lo && snapped <= hi)
            value = snapped;
    }
    return value;
}

void TuningPanel::setValue(int index, double value)
{
    if (index < 0 || index >= count() || !std::isfinite(value))
        return;
    Tuner &tuner = *m_tuners[size_t(index)];
    const double constrained = constrain(tuner, value);
    const bool changed = differs(constrained, tuner.value);
    tuner.value = constrained;
    {
        const QSignalBlocker blocker(tuner.slider);
        tuner.slider->setValue(toSlider(tuner, constrained));
    }
    showValue(tuner);
    updateButtons();
    if (changed)
        Q_EMIT valuesChanged();
}

void TuningPanel::setSliderPosition(int index, int position)
{
    if (index < 0 || index >= count())
        return;
    Tuner &tuner = *m_tuners[size_t(index)];
    {
        const QSignalBlocker blocker(tuner.slider);
        tuner.slider->setValue(position);
    }
    // The slider stays where it was dragged; the value snaps.
    const double constrained = constrain(tuner, fromSlider(tuner, position));
    const bool changed = differs(constrained, tuner.value);
    tuner.value = constrained;
    showValue(tuner);
    updateButtons();
    if (changed)
        Q_EMIT valuesChanged();
}

void TuningPanel::setRange(int index, double minimum, double maximum, bool logarithmic)
{
    if (index < 0 || index >= count() || !(maximum > minimum))
        return;
    Tuner &tuner = *m_tuners[size_t(index)];
    tuner.variable.minimum = minimum;
    tuner.variable.maximum = maximum;
    tuner.variable.logarithmic = logarithmic;
    setValue(index, tuner.value);
    showValue(tuner);
}

void TuningPanel::setSeries(int index, std::optional<ESeries> series)
{
    if (index < 0 || index >= count())
        return;
    Tuner &tuner = *m_tuners[size_t(index)];
    tuner.variable.series = series;
    {
        const QSignalBlocker blocker(tuner.series);
        tuner.series->setCurrentIndex(std::max(0, tuner.series->findData(series ? seriesName(*series) : QString())));
    }
    setValue(index, tuner.value);
}

QStringList TuningPanel::refreshDocumentValues()
{
    QStringList removed;
    if (!m_project || !m_document)
        return removed;
    for (int i = count() - 1; i >= 0; --i) {
        Tuner &tuner = *m_tuners[size_t(i)];
        const auto current = rf::currentValue(*m_project, *m_document, tuner.variable);
        if (!current || !std::isfinite(*current)) {
            removed.prepend(tuner.variable.label());
            tuner.row->hide();
            tuner.row->deleteLater();
            m_tuners.erase(m_tuners.begin() + i);
            continue;
        }
        tuner.documentValue = *current;
        showValue(tuner);
    }
    if (!removed.isEmpty())
        Q_EMIT variablesChanged();
    updateButtons();
    return removed;
}

void TuningPanel::resetValues()
{
    bool changed = false;
    for (auto &tuner : m_tuners) {
        // The document value may lie outside a range edited since.
        tuner->variable.minimum = std::min(tuner->variable.minimum, tuner->documentValue);
        tuner->variable.maximum = std::max(tuner->variable.maximum, tuner->documentValue);
        changed = changed || differs(tuner->value, tuner->documentValue);
        tuner->value = tuner->documentValue;
        {
            const QSignalBlocker blocker(tuner->slider);
            tuner->slider->setValue(toSlider(*tuner, tuner->value));
        }
        showValue(*tuner);
    }
    updateButtons();
    if (changed)
        Q_EMIT valuesChanged();
}

bool TuningPanel::isModified() const
{
    return std::any_of(m_tuners.begin(), m_tuners.end(),
                       [](const auto &tuner) { return differs(tuner->value, tuner->documentValue); });
}

void TuningPanel::setStatusText(const QString &text)
{
    m_status->setText(text);
}

QString TuningPanel::statusText() const
{
    return m_status->text();
}

QSlider *TuningPanel::slider(int index) const
{
    return index >= 0 && index < count() ? m_tuners[size_t(index)]->slider : nullptr;
}

QLineEdit *TuningPanel::valueEdit(int index) const
{
    return index >= 0 && index < count() ? m_tuners[size_t(index)]->valueEdit : nullptr;
}

QLineEdit *TuningPanel::minimumEdit(int index) const
{
    return index >= 0 && index < count() ? m_tuners[size_t(index)]->minimumEdit : nullptr;
}

QLineEdit *TuningPanel::maximumEdit(int index) const
{
    return index >= 0 && index < count() ? m_tuners[size_t(index)]->maximumEdit : nullptr;
}

QCheckBox *TuningPanel::logCheck(int index) const
{
    return index >= 0 && index < count() ? m_tuners[size_t(index)]->log : nullptr;
}

QComboBox *TuningPanel::seriesCombo(int index) const
{
    return index >= 0 && index < count() ? m_tuners[size_t(index)]->series : nullptr;
}

// ---------------------------------------------------------------- widgets

int TuningPanel::indexOf(const QWidget *row) const
{
    for (int i = 0; i < count(); ++i) {
        if (m_tuners[size_t(i)]->row == row)
            return i;
    }
    return -1;
}

void TuningPanel::createRow(Tuner &tuner)
{
    auto *row = new QFrame(m_rowsHost);
    row->setObjectName(QStringLiteral("tunable"));
    row->setFrameShape(QFrame::StyledPanel);
    tuner.row = row;

    tuner.name = new QLabel(tuner.variable.label(), row);
    QFont bold = tuner.name->font();
    bold.setBold(true);
    tuner.name->setFont(bold);
    tuner.name->setToolTip(tuner.variable.kind == rf::DesignVariable::Kind::Variable
                               ? tr("Variable %1 of the schematic").arg(tuner.variable.name)
                               : tr("Parameter %1 of %2").arg(tuner.variable.name, tuner.variable.component));
    tuner.documentLabel = new QLabel(row);
    tuner.documentLabel->setForegroundRole(QPalette::PlaceholderText);
    tuner.remove = new QToolButton(row);
    tuner.remove->setText(QStringLiteral("✕"));
    tuner.remove->setAutoRaise(true);
    tuner.remove->setToolTip(tr("Stop tuning this value"));
    connect(tuner.remove, &QToolButton::clicked, this, [this, row] { removeVariable(indexOf(row)); });

    tuner.slider = new QSlider(Qt::Horizontal, row);
    tuner.slider->setObjectName(QStringLiteral("tuneSlider"));
    tuner.slider->setRange(0, SliderSteps);
    tuner.slider->setSingleStep(5);
    tuner.slider->setPageStep(50);
    tuner.slider->setValue(toSlider(tuner, tuner.value));
    connect(tuner.slider, &QSlider::valueChanged, this, [this, row](int position) {
        setSliderPosition(indexOf(row), position);
    });

    tuner.valueEdit = new QLineEdit(row);
    tuner.valueEdit->setObjectName(QStringLiteral("tuneValue"));
    tuner.valueEdit->setToolTip(tr("Value; type a number with an SI prefix, such as 4.7p"));
    tuner.valueEdit->setMaximumWidth(fontMetrics().horizontalAdvance(QStringLiteral("0000.000 MHz")));
    connect(tuner.valueEdit, &QLineEdit::editingFinished, this, [this, row] {
        if (const int i = indexOf(row); i >= 0)
            editValue(*m_tuners[size_t(i)]);
    });

    tuner.minimumEdit = new QLineEdit(row);
    tuner.minimumEdit->setObjectName(QStringLiteral("tuneMinimum"));
    tuner.minimumEdit->setToolTip(tr("Lower end of the slider"));
    tuner.maximumEdit = new QLineEdit(row);
    tuner.maximumEdit->setObjectName(QStringLiteral("tuneMaximum"));
    tuner.maximumEdit->setToolTip(tr("Upper end of the slider"));
    for (QLineEdit *edit : {tuner.minimumEdit, tuner.maximumEdit}) {
        connect(edit, &QLineEdit::editingFinished, this, [this, row] {
            if (const int i = indexOf(row); i >= 0)
                editRange(*m_tuners[size_t(i)]);
        });
    }
    tuner.log = new QCheckBox(tr("Log"), row);
    tuner.log->setToolTip(tr("Logarithmic slider (needs a positive minimum)"));
    tuner.log->setChecked(tuner.logarithmic());
    connect(tuner.log, &QCheckBox::toggled, this, [this, row](bool on) {
        if (const int i = indexOf(row); i >= 0) {
            Tuner &t = *m_tuners[size_t(i)];
            setRange(i, t.variable.minimum, t.variable.maximum, on);
        }
    });
    tuner.series = new QComboBox(row);
    tuner.series->setToolTip(tr("Step through a standard series"));
    tuner.series->addItem(seriesLabel(std::nullopt), QString());
    for (ESeries series : offeredSeries())
        tuner.series->addItem(seriesName(series), seriesName(series));
    if (tuner.variable.series)
        tuner.series->setCurrentIndex(std::max(0, tuner.series->findData(seriesName(*tuner.variable.series))));
    connect(tuner.series, &QComboBox::activated, this, [this, row](int item) {
        if (const int i = indexOf(row); i >= 0)
            setSeries(i, seriesFromName(m_tuners[size_t(i)]->series->itemData(item).toString()));
    });

    auto *grid = new QGridLayout(row);
    grid->setContentsMargins(6, 4, 4, 6);
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(3);
    auto *top = new QHBoxLayout;
    top->setSpacing(10);
    top->addWidget(tuner.name);
    top->addWidget(tuner.documentLabel, 1);
    top->addWidget(tuner.remove);
    grid->addLayout(top, 0, 0, 1, 4);
    grid->addWidget(tuner.slider, 1, 0, 1, 3);
    grid->addWidget(tuner.valueEdit, 1, 3);
    auto *range = new QHBoxLayout;
    range->setSpacing(4);
    range->addWidget(tuner.minimumEdit, 1);
    auto *to = new QLabel(QStringLiteral("…"), row);
    range->addWidget(to);
    range->addWidget(tuner.maximumEdit, 1);
    range->addWidget(tuner.log);
    range->addWidget(tuner.series);
    grid->addLayout(range, 2, 0, 1, 4);
}

void TuningPanel::showValue(Tuner &tuner)
{
    const QString unit = tuner.variable.unit;
    if (!tuner.valueEdit->hasFocus())
        tuner.valueEdit->setText(formatValue(tuner.value, unit));
    tuner.minimumEdit->setText(formatValue(tuner.variable.minimum, unit));
    tuner.maximumEdit->setText(formatValue(tuner.variable.maximum, unit));
    {
        const QSignalBlocker blocker(tuner.log);
        tuner.log->setChecked(tuner.logarithmic());
        tuner.log->setEnabled(tuner.variable.minimum > 0);
    }
    const bool modified = differs(tuner.value, tuner.documentValue);
    tuner.documentLabel->setText(modified ? tr("schematic: %1").arg(formatValue(tuner.documentValue, unit)) : QString());
    QFont font = tuner.valueEdit->font();
    font.setBold(modified);
    tuner.valueEdit->setFont(font);
    tuner.valueEdit->setToolTip(modified ? tr("Tuned value; the schematic has %1").arg(formatValue(tuner.documentValue, unit))
                                         : tr("Value; type a number with an SI prefix, such as 4.7p"));
}

void TuningPanel::editValue(Tuner &tuner)
{
    const int index = indexOf(tuner.row);
    const auto parsed = parseValue(tuner.valueEdit->text());
    if (!parsed || !std::isfinite(*parsed)) {
        tuner.valueEdit->setText(formatValue(tuner.value, tuner.variable.unit));
        return;
    }
    // A typed value outside the range widens it.
    if (*parsed < tuner.variable.minimum)
        tuner.variable.minimum = *parsed;
    if (*parsed > tuner.variable.maximum)
        tuner.variable.maximum = *parsed;
    setValue(index, *parsed);
    tuner.valueEdit->setText(formatValue(tuner.value, tuner.variable.unit));
}

void TuningPanel::editRange(Tuner &tuner)
{
    const auto minimum = parseValue(tuner.minimumEdit->text());
    const auto maximum = parseValue(tuner.maximumEdit->text());
    if (!minimum || !maximum || !(*maximum > *minimum)) {
        showValue(tuner); // back to the valid range
        return;
    }
    if (*minimum == tuner.variable.minimum && *maximum == tuner.variable.maximum)
        return;
    setRange(indexOf(tuner.row), *minimum, *maximum, tuner.variable.logarithmic);
}

void TuningPanel::updateButtons()
{
    const bool modified = isModified();
    m_applyButton->setEnabled(modified);
    m_resetButton->setEnabled(modified);
    m_hint->setVisible(m_tuners.empty());
}

void TuningPanel::fillAddMenu()
{
    fillDesignVariableMenu(m_addMenu, m_project, m_document, variables(),
                           [this](const rf::DesignVariable &variable) { addVariable(variable); });
}

void TuningPanel::updateStyle()
{
    const bool dark = isDark(palette());
    const QColor border = dark ? QColor(0x55, 0x5A, 0x63) : QColor(0xC8, 0xCC, 0xD2);
    const QColor background = palette().color(QPalette::Base);
    m_rowsHost->setStyleSheet(QStringLiteral("QFrame#tunable { border: 1px solid %1; border-radius: 4px; background: %2; }")
                                  .arg(border.name(), background.name()));
}

void TuningPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_rowsHost)
        updateStyle();
}

} // namespace lvd::sim
