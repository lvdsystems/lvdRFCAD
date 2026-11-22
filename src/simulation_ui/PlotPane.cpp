#include "simulation_ui/PlotPane.h"

#include "core/Expression.h"
#include "plot/PlotWidget.h"
#include "rfdata/NetworkOps.h"
#include "rfsim/Measurement.h"

#include <QComboBox>
#include <QCompleter>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QPainter>
#include <QSplitter>
#include <QStringListModel>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lvd::sim {

namespace {

enum Column { NameColumn, ExpressionColumn, StatusColumn, ColumnCount };
constexpr int ExpressionRole = Qt::UserRole + 1;

PlotKind plotKind(const QString &kind)
{
    if (kind == QLatin1String("smith"))
        return PlotKind::Smith;
    if (kind == QLatin1String("polar"))
        return PlotKind::Polar;
    return PlotKind::Rectangular;
}

QString axisLabel(const QString &unit)
{
    if (unit == QLatin1String("dB"))
        return PlotPane::tr("Magnitude");
    if (unit == QLatin1String("deg"))
        return PlotPane::tr("Phase");
    if (unit == QLatin1String("s"))
        return PlotPane::tr("Group delay");
    if (unit == QLatin1String("ohm"))
        return PlotPane::tr("Impedance");
    if (unit == QLatin1String("S"))
        return PlotPane::tr("Admittance");
    return PlotPane::tr("Value");
}

QIcon swatch(const QColor &color)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(1, 1, 10, 10), 2, 2);
    return QIcon(pixmap);
}

} // namespace

PlotPane::PlotPane(const PlotDefinition &definition, QWidget *parent) : QWidget(parent), m_definition(definition)
{
    setObjectName(QStringLiteral("plotPane"));

    m_plot = new PlotWidget(this);
    m_plot->setObjectName(QStringLiteral("plot"));
    m_plot->setXAxis(tr("Frequency"), QStringLiteral("Hz"));
    m_plot->installEventFilter(this);

    // Trace editor, at the right of the plot.
    m_editor = new QWidget(this);
    m_editor->setObjectName(QStringLiteral("traceEditor"));
    m_name = new QLineEdit(m_editor);
    m_name->setObjectName(QStringLiteral("plotName"));
    m_name->setPlaceholderText(tr("Plot name"));
    m_kind = new QComboBox(m_editor);
    m_kind->setObjectName(QStringLiteral("plotKind"));
    m_kind->addItem(tr("Rectangular"), QStringLiteral("rectangular"));
    m_kind->addItem(tr("Smith chart"), QStringLiteral("smith"));
    m_kind->addItem(tr("Polar"), QStringLiteral("polar"));

    m_expression = new QLineEdit(m_editor);
    m_expression->setObjectName(QStringLiteral("traceExpression"));
    m_expression->setPlaceholderText(tr("Add a trace: dB(S21), NF, K, GD(S21), Zin(1)..."));
    m_expression->setClearButtonEnabled(true);
    m_completionModel = new QStringListModel(this);
    m_completer = new QCompleter(m_completionModel, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setMaxVisibleItems(12);
    m_expression->setCompleter(m_completer);

    const auto toolButton = [this](const QString &text, const QString &tip, const QString &name) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setObjectName(name);
        return b;
    };
    m_addButton = toolButton(tr("Add"), tr("Add the trace"), QStringLiteral("addTrace"));
    m_removeButton = toolButton(tr("Remove"), tr("Remove the selected traces"), QStringLiteral("removeTrace"));
    m_stabilityButton = toolButton(tr("Stability circles"),
                                   tr("Source and load stability circles at the first marker, or where K is lowest"),
                                   QStringLiteral("stabilityCircles"));
    m_stabilityButton->setCheckable(true);
    // Design circles at the first marker's frequency (or mid-sweep), with
    // their levels in the button's menu.
    const auto circleButton = [&](const QString &text, const QString &tip, const QString &name, bool gain) {
        QToolButton *button = toolButton(text, tip, name);
        button->setCheckable(true);
        button->setPopupMode(QToolButton::MenuButtonPopup);
        auto *menu = new QMenu(button);
        QAction *levels = menu->addAction(tr("Levels..."));
        levels->setObjectName(gain ? QStringLiteral("gainCircleLevels") : QStringLiteral("noiseCircleLevels"));
        connect(levels, &QAction::triggered, this, [this, gain] { editCircleLevels(gain); });
        button->setMenu(menu);
        return button;
    };
    m_gainButton = circleButton(tr("Gain circles"),
                                tr("Available-gain (source plane) and operating-power-gain (load plane) circles at the "
                                   "first marker, 1, 2 and 3 dB below MAG by default"),
                                QStringLiteral("gainCircles"), true);
    m_noiseButton = circleButton(tr("Noise circles"),
                                 tr("Noise-figure circles (source plane) at the first marker, NFmin + 0.25, 0.5, 1 and "
                                    "2 dB by default"),
                                 QStringLiteral("noiseCircles"), false);
    m_editorButton =toolButton(tr("Traces"), tr("Show or hide the trace editor"), QStringLiteral("toggleEditor"));
    m_editorButton->setCheckable(true);
    m_closeButton = toolButton(QString(), tr("Remove this plot"), QStringLiteral("removePlot"));
    m_closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));

    m_inputError = new QLabel(m_editor);
    m_inputError->setObjectName(QStringLiteral("traceInputError"));
    m_inputError->setWordWrap(true);
    m_inputError->hide();

    m_traces = new QTreeWidget(m_editor);
    m_traces->setObjectName(QStringLiteral("traces"));
    m_traces->setColumnCount(ColumnCount);
    m_traces->setHeaderLabels({tr("Name"), tr("Expression"), tr("Status")});
    m_traces->setRootIsDecorated(false);
    m_traces->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_traces->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_traces->header()->setSectionResizeMode(NameColumn, QHeaderView::ResizeToContents);
    m_traces->header()->setSectionResizeMode(ExpressionColumn, QHeaderView::ResizeToContents);
    m_traces->header()->setSectionResizeMode(StatusColumn, QHeaderView::Stretch);

    auto *form = new QVBoxLayout(m_editor);
    form->setContentsMargins(6, 0, 0, 0);
    form->setSpacing(4);
    auto *top = new QHBoxLayout;
    top->addWidget(m_name, 1);
    top->addWidget(m_kind);
    form->addLayout(top);
    auto *input = new QHBoxLayout;
    input->addWidget(m_expression, 1);
    input->addWidget(m_addButton);
    form->addLayout(input);
    form->addWidget(m_inputError);
    form->addWidget(m_traces, 1);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_removeButton);
    bottom->addStretch();
    form->addLayout(bottom);
    // The circle switches, under the trace list (Smith charts only).
    auto *circles = new QHBoxLayout;
    circles->addStretch();
    circles->addWidget(m_stabilityButton);
    circles->addWidget(m_gainButton);
    circles->addWidget(m_noiseButton);
    form->addLayout(circles);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(m_plot);
    splitter->addWidget(m_editor);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    m_editor->setMinimumWidth(230);
    m_editor->hide();

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(2);
    header->addStretch();
    header->addWidget(m_editorButton);
    header->addWidget(m_closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(splitter, 1);

    connect(m_editorButton, &QToolButton::toggled, this, &PlotPane::setEditorVisible);
    connect(m_closeButton, &QToolButton::clicked, this, [this] { Q_EMIT removeRequested(m_definition.id); });
    connect(m_addButton, &QToolButton::clicked, this, [this] {
        if (addTrace(m_expression->text()))
            m_expression->clear();
    });
    connect(m_expression, &QLineEdit::returnPressed, this, [this] {
        if (m_completer->popup() && m_completer->popup()->isVisible())
            return;
        if (addTrace(m_expression->text()))
            m_expression->clear();
    });
    connect(m_expression, &QLineEdit::textEdited, this, [this] { setInputError({}); });
    connect(m_removeButton, &QToolButton::clicked, this, [this] {
        QStringList expressions;
        for (QTreeWidgetItem *item : m_traces->selectedItems())
            expressions.append(item->data(NameColumn, ExpressionRole).toString());
        if (expressions.isEmpty())
            return;
        PlotDefinition edited = m_definition;
        for (const QString &expression : expressions) {
            edited.traces.removeAll(expression);
            edited.labels.remove(expression);
        }
        Q_EMIT definitionEdited(edited);
    });
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        if (m_name->text() != m_definition.name)
            rename(m_name->text());
    });
    connect(m_kind, &QComboBox::activated, this, [this](int index) { setKind(m_kind->itemData(index).toString()); });
    connect(m_stabilityButton, &QToolButton::toggled, this, &PlotPane::setStabilityCirclesEnabled);
    connect(m_gainButton, &QToolButton::toggled, this, &PlotPane::setGainCirclesEnabled);
    connect(m_noiseButton, &QToolButton::toggled, this, &PlotPane::setNoiseCirclesEnabled);
    connect(m_traces, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (m_updating || column != NameColumn)
            return;
        renameTrace(item->data(NameColumn, ExpressionRole).toString(), item->text(NameColumn).trimmed());
    });
    connect(m_traces, &QTreeWidget::itemSelectionChanged, this,
            [this] { m_removeButton->setEnabled(!m_traces->selectedItems().isEmpty()); });
    connect(m_plot, &PlotWidget::markersChanged, this, &PlotPane::updateStabilityCircles);

    applyKind();
    syncTraces();
}

PlotPane::~PlotPane() = default;

void PlotPane::setDefinition(const PlotDefinition &definition)
{
    if (definition == m_definition)
        return;
    const bool kindChanged = definition.kind != m_definition.kind;
    m_definition = definition;
    if (kindChanged)
        applyKind();
    syncTraces();
}

void PlotPane::setResult(std::shared_ptr<const rf::LinearResult> result)
{
    m_result = std::move(result);
    evaluate();
}

void PlotPane::setCompletions(const QStringList &completions)
{
    if (m_completionModel->stringList() != completions)
        m_completionModel->setStringList(completions);
}

QString PlotPane::inputError() const
{
    return m_inputError->text();
}

int PlotPane::traceId(const QString &expression) const
{
    const auto it = m_state.constFind(expression);
    return it == m_state.cend() ? -1 : it->id;
}

QString PlotPane::traceError(const QString &expression) const
{
    return m_state.value(expression).error;
}

bool PlotPane::addTrace(const QString &text)
{
    const QString expression = text.trimmed();
    if (expression.isEmpty())
        return false;
    if (m_definition.traces.contains(expression)) {
        setInputError(tr("%1 is already plotted").arg(expression));
        return false;
    }
    if (m_result) {
        const rf::Measurement m = rf::evaluateMeasurement(*m_result, expression);
        if (!m.ok()) {
            setInputError(m.error);
            return false;
        }
        if (m_definition.kind != QLatin1String("rectangular") && !m.isComplex) {
            setInputError(tr("%1 is real; this chart needs a complex quantity such as S11 or Zin(1)").arg(expression));
            return false;
        }
    }
    setInputError({});
    PlotDefinition edited = m_definition;
    edited.traces.append(expression);
    Q_EMIT definitionEdited(edited);
    return true;
}

void PlotPane::removeTrace(const QString &expression)
{
    if (!m_definition.traces.contains(expression))
        return;
    PlotDefinition edited = m_definition;
    edited.traces.removeAll(expression);
    edited.labels.remove(expression);
    Q_EMIT definitionEdited(edited);
}

void PlotPane::renameTrace(const QString &expression, const QString &name)
{
    if (!m_definition.traces.contains(expression) || m_definition.label(expression) == name)
        return;
    PlotDefinition edited = m_definition;
    if (name.isEmpty() || name == expression)
        edited.labels.remove(expression);
    else
        edited.labels.insert(expression, name);
    Q_EMIT definitionEdited(edited);
}

void PlotPane::setKind(const QString &kind)
{
    if (kind == m_definition.kind)
        return;
    PlotDefinition edited = m_definition;
    edited.kind = kind;
    Q_EMIT definitionEdited(edited);
}

void PlotPane::rename(const QString &name)
{
    if (name == m_definition.name)
        return;
    PlotDefinition edited = m_definition;
    edited.name = name;
    Q_EMIT definitionEdited(edited);
}

bool PlotPane::isEditorVisible() const
{
    return !m_editor->isHidden();
}

void PlotPane::setEditorVisible(bool visible)
{
    m_editor->setVisible(visible);
    const QSignalBlocker blocker(m_editorButton);
    m_editorButton->setChecked(visible);
}

void PlotPane::setStabilityCirclesEnabled(bool enabled)
{
    m_stabilityCircles = enabled;
    {
        const QSignalBlocker blocker(m_stabilityButton);
        m_stabilityButton->setChecked(enabled);
    }
    updateStabilityCircles();
}

bool PlotPane::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_plot && event->type() == QEvent::MouseButtonDblClick) {
        // Double-clicking a legend entry renames the trace in the editor.
        const auto *mouse = static_cast<QMouseEvent *>(event);
        for (auto it = m_state.begin(); it != m_state.end(); ++it) {
            if (it->id >= 0 && m_plot->legendItemRect(it->id).contains(mouse->position())) {
                setEditorVisible(true);
                for (int i = 0; i < m_traces->topLevelItemCount(); ++i) {
                    QTreeWidgetItem *item = m_traces->topLevelItem(i);
                    if (item->data(NameColumn, ExpressionRole).toString() == it.key()) {
                        m_traces->setCurrentItem(item);
                        m_traces->editItem(item, NameColumn);
                    }
                }
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PlotPane::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        m_inputError->setStyleSheet(QStringLiteral("color: %1;").arg(errorColor().name()));
        refreshList();
    }
}

void PlotPane::applyKind()
{
    const PlotKind kind = plotKind(m_definition.kind);
    m_plot->setKind(kind);
    m_kind->setCurrentIndex(std::max(0, m_kind->findData(m_definition.kind)));
    m_stabilityButton->setVisible(kind == PlotKind::Smith);
    m_gainButton->setVisible(kind == PlotKind::Smith);
    m_noiseButton->setVisible(kind == PlotKind::Smith);
    // Data depend on the kind (impedances become reflections on a Smith chart).
    // In definition order, so traces are created (and coloured) in that order.
    for (const QString &expression : m_definition.traces) {
        const auto it = m_state.find(expression);
        if (it != m_state.end())
            evaluateTrace(expression, *it);
    }
    layoutAxes();
    updateStabilityCircles();
}

void PlotPane::syncTraces()
{
    m_plot->setTitle(m_definition.name);
    if (m_name->text() != m_definition.name && !m_name->hasFocus())
        m_name->setText(m_definition.name);

    // Drop traces no longer in the definition.
    for (auto it = m_state.begin(); it != m_state.end();) {
        if (!m_definition.traces.contains(it.key())) {
            if (it->id >= 0)
                m_plot->removeTrace(it->id);
            it = m_state.erase(it);
        } else {
            ++it;
        }
    }
    for (const QString &expression : m_definition.traces) {
        auto it = m_state.find(expression);
        if (it == m_state.end()) {
            it = m_state.insert(expression, TraceState{});
            evaluateTrace(expression, *it);
        } else if (it->id >= 0) {
            m_plot->setTraceName(it->id, m_definition.label(expression));
        }
    }
    layoutAxes();
    refreshList();
}

void PlotPane::evaluate()
{
    // In definition order, so traces are created (and coloured) in that order.
    for (const QString &expression : m_definition.traces) {
        const auto it = m_state.find(expression);
        if (it != m_state.end())
            evaluateTrace(expression, *it);
    }
    layoutAxes();
    refreshList();
    updateStabilityCircles();
}

void PlotPane::evaluateTrace(const QString &expression, TraceState &state)
{
    state.noise = isNoiseMeasurement(expression);
    if (!m_result) {
        state.error.clear();
        if (state.id >= 0)
            m_plot->setTraceData(state.id, std::span<const double>(), std::span<const double>());
        return;
    }
    const auto clearData = [&](const QString &error) {
        state.error = error;
        if (state.id >= 0)
            m_plot->setTraceData(state.id, std::span<const double>(), std::span<const double>());
    };
    if (!m_result->ok()) {
        clearData(QString());
        return;
    }
    const rf::Measurement m = rf::evaluateMeasurement(*m_result, expression);
    if (!m.ok()) {
        clearData(m.error);
        return;
    }
    const PlotKind kind = plotKind(m_definition.kind);
    if (kind != PlotKind::Rectangular && !m.isComplex) {
        clearData(tr("real values cannot be drawn on this chart; use a complex quantity such as S11"));
        return;
    }

    QString unit;
    ComplexPart part = ComplexPart::MagnitudeDb;
    std::vector<std::complex<double>> values;
    if (kind == PlotKind::Rectangular) {
        if (m.isComplex) {
            part = m.unit.isEmpty() ? ComplexPart::MagnitudeDb : ComplexPart::Magnitude;
            unit = m.unit.isEmpty() ? QStringLiteral("dB") : m.unit;
        } else {
            unit = m.unit;
        }
    } else if (kind == PlotKind::Smith && (m.unit == QLatin1String("ohm") || m.unit == QLatin1String("S"))) {
        // Impedances and admittances are drawn as their reflection coefficient.
        const double z0 = m_plot->smithZ0();
        values.reserve(m.values.size());
        for (const rf::Complex &v : m.values) {
            const rf::Complex z = m.unit == QLatin1String("S") ? (std::abs(v) > 0 ? 1.0 / v : rf::Complex(INFINITY)) : v;
            values.push_back((z - z0) / (z + z0));
        }
    }

    // A trace's unit is fixed when it is created; a change of unit recreates it.
    if (state.id >= 0 && state.unit != unit) {
        m_plot->removeTrace(state.id);
        state.id = -1;
    }
    state.unit = unit;
    state.error.clear();
    if (state.id < 0)
        state.id = m_plot->addTrace(m_definition.label(expression), m.isComplex ? QString() : unit);
    if (m.isComplex) {
        if (values.empty())
            m_plot->setTraceData(state.id, m.frequencies, m.values);
        else
            m_plot->setTraceData(state.id, m.frequencies, values);
        if (kind == PlotKind::Rectangular)
            m_plot->setTraceComplexPart(state.id, part);
    } else {
        const std::vector<double> real = m.real();
        m_plot->setTraceData(state.id, m.frequencies, real);
    }
}

void PlotPane::layoutAxes()
{
    if (plotKind(m_definition.kind) != PlotKind::Rectangular)
        return;
    // The left axis takes the first trace's unit (gains before noise); noise
    // traces next to gains, and traces in another unit, go to the right.
    QString leftUnit;
    bool haveLeft = false;
    bool anyNonNoise = false;
    for (const QString &expression : m_definition.traces) {
        const TraceState state = m_state.value(expression);
        if (state.id >= 0 && !state.noise)
            anyNonNoise = true;
    }
    for (const QString &expression : m_definition.traces) {
        const TraceState state = m_state.value(expression);
        if (state.id < 0 || (anyNonNoise && state.noise))
            continue;
        leftUnit = state.unit;
        haveLeft = true;
        break;
    }
    QString rightUnit;
    bool rightAllNoise = true;
    for (const QString &expression : m_definition.traces) {
        const TraceState state = m_state.value(expression);
        if (state.id < 0)
            continue;
        const bool right = haveLeft && ((anyNonNoise && state.noise) || state.unit != leftUnit);
        TraceStyle style = m_plot->traceStyle(state.id);
        const PlotYAxis side = right ? PlotYAxis::Right : PlotYAxis::Left;
        if (style.yAxis != side) {
            style.yAxis = side;
            m_plot->setTraceStyle(state.id, style);
        }
        if (right) {
            if (rightUnit.isEmpty())
                rightUnit = state.unit;
            rightAllNoise = rightAllNoise && state.noise;
        }
    }
    m_plot->setYAxis(axisLabel(leftUnit), leftUnit, false, PlotYAxis::Left);
    if (!rightUnit.isEmpty())
        m_plot->setYAxis(rightAllNoise ? tr("Noise figure") : axisLabel(rightUnit), rightUnit, false, PlotYAxis::Right);
}

void PlotPane::refreshList()
{
    m_updating = true;
    const QStringList selected = [this] {
        QStringList list;
        for (QTreeWidgetItem *item : m_traces->selectedItems())
            list.append(item->data(NameColumn, ExpressionRole).toString());
        return list;
    }();
    m_traces->clear();
    const QColor bad = errorColor();
    for (const QString &expression : m_definition.traces) {
        const TraceState state = m_state.value(expression);
        auto *item = new QTreeWidgetItem(m_traces);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(NameColumn, ExpressionRole, expression);
        item->setText(NameColumn, m_definition.label(expression));
        item->setText(ExpressionColumn, expression);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        if (state.id >= 0)
            item->setIcon(NameColumn, swatch(m_plot->traceColor(state.id)));
        if (!state.error.isEmpty()) {
            item->setText(StatusColumn, state.error);
            item->setToolTip(StatusColumn, state.error);
            item->setForeground(StatusColumn, bad);
            item->setForeground(ExpressionColumn, bad);
        } else if (!m_result) {
            item->setText(StatusColumn, tr("not run"));
        } else if (state.id >= 0) {
            item->setText(StatusColumn, tr("ok"));
        }
        item->setSelected(selected.contains(expression));
    }
    m_removeButton->setEnabled(!m_traces->selectedItems().isEmpty());
    m_updating = false;
}

void PlotPane::updateStabilityCircles()
{
    for (int id : m_circleIds)
        m_plot->removeCircle(id);
    m_circleIds.clear();
    updateDesignCircles();
    if (!m_stabilityCircles || plotKind(m_definition.kind) != PlotKind::Smith || !m_result || !m_result->ok())
        return;
    const rf::NetworkData &network = m_result->network;
    if (network.portCount() != 2 || network.frequencies.empty())
        return;

    int lowest = -1;
    double lowestK = INFINITY;
    for (int i = 0; i < network.frequencyCount(); ++i) {
        const double k = rf::stabilityK(network.s[static_cast<size_t>(i)]);
        if (std::isfinite(k) && k < lowestK) {
            lowestK = k;
            lowest = i;
        }
    }
    if (lowest < 0 || lowestK >= 1)
        return; // unconditionally stable everywhere: no circles to show

    int index = lowest;
    const std::vector<PlotMarkerInfo> markers = m_plot->markers();
    if (!markers.empty()) {
        const double f = markers.front().x;
        const auto it = std::lower_bound(network.frequencies.begin(), network.frequencies.end(), f);
        index = static_cast<int>(std::clamp<std::ptrdiff_t>(it - network.frequencies.begin(), 0,
                                                            network.frequencyCount() - 1));
        if (index > 0 && std::abs(network.frequencies[static_cast<size_t>(index - 1)] - f)
                             < std::abs(network.frequencies[static_cast<size_t>(index)] - f))
            --index;
    }
    const rf::CMatrix &s = network.s[static_cast<size_t>(index)];
    const QString at = formatEngineering(network.frequencies[static_cast<size_t>(index)], QStringLiteral("Hz"));
    const bool dark = m_plot->theme().dark;

    const auto add = [&](const rf::SmithCircle &stability, const QString &name, const QColor &color) {
        if (!stability.valid)
            return; // the circle degenerates into a line
        PlotCircle circle;
        circle.name = name;
        circle.centre = stability.centre;
        circle.radius = stability.radius;
        circle.color = color;
        m_circleIds.push_back(m_plot->addCircle(circle));
    };
    add(rf::sourceStabilityCircle(s), tr("Source stability, %1").arg(at),
        dark ? QColor(0xE6, 0x67, 0x67) : QColor(0xC6, 0x2E, 0x2E));
    add(rf::loadStabilityCircle(s), tr("Load stability, %1").arg(at),
        dark ? QColor(0xE0, 0xA0, 0x50) : QColor(0xB0, 0x6A, 0x10));
}

int PlotPane::circleFrequencyIndex() const
{
    const rf::NetworkData &network = m_result->network;
    const std::vector<PlotMarkerInfo> markers = m_plot->markers();
    if (markers.empty())
        return network.frequencyCount() / 2;
    const double f = markers.front().x;
    const auto it = std::lower_bound(network.frequencies.begin(), network.frequencies.end(), f);
    int index = static_cast<int>(
        std::clamp<std::ptrdiff_t>(it - network.frequencies.begin(), 0, network.frequencyCount() - 1));
    if (index > 0
        && std::abs(network.frequencies[static_cast<size_t>(index - 1)] - f)
               < std::abs(network.frequencies[static_cast<size_t>(index)] - f))
        --index;
    return index;
}

void PlotPane::updateDesignCircles()
{
    for (int id : m_designCircleIds)
        m_plot->removeCircle(id);
    m_designCircleIds.clear();
    m_designCircleNames.clear();
    if ((!m_gainCircles && !m_noiseCircles) || plotKind(m_definition.kind) != PlotKind::Smith || !m_result
        || !m_result->ok())
        return;
    const rf::NetworkData &network = m_result->network;
    if (network.portCount() != 2 || network.frequencies.empty())
        return;
    const int index = circleFrequencyIndex();
    const rf::CMatrix &s = network.s[static_cast<size_t>(index)];
    const QString at = formatEngineering(network.frequencies[static_cast<size_t>(index)], QStringLiteral("Hz"));
    const bool dark = m_plot->theme().dark;
    const auto add = [&](const rf::SmithCircle &design, const QString &name, const QColor &color, Qt::PenStyle style) {
        if (!design.valid)
            return;
        PlotCircle circle;
        circle.name = name;
        circle.centre = design.centre;
        circle.radius = design.radius;
        circle.color = color;
        circle.lineStyle = style;
        circle.lineWidth = 1.2;
        m_designCircleIds.push_back(m_plot->addCircle(circle));
        m_designCircleNames.append(name);
    };
    const auto number = [](double value) { return QString::number(value, 'f', 2); };

    if (m_gainCircles) {
        // Levels below MAG (K > 1) or MSG (K < 1), where the circles may
        // cross into the unstable region (see the stability circles).
        const double reference = rf::maximumAvailableGainDb(s);
        const QString referenceName = rf::stabilityK(s) > 1 ? QStringLiteral("MAG") : QStringLiteral("MSG");
        if (std::isfinite(reference)) {
            for (double below : std::as_const(m_gainLevels)) {
                const double gain = reference - below;
                add(rf::availableGainCircle(s, gain),
                    tr("GA = %1 dB (%2 - %3), source plane, %4").arg(number(gain), referenceName, number(below), at),
                    dark ? QColor(0x6C, 0xC6, 0x7A) : QColor(0x1E, 0x8A, 0x3A), Qt::SolidLine);
                add(rf::operatingPowerGainCircle(s, gain),
                    tr("GP = %1 dB (%2 - %3), load plane, %4").arg(number(gain), referenceName, number(below), at),
                    dark ? QColor(0x6A, 0xA8, 0xE8) : QColor(0x1F, 0x5F, 0xB0), Qt::DashDotLine);
            }
        }
    }
    if (m_noiseCircles && network.hasNoiseCorrelation()) {
        const rf::NoiseParameters noise = rf::noiseParametersFromCorrelation(
            s, network.noiseCorrelation[static_cast<size_t>(index)], network.z0[0]);
        if (std::isfinite(noise.fminDb)) {
            for (double above : std::as_const(m_noiseLevels)) {
                const double nf = noise.fminDb + above;
                add(rf::noiseFigureCircle(noise, nf, network.z0[0]),
                    tr("NF = %1 dB (NFmin + %2), source plane, %3").arg(number(nf), number(above), at),
                    dark ? QColor(0xC0, 0x8C, 0xE8) : QColor(0x7A, 0x3A, 0xB0), Qt::DotLine);
            }
        }
    }
}

void PlotPane::setGainCirclesEnabled(bool enabled)
{
    m_gainCircles = enabled;
    {
        const QSignalBlocker blocker(m_gainButton);
        m_gainButton->setChecked(enabled);
    }
    updateDesignCircles();
}

void PlotPane::setNoiseCirclesEnabled(bool enabled)
{
    m_noiseCircles = enabled;
    {
        const QSignalBlocker blocker(m_noiseButton);
        m_noiseButton->setChecked(enabled);
    }
    updateDesignCircles();
}

void PlotPane::setGainCircleLevels(const QList<double> &dbBelowMaximum)
{
    m_gainLevels = dbBelowMaximum;
    updateDesignCircles();
}

void PlotPane::setNoiseCircleLevels(const QList<double> &dbAboveMinimum)
{
    m_noiseLevels = dbAboveMinimum;
    updateDesignCircles();
}

std::optional<QList<double>> PlotPane::parseLevels(const QString &text)
{
    QList<double> levels;
    static const QRegularExpression separator(QStringLiteral("[,;\\s]+"));
    for (const QString &item : text.split(separator, Qt::SkipEmptyParts)) {
        bool ok = false;
        const double value = QLocale::c().toDouble(item, &ok);
        if (!ok || !std::isfinite(value) || value < 0)
            return std::nullopt;
        levels.append(value);
    }
    if (levels.isEmpty() || levels.size() > 12)
        return std::nullopt;
    return levels;
}

void PlotPane::editCircleLevels(bool gain)
{
    const QList<double> &current = gain ? m_gainLevels : m_noiseLevels;
    QStringList text;
    for (double value : current)
        text.append(QLocale::c().toString(value, 'g', 6));
    const QString prompt = gain ? tr("Gain circle levels, dB below MAG (or MSG), separated by commas:")
                                : tr("Noise circle levels, dB above NFmin, separated by commas:");
    bool accepted = false;
    const QString input = QInputDialog::getText(this, gain ? tr("Gain circles") : tr("Noise circles"), prompt,
                                                QLineEdit::Normal, text.join(QStringLiteral(", ")), &accepted);
    if (!accepted)
        return;
    const auto levels = parseLevels(input);
    if (!levels) {
        setInputError(tr("Circle levels must be 1 to 12 non-negative numbers, such as 1, 2, 3"));
        setEditorVisible(true);
        return;
    }
    setInputError({});
    if (gain) {
        setGainCircleLevels(*levels);
        setGainCirclesEnabled(true);
    } else {
        setNoiseCircleLevels(*levels);
        setNoiseCirclesEnabled(true);
    }
}

void PlotPane::setInputError(const QString &error)
{
    m_inputError->setText(error);
    m_inputError->setStyleSheet(QStringLiteral("color: %1;").arg(errorColor().name()));
    m_inputError->setVisible(!error.isEmpty());
}

QColor PlotPane::errorColor() const
{
    return palette().color(QPalette::Window).lightness() < 128 ? QColor(0xF0, 0x80, 0x80) : QColor(0xB0, 0x20, 0x20);
}

} // namespace lvd::sim
