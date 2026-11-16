#include "sparam_ui/TouchstoneEditor.h"

#include "plot/PlotWidget.h"
#include "rfdata/NetworkMath.h"
#include "sparam_ui/SParamDialogs.h"
#include "sparam_ui/SParamFormat.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>

namespace lvd {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kKeyGammaOpt = 100000;
constexpr int kKeySpecial = 200000;

// An undo command made of two functions.
class FunctionCommand : public QUndoCommand
{
public:
    FunctionCommand(const QString &text, std::function<void()> redo, std::function<void()> undo)
        : QUndoCommand(text)
        , m_redo(std::move(redo))
        , m_undo(std::move(undo))
    {
    }
    void redo() override { m_redo(); }
    void undo() override { m_undo(); }

private:
    std::function<void()> m_redo;
    std::function<void()> m_undo;
};

// Typing in the comment field: consecutive edits merge into one command.
class CommentCommand : public QUndoCommand
{
public:
    CommentCommand(std::function<void(const QString &)> apply, const QString &before, const QString &after)
        : QUndoCommand(QCoreApplication::translate("lvd::TouchstoneEditor", "Edit comment"))
        , m_apply(std::move(apply))
        , m_before(before)
        , m_after(after)
    {
    }
    int id() const override { return 0x5350; }
    bool mergeWith(const QUndoCommand *other) override
    {
        m_after = static_cast<const CommentCommand *>(other)->m_after;
        return true;
    }
    void redo() override { m_apply(m_after); }
    void undo() override { m_apply(m_before); }

private:
    std::function<void(const QString &)> m_apply;
    QString m_before;
    QString m_after;
};

QLabel *valueLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QLabel *keyLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

double degrees(double radians)
{
    return radians * 180.0 / rf::Pi;
}

QString rangeText(const std::vector<double> &frequencies, const std::vector<int> &indices)
{
    if (indices.empty())
        return QString();
    const double lo = frequencies[static_cast<std::size_t>(indices.front())];
    const double hi = frequencies[static_cast<std::size_t>(indices.back())];
    if (lo == hi)
        return sparam::formatFrequency(lo);
    return QStringLiteral("%1 – %2").arg(sparam::formatFrequency(lo), sparam::formatFrequency(hi));
}

// Touchstone write options matching a file that was read: version, format and unit.
rf::TouchstoneWriteOptions optionsOfFile(const QString &path)
{
    rf::TouchstoneWriteOptions options;
    options.version = QFileInfo(path).suffix().compare(QLatin1String("ts"), Qt::CaseInsensitive) == 0 ? 2 : 1;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return options;
    for (int lines = 0; lines < 200 && !file.atEnd(); ++lines) {
        const QString line = QString::fromLatin1(file.readLine()).trimmed();
        if (line.startsWith(QLatin1String("[version]"), Qt::CaseInsensitive))
            options.version = 2;
        if (!line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList tokens = line.mid(1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &token : tokens) {
            const QString t = token.toLower();
            if (t == QLatin1String("ri"))
                options.format = rf::TouchstoneFormat::RealImaginary;
            else if (t == QLatin1String("ma"))
                options.format = rf::TouchstoneFormat::MagnitudeAngle;
            else if (t == QLatin1String("db"))
                options.format = rf::TouchstoneFormat::DecibelAngle;
            else
                for (const QString unit : {QStringLiteral("Hz"), QStringLiteral("kHz"), QStringLiteral("MHz"),
                                           QStringLiteral("GHz")})
                    if (t == unit.toLower())
                        options.frequencyUnit = unit;
        }
        // Keep scanning for [Version] only when it could still follow (it precedes '#').
        break;
    }
    return options;
}

} // namespace

// Data of one plot trace.
struct TouchstoneEditor::Series {
    int key = 0;
    QString name;
    QString unit;
    bool complex = false;
    ComplexPart part = ComplexPart::MagnitudeDb;
    const std::vector<double> *x = nullptr; // frequencies
    std::vector<double> y;
    std::vector<rf::Complex> z;
    bool mainGrid = true; // x is the network's frequency grid (highlights apply)
    TraceStyle style;
};

TouchstoneEditor::TouchstoneEditor(QWidget *parent)
    : QWidget(parent)
{
    m_undo = new QUndoStack(this);
    m_undo->setUndoLimit(100);
    m_model = new SParamTableModel(this);
    m_model->setNetwork(&m_network);
    m_model->setEditHandler([this](int k, int row, int column, rf::Complex value) {
        return setSParameter(k, row, column, value);
    });
    createActions();
    buildUi();
    connect(m_undo, &QUndoStack::cleanChanged, this, &TouchstoneEditor::updateModified);
    connect(m_undo, &QUndoStack::indexChanged, this, &TouchstoneEditor::updateModified);
    updateSummary();
    updateWarnings();
    updateActions();
}

TouchstoneEditor::~TouchstoneEditor()
{
    // Commands refer to this editor: drop them before the members go.
    m_undo->disconnect(this);
    m_undo->clear();
}

// ------------------------------------------------------------------ UI

void TouchstoneEditor::createActions()
{
    const auto make = [this](const QString &text, auto slot) {
        auto *action = new QAction(text, this);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };
    const auto separator = [this]() {
        auto *action = new QAction(this);
        action->setSeparator(true);
        return action;
    };
    m_saveAction = make(tr("&Save"), [this]() {
        QString error;
        if (!save(&error) && !error.isEmpty())
            reportError(tr("Save"), error);
    });
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAsAction = make(tr("Save &As…"), &TouchstoneEditor::onSaveAs);
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    m_exportCsvAction = make(tr("Export Table as &CSV…"), &TouchstoneEditor::onExportCsv);

    m_undoAction = m_undo->createUndoAction(this, tr("&Undo"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = m_undo->createRedoAction(this, tr("&Redo"));
    m_redoAction->setShortcut(QKeySequence::Redo);

    m_referencesAction = make(tr("Reference &Impedances…"), &TouchstoneEditor::onReferences);
    m_referencesAction->setToolTip(tr("Change the reference impedance of one or all ports"));
    m_renumberAction = make(tr("Re&number Ports…"), &TouchstoneEditor::onRenumber);
    m_resampleAction = make(tr("Res&ample…"), &TouchstoneEditor::onResample);
    m_resampleAction->setToolTip(tr("Interpolate onto a new frequency grid"));
    m_cropAction = make(tr("C&rop…"), &TouchstoneEditor::onCrop);
    m_cropAction->setToolTip(tr("Keep a frequency range"));
    m_cascadeAction = make(tr("Ca&scade…"), &TouchstoneEditor::onCascade);
    m_cascadeAction->setToolTip(tr("Cascade with a two-port file"));
    m_deembedAction = make(tr("&De-embed…"), &TouchstoneEditor::onDeembed);
    m_deembedAction->setToolTip(tr("Remove two-port fixtures"));
    m_checkAction = make(tr("&Check Passivity && Reciprocity"), &TouchstoneEditor::onCheck);
    m_clearHighlightAction = make(tr("C&lear Highlights"), &TouchstoneEditor::clearHighlights);

    m_plotMenuAction = new QAction(tr("&Plot"), this);
    m_smithMenuAction = new QAction(tr("S&mith Chart"), this);

    for (QAction *action : {m_saveAction, m_saveAsAction, m_undoAction, m_redoAction}) {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        QWidget::addAction(action);
    }

    m_actions = {m_saveAction,      m_saveAsAction,     m_exportCsvAction, separator(),
                 m_undoAction,      m_redoAction,       separator(),       m_referencesAction,
                 m_renumberAction,  m_resampleAction,   m_cropAction,      m_cascadeAction,
                 m_deembedAction,   separator(),        m_checkAction,     m_clearHighlightAction,
                 separator(),       m_plotMenuAction,   m_smithMenuAction};
}

QList<QAction *> TouchstoneEditor::actions() const
{
    return m_actions;
}

void TouchstoneEditor::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_toolBar = new QToolBar(this);
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_toolBar->setIconSize(QSize(16, 16));
    outer->addWidget(m_toolBar);

    // ---- Summary
    auto *summary = new QFrame(this);
    summary->setObjectName(QStringLiteral("summary"));
    auto *grid = new QGridLayout(summary);
    grid->setContentsMargins(10, 6, 10, 6);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(3);
    m_fileLabel = valueLabel(summary);
    m_portsLabel = valueLabel(summary);
    m_frequencyLabel = valueLabel(summary);
    m_referenceLabel = valueLabel(summary);
    m_noiseLabel = valueLabel(summary);
    m_checkLabel = valueLabel(summary);
    m_checkLabel->setWordWrap(true);
    grid->addWidget(keyLabel(tr("File"), summary), 0, 0);
    grid->addWidget(m_fileLabel, 0, 1);
    grid->addWidget(keyLabel(tr("Ports"), summary), 1, 0);
    grid->addWidget(m_portsLabel, 1, 1);
    grid->addWidget(keyLabel(tr("Frequencies"), summary), 2, 0);
    grid->addWidget(m_frequencyLabel, 2, 1);
    grid->addWidget(keyLabel(tr("References"), summary), 0, 2);
    grid->addWidget(m_referenceLabel, 0, 3);
    grid->addWidget(keyLabel(tr("Noise data"), summary), 1, 2);
    grid->addWidget(m_noiseLabel, 1, 3);
    grid->addWidget(keyLabel(tr("Checks"), summary), 2, 2);
    grid->addWidget(m_checkLabel, 2, 3);
    grid->addWidget(keyLabel(tr("Comment"), summary), 0, 4, Qt::AlignTop);
    m_comment = new QPlainTextEdit(summary);
    m_comment->setObjectName(QStringLiteral("comment"));
    m_comment->setPlaceholderText(tr("No comment"));
    m_comment->setTabChangesFocus(true);
    m_comment->setFixedHeight(m_comment->fontMetrics().lineSpacing() * 3 + 12);
    grid->addWidget(m_comment, 0, 5, 3, 1);
    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(3, 3);
    grid->setColumnStretch(5, 4);
    m_warningsButton = new QToolButton(summary);
    m_warningsButton->setCheckable(true);
    m_warningsButton->setAutoRaise(true);
    m_warningsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_warningsButton->setArrowType(Qt::RightArrow);
    grid->addWidget(m_warningsButton, 3, 0, 1, 6, Qt::AlignLeft);
    m_warnings = new QPlainTextEdit(summary);
    m_warnings->setReadOnly(true);
    m_warnings->setFixedHeight(m_warnings->fontMetrics().lineSpacing() * 4 + 12);
    m_warnings->setVisible(false);
    grid->addWidget(m_warnings, 4, 0, 1, 6);
    connect(m_warningsButton, &QToolButton::toggled, this, [this](bool open) {
        m_warnings->setVisible(open);
        m_warningsButton->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(m_comment, &QPlainTextEdit::textChanged, this, [this]() {
        if (m_applyingComment)
            return;
        const QString text = m_comment->toPlainText();
        if (text == m_network.comment)
            return;
        m_undo->push(new CommentCommand([this](const QString &c) { applyComment(c); }, m_network.comment, text));
    });
    outer->addWidget(summary);
    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    outer->addWidget(line);

    // ---- Main area: controls on the left, plots and table on the right.
    auto *main = new QSplitter(Qt::Horizontal, this);
    main->setChildrenCollapsible(false);
    outer->addWidget(main, 1);

    auto *controls = new QWidget;
    auto *controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setContentsMargins(8, 8, 8, 8);

    auto *parameters = new QGroupBox(tr("Parameters"), controls);
    auto *parametersLayout = new QVBoxLayout(parameters);
    m_selectorBox = new QWidget(parameters);
    m_selectorGrid = new QGridLayout(m_selectorBox);
    m_selectorGrid->setContentsMargins(0, 0, 0, 0);
    m_selectorGrid->setHorizontalSpacing(8);
    parametersLayout->addWidget(m_selectorBox);
    auto *quick = new QGridLayout;
    const auto quickButton = [this, parameters](const QString &text, std::function<bool(int, int)> predicate) {
        auto *button = new QPushButton(text, parameters);
        connect(button, &QPushButton::clicked, this, [this, predicate]() {
            std::vector<std::pair<int, int>> selection;
            const int n = m_network.portCount();
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j)
                    if (predicate(i, j))
                        selection.emplace_back(i, j);
            setSelectedParameters(selection);
        });
        return button;
    };
    quick->addWidget(quickButton(tr("All"), [](int, int) { return true; }), 0, 0);
    quick->addWidget(quickButton(tr("None"), [](int, int) { return false; }), 0, 1);
    quick->addWidget(quickButton(tr("Reflection"), [](int i, int j) { return i == j; }), 1, 0);
    quick->addWidget(quickButton(tr("Transmission"), [](int i, int j) { return i != j; }), 1, 1);
    parametersLayout->addLayout(quick);
    controlsLayout->addWidget(parameters);

    auto *plotBox = new QGroupBox(tr("Plot"), controls);
    auto *plotLayout = new QVBoxLayout(plotBox);
    m_quantityCombo = new QComboBox(plotBox);
    m_quantityCombo->setObjectName(QStringLiteral("quantity"));
    const std::pair<SParamQuantity, QString> quantities[] = {
        {SParamQuantity::MagnitudeDb, tr("Magnitude (dB)")},
        {SParamQuantity::Magnitude, tr("Magnitude (linear)")},
        {SParamQuantity::PhaseDeg, tr("Phase")},
        {SParamQuantity::UnwrappedPhaseDeg, tr("Unwrapped phase")},
        {SParamQuantity::GroupDelay, tr("Group delay")},
        {SParamQuantity::Real, tr("Real part")},
        {SParamQuantity::Imaginary, tr("Imaginary part")},
        {SParamQuantity::Vswr, tr("VSWR (reflections)")},
        {SParamQuantity::ReturnLoss, tr("Return loss (reflections)")},
        {SParamQuantity::StabilityK, tr("Stability K and |Δ|")},
        {SParamQuantity::StabilityMu, tr("Stability μ and μ′")},
        {SParamQuantity::MaximumGain, tr("MAG / MSG")},
        {SParamQuantity::NoiseFigureMin, tr("Minimum noise figure")},
        {SParamQuantity::NoiseResistance, tr("Noise resistance Rn")},
    };
    for (const auto &[quantity, text] : quantities)
        m_quantityCombo->addItem(text, static_cast<int>(quantity));
    plotLayout->addWidget(m_quantityCombo);
    m_gammaOptCheck = new QCheckBox(tr("Γopt on the Smith chart"), plotBox);
    m_gammaOptCheck->setChecked(m_gammaOptVisible);
    plotLayout->addWidget(m_gammaOptCheck);
    controlsLayout->addWidget(plotBox);
    connect(m_quantityCombo, &QComboBox::activated, this, [this](int index) {
        setPlotQuantity(static_cast<SParamQuantity>(m_quantityCombo->itemData(index).toInt()));
    });
    connect(m_gammaOptCheck, &QCheckBox::toggled, this, &TouchstoneEditor::setGammaOptVisible);

    auto *tableBox = new QGroupBox(tr("Table"), controls);
    auto *tableForm = new QGridLayout(tableBox);
    m_representationCombo = new QComboBox(tableBox);
    m_representationCombo->setObjectName(QStringLiteral("representation"));
    m_representationCombo->addItem(tr("S-parameters"), static_cast<int>(SParamRepresentation::S));
    m_representationCombo->addItem(tr("Z-parameters"), static_cast<int>(SParamRepresentation::Z));
    m_representationCombo->addItem(tr("Y-parameters"), static_cast<int>(SParamRepresentation::Y));
    m_representationCombo->addItem(tr("ABCD-parameters"), static_cast<int>(SParamRepresentation::Abcd));
    m_representationCombo->addItem(tr("T-parameters"), static_cast<int>(SParamRepresentation::T));
    m_formCombo = new QComboBox(tableBox);
    m_formCombo->setObjectName(QStringLiteral("form"));
    m_formCombo->addItem(tr("Real / imaginary"), static_cast<int>(SParamTableForm::RealImaginary));
    m_formCombo->addItem(tr("Magnitude / angle"), static_cast<int>(SParamTableForm::MagnitudeAngle));
    m_formCombo->addItem(tr("dB / angle"), static_cast<int>(SParamTableForm::DecibelAngle));
    m_formCombo->setCurrentIndex(2);
    tableForm->addWidget(new QLabel(tr("Show:"), tableBox), 0, 0);
    tableForm->addWidget(m_representationCombo, 0, 1);
    tableForm->addWidget(new QLabel(tr("As:"), tableBox), 1, 0);
    tableForm->addWidget(m_formCombo, 1, 1);
    m_tableNote = new QLabel(tableBox);
    m_tableNote->setWordWrap(true);
    m_tableNote->setForegroundRole(QPalette::PlaceholderText);
    tableForm->addWidget(m_tableNote, 2, 0, 1, 2);
    tableForm->setColumnStretch(1, 1);
    controlsLayout->addWidget(tableBox);
    controlsLayout->addStretch(1);
    connect(m_representationCombo, &QComboBox::activated, this, [this](int index) {
        setTableRepresentation(static_cast<SParamRepresentation>(m_representationCombo->itemData(index).toInt()));
    });
    connect(m_formCombo, &QComboBox::activated, this, [this](int index) {
        setTableForm(static_cast<SParamTableForm>(m_formCombo->itemData(index).toInt()));
    });

    auto *scroll = new QScrollArea(main);
    scroll->setWidget(controls);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setMinimumWidth(controls->sizeHint().width() + 16);
    main->addWidget(scroll);

    m_viewSplitter = new QSplitter(Qt::Vertical, main);
    m_viewSplitter->setChildrenCollapsible(false);
    auto *plots = new QSplitter(Qt::Horizontal, m_viewSplitter);
    plots->setChildrenCollapsible(false);
    m_plot = new PlotWidget(plots);
    m_plot->setObjectName(QStringLiteral("rectangularPlot"));
    m_plot->setXAxis(tr("Frequency"), QStringLiteral("Hz"));
    m_smith = new PlotWidget(plots);
    m_smith->setObjectName(QStringLiteral("smithChart"));
    m_smith->setKind(PlotKind::Smith);
    m_smith->setMarkerTableVisible(true);
    plots->addWidget(m_plot);
    plots->addWidget(m_smith);
    plots->setStretchFactor(0, 3);
    plots->setStretchFactor(1, 2);
    m_viewSplitter->addWidget(plots);

    m_table = new QTableView(m_viewSplitter);
    m_table->setObjectName(QStringLiteral("table"));
    m_table->setModel(m_model);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::ContiguousSelection);
    m_table->verticalHeader()->setDefaultSectionSize(m_table->fontMetrics().height() + 6);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->horizontalHeader()->setDefaultSectionSize(118);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_viewSplitter->addWidget(m_table);
    m_viewSplitter->setStretchFactor(0, 3);
    m_viewSplitter->setStretchFactor(1, 2);
    main->addWidget(m_viewSplitter);
    main->setStretchFactor(0, 0);
    main->setStretchFactor(1, 1);

    // Tool bar and plot menus.
    auto *plotMenu = new QMenu(this);
    plotMenu->addActions(m_plot->plotActions());
    m_plotMenuAction->setMenu(plotMenu);
    auto *smithMenu = new QMenu(this);
    smithMenu->addActions(m_smith->plotActions());
    m_smithMenuAction->setMenu(smithMenu);
    m_toolBar->addActions(m_actions);
    for (QAction *action : {m_plotMenuAction, m_smithMenuAction}) {
        if (auto *button = qobject_cast<QToolButton *>(m_toolBar->widgetForAction(action)))
            button->setPopupMode(QToolButton::InstantPopup);
    }

    rebuildSelector();
    updateQuantityItems();
    updateTableEditability();
}

void TouchstoneEditor::updateActions()
{
    const int ports = m_network.portCount();
    const bool hasData = m_network.frequencyCount() > 0;
    m_saveAction->setEnabled(hasData);
    m_saveAsAction->setEnabled(hasData);
    m_exportCsvAction->setEnabled(hasData);
    m_referencesAction->setEnabled(hasData);
    m_renumberAction->setEnabled(hasData && ports > 1);
    m_resampleAction->setEnabled(hasData);
    m_cropAction->setEnabled(hasData);
    m_cascadeAction->setEnabled(hasData && ports == 2);
    m_deembedAction->setEnabled(hasData && ports == 2);
    m_checkAction->setEnabled(hasData);
    m_clearHighlightAction->setEnabled(!m_nonPassive.empty() || !m_nonReciprocal.empty());
}

// ------------------------------------------------------------------ selector

void TouchstoneEditor::rebuildSelector()
{
    const int n = m_network.portCount();
    std::vector<std::vector<bool>> previous = m_selected;
    for (auto &row : m_selectorChecks)
        for (QCheckBox *check : row)
            delete check;
    m_selectorChecks.clear();
    m_selected.assign(static_cast<std::size_t>(n), std::vector<bool>(static_cast<std::size_t>(n), false));
    if (static_cast<int>(previous.size()) == n && n > 0) {
        m_selected = previous;
    } else if (n > 0) {
        m_selected[0][0] = true;
        if (n > 1)
            m_selected[1][0] = true;
    }
    m_selectorChecks.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            auto *check = new QCheckBox(sparam::parameterName(QStringLiteral("S"), i, j, n), m_selectorBox);
            check->setObjectName(QStringLiteral("select_%1_%2").arg(i + 1).arg(j + 1));
            check->setChecked(m_selected[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]);
            connect(check, &QCheckBox::toggled, this, &TouchstoneEditor::selectionChangedByUser);
            m_selectorGrid->addWidget(check, i, j);
            m_selectorChecks[static_cast<std::size_t>(i)].push_back(check);
        }
    }
    m_model->setParameters(selectedParameters());
}

void TouchstoneEditor::selectionChangedByUser()
{
    const int n = m_network.portCount();
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            m_selected[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]
                = m_selectorChecks[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]->isChecked();
    m_model->setParameters(selectedParameters());
    refreshPlots(false);
    refreshSmith(false);
}

void TouchstoneEditor::setSelectedParameters(const std::vector<std::pair<int, int>> &parameters)
{
    const int n = m_network.portCount();
    for (auto &row : m_selected)
        std::fill(row.begin(), row.end(), false);
    for (const auto &[i, j] : parameters) {
        if (i >= 0 && j >= 0 && i < n && j < n)
            m_selected[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = true;
    }
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            QCheckBox *check = m_selectorChecks[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            const QSignalBlocker blocker(check);
            check->setChecked(m_selected[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]);
        }
    }
    m_model->setParameters(selectedParameters());
    refreshPlots(false);
    refreshSmith(false);
}

std::vector<std::pair<int, int>> TouchstoneEditor::selectedParameters() const
{
    std::vector<std::pair<int, int>> result;
    for (std::size_t i = 0; i < m_selected.size(); ++i)
        for (std::size_t j = 0; j < m_selected[i].size(); ++j)
            if (m_selected[i][j])
                result.emplace_back(static_cast<int>(i), static_cast<int>(j));
    return result;
}

bool TouchstoneEditor::isQuantityAvailable(SParamQuantity quantity) const
{
    const int ports = m_network.portCount();
    switch (quantity) {
    case SParamQuantity::StabilityK:
    case SParamQuantity::StabilityMu:
    case SParamQuantity::MaximumGain:
        return ports == 2;
    case SParamQuantity::NoiseFigureMin:
    case SParamQuantity::NoiseResistance:
        return ports == 2 && (m_network.hasNoiseParameters() || m_network.hasNoiseCorrelation());
    default:
        return true;
    }
}

void TouchstoneEditor::updateQuantityItems()
{
    auto *model = qobject_cast<QStandardItemModel *>(m_quantityCombo->model());
    for (int i = 0; i < m_quantityCombo->count(); ++i) {
        const auto quantity = static_cast<SParamQuantity>(m_quantityCombo->itemData(i).toInt());
        if (model && model->item(i))
            model->item(i)->setEnabled(isQuantityAvailable(quantity));
    }
    if (!isQuantityAvailable(m_quantity))
        m_quantity = SParamQuantity::MagnitudeDb;
    m_quantityCombo->setCurrentIndex(m_quantityCombo->findData(static_cast<int>(m_quantity)));
    const bool noise = isQuantityAvailable(SParamQuantity::NoiseFigureMin);
    m_gammaOptCheck->setEnabled(noise);

    auto *representations = qobject_cast<QStandardItemModel *>(m_representationCombo->model());
    for (int i = 0; i < m_representationCombo->count(); ++i) {
        const auto r = static_cast<SParamRepresentation>(m_representationCombo->itemData(i).toInt());
        if (representations && representations->item(i))
            representations->item(i)->setEnabled(SParamTableModel::isAvailable(r, m_network.portCount()));
    }
    if (!SParamTableModel::isAvailable(m_model->representation(), m_network.portCount()))
        m_model->setRepresentation(SParamRepresentation::S);
    m_representationCombo->setCurrentIndex(
        m_representationCombo->findData(static_cast<int>(m_model->representation())));
}

void TouchstoneEditor::setPlotQuantity(SParamQuantity quantity)
{
    if (!isQuantityAvailable(quantity))
        quantity = SParamQuantity::MagnitudeDb;
    m_quantity = quantity;
    m_quantityCombo->setCurrentIndex(m_quantityCombo->findData(static_cast<int>(quantity)));
    refreshPlots(false);
}

void TouchstoneEditor::setGammaOptVisible(bool visible)
{
    m_gammaOptVisible = visible;
    {
        const QSignalBlocker blocker(m_gammaOptCheck);
        m_gammaOptCheck->setChecked(visible);
    }
    refreshSmith(false);
}

void TouchstoneEditor::setTableRepresentation(SParamRepresentation representation)
{
    if (!SParamTableModel::isAvailable(representation, m_network.portCount()))
        representation = SParamRepresentation::S;
    m_model->setRepresentation(representation);
    m_representationCombo->setCurrentIndex(m_representationCombo->findData(static_cast<int>(representation)));
    updateTableEditability();
}

SParamRepresentation TouchstoneEditor::tableRepresentation() const
{
    return m_model->representation();
}

void TouchstoneEditor::setTableForm(SParamTableForm form)
{
    m_model->setForm(form);
    m_formCombo->setCurrentIndex(m_formCombo->findData(static_cast<int>(form)));
}

SParamTableForm TouchstoneEditor::tableForm() const
{
    return m_model->form();
}

void TouchstoneEditor::updateTableEditability()
{
    const bool editable = m_model->representation() == SParamRepresentation::S;
    m_table->setEditTriggers(editable ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                                            | QAbstractItemView::AnyKeyPressed
                                      : QAbstractItemView::NoEditTriggers);
    m_tableNote->setText(editable ? tr("Double-click a cell to edit the S-parameter (undoable).")
                                  : tr("Converted from S, read only."));
}

// ------------------------------------------------------------------ summary

void TouchstoneEditor::updateSummary()
{
    const QLocale locale;
    m_fileLabel->setText(m_filePath.isEmpty() ? (m_name.isEmpty() ? tr("(none)") : tr("%1 (not saved)").arg(m_name))
                                              : QFileInfo(m_filePath).fileName());
    m_fileLabel->setToolTip(QDir::toNativeSeparators(m_filePath));
    const int ports = m_network.portCount();
    m_portsLabel->setText(ports > 0 ? QString::number(ports) : QStringLiteral("—"));
    const int n = m_network.frequencyCount();
    if (n == 0)
        m_frequencyLabel->setText(QStringLiteral("—"));
    else
        m_frequencyLabel->setText((n == 1 ? tr("1 point, %1").arg(rangeText(m_network.frequencies, {0, 0}))
                                               : tr("%1 points, %2").arg(n).arg(rangeText(m_network.frequencies, {0, n - 1}))));
    m_referenceLabel->setText(ports > 0 ? sparam::referenceImpedanceText(m_network.z0) : QStringLiteral("—"));
    if (m_network.hasNoiseParameters()) {
        std::vector<int> all{0, static_cast<int>(m_network.noiseFrequencies.size()) - 1};
        m_noiseLabel->setText(tr("NFmin, Γopt, Rn at %1 frequencies, %2")
                                  .arg(m_network.noiseFrequencies.size())
                                  .arg(rangeText(m_network.noiseFrequencies, all)));
    } else if (m_network.hasNoiseCorrelation()) {
        m_noiseLabel->setText(tr("Noise correlation matrices (simulation)"));
    } else {
        m_noiseLabel->setText(tr("None"));
    }
    m_checkLabel->setText(m_checkText.isEmpty() ? tr("Not checked") : m_checkText);
    {
        const QSignalBlocker blocker(m_comment);
        if (m_comment->toPlainText() != m_network.comment) {
            m_applyingComment = true;
            m_comment->setPlainText(m_network.comment);
            m_applyingComment = false;
        }
    }
}

void TouchstoneEditor::updateWarnings()
{
    m_warningsButton->setText(tr("Read warnings (%1)").arg(m_readWarnings.size()));
    m_warningsButton->setVisible(!m_readWarnings.isEmpty());
    m_warnings->setPlainText(m_readWarnings.join(QLatin1Char('\n')));
    if (m_readWarnings.isEmpty()) {
        m_warningsButton->setChecked(false);
        m_warnings->setVisible(false);
    }
}

QString TouchstoneEditor::summaryText(const QString &key) const
{
    if (key == QLatin1String("file"))
        return m_fileLabel->text();
    if (key == QLatin1String("ports"))
        return m_portsLabel->text();
    if (key == QLatin1String("frequencies"))
        return m_frequencyLabel->text();
    if (key == QLatin1String("references"))
        return m_referenceLabel->text();
    if (key == QLatin1String("noise"))
        return m_noiseLabel->text();
    if (key == QLatin1String("checks"))
        return m_checkLabel->text();
    if (key == QLatin1String("comment"))
        return m_comment->toPlainText();
    return QString();
}

// ------------------------------------------------------------------ document

void TouchstoneEditor::loadNetwork(const rf::NetworkData &network, const QString &path, const QString &name,
                                   const QStringList &warnings, bool modified)
{
    m_undo->clear();
    m_network = network;
    m_filePath = path;
    m_name = name;
    m_readWarnings = warnings;
    m_unsavedNew = modified;
    m_nonPassive.clear();
    m_nonReciprocal.clear();
    m_checkText.clear();
    m_selected.clear();
    rebuildSelector();
    m_model->setNetwork(&m_network);
    updateQuantityItems();
    updateTableEditability();
    updateSummary();
    updateWarnings();
    const double z0 = m_network.z0.empty() ? rf::DefaultZ0 : m_network.z0.front();
    m_smith->setSmithZ0(z0);
    refreshPlots(true);
    refreshSmith(true);
    updateActions();
    m_undo->setClean();
    updateModified();
    m_lastTitle.clear();
    Q_EMIT titleChanged(title());
    m_lastTitle = title();
}

bool TouchstoneEditor::openFile(const QString &path, QString *error)
{
    rf::TouchstoneReadResult result = rf::readTouchstone(path);
    if (!result.network) {
        if (error)
            *error = result.error;
        return false;
    }
    const QString problem = result.network->validate();
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return false;
    }
    m_writeOptions = optionsOfFile(path);
    const QFileInfo info(path);
    loadNetwork(*result.network, info.absoluteFilePath(), info.fileName(), result.warnings, false);
    report(tr("Opened %1: %2 ports, %3 frequencies")
               .arg(info.fileName())
               .arg(m_network.portCount())
               .arg(m_network.frequencyCount()));
    return true;
}

void TouchstoneEditor::setNetwork(const rf::NetworkData &network, const QString &suggestedName)
{
    const QString problem = network.validate();
    if (!problem.isEmpty()) {
        report(tr("Cannot show the network: %1").arg(problem));
        return;
    }
    m_writeOptions = rf::TouchstoneWriteOptions();
    if (!sparam::allEqual(network.z0))
        m_writeOptions.version = 2;
    loadNetwork(network, QString(), suggestedName, {}, true);
}

bool TouchstoneEditor::isModified() const
{
    return m_unsavedNew || !m_undo->isClean();
}

QString TouchstoneEditor::title() const
{
    QString name = m_filePath.isEmpty() ? m_name : QFileInfo(m_filePath).fileName();
    if (name.isEmpty())
        name = tr("Untitled");
    return isModified() ? name + QLatin1Char('*') : name;
}

void TouchstoneEditor::updateModified()
{
    const bool modified = isModified();
    if (modified != m_lastModified) {
        m_lastModified = modified;
        Q_EMIT modifiedChanged(modified);
    }
    const QString t = title();
    if (t != m_lastTitle) {
        m_lastTitle = t;
        Q_EMIT titleChanged(t);
    }
}

bool TouchstoneEditor::save(QString *error)
{
    if (m_filePath.isEmpty())
        return saveAsInteractive();
    rf::TouchstoneWriteOptions options = m_writeOptions;
    if (options.version == 1 && !sparam::allEqual(m_network.z0)) {
        options.version = 2;
        report(tr("The reference impedances differ between ports: saving as Touchstone 2.0."));
    }
    return saveAs(m_filePath, options, error);
}

bool TouchstoneEditor::saveAs(const QString &path, const rf::TouchstoneWriteOptions &options, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (m_network.frequencyCount() == 0)
        return fail(tr("There is no data to save."));
    if (options.version == 1 && !sparam::allEqual(m_network.z0))
        return fail(tr("Touchstone 1 needs the same reference impedance at every port; use version 2."));
    const int ports = m_network.portCount();
    if (!sparam::extensionMatches(path, ports, options.version))
        return fail(options.version >= 2
                        ? tr("The file extension must be %1 or .ts for a %2-port network.")
                              .arg(sparam::touchstoneExtension(ports))
                              .arg(ports)
                        : tr("The file extension must be %1 for a %2-port network.")
                              .arg(sparam::touchstoneExtension(ports))
                              .arg(ports));
    QString writeError;
    if (!rf::writeTouchstone(path, m_network, options, &writeError))
        return fail(writeError.isEmpty() ? tr("Cannot write %1.").arg(QDir::toNativeSeparators(path)) : writeError);
    const QFileInfo info(path);
    m_filePath = info.absoluteFilePath();
    m_name = info.fileName();
    m_writeOptions = options;
    m_unsavedNew = false;
    m_undo->setClean();
    updateSummary();
    updateModified();
    report(tr("Saved %1 (Touchstone %2)").arg(info.fileName(), options.version >= 2 ? QStringLiteral("2.0")
                                                                                     : QStringLiteral("1.1")));
    return true;
}

bool TouchstoneEditor::saveAsInteractive()
{
    if (m_network.frequencyCount() == 0)
        return false;
    const int ports = m_network.portCount();
    QString suggested = m_filePath;
    if (suggested.isEmpty()) {
        QString base = m_name.isEmpty() ? tr("untitled") : m_name;
        if (!QFileInfo(base).suffix().isEmpty())
            base = QFileInfo(base).completeBaseName();
        suggested = QDir(directory()).filePath(base + sparam::touchstoneExtension(ports));
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Save Touchstone File"), suggested, touchstoneFileFilter());
    if (path.isEmpty())
        return false;
    SaveOptionsDialog dialog(m_writeOptions, sparam::allEqual(m_network.z0), this);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    const rf::TouchstoneWriteOptions options = dialog.options();
    if (!sparam::extensionMatches(path, ports, options.version)) {
        const QString fixed = sparam::withMatchingExtension(path, ports);
        const auto answer = QMessageBox::question(
            this, tr("File Extension"),
            tr("The extension of \"%1\" does not match a %2-port network. Save as \"%3\" instead?")
                .arg(QFileInfo(path).fileName())
                .arg(ports)
                .arg(QFileInfo(fixed).fileName()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            return false;
        if (QFileInfo::exists(fixed)
            && QMessageBox::question(this, tr("File Exists"),
                                     tr("\"%1\" already exists. Replace it?").arg(QFileInfo(fixed).fileName()))
                   != QMessageBox::Yes)
            return false;
        path = fixed;
    }
    QString error;
    if (!saveAs(path, options, &error)) {
        reportError(tr("Save"), error);
        return false;
    }
    return true;
}

bool TouchstoneEditor::maybeSave()
{
    if (!isModified())
        return true;
    const auto answer = QMessageBox::warning(
        this, tr("Unsaved Changes"), tr("\"%1\" has unsaved changes. Save them?").arg(title().chopped(1)),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Discard)
        return true;
    QString error;
    if (save(&error))
        return true;
    if (!error.isEmpty())
        reportError(tr("Save"), error);
    return false;
}

QString TouchstoneEditor::directory() const
{
    if (!m_filePath.isEmpty())
        return QFileInfo(m_filePath).absolutePath();
    return QDir::homePath();
}

// ------------------------------------------------------------------ editing

void TouchstoneEditor::replaceNetwork(const rf::NetworkData &network)
{
    const bool portsChanged = network.portCount() != m_network.portCount();
    m_network = network;
    m_nonPassive.clear();
    m_nonReciprocal.clear();
    m_checkText.clear();
    if (portsChanged) {
        m_selected.clear();
        rebuildSelector();
    }
    m_model->networkReset();
    updateQuantityItems();
    updateTableEditability();
    updateSummary();
    m_smith->setSmithZ0(m_network.z0.empty() ? rf::DefaultZ0 : m_network.z0.front());
    refreshPlots(false);
    refreshSmith(false);
    updateActions();
}

void TouchstoneEditor::setElement(int frequencyIndex, int row, int column, rf::Complex value)
{
    m_network.s[static_cast<std::size_t>(frequencyIndex)](row, column) = value;
    const bool hadChecks = !m_checkText.isEmpty();
    m_nonPassive.clear();
    m_nonReciprocal.clear();
    m_checkText.clear();
    if (hadChecks)
        updateSummary();
    m_model->frequencyChanged(frequencyIndex);
    refreshPlots(false);
    refreshSmith(false);
    updateActions();
}

void TouchstoneEditor::pushNetworkChange(const rf::NetworkData &after, const QString &text)
{
    auto before = std::make_shared<const rf::NetworkData>(m_network);
    auto next = std::make_shared<const rf::NetworkData>(after);
    m_undo->push(new FunctionCommand(
        text, [this, next]() { replaceNetwork(*next); }, [this, before]() { replaceNetwork(*before); }));
    report(text);
}

bool TouchstoneEditor::setSParameter(int frequencyIndex, int row, int column, rf::Complex value)
{
    const int ports = m_network.portCount();
    if (frequencyIndex < 0 || frequencyIndex >= m_network.frequencyCount() || row < 0 || column < 0 || row >= ports
        || column >= ports || !std::isfinite(value.real()) || !std::isfinite(value.imag()))
        return false;
    const rf::Complex old = m_network.s[static_cast<std::size_t>(frequencyIndex)](row, column);
    if (old == value)
        return true;
    const QString text = tr("Edit %1 at %2")
                             .arg(sparam::parameterName(QStringLiteral("S"), row, column, ports),
                                  sparam::formatFrequency(m_network.frequencies[static_cast<std::size_t>(frequencyIndex)]));
    m_undo->push(new FunctionCommand(
        text, [this, frequencyIndex, row, column, value]() { setElement(frequencyIndex, row, column, value); },
        [this, frequencyIndex, row, column, old]() { setElement(frequencyIndex, row, column, old); }));
    return true;
}

void TouchstoneEditor::applyComment(const QString &comment)
{
    m_network.comment = comment;
    if (m_comment->toPlainText() != comment) {
        m_applyingComment = true;
        m_comment->setPlainText(comment);
        m_applyingComment = false;
    }
}

void TouchstoneEditor::setComment(const QString &comment)
{
    if (comment == m_network.comment)
        return;
    m_undo->push(new CommentCommand([this](const QString &c) { applyComment(c); }, m_network.comment, comment));
}

bool TouchstoneEditor::setReferenceImpedances(const std::vector<double> &z0, QString *error)
{
    if (z0.size() != m_network.z0.size() || z0.empty()) {
        if (error)
            *error = tr("Give one reference impedance per port (%1).").arg(m_network.portCount());
        return false;
    }
    for (const double z : z0) {
        if (!(z > 0) || !std::isfinite(z)) {
            if (error)
                *error = tr("Reference impedances must be positive.");
            return false;
        }
    }
    if (z0 == m_network.z0)
        return true;
    pushNetworkChange(rf::renormalized(m_network, z0),
                      tr("Change reference impedances to %1").arg(sparam::referenceImpedanceText(z0)));
    return true;
}

bool TouchstoneEditor::renumberPorts(const std::vector<int> &order, QString *error)
{
    std::vector<int> identity(order.size());
    for (std::size_t i = 0; i < identity.size(); ++i)
        identity[i] = static_cast<int>(i);
    std::optional<rf::NetworkData> result = rf::renumberPorts(m_network, order, error);
    if (!result)
        return false;
    if (order == identity)
        return true;
    QStringList numbers;
    for (const int old : order)
        numbers << QString::number(old + 1);
    pushNetworkChange(*result, tr("Renumber ports (%1)").arg(numbers.join(QLatin1Char(','))));
    return true;
}

bool TouchstoneEditor::resample(const std::vector<double> &frequencies, rf::Extrapolation extrapolation,
                                QString *error, QString *warning)
{
    for (const double f : frequencies) {
        if (!(f >= 0) || !std::isfinite(f)) {
            if (error)
                *error = tr("Frequencies must be zero or positive.");
            return false;
        }
    }
    QString localWarning;
    std::optional<rf::NetworkData> result = rf::interpolate(m_network, frequencies, extrapolation, error, &localWarning);
    if (!result)
        return false;
    if (warning)
        *warning = localWarning;
    pushNetworkChange(*result, tr("Resample to %1 frequencies").arg(frequencies.size()));
    if (!localWarning.isEmpty())
        report(localWarning);
    return true;
}

bool TouchstoneEditor::cropFrequencies(double minimumHz, double maximumHz, QString *error)
{
    if (!(maximumHz >= minimumHz)) {
        if (error)
            *error = tr("The upper frequency must not be below the lower one.");
        return false;
    }
    const double tolerance = 1e-12 * std::max(std::abs(minimumHz), std::abs(maximumHz));
    rf::NetworkData result;
    result.z0 = m_network.z0;
    result.comment = m_network.comment;
    for (std::size_t k = 0; k < m_network.frequencies.size(); ++k) {
        const double f = m_network.frequencies[k];
        if (f >= minimumHz - tolerance && f <= maximumHz + tolerance) {
            result.frequencies.push_back(f);
            result.s.push_back(m_network.s[k]);
            if (k < m_network.noiseCorrelation.size())
                result.noiseCorrelation.push_back(m_network.noiseCorrelation[k]);
        }
    }
    for (std::size_t k = 0; k < m_network.noiseFrequencies.size() && k < m_network.noise.size(); ++k) {
        const double f = m_network.noiseFrequencies[k];
        if (f >= minimumHz - tolerance && f <= maximumHz + tolerance) {
            result.noiseFrequencies.push_back(f);
            result.noise.push_back(m_network.noise[k]);
        }
    }
    if (result.frequencies.empty()) {
        if (error)
            *error = tr("No frequency lies inside %1 – %2.")
                         .arg(sparam::formatFrequency(minimumHz), sparam::formatFrequency(maximumHz));
        return false;
    }
    if (result.frequencies.size() == m_network.frequencies.size()
        && result.noiseFrequencies.size() == m_network.noiseFrequencies.size())
        return true;
    pushNetworkChange(result, tr("Crop to %1 – %2")
                                  .arg(sparam::formatFrequency(result.frequencies.front()),
                                       sparam::formatFrequency(result.frequencies.back())));
    return true;
}

bool TouchstoneEditor::cascadeWith(const rf::NetworkData &other, bool otherAfter, QString *error)
{
    if (m_network.portCount() != 2 || other.portCount() != 2) {
        if (error)
            *error = tr("Cascading needs two two-port networks.");
        return false;
    }
    // Keep this network's frequency grid in both directions.
    QString interpolationError;
    std::optional<rf::NetworkData> resampled =
        rf::interpolate(other, m_network.frequencies, rf::Extrapolation::Error, &interpolationError);
    if (!resampled) {
        if (error)
            *error = tr("The other network does not cover this frequency range: %1").arg(interpolationError);
        return false;
    }
    std::optional<rf::NetworkData> result = otherAfter ? rf::cascade(m_network, *resampled, error)
                                                       : rf::cascade(*resampled, m_network, error);
    if (!result)
        return false;
    result->comment = m_network.comment;
    pushNetworkChange(*result, otherAfter ? tr("Cascade with a two-port after") : tr("Cascade with a two-port before"));
    return true;
}

bool TouchstoneEditor::deembed(const std::optional<rf::NetworkData> &left, const std::optional<rf::NetworkData> &right,
                               QString *error)
{
    if (!left && !right) {
        if (error)
            *error = tr("Choose a left and/or a right fixture.");
        return false;
    }
    std::optional<rf::NetworkData> result = rf::deembed(m_network, left, right, error);
    if (!result)
        return false;
    pushNetworkChange(*result, left && right ? tr("De-embed both fixtures")
                                             : (left ? tr("De-embed the left fixture") : tr("De-embed the right fixture")));
    return true;
}

SParamCheckReport TouchstoneEditor::checkPassivityAndReciprocity(double tolerance)
{
    SParamCheckReport report;
    report.nonPassive = rf::nonPassiveFrequencies(m_network, tolerance);
    report.nonReciprocal = rf::nonReciprocalFrequencies(m_network, tolerance);
    const int n = m_network.frequencyCount();
    QStringList lines;
    if (report.passive()) {
        lines << tr("Passive at all %1 frequencies.").arg(n);
    } else {
        lines << tr("Not passive at %1 of %2 frequencies (%3).")
                     .arg(report.nonPassive.size())
                     .arg(n)
                     .arg(rangeText(m_network.frequencies, report.nonPassive));
    }
    if (report.reciprocal()) {
        lines << tr("Reciprocal at all %1 frequencies.").arg(n);
    } else {
        lines << tr("Not reciprocal at %1 of %2 frequencies (%3).")
                     .arg(report.nonReciprocal.size())
                     .arg(n)
                     .arg(rangeText(m_network.frequencies, report.nonReciprocal));
    }
    report.text = lines.join(QLatin1Char('\n'));
    m_nonPassive = report.nonPassive;
    m_nonReciprocal = report.nonReciprocal;
    m_checkText = lines.join(QStringLiteral(" "));
    updateSummary();
    refreshPlots(false);
    refreshSmith(false);
    updateActions();
    this->report(m_checkText);
    return report;
}

void TouchstoneEditor::clearHighlights()
{
    m_nonPassive.clear();
    m_nonReciprocal.clear();
    refreshPlots(false);
    refreshSmith(false);
    updateActions();
}

std::vector<int> TouchstoneEditor::highlightedFrequencies() const
{
    std::vector<int> all;
    std::set_union(m_nonPassive.begin(), m_nonPassive.end(), m_nonReciprocal.begin(), m_nonReciprocal.end(),
                   std::back_inserter(all));
    return all;
}

// ------------------------------------------------------------------ plots

std::vector<rf::NoiseParameters> TouchstoneEditor::noiseParameters(std::vector<double> *frequencies) const
{
    if (m_network.portCount() != 2)
        return {};
    if (m_network.hasNoiseParameters()) {
        *frequencies = m_network.noiseFrequencies;
        return m_network.noise;
    }
    std::vector<rf::NoiseParameters> result;
    if (!m_network.hasNoiseCorrelation())
        return result;
    frequencies->clear();
    const std::vector<double> &z0 = m_network.z0;
    const std::vector<double> common{z0[0], z0[0]};
    for (std::size_t k = 0; k < m_network.frequencies.size() && k < m_network.noiseCorrelation.size(); ++k) {
        rf::CMatrix s = m_network.s[k];
        rf::CMatrix c = m_network.noiseCorrelation[k];
        if (z0[1] != z0[0]) {
            const rf::CMatrix t = rf::noiseRenormalization(s, z0, common);
            c = t * c * t.adjoint();
            s = rf::renormalize(s, z0, common);
        }
        frequencies->push_back(m_network.frequencies[k]);
        result.push_back(rf::noiseParametersFromCorrelation(s, c, z0[0]));
    }
    return result;
}

bool TouchstoneEditor::showSeries(PlotWidget *plot, const std::vector<Series> &series, bool rebuild,
                                  std::vector<int> &keys, std::map<int, int> &traces, std::vector<int> &overlays)
{
    for (const int id : overlays)
        plot->removeTrace(id);
    overlays.clear();
    std::vector<int> newKeys;
    for (const Series &s : series)
        newKeys.push_back(s.key);
    const bool layoutChanged = rebuild || newKeys != keys;
    if (layoutChanged) {
        plot->clear();
        traces.clear();
        for (const Series &s : series)
            traces[s.key] = plot->addTrace(s.name, s.unit, s.style);
        keys = newKeys;
    }
    for (const Series &s : series) {
        const int id = traces[s.key];
        if (s.complex) {
            plot->setTraceData(id, *s.x, std::span<const std::complex<double>>(s.z));
            plot->setTraceComplexPart(id, s.part);
        } else {
            plot->setTraceData(id, *s.x, std::span<const double>(s.y));
        }
    }

    // Highlighted frequencies: a wide translucent band over each trace.
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    const struct {
        const std::vector<int> *indices;
        QString label;
        QColor color;
    } kinds[] = {
        {&m_nonPassive, tr("Not passive"), dark ? QColor(255, 99, 99, 90) : QColor(214, 39, 40, 70)},
        {&m_nonReciprocal, tr("Not reciprocal"), dark ? QColor(255, 190, 70, 80) : QColor(230, 140, 0, 70)},
    };
    const std::size_t n = m_network.frequencies.size();
    for (const auto &kind : kinds) {
        if (kind.indices->empty())
            continue;
        std::vector<char> mask(n, 0);
        for (const int k : *kind.indices)
            if (k >= 0 && static_cast<std::size_t>(k) < n)
                mask[static_cast<std::size_t>(k)] = 1;
        // One trace per kind: the highlighted runs of every series, separated
        // by gaps. Each run reaches halfway to its neighbours, so a single
        // failing frequency still shows as a short band.
        std::vector<double> x;
        std::vector<double> y;
        std::vector<rf::Complex> z;
        bool complex = false;
        ComplexPart part = ComplexPart::MagnitudeDb;
        const auto gap = [&]() {
            x.push_back(kNaN);
            if (complex)
                z.emplace_back(kNaN, kNaN);
            else
                y.push_back(kNaN);
        };
        for (const Series &s : series) {
            if (!s.mainGrid || s.x->size() != n || n < 2)
                continue;
            complex = s.complex;
            part = s.part;
            const std::vector<double> &f = *s.x;
            const auto addMid = [&](std::size_t a, std::size_t b) {
                x.push_back((f[a] + f[b]) / 2);
                if (complex)
                    z.push_back((s.z[a] + s.z[b]) / 2.0);
                else
                    y.push_back((s.y[a] + s.y[b]) / 2);
            };
            const auto addPoint = [&](std::size_t k) {
                x.push_back(f[k]);
                if (complex)
                    z.push_back(s.z[k]);
                else
                    y.push_back(s.y[k]);
            };
            for (std::size_t k = 0; k < n;) {
                if (!mask[k]) {
                    ++k;
                    continue;
                }
                std::size_t end = k;
                while (end + 1 < n && mask[end + 1])
                    ++end;
                if (k > 0)
                    addMid(k - 1, k);
                for (std::size_t i = k; i <= end; ++i)
                    addPoint(i);
                if (end + 1 < n)
                    addMid(end, end + 1);
                gap();
                k = end + 1;
            }
        }
        if (x.empty())
            continue;
        TraceStyle style;
        style.color = kind.color;
        style.lineWidth = 7;
        const int id = plot->addTrace(kind.label, QString(), style);
        if (complex) {
            plot->setTraceData(id, x, std::span<const std::complex<double>>(z));
            plot->setTraceComplexPart(id, part);
        } else {
            plot->setTraceData(id, x, std::span<const double>(y));
        }
        overlays.push_back(id);
    }
    return layoutChanged;
}

void TouchstoneEditor::refreshPlots(bool rebuild)
{
    const int ports = m_network.portCount();
    const std::size_t n = m_network.frequencies.size();
    const std::vector<double> &f = m_network.frequencies;
    std::vector<Series> series;
    std::vector<double> noiseX; // x of the noise traces, alive until shown
    QString label;
    QString unit;
    const auto parameterSeries = [&](int i, int j) {
        Series s;
        s.key = i * ports + j;
        s.name = sparam::parameterName(QStringLiteral("S"), i, j, ports);
        s.x = &f;
        return s;
    };
    const auto complexPart = [&](ComplexPart part) {
        for (const auto &[i, j] : selectedParameters()) {
            Series s = parameterSeries(i, j);
            s.complex = true;
            s.part = part;
            s.z.resize(n);
            for (std::size_t k = 0; k < n; ++k)
                s.z[k] = m_network.s[k](i, j);
            series.push_back(std::move(s));
        }
    };
    switch (m_quantity) {
    case SParamQuantity::MagnitudeDb:
        complexPart(ComplexPart::MagnitudeDb);
        label = tr("Magnitude");
        unit = QStringLiteral("dB");
        break;
    case SParamQuantity::Magnitude:
        complexPart(ComplexPart::Magnitude);
        label = tr("Magnitude");
        break;
    case SParamQuantity::PhaseDeg:
        complexPart(ComplexPart::PhaseDeg);
        label = tr("Phase");
        unit = QStringLiteral("deg");
        break;
    case SParamQuantity::Real:
        complexPart(ComplexPart::Real);
        label = tr("Real part");
        break;
    case SParamQuantity::Imaginary:
        complexPart(ComplexPart::Imaginary);
        label = tr("Imaginary part");
        break;
    case SParamQuantity::UnwrappedPhaseDeg:
    case SParamQuantity::GroupDelay: {
        const bool delay = m_quantity == SParamQuantity::GroupDelay;
        label = delay ? tr("Group delay") : tr("Unwrapped phase");
        unit = delay ? QStringLiteral("s") : QStringLiteral("deg");
        for (const auto &[i, j] : selectedParameters()) {
            Series s = parameterSeries(i, j);
            s.unit = unit;
            if (delay) {
                s.y = rf::groupDelay(m_network, i, j);
            } else {
                s.y = rf::unwrappedPhase(m_network, i, j);
                for (double &v : s.y)
                    v = degrees(v);
            }
            series.push_back(std::move(s));
        }
        break;
    }
    case SParamQuantity::Vswr:
    case SParamQuantity::ReturnLoss: {
        const bool vswr = m_quantity == SParamQuantity::Vswr;
        label = vswr ? tr("VSWR") : tr("Return loss");
        unit = vswr ? QString() : QStringLiteral("dB");
        for (const auto &[i, j] : selectedParameters()) {
            if (i != j)
                continue;
            Series s = parameterSeries(i, j);
            s.name = (vswr ? tr("VSWR%1") : tr("RL%1")).arg(i + 1);
            s.unit = unit;
            s.y.resize(n);
            for (std::size_t k = 0; k < n; ++k) {
                const rf::Complex g = m_network.s[k](i, i);
                s.y[k] = vswr ? rf::vswr(g) : -rf::db20(g);
                if (!std::isfinite(s.y[k]))
                    s.y[k] = kNaN;
            }
            series.push_back(std::move(s));
        }
        break;
    }
    case SParamQuantity::StabilityK:
    case SParamQuantity::StabilityMu:
    case SParamQuantity::MaximumGain: {
        if (ports != 2)
            break;
        const auto add = [&](int key, const QString &name, const QString &u, auto function) {
            Series s;
            s.key = kKeySpecial + key;
            s.name = name;
            s.unit = u;
            s.x = &f;
            s.y.resize(n);
            for (std::size_t k = 0; k < n; ++k) {
                s.y[k] = function(m_network.s[k]);
                if (!std::isfinite(s.y[k]))
                    s.y[k] = kNaN;
            }
            series.push_back(std::move(s));
        };
        if (m_quantity == SParamQuantity::StabilityK) {
            label = tr("Stability");
            add(0, tr("K"), QString(), [](const rf::CMatrix &s) { return rf::stabilityK(s); });
            add(1, tr("|Δ|"), QString(), [](const rf::CMatrix &s) { return std::abs(rf::stabilityDelta(s)); });
        } else if (m_quantity == SParamQuantity::StabilityMu) {
            label = tr("Stability");
            add(2, tr("μ"), QString(), [](const rf::CMatrix &s) { return rf::stabilityMu(s); });
            add(3, tr("μ′"), QString(), [](const rf::CMatrix &s) { return rf::stabilityMuPrime(s); });
        } else {
            label = tr("Maximum gain");
            unit = QStringLiteral("dB");
            add(4, tr("MAG/MSG"), unit, [](const rf::CMatrix &s) { return rf::maximumAvailableGainDb(s); });
        }
        break;
    }
    case SParamQuantity::NoiseFigureMin:
    case SParamQuantity::NoiseResistance: {
        const bool nf = m_quantity == SParamQuantity::NoiseFigureMin;
        label = nf ? tr("Minimum noise figure") : tr("Noise resistance");
        unit = nf ? QStringLiteral("dB") : QStringLiteral("Ω");
        const std::vector<rf::NoiseParameters> noise = noiseParameters(&noiseX);
        if (noise.empty())
            break;
        Series s;
        s.key = kKeySpecial + (nf ? 10 : 11);
        s.name = nf ? tr("NFmin") : tr("Rn");
        s.unit = unit;
        s.x = &noiseX;
        s.mainGrid = false;
        s.style.showPoints = true;
        for (const rf::NoiseParameters &p : noise)
            s.y.push_back(nf ? p.fminDb : p.rn);
        series.push_back(std::move(s));
        break;
    }
    }
    // A key change of quantity with identical keys (dB -> phase) must rebuild.
    const bool quantityChanged = m_quantity != m_plotKeysQuantity;
    m_plotKeysQuantity = m_quantity;
    if (showSeries(m_plot, series, rebuild || quantityChanged, m_plotKeys, m_plotTraces, m_overlayTraces)
        || quantityChanged || rebuild)
        m_plot->setYAxis(label, unit);
}

void TouchstoneEditor::refreshSmith(bool rebuild)
{
    const int ports = m_network.portCount();
    const std::size_t n = m_network.frequencies.size();
    std::vector<Series> series;
    for (const auto &[i, j] : selectedParameters()) {
        if (i != j)
            continue;
        Series s;
        s.key = i;
        s.name = sparam::parameterName(QStringLiteral("S"), i, i, ports);
        s.x = &m_network.frequencies;
        s.complex = true;
        s.z.resize(n);
        for (std::size_t k = 0; k < n; ++k)
            s.z[k] = m_network.s[k](i, i);
        series.push_back(std::move(s));
    }
    std::vector<double> noiseX;
    if (m_gammaOptVisible && isQuantityAvailable(SParamQuantity::NoiseFigureMin)) {
        const std::vector<rf::NoiseParameters> noise = noiseParameters(&noiseX);
        if (!noise.empty()) {
            Series s;
            s.key = kKeyGammaOpt;
            s.name = tr("Γopt");
            s.x = &noiseX;
            s.complex = true;
            s.mainGrid = false;
            s.style.showPoints = true;
            s.style.lineStyle = Qt::DashLine;
            for (const rf::NoiseParameters &p : noise)
                s.z.push_back(p.gammaOpt);
            series.push_back(std::move(s));
        }
    }
    showSeries(m_smith, series, rebuild, m_smithKeys, m_smithTraces, m_smithOverlayTraces);
}

// ------------------------------------------------------------------ CSV

QString TouchstoneEditor::tableCsv() const
{
    return m_model->csvText();
}

bool TouchstoneEditor::exportTableCsv(const QString &path, QString *error) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(tableCsv().toUtf8());
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ interactive

void TouchstoneEditor::report(const QString &message)
{
    Q_EMIT statusMessage(message);
}

void TouchstoneEditor::reportError(const QString &title, const QString &message)
{
    Q_EMIT statusMessage(message);
    QMessageBox::warning(this, title, message);
}

std::optional<rf::NetworkData> TouchstoneEditor::readTwoPort(const QString &path, QString *error) const
{
    rf::TouchstoneReadResult result = rf::readTouchstone(path);
    if (!result.network) {
        *error = tr("%1: %2").arg(QFileInfo(path).fileName(), result.error);
        return std::nullopt;
    }
    if (result.network->portCount() != 2) {
        *error = tr("%1 is a %2-port network; a two-port is needed.")
                     .arg(QFileInfo(path).fileName())
                     .arg(result.network->portCount());
        return std::nullopt;
    }
    return result.network;
}

void TouchstoneEditor::onSaveAs()
{
    saveAsInteractive();
}

void TouchstoneEditor::onExportCsv()
{
    QString base = QFileInfo(m_filePath.isEmpty() ? m_name : m_filePath).completeBaseName();
    if (base.isEmpty())
        base = tr("table");
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Table"), QDir(directory()).filePath(base + QStringLiteral(".csv")),
                                                      tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!exportTableCsv(path, &error))
        reportError(tr("Export Table"), error);
    else
        report(tr("Exported the table to %1").arg(QDir::toNativeSeparators(path)));
}

void TouchstoneEditor::onReferences()
{
    ReferenceImpedanceDialog dialog(m_network.z0, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    if (!setReferenceImpedances(dialog.impedances(), &error))
        reportError(tr("Reference Impedances"), error);
}

void TouchstoneEditor::onRenumber()
{
    RenumberPortsDialog dialog(m_network.portCount(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    if (!renumberPorts(dialog.order(), &error))
        reportError(tr("Renumber Ports"), error);
}

void TouchstoneEditor::onResample()
{
    if (m_network.frequencies.empty())
        return;
    ResampleDialog dialog(m_network.frequencies.front(), m_network.frequencies.back(), m_network.frequencyCount(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    const std::optional<std::vector<double>> grid = dialog.frequencies(&error);
    QString warning;
    if (!grid || !resample(*grid, dialog.extrapolation(), &error, &warning))
        reportError(tr("Resample"), error);
}

void TouchstoneEditor::onCrop()
{
    if (m_network.frequencies.empty())
        return;
    CropDialog dialog(m_network.frequencies.front(), m_network.frequencies.back(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    const auto range = dialog.range(&error);
    if (!range || !cropFrequencies(range->first, range->second, &error))
        reportError(tr("Crop"), error);
}

void TouchstoneEditor::onCascade()
{
    CascadeDialog dialog(directory(), this);
    if (dialog.exec() != QDialog::Accepted || dialog.path().isEmpty())
        return;
    QString error;
    const std::optional<rf::NetworkData> other = readTwoPort(dialog.path(), &error);
    if (!other || !cascadeWith(*other, dialog.otherAfter(), &error))
        reportError(tr("Cascade"), error);
}

void TouchstoneEditor::onDeembed()
{
    DeembedDialog dialog(directory(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    std::optional<rf::NetworkData> left, right;
    if (!dialog.leftPath().isEmpty()) {
        left = readTwoPort(dialog.leftPath(), &error);
        if (!left) {
            reportError(tr("De-embed"), error);
            return;
        }
    }
    if (!dialog.rightPath().isEmpty()) {
        right = readTwoPort(dialog.rightPath(), &error);
        if (!right) {
            reportError(tr("De-embed"), error);
            return;
        }
    }
    if (!deembed(left, right, &error))
        reportError(tr("De-embed"), error);
}

void TouchstoneEditor::onCheck()
{
    const SParamCheckReport result = checkPassivityAndReciprocity();
    if (result.passive() && result.reciprocal())
        QMessageBox::information(this, tr("Passivity and Reciprocity"), result.text);
    else
        QMessageBox::warning(this, tr("Passivity and Reciprocity"),
                             result.text + QStringLiteral("\n\n")
                                 + tr("The failing frequencies are highlighted on the plots. Nothing was changed."));
}

} // namespace lvd
