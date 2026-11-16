#include "spice_ui/WaveformViewer.h"

#include "spice/VectorExpression.h"
#include "spice_ui/MeasurementPanel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCompleter>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringListModel>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lvd::spiceui {

namespace {

// Trace-expression functions offered by the completer.
const char *const kFunctions[] = {"db",   "db10", "mag",   "ph",    "cph",   "real",  "imag", "conj", "sqrt",
                                  "exp",  "ln",   "log10", "sin",   "cos",   "tan",   "atan", "sinh", "cosh",
                                  "tanh", "deriv", "integ", "gd",   "mean",  "rms",   "max",  "min",  "pkpk"};

bool isWaveformPlotType(const QString &type)
{
    return type != QLatin1String("op") && type != QLatin1String("tf") && type != QLatin1String("pz")
           && type != QLatin1String("sens");
}

QString analysisName(const QString &type)
{
    if (type == QLatin1String("op"))
        return WaveformViewer::tr("operating point");
    if (type == QLatin1String("tf"))
        return WaveformViewer::tr("transfer function");
    if (type == QLatin1String("pz"))
        return WaveformViewer::tr("pole-zero");
    if (type == QLatin1String("sens"))
        return WaveformViewer::tr("sensitivity");
    return type;
}

// Line edit completing the vector or function name under the cursor, not
// the whole text: "db(v(o" offers "v(out)".
class ExpressionEdit : public QLineEdit
{
public:
    explicit ExpressionEdit(QWidget *parent) : QLineEdit(parent) {}

    void setExpressionCompleter(QCompleter *completer)
    {
        m_completer = completer;
        completer->setWidget(this);
        completer->setCompletionMode(QCompleter::PopupCompletion);
        QObject::connect(completer, qOverload<const QString &>(&QCompleter::activated), this,
                         [this](const QString &text) { insertCompletion(text); });
    }

    bool popupVisible() const { return m_completer && m_completer->popup()->isVisible(); }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (popupVisible()) {
            switch (event->key()) {
            case Qt::Key_Enter:
            case Qt::Key_Return:
            case Qt::Key_Escape:
            case Qt::Key_Tab:
            case Qt::Key_Backtab:
                event->ignore();
                return;
            default:
                break;
            }
        }
        QLineEdit::keyPressEvent(event);
        if (!m_completer)
            return;
        const QString token = currentToken();
        if (token.isEmpty() || event->text().isEmpty()) {
            if (token.isEmpty())
                m_completer->popup()->hide();
            return;
        }
        m_completer->setCompletionPrefix(token);
        if (m_completer->completionCount() == 0
            || (m_completer->completionCount() == 1 && m_completer->currentCompletion() == token)) {
            m_completer->popup()->hide();
            return;
        }
        m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));
        QRect r = cursorRect();
        r.setWidth(std::max(220, m_completer->popup()->sizeHintForColumn(0) + 24));
        m_completer->complete(r);
    }

private:
    QString currentToken() const
    {
        static const QRegularExpression token(QStringLiteral("([vViI]\\([^()]*|[A-Za-z_][A-Za-z0-9_#.\\-]*)$"));
        const QString before = text().left(cursorPosition());
        const QRegularExpressionMatch match = token.match(before);
        return match.hasMatch() ? match.captured(1) : QString();
    }

    void insertCompletion(const QString &completion)
    {
        const QString token = currentToken();
        const int position = cursorPosition();
        QString t = text();
        t.replace(position - token.size(), token.size(), completion);
        setText(t);
        setCursorPosition(position - int(token.size()) + int(completion.size()));
    }

    QCompleter *m_completer = nullptr;
};

} // namespace

WaveformViewer::WaveformViewer(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("waveformViewer"));

    // ---- Actions and tool bar
    m_toolBar = new QToolBar(this);
    m_toolBar->setObjectName(QStringLiteral("waveformToolBar"));
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_toolBar->setIconSize(QSize(16, 16));

    m_addPaneAction = new QAction(tr("Add Pane"), this);
    m_addPaneAction->setToolTip(tr("Add an empty pane below the others"));
    connect(m_addPaneAction, &QAction::triggered, this, [this] { addPane(); });
    m_removePaneAction = new QAction(tr("Remove Pane"), this);
    m_removePaneAction->setToolTip(tr("Remove the current pane"));
    connect(m_removePaneAction, &QAction::triggered, this, [this] { removePane(m_currentId); });
    for (int which = 0; which < 2; ++which) {
        auto *a = new QAction(which == 0 ? tr("Cursor A") : tr("Cursor B"), this);
        a->setCheckable(true);
        a->setToolTip(tr("Show cursor %1 on the selected trace of the current pane (drag it on the plot)")
                          .arg(which == 0 ? QStringLiteral("A") : QStringLiteral("B")));
        connect(a, &QAction::triggered, this, [this, which](bool on) { setCursorShown(which, on); });
        m_cursorActions[which] = a;
    }
    m_fftAction = new QAction(tr("FFT"), this);
    m_fftAction->setToolTip(tr("New pane with the spectrum of the selected transient trace"));
    connect(m_fftAction, &QAction::triggered, this, [this] {
        QString error;
        if (fftOfSelectedTrace(&error).isEmpty())
            Q_EMIT statusMessage(error);
    });
    m_autoscaleAction = new QAction(tr("Autoscale"), this);
    m_autoscaleAction->setToolTip(tr("Show all data in every pane (double-click a plot)"));
    connect(m_autoscaleAction, &QAction::triggered, this, [this] {
        for (WaveformPane *p : m_panes)
            p->autoscale();
    });
    m_logXAction = new QAction(tr("Log X"), this);
    m_logXAction->setCheckable(true);
    m_logXAction->setToolTip(tr("Logarithmic x axis in the current pane"));
    connect(m_logXAction, &QAction::triggered, this, [this](bool on) {
        if (WaveformPane *p = currentPane())
            p->setLogX(on);
    });
    const auto forward = [this](const QString &text, int index) {
        auto *a = new QAction(text, this);
        connect(a, &QAction::triggered, this, [this, index] {
            if (WaveformPane *p = currentPane()) {
                const QList<QAction *> actions = p->plot()->plotActions();
                if (index < actions.size())
                    actions[index]->trigger();
            }
        });
        return a;
    };
    m_exportPngAction = forward(tr("Export PNG..."), 3);
    m_exportSvgAction = forward(tr("Export SVG..."), 4);
    m_exportSvgAction->setEnabled(PlotWidget::svgSupported());
    m_exportCsvAction = forward(tr("Export CSV..."), 5);
    m_copyAction = forward(tr("Copy Image"), 6);

    m_toolBar->addAction(m_addPaneAction);
    m_toolBar->addAction(m_removePaneAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_cursorActions[0]);
    m_toolBar->addAction(m_cursorActions[1]);
    m_toolBar->addAction(m_fftAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_autoscaleAction);
    m_toolBar->addAction(m_logXAction);
    m_toolBar->addSeparator();
    auto *exportButton = new QToolButton(m_toolBar);
    exportButton->setText(tr("Export"));
    exportButton->setPopupMode(QToolButton::InstantPopup);
    auto *exportMenu = new QMenu(exportButton);
    exportMenu->addAction(m_copyAction);
    exportMenu->addSeparator();
    exportMenu->addAction(m_exportPngAction);
    exportMenu->addAction(m_exportSvgAction);
    exportMenu->addAction(m_exportCsvAction);
    exportButton->setMenu(exportMenu);
    m_toolBar->addWidget(exportButton);

    // ---- Side panel: trace entry, vectors, measurements
    auto *entry = new QWidget(this);
    m_expression = new ExpressionEdit(entry);
    m_expression->setObjectName(QStringLiteral("traceExpression"));
    m_expression->setPlaceholderText(tr("Expression: v(out), db(v(out)/v(in))..."));
    m_expression->setClearButtonEnabled(true);
    m_completionModel = new QStringListModel(this);
    m_completer = new QCompleter(m_completionModel, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setMaxVisibleItems(12);
    static_cast<ExpressionEdit *>(m_expression)->setExpressionCompleter(m_completer);
    m_addButton = new QPushButton(tr("Add"), entry);
    m_addButton->setObjectName(QStringLiteral("addTrace"));
    m_addButton->setToolTip(tr("Add the trace to the current pane"));
    m_inputError = new QLabel(entry);
    m_inputError->setObjectName(QStringLiteral("traceInputError"));
    m_inputError->setWordWrap(true);
    m_inputError->hide();
    m_filter = new QLineEdit(entry);
    m_filter->setObjectName(QStringLiteral("vectorFilter"));
    m_filter->setPlaceholderText(tr("Filter vectors"));
    m_filter->setClearButtonEnabled(true);
    m_vectors = new QListWidget(entry);
    m_vectors->setObjectName(QStringLiteral("vectors"));
    m_vectors->setToolTip(tr("Double-click a vector to add it to the current pane"));
    m_vectors->setUniformItemSizes(true);

    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(4);
    inputRow->addWidget(m_expression, 1);
    inputRow->addWidget(m_addButton);
    auto *entryLayout = new QVBoxLayout(entry);
    entryLayout->setContentsMargins(0, 0, 0, 0);
    entryLayout->setSpacing(4);
    entryLayout->addLayout(inputRow);
    entryLayout->addWidget(m_inputError);
    entryLayout->addWidget(m_filter);
    entryLayout->addWidget(m_vectors, 1);

    m_measurements = new MeasurementPanel(this);

    auto *side = new QSplitter(Qt::Vertical, this);
    side->setObjectName(QStringLiteral("waveformSide"));
    side->setChildrenCollapsible(false);
    side->addWidget(entry);
    side->addWidget(m_measurements);
    side->setStretchFactor(0, 2);
    side->setStretchFactor(1, 3);

    m_paneSplitter = new QSplitter(Qt::Vertical, this);
    m_paneSplitter->setObjectName(QStringLiteral("panes"));
    m_paneSplitter->setChildrenCollapsible(false);
    m_paneSplitter->setHandleWidth(5);

    m_mainSplitter = new QSplitter(Qt::Horizontal, this);
    m_mainSplitter->setChildrenCollapsible(false);
    m_mainSplitter->addWidget(m_paneSplitter);
    m_mainSplitter->addWidget(side);
    m_mainSplitter->setStretchFactor(0, 1);
    m_mainSplitter->setStretchFactor(1, 0);
    side->setMinimumWidth(220);
    m_mainSplitter->setSizes({800, 260});

    auto *content = new QWidget(this);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(2);
    contentLayout->addWidget(m_toolBar);
    contentLayout->addWidget(m_mainSplitter, 1);

    m_placeholder = new QLabel(this);
    m_placeholder->setObjectName(QStringLiteral("waveformPlaceholder"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setMargin(24);
    QPalette muted = m_placeholder->palette();
    muted.setColor(QPalette::WindowText, muted.color(QPalette::PlaceholderText));
    m_placeholder->setPalette(muted);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(content);
    m_stack->addWidget(m_placeholder);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);

    connect(m_addButton, &QPushButton::clicked, this, &WaveformViewer::submitExpression);
    connect(m_expression, &QLineEdit::returnPressed, this, [this] {
        if (!static_cast<ExpressionEdit *>(m_expression)->popupVisible())
            submitExpression();
    });
    connect(m_expression, &QLineEdit::textEdited, this, [this] { setInputError({}); });
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        for (int i = 0; i < m_vectors->count(); ++i) {
            QListWidgetItem *item = m_vectors->item(i);
            item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive));
        }
    });
    connect(m_vectors, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        QString error;
        if (!addTrace(item->text(), QString(), &error))
            Q_EMIT statusMessage(error);
    });

    updatePlaceholder();
    updateActions();
}

WaveformViewer::~WaveformViewer() = default;

// ---------------------------------------------------------------- data

const spice::SpicePlot *WaveformViewer::shownPlot() const
{
    if (!m_result || m_plotIndex < 0 || m_plotIndex >= m_result->plots.size())
        return nullptr;
    return &m_result->plots[m_plotIndex];
}

bool WaveformViewer::hasWaveforms() const
{
    const spice::SpicePlot *plot = shownPlot();
    if (!plot || !isWaveformPlotType(plot->type))
        return false;
    const spice::SpiceVector *scale = plot->scale();
    return scale && scale->size() > 1;
}

QString WaveformViewer::placeholderText() const
{
    const spice::SpicePlot *plot = shownPlot();
    if (!plot)
        return tr("No simulation results. Run a transient, AC, DC sweep or noise analysis to see its waveforms "
                  "here.");
    if (!isWaveformPlotType(plot->type) || !plot->scale())
        return tr("The %1 analysis gives single values, not waveforms: see its results table.")
            .arg(analysisName(plot->type));
    return tr("The %1 result has a single point: there is no waveform to plot.").arg(plot->type);
}

void WaveformViewer::updatePlaceholder()
{
    const bool show = hasWaveforms();
    m_placeholder->setText(show ? QString() : placeholderText());
    m_stack->setCurrentIndex(show ? 0 : 1);
}

void WaveformViewer::setData(std::shared_ptr<const spice::SpiceRunResult> result, int plotIndex)
{
    const spice::SpicePlot *previous = hasWaveforms() ? shownPlot() : nullptr;
    const QString previousType = previous ? previous->type : QString();
    // Keep the previous result alive until every pane has moved on.
    const auto old = std::move(m_result);
    m_result = std::move(result);
    m_plotIndex = plotIndex;
    const bool waveforms = hasWaveforms();
    const spice::SpicePlot *plot = waveforms ? shownPlot() : nullptr;
    const bool keep = plot && plot->type == previousType;
    if (plot && m_panes.isEmpty()) {
        WaveformPane *p = createPane(defaultPane(plot, nextPaneId()));
        m_currentId = p->id();
        p->setCurrent(true);
    }
    for (WaveformPane *p : m_panes)
        p->setPlot(plot, keep);
    updatePlaceholder();
    updateVectorList();
    // Panes start in step: the current one leads.
    if (WaveformPane *lead = currentPane()) {
        const auto [lo, hi] = lead->xRange();
        syncXRange(lead, lo, hi);
    }
    updateActions();
    updateMeasurements();
}

void WaveformViewer::clearData()
{
    setData(nullptr, -1);
}

void WaveformViewer::setNameResolver(WaveformNameResolver resolver)
{
    m_nameResolver = std::move(resolver);
    for (WaveformPane *p : m_panes)
        p->setNameResolver(m_nameResolver);
    updateMeasurements();
}

QStringList WaveformViewer::availableVectors() const
{
    QStringList names;
    const spice::SpicePlot *plot = shownPlot();
    if (!plot)
        return names;
    if (const spice::SpiceVector *scale = plot->scale())
        names << scale->name;
    for (const spice::SpiceVector &v : plot->vectors)
        if (!names.contains(v.name))
            names << v.name;
    return names;
}

void WaveformViewer::updateVectorList()
{
    m_vectors->clear();
    const spice::SpicePlot *plot = hasWaveforms() ? shownPlot() : nullptr;
    if (plot) {
        for (const QString &name : availableVectors()) {
            const spice::SpiceVector *v = plot->vector(name);
            auto *item = new QListWidgetItem(name, m_vectors);
            QString unit = v ? spice::vectorKindUnit(v->kind) : QString();
            QString tip = unit.isEmpty() ? name : tr("%1 (%2)").arg(name, unit);
            if (v && v->isComplex)
                tip += tr(", complex");
            if (v)
                tip += tr(", %n points", nullptr, int(v->size()));
            if (plot->scale() && plot->scale()->name == name) {
                tip += tr(", scale");
                QFont f = item->font();
                f.setItalic(true);
                item->setFont(f);
            }
            item->setToolTip(tip);
        }
    }
    const QString filter = m_filter->text();
    if (!filter.isEmpty())
        for (int i = 0; i < m_vectors->count(); ++i)
            m_vectors->item(i)->setHidden(!m_vectors->item(i)->text().contains(filter, Qt::CaseInsensitive));
    updateCompletions();
}

void WaveformViewer::updateCompletions()
{
    QStringList words = availableVectors();
    for (const char *f : kFunctions)
        words << QLatin1String(f) + QLatin1Char('(');
    m_completionModel->setStringList(words);
}

// ---------------------------------------------------------------- panes

WaveformPaneDefinition WaveformViewer::defaultPane(const spice::SpicePlot *plot, const QString &id)
{
    WaveformPaneDefinition d;
    d.id = id;
    d.logX = plot && (plot->type == QLatin1String("ac") || plot->type == QLatin1String("noise"));
    return d;
}

QJsonArray WaveformViewer::panesToJson(const QList<WaveformPaneDefinition> &panes)
{
    QJsonArray array;
    for (const WaveformPaneDefinition &p : panes)
        array.append(p.toJson());
    return array;
}

QList<WaveformPaneDefinition> WaveformViewer::panesFromJson(const QJsonArray &array)
{
    QList<WaveformPaneDefinition> panes;
    for (const QJsonValue &v : array)
        if (v.isObject())
            panes << WaveformPaneDefinition::fromJson(v.toObject());
    return panes;
}

QString WaveformViewer::nextPaneId() const
{
    int highest = 0;
    for (WaveformPane *p : m_panes) {
        const QString id = p->id();
        if (id.startsWith(QLatin1Char('p'))) {
            bool ok = false;
            const int n = id.mid(1).toInt(&ok);
            if (ok)
                highest = std::max(highest, n);
        }
    }
    return QStringLiteral("p%1").arg(highest + 1);
}

WaveformPane *WaveformViewer::createPane(const WaveformPaneDefinition &definition)
{
    WaveformPaneDefinition d = definition;
    if (d.id.isEmpty() || pane(d.id))
        d.id = nextPaneId();
    auto *p = new WaveformPane(d, m_paneSplitter);
    if (m_nameResolver)
        p->setNameResolver(m_nameResolver);
    m_panes.append(p);
    m_paneSplitter->addWidget(p);
    m_paneSplitter->setStretchFactor(m_paneSplitter->indexOf(p), 1);
    // Equal heights.
    QList<int> sizes;
    for (int i = 0; i < m_paneSplitter->count(); ++i)
        sizes << 1000;
    m_paneSplitter->setSizes(sizes);
    if (hasWaveforms())
        p->setPlot(shownPlot(), false);

    connect(p, &WaveformPane::activated, this, [this, p] { setCurrentPane(p->id()); });
    connect(p, &WaveformPane::definitionEdited, this, [this, p] {
        if (p == currentPane()) {
            updateActions();
            updateMeasurements();
        }
        emitEdited();
    });
    connect(p, &WaveformPane::xRangeChanged, this, [this, p](double lo, double hi) { syncXRange(p, lo, hi); });
    connect(p, &WaveformPane::selectionChanged, this, [this, p] {
        if (p == currentPane()) {
            updateActions();
            updateMeasurements();
        }
    });
    connect(p, &WaveformPane::cursorsChanged, this, [this, p] {
        if (p == currentPane()) {
            updateActions();
            updateMeasurements();
        }
    });
    connect(p, &WaveformPane::fftRequested, this, [this](const QString &expression) {
        QString error;
        if (addFftPane(expression, &error).isEmpty())
            Q_EMIT statusMessage(error);
    });
    connect(p, &WaveformPane::addPaneRequested, this, [this] { addPane(); });
    connect(p, &WaveformPane::removeRequested, this, [this, p] { removePane(p->id()); });
    connect(p, &WaveformPane::statusMessage, this, &WaveformViewer::statusMessage);
    return p;
}

void WaveformViewer::deletePanes()
{
    for (WaveformPane *p : m_panes) {
        p->disconnect(this);
        p->hide();
        p->deleteLater();
        p->setParent(nullptr);
    }
    m_panes.clear();
    m_currentId.clear();
}

void WaveformViewer::setPanes(const QList<WaveformPaneDefinition> &panes)
{
    const QString previousCurrent = m_currentId;
    deletePanes();
    for (const WaveformPaneDefinition &d : panes)
        createPane(d);
    if (m_panes.isEmpty() && hasWaveforms())
        createPane(defaultPane(shownPlot(), QStringLiteral("p1")));
    const QString current = pane(previousCurrent) ? previousCurrent
                            : m_panes.isEmpty()   ? QString()
                                                  : m_panes.front()->id();
    m_currentId.clear();
    setCurrentPane(current);
    if (WaveformPane *lead = currentPane()) {
        const auto [lo, hi] = lead->xRange();
        syncXRange(lead, lo, hi);
    }
    updateActions();
    updateMeasurements();
}

QList<WaveformPaneDefinition> WaveformViewer::panes() const
{
    QList<WaveformPaneDefinition> list;
    for (int i = 0; i < m_paneSplitter->count(); ++i)
        if (auto *p = qobject_cast<WaveformPane *>(m_paneSplitter->widget(i)))
            if (m_panes.contains(p))
                list << p->definition();
    return list;
}

void WaveformViewer::emitEdited()
{
    Q_EMIT panesEdited(panes());
}

QString WaveformViewer::addPane(const QString &name)
{
    WaveformPaneDefinition d = defaultPane(hasWaveforms() ? shownPlot() : nullptr, nextPaneId());
    d.name = name;
    WaveformPane *p = createPane(d);
    // Same x range as the others.
    for (WaveformPane *other : m_panes) {
        if (other != p && other->isFft() == p->isFft() && hasWaveforms()) {
            const auto [lo, hi] = other->xRange();
            if (!(p->plot()->xScale().isLog() && lo <= 0))
                p->setXRange(lo, hi);
            break;
        }
    }
    setCurrentPane(p->id());
    emitEdited();
    return p->id();
}

bool WaveformViewer::removePane(const QString &paneId)
{
    WaveformPane *p = pane(paneId);
    if (!p)
        return false;
    const int index = int(m_panes.indexOf(p));
    m_panes.removeAll(p);
    p->disconnect(this);
    p->hide();
    p->setParent(nullptr);
    p->deleteLater();
    if (m_currentId == paneId) {
        m_currentId.clear();
        if (!m_panes.isEmpty())
            setCurrentPane(m_panes[std::clamp(index, 0, int(m_panes.size()) - 1)]->id());
        else
            Q_EMIT currentPaneChanged(QString());
    }
    updateActions();
    updateMeasurements();
    emitEdited();
    return true;
}

WaveformPane *WaveformViewer::pane(const QString &paneId) const
{
    if (paneId.isEmpty())
        return nullptr;
    for (WaveformPane *p : m_panes)
        if (p->id() == paneId)
            return p;
    return nullptr;
}

WaveformPane *WaveformViewer::currentPane() const
{
    return pane(m_currentId);
}

QList<WaveformPane *> WaveformViewer::paneWidgets() const
{
    return m_panes;
}

void WaveformViewer::setCurrentPane(const QString &paneId)
{
    if (!pane(paneId) || paneId == m_currentId)
        return;
    m_currentId = paneId;
    for (WaveformPane *p : m_panes)
        p->setCurrent(p->id() == paneId);
    updateActions();
    updateMeasurements();
    Q_EMIT currentPaneChanged(paneId);
}

// ---------------------------------------------------------------- traces

bool WaveformViewer::addTrace(const QString &text, const QString &paneId, QString *error)
{
    const QString expression = text.trimmed();
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (expression.isEmpty())
        return fail(tr("Type an expression, such as v(out)."));
    QString parseError;
    const auto parsed = spice::VectorExpression::parse(expression, &parseError);
    if (!parsed)
        return fail(tr("%1: %2").arg(expression, parseError));
    WaveformPane *target = paneId.isEmpty() ? currentPane() : pane(paneId);
    if (!paneId.isEmpty() && !target)
        return fail(tr("There is no pane %1.").arg(paneId));
    if (target && target->isFft())
        return fail(tr("%1 shows a spectrum; select another pane to add traces.").arg(target->displayName()));
    if (target && target->definition().traces.contains(expression))
        return fail(tr("%1 is already in %2.").arg(expression, target->displayName()));
    if (hasWaveforms()) {
        const spice::SpicePlot *plot = shownPlot();
        const auto resolver = [this, plot](const QString &name) -> std::optional<spice::Waveform> {
            if (const spice::SpiceVector *v = plot->vector(name))
                return plotVectorWaveform(*plot, *v);
            if (m_nameResolver)
                return m_nameResolver(*plot, name);
            return std::nullopt;
        };
        // Check the names first: evaluating a long trace only to find a typo is wasteful.
        for (const QString &name : parsed->referencedVectors()) {
            if (!plot->vector(name) && !(m_nameResolver && m_nameResolver(*plot, name)))
                return fail(tr("%1: unknown vector '%2'").arg(expression, name));
        }
        QString evalError;
        if (!parsed->evaluate(resolver, &evalError))
            return fail(tr("%1: %2").arg(expression, evalError));
    }
    if (!target) {
        target = createPane(defaultPane(hasWaveforms() ? shownPlot() : nullptr, nextPaneId()));
        setCurrentPane(target->id());
    }
    WaveformPaneDefinition d = target->definition();
    d.traces << expression;
    target->setDefinition(d);
    target->setSelectedTrace(target->traceCount() - 1);
    updateActions();
    updateMeasurements();
    emitEdited();
    Q_EMIT statusMessage(tr("Added %1 to %2").arg(expression, target->displayName()));
    return true;
}

bool WaveformViewer::removeTrace(const QString &paneId, int index)
{
    WaveformPane *p = pane(paneId.isEmpty() ? m_currentId : paneId);
    if (!p || p->isFft() || index < 0 || index >= p->definition().traces.size())
        return false;
    p->removeTraceAt(index); // emits definitionEdited -> panesEdited
    return true;
}

QString WaveformViewer::addFftPane(const QString &text, QString *error)
{
    const QString expression = text.trimmed();
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    const spice::SpicePlot *plot = hasWaveforms() ? shownPlot() : nullptr;
    if (!plot || plot->type != QLatin1String("tran"))
        return fail(tr("The FFT needs a transient result."));
    QString parseError;
    const auto parsed = spice::VectorExpression::parse(expression, &parseError);
    if (!parsed)
        return fail(tr("%1: %2").arg(expression, parseError));
    const auto resolver = [this, plot](const QString &name) -> std::optional<spice::Waveform> {
        if (const spice::SpiceVector *v = plot->vector(name))
            return plotVectorWaveform(*plot, *v);
        if (m_nameResolver)
            return m_nameResolver(*plot, name);
        return std::nullopt;
    };
    QString evalError;
    const auto w = parsed->evaluate(resolver, &evalError);
    if (!w)
        return fail(tr("%1: %2").arg(expression, evalError));
    if (w->isComplex)
        return fail(tr("%1: the FFT needs a real trace.").arg(expression));
    WaveformPaneDefinition d;
    d.id = nextPaneId();
    d.fftSource = expression;
    d.fftWindow = fftWindowKey(spice::WindowType::Hann);
    WaveformPane *p = createPane(d);
    // Other spectra keep their own frequency range; a new one starts autoscaled.
    setCurrentPane(p->id());
    emitEdited();
    Q_EMIT statusMessage(tr("Spectrum of %1").arg(expression));
    return p->id();
}

QString WaveformViewer::fftOfSelectedTrace(QString *error)
{
    WaveformPane *p = currentPane();
    if (!p || p->isFft() || p->traceCount() == 0) {
        if (error)
            *error = tr("Select a transient trace first.");
        return {};
    }
    return addFftPane(p->traceExpression(p->selectedTrace()), error);
}

void WaveformViewer::setCursorShown(int which, bool shown)
{
    if (WaveformPane *p = currentPane())
        p->showCursor(which, shown);
    updateActions();
}

void WaveformViewer::submitExpression()
{
    QString error;
    if (addTrace(m_expression->text(), QString(), &error)) {
        m_expression->clear();
        setInputError({});
    } else {
        setInputError(error);
        Q_EMIT statusMessage(error);
    }
}

void WaveformViewer::setInputError(const QString &message)
{
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    m_inputError->setStyleSheet(
        QStringLiteral("color: %1;").arg((dark ? QColor(0xF0, 0x80, 0x80) : QColor(0xB0, 0x20, 0x20)).name()));
    m_inputError->setText(message);
    m_inputError->setVisible(!message.isEmpty());
}

// ---------------------------------------------------------------- sync, measurements

void WaveformViewer::syncXRange(WaveformPane *source, double min, double max)
{
    if (m_syncing || !(max > min))
        return;
    m_syncing = true;
    for (WaveformPane *p : m_panes) {
        if (p == source || p->isFft() != source->isFft())
            continue;
        if (p->plot()->xScale().isLog() && min <= 0)
            continue;
        const auto [lo, hi] = p->xRange();
        if (lo != min || hi != max)
            p->setXRange(min, max);
    }
    m_syncing = false;
    updateMeasurements();
}

void WaveformViewer::updateMeasurements()
{
    WaveformPane *p = currentPane();
    if (!p || !hasWaveforms()) {
        m_measurements->clearSource();
        return;
    }
    const int index = p->selectedTrace();
    if (p->traceCount() == 0) {
        m_measurements->clearSource(tr("No trace in %1").arg(p->displayName()));
        return;
    }
    const auto w = p->shownWaveform(index);
    if (!w) {
        m_measurements->clearSource(tr("%1: %2").arg(p->traceExpression(index), p->traceError(index)));
        return;
    }
    MeasurementPanel::Source source;
    source.name = p->plot()->traceName(p->plotTraceId(index));
    if (source.name.isEmpty())
        source.name = p->traceExpression(index);
    source.waveform = *w;
    source.xUnit = p->xUnit();
    source.transient = p->isTimeDomain();
    const auto a = p->cursorX(0);
    const auto b = p->cursorX(1);
    if (a && b && *a != *b) {
        source.window = {std::min(*a, *b), std::max(*a, *b)};
        source.rangeText = tr("Between cursors A and B");
    } else {
        const auto [lo, hi] = p->xRange();
        source.window = {lo, hi};
        source.rangeText = tr("Visible range");
    }
    m_measurements->setSource(source);
}

void WaveformViewer::updateActions()
{
    WaveformPane *p = currentPane();
    const bool shown = hasWaveforms();
    m_removePaneAction->setEnabled(p != nullptr);
    for (int which = 0; which < 2; ++which) {
        m_cursorActions[which]->setEnabled(p && shown && p->traceCount() > 0);
        m_cursorActions[which]->setChecked(p && p->hasCursor(which));
    }
    const bool fft = p && shown && !p->isFft() && p->isTimeDomain() && p->traceWaveform(p->selectedTrace())
                     && !p->traceWaveform(p->selectedTrace())->isComplex;
    m_fftAction->setEnabled(fft);
    m_logXAction->setEnabled(p != nullptr);
    m_logXAction->setChecked(p && p->definition().logX);
    for (QAction *a : {m_exportPngAction, m_exportCsvAction, m_copyAction})
        a->setEnabled(p != nullptr);
    m_exportSvgAction->setEnabled(p != nullptr && PlotWidget::svgSupported());
}

} // namespace lvd::spiceui
