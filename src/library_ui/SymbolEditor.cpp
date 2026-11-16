#include "library_ui/SymbolEditor.h"

#include "core/Expression.h"
#include "library_ui/LibraryTargetDialog.h"
#include "library_ui/LibraryUiCommon.h"
#include "library_ui/SymbolPreview.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringListModel>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lvd {

namespace {

enum ParameterColumn { NameColumn, LabelColumn, UnitColumn, DefaultColumn, ShownColumn, KindColumn, ChoicesColumn, ColumnCount };
enum ParameterKind { ExpressionKind, ChoicesKind, FileKind, TextKind };

const QStringList &modelKinds()
{
    static const QStringList kinds{QStringLiteral("rf-behavioural"), QStringLiteral("rf-element"),
                                   QStringLiteral("sparameters"),    QStringLiteral("spice"),
                                   QStringLiteral("subschematic"),   QStringLiteral("python")};
    return kinds;
}

QString modelHint(const QString &kind)
{
    if (kind == QLatin1String("rf-behavioural"))
        return SymbolEditor::tr("Budget, spur and linear RF analyses. Field \"block\": a built-in system block type "
                                "(amplifier, filter, mixer, ...); its parameters are read by name.");
    if (kind == QLatin1String("rf-element"))
        return SymbolEditor::tr("Linear RF analysis. Field \"element\": a built-in circuit element type (resistor, "
                                "tline, microstrip, ...).");
    if (kind == QLatin1String("sparameters"))
        return SymbolEditor::tr("Linear RF analysis. Field \"file\": Touchstone path, usually \"{file}\" taken from a "
                                "file parameter.");
    if (kind == QLatin1String("spice"))
        return SymbolEditor::tr("SPICE. Field \"template\": netlist line with {ref}, {pin:<name>} and {<parameter>} "
                                "placeholders; optional \"include\" (file) or \"subcircuit\" (text).");
    if (kind == QLatin1String("subschematic"))
        return SymbolEditor::tr("All analyses. Field \"document\": a schematic whose sheet ports match the pin names.");
    if (kind == QLatin1String("python"))
        return SymbolEditor::tr("Linear RF analysis and SPICE. Field \"source\" or \"file\": code defining the model "
                                "class.");
    return SymbolEditor::tr("Unknown model kind: analyses that do not understand it skip this model.");
}

QJsonObject defaultModelData(const QString &kind, const ComponentDefinition &definition)
{
    QJsonObject data{{QStringLiteral("kind"), kind}};
    if (kind == QLatin1String("rf-behavioural")) {
        data.insert(QStringLiteral("block"), definition.id);
    } else if (kind == QLatin1String("rf-element")) {
        data.insert(QStringLiteral("element"), definition.id);
    } else if (kind == QLatin1String("sparameters")) {
        data.insert(QStringLiteral("file"), QStringLiteral("{file}"));
    } else if (kind == QLatin1String("spice")) {
        QString line = QStringLiteral("X{ref}");
        for (const QString &pin : definition.pins)
            line += QStringLiteral(" {pin:%1}").arg(pin);
        data.insert(QStringLiteral("template"), line + QLatin1Char(' ') + definition.id);
    } else if (kind == QLatin1String("subschematic")) {
        data.insert(QStringLiteral("document"), QString());
    } else if (kind == QLatin1String("python")) {
        data.insert(QStringLiteral("source"), QString());
    }
    return data;
}

QString jsonValueText(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::String: return value.toString();
    case QJsonValue::Bool: return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double: return QString::number(value.toDouble(), 'g', 15);
    case QJsonValue::Array: return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    case QJsonValue::Object: return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    default: return {};
    }
}

// Text typed into a field becomes a string, unless the field held another JSON
// type before and the text still parses as JSON.
QJsonValue jsonValueFromText(const QString &text, const QJsonValue &original)
{
    if (original.isUndefined() || original.isString() || original.isNull())
        return text;
    const QJsonDocument document = QJsonDocument::fromJson(QStringLiteral("[%1]").arg(text).toUtf8());
    if (document.isArray() && document.array().size() == 1)
        return document.array().at(0);
    return text;
}

int mergeIdFor(const QString &key)
{
    if (key.isEmpty())
        return -1;
    return static_cast<int>(qHash(key) & 0x3fffffff) + 1;
}

ComponentDefinition templateDefinition()
{
    ComponentDefinition definition;
    definition.id = QStringLiteral("new_component");
    definition.name = SymbolEditor::tr("New component");
    definition.category = QStringLiteral("Custom");
    definition.referencePrefix = QStringLiteral("U");
    definition.pins = {QStringLiteral("in"), QStringLiteral("out")};
    SymbolGraphic body;
    body.type = SymbolGraphic::Type::Rect;
    body.from = QPoint(-20, -20);
    body.to = QPoint(20, 20);
    body.fill = Fill::Background;
    definition.symbol.graphics.append(body);
    definition.symbol.pins.append(SymbolPin{QStringLiteral("in"), QPoint(-30, 0), 10, PinDirection::Right});
    definition.symbol.pins.append(SymbolPin{QStringLiteral("out"), QPoint(30, 0), 10, PinDirection::Left});
    definition.symbol.referenceAt = QPoint(0, -30);
    definition.symbol.valueAt = QPoint(0, 30);
    return definition;
}

int parameterKind(const ParameterDefinition &parameter)
{
    if (!parameter.choices.isEmpty())
        return ChoicesKind;
    if (parameter.isFile)
        return FileKind;
    if (parameter.isText)
        return TextKind;
    return ExpressionKind;
}

void setTextIfChanged(QLineEdit *edit, const QString &text)
{
    if (edit->text() != text) {
        const QSignalBlocker blocker(edit);
        edit->setText(text);
    }
}

QString graphicTypeName(SymbolGraphic::Type type)
{
    switch (type) {
    case SymbolGraphic::Type::Line: return SymbolEditor::tr("Line");
    case SymbolGraphic::Type::Polygon: return SymbolEditor::tr("Polygon");
    case SymbolGraphic::Type::Rect: return SymbolEditor::tr("Rectangle");
    case SymbolGraphic::Type::Circle: return SymbolEditor::tr("Circle");
    case SymbolGraphic::Type::Arc: return SymbolEditor::tr("Arc");
    case SymbolGraphic::Type::Text: return SymbolEditor::tr("Text");
    }
    return {};
}

QIcon toolIcon(SymbolCanvas::Tool tool, const QColor &color, const QColor &accent)
{
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(2, 2);
    QPen pen(color, 1.8);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    switch (tool) {
    case SymbolCanvas::Tool::Select: {
        QPainterPath arrow;
        arrow.moveTo(7, 4);
        arrow.lineTo(7, 19);
        arrow.lineTo(11, 15);
        arrow.lineTo(14, 21);
        arrow.lineTo(16.5, 20);
        arrow.lineTo(13.5, 14);
        arrow.lineTo(19, 14);
        arrow.closeSubpath();
        painter.setBrush(color);
        painter.drawPath(arrow);
        break;
    }
    case SymbolCanvas::Tool::Line:
        painter.drawPolyline(QPolygonF({QPointF(3, 19), QPointF(9, 7), QPointF(15, 17), QPointF(21, 5)}));
        break;
    case SymbolCanvas::Tool::Polygon:
        painter.drawPolygon(QPolygonF({QPointF(4, 19), QPointF(12, 4), QPointF(20, 19)}));
        break;
    case SymbolCanvas::Tool::Rect:
        painter.drawRect(QRectF(4, 6, 16, 12));
        break;
    case SymbolCanvas::Tool::Circle:
        painter.drawEllipse(QPointF(12, 12), 8, 8);
        break;
    case SymbolCanvas::Tool::Arc:
        painter.drawArc(QRectF(4, 6, 16, 16), 0, 180 * 16);
        painter.setPen(QPen(accent, 1.2));
        painter.drawPoint(QPointF(12, 14));
        break;
    case SymbolCanvas::Tool::Text: {
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(17);
        painter.setFont(font);
        painter.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, QStringLiteral("T"));
        break;
    }
    case SymbolCanvas::Tool::Pin:
        painter.drawLine(QPointF(4, 12), QPointF(15, 12));
        painter.drawRect(QRectF(15, 6, 6, 12));
        painter.setPen(QPen(accent, 1.4));
        painter.drawEllipse(QPointF(4, 12), 2.2, 2.2);
        break;
    }
    painter.end();
    return QIcon(pixmap);
}

} // namespace

class DefinitionCommand : public QUndoCommand
{
public:
    DefinitionCommand(SymbolEditor *editor, ComponentDefinition before, ComponentDefinition after, const QString &text,
                      int mergeId)
        : QUndoCommand(text), m_editor(editor), m_before(std::move(before)), m_after(std::move(after)), m_mergeId(mergeId)
    {
    }

    void undo() override { m_editor->applyDefinition(m_before); }
    void redo() override { m_editor->applyDefinition(m_after); }
    int id() const override { return m_mergeId; }

    bool mergeWith(const QUndoCommand *other) override
    {
        const auto *command = static_cast<const DefinitionCommand *>(other);
        if (command->m_mergeId != m_mergeId)
            return false;
        m_after = command->m_after;
        setObsolete(m_after == m_before);
        return true;
    }

private:
    SymbolEditor *m_editor;
    ComponentDefinition m_before;
    ComponentDefinition m_after;
    int m_mergeId;
};

SymbolEditor::SymbolEditor(LibraryManager *libraries, QWidget *parent)
    : QMainWindow(parent), m_libraries(libraries)
{
    setWindowFlag(Qt::Window);
    setObjectName(QStringLiteral("SymbolEditor"));
    m_undo = new QUndoStack(this);

    m_canvas = new SymbolCanvas(this);
    m_canvas->setFrameShape(QFrame::NoFrame);

    m_banner = new QFrame(this);
    m_banner->setAutoFillBackground(true);
    m_banner->setBackgroundRole(QPalette::AlternateBase);
    static_cast<QFrame *>(m_banner)->setFrameShape(QFrame::StyledPanel);
    m_bannerText = new QLabel(m_banner);
    m_bannerText->setWordWrap(true);
    m_bannerButton = new QPushButton(tr("Save a Copy..."), m_banner);
    auto *bannerIcon = new QLabel(m_banner);
    bannerIcon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(24, 24));
    auto *bannerLayout = new QHBoxLayout(m_banner);
    bannerLayout->setContentsMargins(8, 4, 8, 4);
    bannerLayout->addWidget(bannerIcon, 0, Qt::AlignTop);
    bannerLayout->addWidget(m_bannerText, 1);
    bannerLayout->addWidget(m_bannerButton, 0, Qt::AlignVCenter);
    connect(m_bannerButton, &QPushButton::clicked, this, &SymbolEditor::saveCopyInteractive);

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(m_banner);
    centralLayout->addWidget(m_canvas, 1);
    setCentralWidget(central);

    createDocks();
    createActions();

    m_cursorLabel = new QLabel(this);
    m_cursorLabel->setMinimumWidth(160);
    statusBar()->addPermanentWidget(m_cursorLabel);

    connect(m_canvas, &SymbolCanvas::definitionEdited, this,
            [this](const ComponentDefinition &definition, const QString &description) {
                if (m_applying || definition == m_definition)
                    return;
                m_undo->push(new DefinitionCommand(this, m_definition, definition, description, -1));
            });
    connect(m_canvas, &SymbolCanvas::selectionChanged, this, [this] {
        rebuildProperties();
        // Follow the canvas in the pin list.
        const auto selection = m_canvas->selection();
        if (selection.size() == 1 && selection.first().kind == ElementRef::Kind::Pin) {
            const int row = static_cast<int>(m_definition.pins.indexOf(m_definition.symbol.pins.at(selection.first().index).pin));
            const QSignalBlocker blocker(m_pinList);
            m_pinList->setCurrentRow(row);
        }
        const bool any = !selection.isEmpty();
        m_deleteAction->setEnabled(any);
        m_duplicateAction->setEnabled(any);
    });
    connect(m_canvas, &SymbolCanvas::elementCreated, this, [this](const ElementRef &element) {
        if (element.kind == ElementRef::Kind::Graphic
            && m_definition.symbol.graphics.at(element.index).type == SymbolGraphic::Type::Text && m_firstTextField) {
            m_firstTextField->setFocus();
            m_firstTextField->selectAll();
        }
    });
    connect(m_canvas, &SymbolCanvas::cursorMoved, this, [this](QPointF point) {
        m_cursorLabel->setText(tr("x %1, y %2   zoom %3%")
                                   .arg(qRound(point.x()))
                                   .arg(qRound(point.y()))
                                   .arg(qRound(m_canvas->zoom() * 25)));
    });
    connect(m_undo, &QUndoStack::cleanChanged, this, &SymbolEditor::updateTitle);
    connect(m_libraries, &LibraryManager::librariesChanged, this, [this] {
        updateCategoryCompletion();
        updateBanner();
        revalidate();
    });

    updateCategoryCompletion();
    resetDefinition(templateDefinition());
    resize(1280, 820);
}

SymbolEditor::~SymbolEditor()
{
    // Commands refer back to the editor; drop them before the widgets go.
    m_undo->disconnect(this);
    m_undo->clear();
}

// --- construction ---

void SymbolEditor::createActions()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    m_saveAction = fileMenu->addAction(tr("&Save to Library"), this, [this] {
        if (isReadOnlySource()) {
            saveCopyInteractive();
            return;
        }
        QString error;
        if (!save(&error))
            QMessageBox::warning(this, tr("Save Component"), error);
    });
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_saveCopyAction = fileMenu->addAction(tr("Save a &Copy..."), this, &SymbolEditor::saveCopyInteractive);
    fileMenu->addSeparator();
    QAction *closeAction = fileMenu->addAction(tr("&Close"), this, &QWidget::close);
    closeAction->setShortcut(QKeySequence::Close);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    QAction *undoAction = m_undo->createUndoAction(this, tr("&Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    undoAction->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    QAction *redoAction = m_undo->createRedoAction(this, tr("&Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
    redoAction->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    editMenu->addAction(undoAction);
    editMenu->addAction(redoAction);
    editMenu->addSeparator();
    m_deleteAction = editMenu->addAction(tr("&Delete"), m_canvas, &SymbolCanvas::deleteSelection);
    m_deleteAction->setShortcut(QKeySequence::Delete);
    m_deleteAction->setShortcutContext(Qt::WidgetShortcut);
    m_deleteAction->setEnabled(false);
    m_canvas->addAction(m_deleteAction);
    m_duplicateAction = editMenu->addAction(tr("D&uplicate"), m_canvas, &SymbolCanvas::duplicateSelection);
    m_duplicateAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    m_duplicateAction->setEnabled(false);
    QAction *selectAll = editMenu->addAction(tr("Select &All"), m_canvas, &SymbolCanvas::selectAll);
    selectAll->setShortcut(QKeySequence::SelectAll);
    selectAll->setShortcutContext(Qt::WidgetShortcut);
    m_canvas->addAction(selectAll);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    QAction *fit = viewMenu->addAction(tr("Zoom to &Fit"), m_canvas, &SymbolCanvas::zoomToFit);
    fit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    viewMenu->addSeparator();
    for (QDockWidget *dock : findChildren<QDockWidget *>())
        viewMenu->addAction(dock->toggleViewAction());

    QToolBar *toolBar = addToolBar(tr("Tools"));
    toolBar->setObjectName(QStringLiteral("tools"));
    toolBar->setMovable(false);
    toolBar->setIconSize(QSize(22, 22));
    m_toolGroup = new QActionGroup(this);
    const QList<std::tuple<SymbolCanvas::Tool, QString, QKeySequence>> tools = {
        {SymbolCanvas::Tool::Select, tr("Select and move (S)"), QKeySequence(Qt::Key_S)},
        {SymbolCanvas::Tool::Line, tr("Line or polyline (L): click points, double-click or Enter to finish"),
         QKeySequence(Qt::Key_L)},
        {SymbolCanvas::Tool::Polygon, tr("Polygon (G): click points, double-click or Enter to close"),
         QKeySequence(Qt::Key_G)},
        {SymbolCanvas::Tool::Rect, tr("Rectangle (R): drag, or click two corners"), QKeySequence(Qt::Key_R)},
        {SymbolCanvas::Tool::Circle, tr("Circle (C): drag from the centre"), QKeySequence(Qt::Key_C)},
        {SymbolCanvas::Tool::Arc, tr("Arc (A): click the centre, the start and the end (counter-clockwise)"),
         QKeySequence(Qt::Key_A)},
        {SymbolCanvas::Tool::Text, tr("Text (T)"), QKeySequence(Qt::Key_T)},
        {SymbolCanvas::Tool::Pin, tr("Pin (P): click a connection point on the 10-unit grid"), QKeySequence(Qt::Key_P)},
    };
    for (const auto &[tool, tip, key] : tools) {
        auto *action = new QAction(tip.section(QLatin1Char(' '), 0, 0), this);
        action->setToolTip(tip);
        action->setCheckable(true);
        action->setShortcut(key);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        m_canvas->addAction(action);
        m_toolGroup->addAction(action);
        toolBar->addAction(action);
        const SymbolCanvas::Tool selected = tool;
        connect(action, &QAction::triggered, this, [this, selected] {
            m_canvas->setTool(selected);
            m_canvas->setFocus();
        });
        m_toolActions.append({tool, action});
    }
    m_toolActions.first().second->setChecked(true);
    connect(m_canvas, &SymbolCanvas::toolChanged, this, [this](SymbolCanvas::Tool tool) {
        for (const auto &[candidate, action] : std::as_const(m_toolActions)) {
            if (candidate == tool)
                action->setChecked(true);
        }
    });
    updateToolIcons();

    toolBar->addSeparator();
    auto *gridLabel = new QLabel(tr(" Grid "), toolBar);
    toolBar->addWidget(gridLabel);
    m_gridCombo = new QComboBox(toolBar);
    for (int grid : {1, 5, 10})
        m_gridCombo->addItem(QString::number(grid), grid);
    m_gridCombo->setCurrentIndex(1);
    m_gridCombo->setToolTip(tr("Snap grid for graphics, in units. Pins always snap to the 10-unit grid."));
    connect(m_gridCombo, &QComboBox::currentIndexChanged, this,
            [this] { m_canvas->setGraphicsGrid(m_gridCombo->currentData().toInt()); });
    toolBar->addWidget(m_gridCombo);
    toolBar->addSeparator();
    toolBar->addAction(undoAction);
    toolBar->addAction(redoAction);
    toolBar->addAction(fit);
    toolBar->addSeparator();
    toolBar->addAction(m_saveAction);
}

void SymbolEditor::updateToolIcons()
{
    const QColor color = palette().color(QPalette::WindowText);
    const QColor accent = SchematicStyle::fromPalette(palette()).selection;
    for (const auto &[tool, action] : std::as_const(m_toolActions))
        action->setIcon(toolIcon(tool, color, accent));
}

void SymbolEditor::createDocks()
{
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);

    // Selected element.
    auto *propertyScroll = new QScrollArea(this);
    propertyScroll->setWidgetResizable(true);
    propertyScroll->setFrameShape(QFrame::NoFrame);
    propertyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_propertyHost = new QWidget(propertyScroll);
    // Follow the dock width instead of growing to the widest row.
    m_propertyHost->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *hostLayout = new QVBoxLayout(m_propertyHost);
    hostLayout->setContentsMargins(6, 6, 6, 6);
    hostLayout->addStretch(1);
    propertyScroll->setWidget(m_propertyHost);
    auto *propertyDock = new QDockWidget(tr("Selection"), this);
    propertyDock->setObjectName(QStringLiteral("selection"));
    propertyDock->setWidget(propertyScroll);
    propertyDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    propertyDock->setMinimumWidth(250);
    addDockWidget(Qt::LeftDockWidgetArea, propertyDock);

    // Component data.
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("componentTabs"));
    createGeneralTab(m_tabs);
    createPinsTab(m_tabs);
    createParametersTab(m_tabs);
    createModelsTab(m_tabs);
    auto *componentDock = new QDockWidget(tr("Component"), this);
    componentDock->setObjectName(QStringLiteral("component"));
    componentDock->setWidget(m_tabs);
    componentDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    componentDock->setMinimumWidth(440);
    addDockWidget(Qt::RightDockWidgetArea, componentDock);

    // Orientation preview.
    m_orientationPreview = new SymbolPreview(this);
    m_orientationPreview->setViews({{Orientation{0, false}, tr("0°")},
                                    {Orientation{90, false}, tr("90°")},
                                    {Orientation{180, false}, tr("180°")},
                                    {Orientation{0, true}, tr("mirrored")}});
    m_orientationPreview->setMaximumScale(2.5);
    m_orientationPreview->setMinimumHeight(120);
    m_orientationPreview->setToolTip(tr("The symbol as placed in other orientations: pin names must stay readable and "
                                        "pins must point away from the body."));
    auto *previewDock = new QDockWidget(tr("Orientations"), this);
    previewDock->setObjectName(QStringLiteral("orientations"));
    previewDock->setWidget(m_orientationPreview);
    previewDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, previewDock);

    // Problems.
    m_problemList = new QListWidget(this);
    m_problemList->setWordWrap(true);
    m_problemList->setToolTip(tr("Click a problem to go to it."));
    connect(m_problemList, &QListWidget::itemClicked, this, &SymbolEditor::showProblem);
    connect(m_problemList, &QListWidget::itemActivated, this, &SymbolEditor::showProblem);
    auto *problemDock = new QDockWidget(tr("Problems"), this);
    problemDock->setObjectName(QStringLiteral("problems"));
    problemDock->setWidget(m_problemList);
    problemDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, problemDock);
    splitDockWidget(previewDock, problemDock, Qt::Horizontal);
    resizeDocks({previewDock, problemDock}, {3, 2}, Qt::Horizontal);
    resizeDocks({previewDock}, {150}, Qt::Vertical);
}

void SymbolEditor::createGeneralTab(QTabWidget *tabs)
{
    auto *page = new QWidget(tabs);
    auto *form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_targetLabel = new QLabel(page);
    m_targetLabel->setWordWrap(true);
    m_targetLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Library:"), m_targetLabel);

    const auto lineEdit = [&](const QString &label, const QString &tip, const QString &placeholder) {
        auto *edit = new QLineEdit(page);
        edit->setToolTip(tip);
        edit->setPlaceholderText(placeholder);
        form->addRow(label, edit);
        return edit;
    };
    m_idEdit = lineEdit(tr("Id:"), tr("Identifier unique in the library: a-z, 0-9 and _. Schematics refer to the "
                                      "component as library:id, so changing it breaks existing references."),
                        QStringLiteral("my_amplifier"));
    m_nameEdit = lineEdit(tr("Name:"), tr("Display name"), QString());
    m_categoryEdit = lineEdit(tr("Category:"), tr("Slash-separated path in the library browser, such as RF/Amplifiers"),
                              QStringLiteral("RF/Amplifiers"));
    m_categoryCompleter = new QCompleter(this);
    m_categoryCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_categoryCompleter->setFilterMode(Qt::MatchContains);
    m_categoryCompleter->setModel(new QStringListModel(m_categoryCompleter));
    m_categoryEdit->setCompleter(m_categoryCompleter);
    m_descriptionEdit = lineEdit(tr("Description:"), tr("One sentence"), QString());
    m_prefixEdit = lineEdit(tr("Reference prefix:"), tr("Letters for references: A gives A1, A2, ..."), QStringLiteral("U"));
    m_netNameEdit = lineEdit(tr("Global net:"), tr("Global net that the single pin connects to in every sheet (ground: 0). "
                                                   "Leave empty for normal components."),
                             tr("none"));
    m_roleCombo = new QComboBox(page);
    m_roleCombo->addItem(tr("Normal component"), QString());
    m_roleCombo->addItem(tr("Sheet port (hierarchical port)"), ComponentDefinition::sheetPortRole());
    m_roleCombo->setToolTip(tr("A sheet port needs exactly one pin and a \"name\" parameter."));
    form->addRow(tr("Role:"), m_roleCombo);

    const auto bind = [this](QLineEdit *field, const QString &key, const QString &description,
                             void (*apply)(ComponentDefinition &, const QString &)) {
        connect(field, &QLineEdit::textEdited, this, [this, apply, key, description](const QString &text) {
            edit([apply, text](ComponentDefinition &definition) { apply(definition, text); }, description,
                 QStringLiteral("general:") + key);
        });
    };
    bind(m_idEdit, QStringLiteral("id"), tr("Change id"), [](ComponentDefinition &d, const QString &t) { d.id = t; });
    bind(m_nameEdit, QStringLiteral("name"), tr("Change name"), [](ComponentDefinition &d, const QString &t) { d.name = t; });
    bind(m_categoryEdit, QStringLiteral("category"), tr("Change category"),
         [](ComponentDefinition &d, const QString &t) { d.category = t; });
    bind(m_descriptionEdit, QStringLiteral("description"), tr("Change description"),
         [](ComponentDefinition &d, const QString &t) { d.description = t; });
    bind(m_prefixEdit, QStringLiteral("prefix"), tr("Change reference prefix"),
         [](ComponentDefinition &d, const QString &t) { d.referencePrefix = t; });
    bind(m_netNameEdit, QStringLiteral("netName"), tr("Change global net"),
         [](ComponentDefinition &d, const QString &t) { d.netName = t; });
    // The completer sets the text without textEdited.
    connect(m_categoryCompleter, qOverload<const QString &>(&QCompleter::activated), this, [this](const QString &text) {
        edit([text](ComponentDefinition &definition) { definition.category = text; }, tr("Change category"),
             QStringLiteral("general:category"));
    });
    connect(m_roleCombo, &QComboBox::currentIndexChanged, this, [this] {
        if (m_applying)
            return;
        const QString role = m_roleCombo->currentData().toString();
        edit([role](ComponentDefinition &definition) { definition.role = role; }, tr("Change role"));
    });

    tabs->addTab(page, tr("General"));
}

void SymbolEditor::createPinsTab(QTabWidget *tabs)
{
    auto *page = new QWidget(tabs);
    auto *layout = new QVBoxLayout(page);
    auto *hint = new QLabel(tr("The order is the netlist and S-parameter port order. Drag to reorder, double-click to "
                               "rename; the symbol pin is renamed with it."),
                            page);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_pinList = new QListWidget(page);
    m_pinList->setDragDropMode(QAbstractItemView::InternalMove);
    m_pinList->setDefaultDropAction(Qt::MoveAction);
    m_pinList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    layout->addWidget(m_pinList, 1);

    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add"), page);
    auto *remove = new QPushButton(tr("Remove"), page);
    auto *up = new QPushButton(tr("Move Up"), page);
    auto *down = new QPushButton(tr("Move Down"), page);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    buttons->addWidget(up);
    buttons->addWidget(down);
    layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, &SymbolEditor::addPin);
    connect(remove, &QPushButton::clicked, this, &SymbolEditor::removeSelectedPin);
    connect(up, &QPushButton::clicked, this, [this] { movePin(-1); });
    connect(down, &QPushButton::clicked, this, [this] { movePin(1); });
    connect(m_pinList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (m_applying)
            return;
        const int row = m_pinList->row(item);
        if (row < 0 || row >= m_definition.pins.size())
            return;
        renamePin(m_definition.pins.at(row), item->text());
    });
    connect(m_pinList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_applying || row < 0 || row >= m_definition.pins.size())
            return;
        const QString name = m_definition.pins.at(row);
        for (int i = 0; i < m_definition.symbol.pins.size(); ++i) {
            if (m_definition.symbol.pins.at(i).pin == name)
                m_canvas->setSelection({ElementRef{ElementRef::Kind::Pin, i}});
        }
    });
    // Drag and drop reorders the rows; read the new order once the view is done.
    const auto scheduleReorder = [this] {
        if (!m_applying)
            QTimer::singleShot(0, this, &SymbolEditor::onPinRowsMoved);
    };
    connect(m_pinList->model(), &QAbstractItemModel::rowsMoved, this, scheduleReorder);
    connect(m_pinList->model(), &QAbstractItemModel::rowsInserted, this, scheduleReorder);
    connect(m_pinList->model(), &QAbstractItemModel::rowsRemoved, this, scheduleReorder);

    tabs->addTab(page, tr("Pins"));
}

void SymbolEditor::createParametersTab(QTabWidget *tabs)
{
    auto *page = new QWidget(tabs);
    auto *layout = new QVBoxLayout(page);
    m_parameterTable = new QTableWidget(0, ColumnCount, page);
    m_parameterTable->setHorizontalHeaderLabels(
        {tr("Name"), tr("Label"), tr("Unit"), tr("Default"), tr("Shown"), tr("Kind"), tr("Choices")});
    m_parameterTable->horizontalHeaderItem(NameColumn)->setToolTip(tr("Identifier used in expressions and models"));
    m_parameterTable->horizontalHeaderItem(DefaultColumn)->setToolTip(tr("Expression text, choice, file path or text"));
    m_parameterTable->horizontalHeaderItem(ShownColumn)->setToolTip(tr("Drawn next to the placed symbol"));
    m_parameterTable->horizontalHeaderItem(ChoicesColumn)->setToolTip(tr("Comma-separated allowed values"));
    m_parameterTable->verticalHeader()->setVisible(false);
    m_parameterTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_parameterTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_parameterTable->horizontalHeader()->setStretchLastSection(true);
    m_parameterTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_parameterTable->setColumnWidth(NameColumn, 80);
    m_parameterTable->setColumnWidth(LabelColumn, 110);
    m_parameterTable->setColumnWidth(UnitColumn, 45);
    m_parameterTable->setColumnWidth(DefaultColumn, 70);
    m_parameterTable->setColumnWidth(ShownColumn, 48);
    m_parameterTable->setColumnWidth(KindColumn, 92);
    layout->addWidget(m_parameterTable, 1);

    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add"), page);
    auto *remove = new QPushButton(tr("Remove"), page);
    auto *up = new QPushButton(tr("Move Up"), page);
    auto *down = new QPushButton(tr("Move Down"), page);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    buttons->addWidget(up);
    buttons->addWidget(down);
    layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, &SymbolEditor::addParameter);
    connect(remove, &QPushButton::clicked, this, &SymbolEditor::removeSelectedParameter);
    connect(up, &QPushButton::clicked, this, [this] { moveParameter(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveParameter(1); });
    connect(m_parameterTable, &QTableWidget::cellChanged, this, &SymbolEditor::onParameterItemChanged);

    tabs->addTab(page, tr("Parameters"));
}

void SymbolEditor::createModelsTab(QTabWidget *tabs)
{
    auto *page = new QWidget(tabs);
    auto *layout = new QVBoxLayout(page);

    auto *listRow = new QHBoxLayout;
    m_modelList = new QListWidget(page);
    m_modelList->setMaximumHeight(110);
    m_modelList->setToolTip(tr("An analysis uses the first model it understands."));
    listRow->addWidget(m_modelList, 1);
    auto *listButtons = new QVBoxLayout;
    auto *add = new QToolButton(page);
    add->setText(tr("Add"));
    add->setPopupMode(QToolButton::InstantPopup);
    add->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto *addMenu = new QMenu(add);
    for (const QString &kind : modelKinds())
        addMenu->addAction(kind, this, [this, kind] { addModel(kind); });
    add->setMenu(addMenu);
    add->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *remove = new QPushButton(tr("Remove"), page);
    auto *up = new QPushButton(tr("Up"), page);
    auto *down = new QPushButton(tr("Down"), page);
    listButtons->addWidget(add);
    listButtons->addWidget(remove);
    listButtons->addWidget(up);
    listButtons->addWidget(down);
    listButtons->addStretch(1);
    listRow->addLayout(listButtons);
    layout->addLayout(listRow);

    auto *kindRow = new QFormLayout;
    m_modelKind = new QComboBox(page);
    m_modelKind->setEditable(true);
    m_modelKind->addItems(modelKinds());
    kindRow->addRow(tr("Kind:"), m_modelKind);
    layout->addLayout(kindRow);
    m_modelHint = new QLabel(page);
    m_modelHint->setWordWrap(true);
    m_modelHint->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(m_modelHint);

    m_modelTabs = new QTabWidget(page);
    auto *fieldsPage = new QWidget(m_modelTabs);
    auto *fieldsLayout = new QVBoxLayout(fieldsPage);
    fieldsLayout->setContentsMargins(0, 4, 0, 0);
    m_modelFields = new QTableWidget(0, 2, fieldsPage);
    m_modelFields->setHorizontalHeaderLabels({tr("Field"), tr("Value")});
    m_modelFields->verticalHeader()->setVisible(false);
    m_modelFields->horizontalHeader()->setStretchLastSection(true);
    m_modelFields->setColumnWidth(0, 110);
    m_modelFields->setSelectionBehavior(QAbstractItemView::SelectRows);
    fieldsLayout->addWidget(m_modelFields, 1);
    auto *fieldButtons = new QHBoxLayout;
    auto *addField = new QPushButton(tr("Add Field"), fieldsPage);
    auto *removeField = new QPushButton(tr("Remove Field"), fieldsPage);
    fieldButtons->addWidget(addField);
    fieldButtons->addWidget(removeField);
    fieldButtons->addStretch(1);
    fieldsLayout->addLayout(fieldButtons);
    m_modelTabs->addTab(fieldsPage, tr("Fields"));

    auto *jsonPage = new QWidget(m_modelTabs);
    auto *jsonLayout = new QVBoxLayout(jsonPage);
    jsonLayout->setContentsMargins(0, 4, 0, 0);
    m_modelJson = new QPlainTextEdit(jsonPage);
    m_modelJson->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_modelJson->setToolTip(tr("The whole model object as stored in the library file (advanced)."));
    jsonLayout->addWidget(m_modelJson, 1);
    auto *applyJson = new QPushButton(tr("Apply JSON"), jsonPage);
    auto *jsonButtons = new QHBoxLayout;
    jsonButtons->addStretch(1);
    jsonButtons->addWidget(applyJson);
    jsonLayout->addLayout(jsonButtons);
    m_modelTabs->addTab(jsonPage, tr("JSON"));
    layout->addWidget(m_modelTabs, 1);

    connect(remove, &QPushButton::clicked, this, &SymbolEditor::removeSelectedModel);
    connect(up, &QPushButton::clicked, this, [this] { moveModel(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveModel(1); });
    connect(m_modelList, &QListWidget::currentRowChanged, this, [this] {
        if (!m_applying)
            refreshModelFields();
    });
    connect(m_modelKind, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        const int row = currentModelRow();
        if (m_applying || row < 0)
            return;
        edit(
            [row, text](ComponentDefinition &definition) {
                definition.models[row].kind = text.trimmed();
                definition.models[row].data.insert(QStringLiteral("kind"), text.trimmed());
            },
            tr("Change model kind"), QStringLiteral("model:kind:%1").arg(row));
    });
    connect(m_modelFields, &QTableWidget::cellChanged, this, &SymbolEditor::onModelFieldChanged);
    connect(addField, &QPushButton::clicked, this, [this] {
        const int row = currentModelRow();
        if (row < 0)
            return;
        edit(
            [row](ComponentDefinition &definition) {
                QJsonObject &data = definition.models[row].data;
                QString key = QStringLiteral("field");
                for (int n = 2; data.contains(key); ++n)
                    key = QStringLiteral("field%1").arg(n);
                data.insert(key, QString());
            },
            tr("Add model field"));
    });
    connect(removeField, &QPushButton::clicked, this, [this] {
        const int row = currentModelRow();
        const int field = m_modelFields->currentRow();
        if (row < 0 || field < 0 || !m_modelFields->item(field, 0))
            return;
        const QString key = m_modelFields->item(field, 0)->data(Qt::UserRole).toString();
        edit([row, key](ComponentDefinition &definition) { definition.models[row].data.remove(key); },
             tr("Remove model field"));
    });
    connect(applyJson, &QPushButton::clicked, this, &SymbolEditor::applyModelJson);

    tabs->addTab(page, tr("Models"));
}

// --- opening and saving ---

void SymbolEditor::resetDefinition(const ComponentDefinition &definition)
{
    m_undo->clear();
    m_canvas->setSelection({});
    applyDefinition(definition);
    m_undo->setClean();
    m_canvas->zoomToFit();
    updateBanner();
    updateTitle();
}

void SymbolEditor::newComponent(const QString &libraryName, LibraryScope scope)
{
    m_targetLibrary = libraryName;
    m_targetScope = scope;
    m_sourceKey.clear();
    m_originalId.clear();
    m_neverSaved = true;

    ComponentDefinition definition = templateDefinition();
    if (const LibraryManager::Entry *entry = entryInScope(*m_libraries, libraryName, scope)) {
        for (int n = 2; entry->library.component(definition.id); ++n)
            definition.id = QStringLiteral("new_component_%1").arg(n);
    }
    resetDefinition(definition);
}

bool SymbolEditor::newComponentInteractive(const QString &preferredLibrary)
{
    LibraryTargetDialog dialog(m_libraries, LibraryTargetDialog::Mode::ExistingOrNew, this);
    dialog.setExplanation(tr("Choose the library the new component is saved into."));
    if (!preferredLibrary.isEmpty())
        dialog.setPreferredLibrary(preferredLibrary);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    newComponent(dialog.libraryName(), dialog.scope());
    if (dialog.isNewLibrary() && !dialog.description().isEmpty()) {
        // An empty library file with the description; the component is added on save.
        Library library;
        library.name = dialog.libraryName();
        library.description = dialog.description();
        QString error;
        if (!m_libraries->saveLibrary(library, dialog.scope(), &error))
            QMessageBox::warning(this, tr("New Library"), error);
    }
    return true;
}

bool SymbolEditor::openComponent(const QString &definitionKey, QString *error)
{
    QString libraryName, componentId;
    if (!LibraryManager::splitKey(definitionKey, &libraryName, &componentId)) {
        if (error)
            *error = tr("\"%1\" is not a component key").arg(definitionKey);
        return false;
    }
    const LibraryManager::Entry *entry = effectiveEntry(*m_libraries, libraryName);
    const DefinitionPtr definition = entry ? entry->library.component(componentId) : nullptr;
    if (!definition) {
        if (error)
            *error = tr("Component %1 was not found").arg(definitionKey);
        return false;
    }
    m_sourceKey = definitionKey;
    m_neverSaved = false;
    if (isWritableScope(entry->scope)) {
        m_targetLibrary = libraryName;
        m_targetScope = entry->scope;
        m_originalId = componentId;
    } else {
        m_targetLibrary.clear();
        m_originalId.clear();
    }
    resetDefinition(*definition);
    return true;
}

QString SymbolEditor::definitionKey() const
{
    if (m_targetLibrary.isEmpty())
        return {};
    return LibraryManager::makeKey(m_targetLibrary, m_definition.id);
}

bool SymbolEditor::isModified() const
{
    return m_neverSaved || !m_undo->isClean();
}

void SymbolEditor::setDefinition(const ComponentDefinition &definition, const QString &description)
{
    if (definition == m_definition)
        return;
    m_undo->push(new DefinitionCommand(this, m_definition, definition,
                                       description.isEmpty() ? tr("Change component") : description, -1));
}

bool SymbolEditor::canSave() const
{
    return problems().isEmpty();
}

bool SymbolEditor::save(QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (isReadOnlySource())
        return fail(tr("The component comes from a read-only library; save a copy instead."));
    const QList<Problem> list = problems();
    if (!list.isEmpty())
        return fail(tr("The component cannot be saved: %1").arg(list.first().text));

    Library library;
    if (const LibraryManager::Entry *entry = entryInScope(*m_libraries, m_targetLibrary, m_targetScope)) {
        library = entry->library;
    } else {
        QString problem;
        if (!LibraryTargetDialog::checkNewLibrary(*m_libraries, m_targetLibrary, m_targetScope, &problem))
            return fail(problem);
        library.name = m_targetLibrary;
    }
    // A changed id renames the component in its library.
    if (!m_originalId.isEmpty() && m_originalId != m_definition.id)
        library.removeComponent(m_originalId);
    library.setComponent(m_definition);
    QString saveError;
    if (!m_libraries->saveLibrary(library, m_targetScope, &saveError))
        return fail(saveError);

    m_originalId = m_definition.id;
    m_neverSaved = false;
    m_sourceKey = definitionKey();
    m_undo->setClean();
    updateBanner();
    updateTitle();
    revalidate();
    statusBar()->showMessage(tr("Saved %1").arg(definitionKey()), 5000);
    Q_EMIT componentSaved(definitionKey());
    return true;
}

bool SymbolEditor::saveCopy(const QString &libraryName, LibraryScope scope, QString *error)
{
    if (!isWritableScope(scope)) {
        if (error)
            *error = tr("Built-in libraries are read-only.");
        return false;
    }
    if (const LibraryManager::Entry *entry = entryInScope(*m_libraries, libraryName, scope)) {
        if (entry->library.component(m_definition.id)) {
            if (error)
                *error = tr("Library \"%1\" already has a component \"%2\"; change the id first.")
                             .arg(libraryName, m_definition.id);
            return false;
        }
    }
    const QString previousLibrary = m_targetLibrary;
    const LibraryScope previousScope = m_targetScope;
    const QString previousId = m_originalId;
    m_targetLibrary = libraryName;
    m_targetScope = scope;
    m_originalId.clear();
    if (!save(error)) {
        m_targetLibrary = previousLibrary;
        m_targetScope = previousScope;
        m_originalId = previousId;
        updateBanner();
        updateTitle();
        revalidate();
        return false;
    }
    return true;
}

bool SymbolEditor::saveCopyInteractive()
{
    if (!canSave()) {
        QMessageBox::warning(this, tr("Save a Copy"),
                             tr("Fix the problems listed under Problems first:\n%1").arg(problemTexts().join(QLatin1Char('\n'))));
        return false;
    }
    QString sourceLibrary, sourceId;
    LibraryManager::splitKey(m_sourceKey, &sourceLibrary, &sourceId);
    LibraryTargetDialog dialog(m_libraries, LibraryTargetDialog::Mode::ExistingOrNew, this);
    dialog.setWindowTitle(tr("Save a Copy"));
    if (isReadOnlySource() && !sourceLibrary.isEmpty()) {
        dialog.setExplanation(
            tr("The built-in library \"%1\" is read-only. Save the component into a user or project library.\n\n"
               "A library named \"%1\" in the user or project folder would hide the whole built-in library, not just "
               "this component, so the copy goes into a library with a different name.")
                .arg(sourceLibrary));
        QString suggestion = sourceLibrary + QStringLiteral("-custom");
        for (int n = 2; !m_libraries->libraryNames().filter(QRegularExpression(
                            QStringLiteral("^%1$").arg(QRegularExpression::escape(suggestion)))).isEmpty();
             ++n)
            suggestion = QStringLiteral("%1-custom-%2").arg(sourceLibrary).arg(n);
        dialog.setSuggestedNewLibrary(suggestion);
    } else {
        dialog.setExplanation(tr("Save a copy of the component into another library."));
    }
    if (dialog.exec() != QDialog::Accepted)
        return false;
    QString error;
    if (!saveCopy(dialog.libraryName(), dialog.scope(), &error)) {
        QMessageBox::warning(this, tr("Save a Copy"), error);
        return false;
    }
    if (!dialog.description().isEmpty() && dialog.isNewLibrary()) {
        if (const LibraryManager::Entry *entry = entryInScope(*m_libraries, dialog.libraryName(), dialog.scope())) {
            Library library = entry->library;
            library.description = dialog.description();
            m_libraries->saveLibrary(library, dialog.scope());
        }
    }
    return true;
}

void SymbolEditor::closeEvent(QCloseEvent *event)
{
    if (!isModified()) {
        event->accept();
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Symbol Editor"), tr("The component \"%1\" has unsaved changes. Save them?").arg(m_definition.name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Discard) {
        event->accept();
        return;
    }
    if (answer == QMessageBox::Cancel) {
        event->ignore();
        return;
    }
    bool saved = false;
    if (isReadOnlySource()) {
        saved = saveCopyInteractive();
    } else {
        QString error;
        saved = save(&error);
        if (!saved)
            QMessageBox::warning(this, tr("Save Component"), error);
    }
    if (saved)
        event->accept();
    else
        event->ignore();
}

void SymbolEditor::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && !m_toolActions.isEmpty()) {
        updateToolIcons();
        m_canvas->viewport()->update();
    }
}

// --- definition changes ---

void SymbolEditor::edit(const std::function<void(ComponentDefinition &)> &change, const QString &description,
                        const QString &mergeKey)
{
    if (m_applying)
        return;
    ComponentDefinition next = m_definition;
    change(next);
    if (next == m_definition) {
        refreshProperties(); // put back a rejected value
        return;
    }
    m_undo->push(new DefinitionCommand(this, m_definition, next, description, mergeIdFor(mergeKey)));
}

void SymbolEditor::applyDefinition(const ComponentDefinition &definition)
{
    m_applying = true;
    m_definition = definition;
    m_canvas->setDefinition(definition);
    refreshGeneral();
    refreshPins();
    refreshParameters();
    refreshModels();
    m_orientationPreview->setSymbol(definition.symbol);
    m_applying = false;
    rebuildProperties();
    revalidate();
    updateTitle();
}

void SymbolEditor::refreshGeneral()
{
    setTextIfChanged(m_idEdit, m_definition.id);
    setTextIfChanged(m_nameEdit, m_definition.name);
    setTextIfChanged(m_categoryEdit, m_definition.category);
    setTextIfChanged(m_descriptionEdit, m_definition.description);
    setTextIfChanged(m_prefixEdit, m_definition.referencePrefix);
    setTextIfChanged(m_netNameEdit, m_definition.netName);
    const int role = m_roleCombo->findData(m_definition.role);
    const QSignalBlocker blocker(m_roleCombo);
    if (role >= 0) {
        m_roleCombo->setCurrentIndex(role);
    } else {
        m_roleCombo->addItem(m_definition.role, m_definition.role);
        m_roleCombo->setCurrentIndex(m_roleCombo->count() - 1);
    }
}

void SymbolEditor::refreshPins()
{
    const QSignalBlocker blocker(m_pinList);
    const int current = m_pinList->currentRow();
    if (m_pinList->count() != m_definition.pins.size()) {
        m_pinList->clear();
        for (int i = 0; i < m_definition.pins.size(); ++i) {
            auto *item = new QListWidgetItem(m_pinList);
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
        }
    }
    const QIcon warningIcon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
    for (int i = 0; i < m_definition.pins.size(); ++i) {
        QListWidgetItem *item = m_pinList->item(i);
        const QString &name = m_definition.pins.at(i);
        if (item->text() != name)
            item->setText(name);
        const SymbolPin *pin = m_definition.symbol.pin(name);
        item->setIcon(pin ? QIcon() : warningIcon);
        item->setToolTip(pin ? tr("Pin %1: %2 at (%3, %4), length %5")
                                   .arg(i + 1)
                                   .arg(pinDirectionName(pin->direction))
                                   .arg(pin->at.x())
                                   .arg(pin->at.y())
                                   .arg(pin->length)
                             : tr("Pin %1 is not placed on the symbol").arg(i + 1));
    }
    if (current >= 0 && current < m_pinList->count())
        m_pinList->setCurrentRow(current);
}

void SymbolEditor::refreshParameters()
{
    const QSignalBlocker blocker(m_parameterTable);
    const int rows = static_cast<int>(m_definition.parameters.size());
    const bool rebuildCombos = m_parameterTable->rowCount() != rows;
    m_parameterTable->setRowCount(rows);
    const QColor disabled = palette().color(QPalette::Disabled, QPalette::Text);
    for (int row = 0; row < rows; ++row) {
        const ParameterDefinition &parameter = m_definition.parameters.at(row);
        const auto setCell = [&](int column, const QString &text, bool editable) {
            QTableWidgetItem *item = m_parameterTable->item(row, column);
            if (!item) {
                item = new QTableWidgetItem;
                m_parameterTable->setItem(row, column, item);
            }
            if (item->text() != text)
                item->setText(text);
            item->setFlags(editable ? Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable
                                    : Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            item->setData(Qt::ForegroundRole, editable ? QVariant() : QVariant(disabled));
        };
        setCell(NameColumn, parameter.name, true);
        setCell(LabelColumn, parameter.label, true);
        setCell(UnitColumn, parameter.unit, true);
        setCell(DefaultColumn, parameter.defaultValue, true);
        setCell(ChoicesColumn, parameter.choices.join(QStringLiteral(", ")), !parameter.choices.isEmpty());

        QTableWidgetItem *shown = m_parameterTable->item(row, ShownColumn);
        if (!shown) {
            shown = new QTableWidgetItem;
            shown->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            m_parameterTable->setItem(row, ShownColumn, shown);
        }
        shown->setCheckState(parameter.shown ? Qt::Checked : Qt::Unchecked);

        auto *combo = qobject_cast<QComboBox *>(m_parameterTable->cellWidget(row, KindColumn));
        if (!combo || rebuildCombos) {
            combo = new QComboBox;
            combo->addItems({tr("expression"), tr("choices"), tr("file"), tr("text")});
            combo->setFrame(false);
            m_parameterTable->setCellWidget(row, KindColumn, combo);
            connect(combo, &QComboBox::currentIndexChanged, this, [this, row](int kind) {
                if (!m_applying)
                    setParameterKind(row, kind);
            });
        }
        const QSignalBlocker comboBlocker(combo);
        combo->setCurrentIndex(parameterKind(parameter));
    }
}

void SymbolEditor::refreshModels()
{
    const QSignalBlocker blocker(m_modelList);
    const int current = m_modelList->currentRow();
    if (m_modelList->count() != m_definition.models.size()) {
        m_modelList->clear();
        for (int i = 0; i < m_definition.models.size(); ++i)
            m_modelList->addItem(QString());
    }
    for (int i = 0; i < m_definition.models.size(); ++i) {
        const ModelDefinition &model = m_definition.models.at(i);
        QString detail;
        for (auto it = model.data.begin(); it != model.data.end(); ++it) {
            if (it.key() != QLatin1String("kind") && it.value().isString() && !it.value().toString().isEmpty()) {
                detail = QStringLiteral("%1: %2").arg(it.key(), it.value().toString().left(40));
                break;
            }
        }
        m_modelList->item(i)->setText(detail.isEmpty() ? model.kind : QStringLiteral("%1   (%2)").arg(model.kind, detail));
    }
    if (m_modelList->count() > 0)
        m_modelList->setCurrentRow(std::clamp(current, 0, m_modelList->count() - 1));
    refreshModelFields();
}

int SymbolEditor::currentModelRow() const
{
    const int row = m_modelList->currentRow();
    return row >= 0 && row < m_definition.models.size() ? row : -1;
}

void SymbolEditor::refreshModelFields()
{
    const int row = currentModelRow();
    const bool any = row >= 0;
    m_modelKind->setEnabled(any);
    m_modelTabs->setEnabled(any);
    const bool wasApplying = m_applying;
    m_applying = true;
    {
        const QSignalBlocker kindBlocker(m_modelKind);
        const QSignalBlocker fieldBlocker(m_modelFields);
        if (!any) {
            m_modelKind->setEditText(QString());
            m_modelFields->setRowCount(0);
            m_modelJson->clear();
            m_modelHint->setText(tr("No models: the component only defines connectivity, or add a model with Add."));
        } else {
            const ModelDefinition &model = m_definition.models.at(row);
            if (m_modelKind->currentText() != model.kind)
                m_modelKind->setEditText(model.kind);
            m_modelHint->setText(modelHint(model.kind));
            QStringList keys = model.data.keys();
            keys.removeAll(QStringLiteral("kind"));
            m_modelFields->setRowCount(static_cast<int>(keys.size()));
            for (int i = 0; i < keys.size(); ++i) {
                auto *key = m_modelFields->item(i, 0);
                if (!key) {
                    key = new QTableWidgetItem;
                    m_modelFields->setItem(i, 0, key);
                }
                key->setText(keys.at(i));
                key->setData(Qt::UserRole, keys.at(i));
                auto *value = m_modelFields->item(i, 1);
                if (!value) {
                    value = new QTableWidgetItem;
                    m_modelFields->setItem(i, 1, value);
                }
                value->setText(jsonValueText(model.data.value(keys.at(i))));
            }
            if (!m_modelJson->hasFocus()) {
                QJsonObject object = model.data;
                object.insert(QStringLiteral("kind"), model.kind);
                m_modelJson->setPlainText(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)));
            }
        }
    }
    m_applying = wasApplying;
}

// --- pins ---

void SymbolEditor::renamePin(const QString &oldName, const QString &newName)
{
    const QString name = newName.trimmed();
    if (name == oldName)
        return;
    if (name.isEmpty() || m_definition.pins.contains(name)) {
        statusBar()->showMessage(name.isEmpty() ? tr("A pin needs a name.") : tr("Pin \"%1\" already exists.").arg(name),
                                 5000);
        refreshPins();
        return;
    }
    edit(
        [oldName, name](ComponentDefinition &definition) {
            const qsizetype index = definition.pins.indexOf(oldName);
            if (index >= 0)
                definition.pins[index] = name;
            for (SymbolPin &pin : definition.symbol.pins) {
                if (pin.pin == oldName)
                    pin.pin = name;
            }
        },
        tr("Rename pin"));
}

void SymbolEditor::addPin()
{
    edit(
        [](ComponentDefinition &definition) {
            const QString name = SymbolCanvas::newPinName(definition);
            QRect bounds = definition.symbol.boundingRect();
            if (bounds.isNull())
                bounds = QRect(-20, -20, 40, 40);
            const int x = static_cast<int>(std::floor(bounds.left() / 10.0)) * 10 - 10;
            int y = 0;
            for (int step = 0; step < 200; ++step) {
                y = (step % 2 == 0 ? 1 : -1) * ((step + 1) / 2) * 10;
                const QPoint candidate(x, y);
                if (std::none_of(definition.symbol.pins.begin(), definition.symbol.pins.end(),
                                 [&](const SymbolPin &pin) { return pin.at == candidate; }))
                    break;
            }
            definition.pins.append(name);
            definition.symbol.pins.append(SymbolPin{name, QPoint(x, y), 10, PinDirection::Right});
        },
        tr("Add pin"));
    m_pinList->setCurrentRow(m_pinList->count() - 1);
}

void SymbolEditor::removeSelectedPin()
{
    const int row = m_pinList->currentRow();
    if (row < 0 || row >= m_definition.pins.size())
        return;
    const QString name = m_definition.pins.at(row);
    edit(
        [name](ComponentDefinition &definition) {
            definition.pins.removeAll(name);
            definition.symbol.pins.removeIf([&](const SymbolPin &pin) { return pin.pin == name; });
        },
        tr("Remove pin"));
}

void SymbolEditor::movePin(int delta)
{
    const int row = m_pinList->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_definition.pins.size())
        return;
    edit([row, target](ComponentDefinition &definition) { definition.pins.move(row, target); }, tr("Reorder pins"));
    m_pinList->setCurrentRow(target);
}

void SymbolEditor::onPinRowsMoved()
{
    if (m_applying)
        return;
    QStringList order;
    for (int i = 0; i < m_pinList->count(); ++i)
        order.append(m_pinList->item(i)->text());
    if (order == m_definition.pins)
        return;
    QStringList sortedOrder = order, sortedPins = m_definition.pins;
    sortedOrder.sort();
    sortedPins.sort();
    if (sortedOrder != sortedPins) {
        refreshPins(); // not a pure reordering; show the real list again
        return;
    }
    edit([order](ComponentDefinition &definition) { definition.pins = order; }, tr("Reorder pins"));
}

// --- parameters ---

void SymbolEditor::addParameter()
{
    edit(
        [](ComponentDefinition &definition) {
            ParameterDefinition parameter;
            parameter.name = QStringLiteral("param");
            for (int n = 2; definition.parameter(parameter.name); ++n)
                parameter.name = QStringLiteral("param%1").arg(n);
            parameter.defaultValue = QStringLiteral("0");
            definition.parameters.append(parameter);
        },
        tr("Add parameter"));
    m_parameterTable->setCurrentCell(m_parameterTable->rowCount() - 1, NameColumn);
}

void SymbolEditor::removeSelectedParameter()
{
    const int row = m_parameterTable->currentRow();
    if (row < 0 || row >= m_definition.parameters.size())
        return;
    edit([row](ComponentDefinition &definition) { definition.parameters.removeAt(row); }, tr("Remove parameter"));
}

void SymbolEditor::moveParameter(int delta)
{
    const int row = m_parameterTable->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_definition.parameters.size())
        return;
    edit([row, target](ComponentDefinition &definition) { definition.parameters.move(row, target); },
         tr("Reorder parameters"));
    m_parameterTable->setCurrentCell(target, m_parameterTable->currentColumn());
}

void SymbolEditor::onParameterItemChanged(int row, int column)
{
    if (m_applying || row < 0 || row >= m_definition.parameters.size())
        return;
    const QTableWidgetItem *item = m_parameterTable->item(row, column);
    if (!item)
        return;
    const QString text = item->text();
    const bool checked = item->checkState() == Qt::Checked;
    edit(
        [row, column, text, checked](ComponentDefinition &definition) {
            ParameterDefinition &parameter = definition.parameters[row];
            switch (column) {
            case NameColumn: parameter.name = text.trimmed(); break;
            case LabelColumn: parameter.label = text; break;
            case UnitColumn: parameter.unit = text.trimmed(); break;
            case DefaultColumn: parameter.defaultValue = text; break;
            case ShownColumn: parameter.shown = checked; break;
            case ChoicesColumn: {
                QStringList choices;
                for (const QString &choice : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    if (!choice.trimmed().isEmpty())
                        choices.append(choice.trimmed());
                }
                if (!choices.isEmpty())
                    parameter.choices = choices;
                break;
            }
            default: break;
            }
        },
        tr("Edit parameter"));
}

void SymbolEditor::setParameterKind(int row, int kind)
{
    if (row < 0 || row >= m_definition.parameters.size())
        return;
    edit(
        [row, kind](ComponentDefinition &definition) {
            ParameterDefinition &parameter = definition.parameters[row];
            parameter.isFile = kind == FileKind;
            parameter.isText = kind == TextKind;
            if (kind == ChoicesKind) {
                if (parameter.choices.isEmpty())
                    parameter.choices = {parameter.defaultValue.isEmpty() ? QStringLiteral("a") : parameter.defaultValue};
                if (!parameter.choices.contains(parameter.defaultValue))
                    parameter.defaultValue = parameter.choices.first();
            } else {
                parameter.choices.clear();
            }
        },
        tr("Change parameter kind"));
}

// --- models ---

void SymbolEditor::addModel(const QString &kind)
{
    edit(
        [kind](ComponentDefinition &definition) {
            definition.models.append(ModelDefinition{kind, defaultModelData(kind, definition)});
        },
        tr("Add model"));
    m_modelList->setCurrentRow(m_modelList->count() - 1);
    m_tabs->setCurrentIndex(3);
}

void SymbolEditor::removeSelectedModel()
{
    const int row = currentModelRow();
    if (row < 0)
        return;
    edit([row](ComponentDefinition &definition) { definition.models.removeAt(row); }, tr("Remove model"));
}

void SymbolEditor::moveModel(int delta)
{
    const int row = currentModelRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_definition.models.size())
        return;
    edit([row, target](ComponentDefinition &definition) { definition.models.move(row, target); }, tr("Reorder models"));
    m_modelList->setCurrentRow(target);
}

void SymbolEditor::onModelFieldChanged(int, int)
{
    const int row = currentModelRow();
    if (m_applying || row < 0)
        return;
    const QJsonObject original = m_definition.models.at(row).data;
    QJsonObject object{{QStringLiteral("kind"), m_definition.models.at(row).kind}};
    for (int i = 0; i < m_modelFields->rowCount(); ++i) {
        const QTableWidgetItem *key = m_modelFields->item(i, 0);
        const QTableWidgetItem *value = m_modelFields->item(i, 1);
        if (!key || key->text().trimmed().isEmpty() || key->text().trimmed() == QLatin1String("kind"))
            continue;
        const QJsonValue before = original.value(key->data(Qt::UserRole).toString());
        object.insert(key->text().trimmed(), jsonValueFromText(value ? value->text() : QString(), before));
    }
    edit([row, object](ComponentDefinition &definition) { definition.models[row].data = object; }, tr("Edit model"));
}

void SymbolEditor::applyModelJson()
{
    const int row = currentModelRow();
    if (row < 0)
        return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(m_modelJson->toPlainText().toUtf8(), &parseError);
    if (!document.isObject()) {
        QMessageBox::warning(this, tr("Model JSON"),
                             parseError.error != QJsonParseError::NoError
                                 ? tr("Not valid JSON: %1 at offset %2").arg(parseError.errorString()).arg(parseError.offset)
                                 : tr("The model must be a JSON object."));
        return;
    }
    const QJsonObject object = document.object();
    const QString kind = object.value(QStringLiteral("kind")).toString();
    edit([row, object, kind](ComponentDefinition &definition) { definition.models[row] = ModelDefinition{kind, object}; },
         tr("Edit model JSON"));
    m_modelJson->clearFocus();
    refreshModelFields();
}

// --- selected element properties ---

void SymbolEditor::rebuildProperties()
{
    const QList<ElementRef> selection = m_canvas->selection();
    QString key;
    if (selection.isEmpty()) {
        key = QStringLiteral("none");
    } else if (selection.size() > 1) {
        key = QStringLiteral("multi:%1").arg(selection.size());
    } else {
        const ElementRef &element = selection.first();
        key = QStringLiteral("%1:%2").arg(static_cast<int>(element.kind)).arg(element.index);
        if (element.kind == ElementRef::Kind::Graphic)
            key += QStringLiteral(":%1").arg(static_cast<int>(m_definition.symbol.graphics.at(element.index).type));
    }
    if (key == m_propertyKey && m_propertyForm) {
        refreshProperties();
        return;
    }
    m_propertyKey = key;
    m_propertyRefreshers.clear();
    m_firstTextField = nullptr;

    auto *hostLayout = static_cast<QVBoxLayout *>(m_propertyHost->layout());
    while (hostLayout->count() > 1) {
        QLayoutItem *item = hostLayout->takeAt(0);
        if (QWidget *old = item->widget()) {
            // A field of the old page may be emitting the signal that got us here.
            old->hide();
            old->deleteLater();
        }
        delete item;
    }
    auto *page = new QWidget(m_propertyHost);
    m_propertyForm = new QFormLayout(page);
    m_propertyForm->setContentsMargins(0, 0, 0, 0);
    m_propertyForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    hostLayout->insertWidget(0, page);

    auto *title = new QLabel(page);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    m_propertyForm->addRow(title);
    const auto addHint = [&](const QString &text) {
        auto *hint = new QLabel(text, page);
        hint->setWordWrap(true);
        hint->setForegroundRole(QPalette::PlaceholderText);
        m_propertyForm->addRow(hint);
    };

    const QStringList fills{tr("none"), tr("outline"), tr("background")};
    if (selection.isEmpty()) {
        title->setText(tr("Symbol"));
        addHint(tr("Click an element to edit it, drag to select several. Middle button pans, the wheel zooms."));
        addPointRow(tr("Reference at:"), [](const ComponentDefinition &d) { return d.symbol.referenceAt; },
                    [](ComponentDefinition &d, QPoint p) { d.symbol.referenceAt = p; }, 5, QStringLiteral("referenceAt"));
        addPointRow(tr("Value at:"), [](const ComponentDefinition &d) { return d.symbol.valueAt; },
                    [](ComponentDefinition &d, QPoint p) { d.symbol.valueAt = p; }, 5, QStringLiteral("valueAt"));
    } else if (selection.size() > 1) {
        title->setText(tr("%n elements selected", nullptr, static_cast<int>(selection.size())));
        addHint(tr("Drag to move them, arrow keys move by one grid step, Delete removes them and Ctrl+D duplicates."));
    } else {
        const ElementRef element = selection.first();
        const int i = element.index;
        const QString prefix = QStringLiteral("%1:").arg(key);
        switch (element.kind) {
        case ElementRef::Kind::Reference:
            title->setText(tr("Reference position"));
            addHint(tr("Where the reference (such as A1) of a placed component is drawn."));
            addPointRow(tr("Position:"), [](const ComponentDefinition &d) { return d.symbol.referenceAt; },
                        [](ComponentDefinition &d, QPoint p) { d.symbol.referenceAt = p; }, 5, QStringLiteral("referenceAt"));
            break;
        case ElementRef::Kind::Value:
            title->setText(tr("Value position"));
            addHint(tr("Where the shown parameters of a placed component are drawn."));
            addPointRow(tr("Position:"), [](const ComponentDefinition &d) { return d.symbol.valueAt; },
                        [](ComponentDefinition &d, QPoint p) { d.symbol.valueAt = p; }, 5, QStringLiteral("valueAt"));
            break;
        case ElementRef::Kind::Pin: {
            title->setText(tr("Pin"));
            addTextRow(
                tr("Name:"), [i](const ComponentDefinition &d) { return d.symbol.pins.at(i).pin; },
                [i](ComponentDefinition &d, const QString &text) {
                    const QString name = text.trimmed();
                    const QString old = d.symbol.pins.at(i).pin;
                    if (name.isEmpty() || name == old || d.pins.contains(name))
                        return;
                    const qsizetype index = d.pins.indexOf(old);
                    if (index >= 0)
                        d.pins[index] = name;
                    else
                        d.pins.append(name);
                    d.symbol.pins[i].pin = name;
                },
                QString(), true);
            addPointRow(tr("Connection point:"), [i](const ComponentDefinition &d) { return d.symbol.pins.at(i).at; },
                        [i](ComponentDefinition &d, QPoint p) { d.symbol.pins[i].at = p; }, 10, prefix + QStringLiteral("at"));
            addChoiceRow(tr("Direction:"), {tr("left"), tr("right"), tr("up"), tr("down")},
                         [i](const ComponentDefinition &d) { return static_cast<int>(d.symbol.pins.at(i).direction); },
                         [i](ComponentDefinition &d, int value) {
                             d.symbol.pins[i].direction = static_cast<PinDirection>(value);
                         });
            addIntRow(tr("Length:"), [i](const ComponentDefinition &d) { return d.symbol.pins.at(i).length; },
                      [i](ComponentDefinition &d, int value) { d.symbol.pins[i].length = value; }, 0, 500, 5,
                      prefix + QStringLiteral("length"));
            addHint(tr("The direction points from the connection point toward the body; the pin line is drawn that way."));
            break;
        }
        case ElementRef::Kind::Graphic: {
            const SymbolGraphic &graphic = m_definition.symbol.graphics.at(i);
            title->setText(graphicTypeName(graphic.type));
            const auto fillRow = [&] {
                addChoiceRow(tr("Fill:"), fills, [i](const ComponentDefinition &d) {
                    return static_cast<int>(d.symbol.graphics.at(i).fill);
                }, [i](ComponentDefinition &d, int value) { d.symbol.graphics[i].fill = static_cast<Fill>(value); });
            };
            switch (graphic.type) {
            case SymbolGraphic::Type::Line:
                addPointsRow(tr("Points:"), i);
                break;
            case SymbolGraphic::Type::Polygon:
                addPointsRow(tr("Points:"), i);
                fillRow();
                break;
            case SymbolGraphic::Type::Rect:
                addPointRow(tr("From:"), [i](const ComponentDefinition &d) { return d.symbol.graphics.at(i).from; },
                            [i](ComponentDefinition &d, QPoint p) { d.symbol.graphics[i].from = p; }, 1,
                            prefix + QStringLiteral("from"));
                addPointRow(tr("To:"), [i](const ComponentDefinition &d) { return d.symbol.graphics.at(i).to; },
                            [i](ComponentDefinition &d, QPoint p) { d.symbol.graphics[i].to = p; }, 1,
                            prefix + QStringLiteral("to"));
                fillRow();
                break;
            case SymbolGraphic::Type::Circle:
            case SymbolGraphic::Type::Arc:
                addPointRow(tr("Centre:"), [i](const ComponentDefinition &d) { return d.symbol.graphics.at(i).center; },
                            [i](ComponentDefinition &d, QPoint p) { d.symbol.graphics[i].center = p; }, 1,
                            prefix + QStringLiteral("center"));
                addIntRow(tr("Radius:"), [i](const ComponentDefinition &d) { return d.symbol.graphics.at(i).radius; },
                          [i](ComponentDefinition &d, int value) { d.symbol.graphics[i].radius = value; }, 1, 1000, 1,
                          prefix + QStringLiteral("radius"));
                if (graphic.type == SymbolGraphic::Type::Circle) {
                    fillRow();
                } else {
                    addDoubleRow(tr("Start angle:"),
                                 [i](const ComponentDefinition &d) { return d.symbol.graphics.at(i).startAngle; },
                                 [i](ComponentDefinition &d, double value) { d.symbol.graphics[i].startAngle = value; },
                                 -360, 360, prefix + QStringLiteral("start"), tr("°"));
                    addDoubleRow(tr("Span angle:"),
                                 [i](const ComponentDefinition &d) { return d.symbol.graphics.at(i).spanAngle; },
                                 [i](ComponentDefinition &d, double value) { d.symbol.graphics[i].spanAngle = value; },
                                 -360, 360, prefix + QStringLiteral("span"), tr("°"));
                    addHint(tr("Angles are counter-clockwise on screen from the +X axis."));
                }
                break;
            case SymbolGraphic::Type::Text:
                addTextRow(tr("Text:"), [i](const ComponentDefinition &d) { return d.symbol.graphics.at(i).text; },
                           [i](ComponentDefinition &d, const QString &text) { d.symbol.graphics[i].text = text; },
                           prefix + QStringLiteral("text"));
                addPointRow(tr("Position:"), [i](const ComponentDefinition &d) { return d.symbol.graphics.at(i).at; },
                            [i](ComponentDefinition &d, QPoint p) { d.symbol.graphics[i].at = p; }, 1,
                            prefix + QStringLiteral("at"));
                addIntRow(tr("Size:"), [i](const ComponentDefinition &d) { return d.symbol.graphics.at(i).size; },
                          [i](ComponentDefinition &d, int value) { d.symbol.graphics[i].size = value; }, 1, 200, 1,
                          prefix + QStringLiteral("size"));
                addChoiceRow(tr("Anchor:"), {tr("left"), tr("centre"), tr("right")},
                             [i](const ComponentDefinition &d) { return static_cast<int>(d.symbol.graphics.at(i).anchor); },
                             [i](ComponentDefinition &d, int value) {
                                 d.symbol.graphics[i].anchor = static_cast<TextAnchor>(value);
                             });
                break;
            }
            break;
        }
        }
    }
    refreshProperties();
}

void SymbolEditor::refreshProperties()
{
    const bool wasApplying = m_applying;
    m_applying = true;
    for (const Refresher &refresh : std::as_const(m_propertyRefreshers))
        refresh();
    m_applying = wasApplying;
}

void SymbolEditor::addPointRow(const QString &label, const std::function<QPoint(const ComponentDefinition &)> &get,
                               const std::function<void(ComponentDefinition &, QPoint)> &set, int step,
                               const QString &mergeKey)
{
    auto *row = new QWidget(m_propertyForm->parentWidget());
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *x = new QSpinBox(row);
    auto *y = new QSpinBox(row);
    for (QSpinBox *spin : {x, y}) {
        spin->setRange(-5000, 5000);
        spin->setSingleStep(step);
        spin->setAccelerated(true);
        spin->setMinimumWidth(50);
    }
    x->setPrefix(tr("x "));
    y->setPrefix(tr("y "));
    layout->addWidget(x);
    layout->addWidget(y);
    m_propertyForm->addRow(label, row);
    const auto changed = [this, x, y, set, label, mergeKey] {
        if (m_applying)
            return;
        const QPoint point(x->value(), y->value());
        edit([set, point](ComponentDefinition &definition) { set(definition, point); },
             tr("Edit %1").arg(QString(label).remove(QLatin1Char(':')).toLower()), mergeKey);
    };
    connect(x, &QSpinBox::valueChanged, this, changed);
    connect(y, &QSpinBox::valueChanged, this, changed);
    m_propertyRefreshers.append([this, x, y, get] {
        const QPoint point = get(m_definition);
        const QSignalBlocker bx(x), by(y);
        x->setValue(point.x());
        y->setValue(point.y());
    });
}

void SymbolEditor::addIntRow(const QString &label, const std::function<int(const ComponentDefinition &)> &get,
                             const std::function<void(ComponentDefinition &, int)> &set, int minimum, int maximum, int step,
                             const QString &mergeKey, const QString &suffix)
{
    auto *spin = new QSpinBox(m_propertyForm->parentWidget());
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setSuffix(suffix);
    m_propertyForm->addRow(label, spin);
    connect(spin, &QSpinBox::valueChanged, this, [this, set, label, mergeKey](int value) {
        if (m_applying)
            return;
        edit([set, value](ComponentDefinition &definition) { set(definition, value); },
             tr("Edit %1").arg(QString(label).remove(QLatin1Char(':')).toLower()), mergeKey);
    });
    m_propertyRefreshers.append([this, spin, get] {
        const QSignalBlocker blocker(spin);
        spin->setValue(get(m_definition));
    });
}

void SymbolEditor::addDoubleRow(const QString &label, const std::function<double(const ComponentDefinition &)> &get,
                                const std::function<void(ComponentDefinition &, double)> &set, double minimum,
                                double maximum, const QString &mergeKey, const QString &suffix)
{
    auto *spin = new QDoubleSpinBox(m_propertyForm->parentWidget());
    spin->setRange(minimum, maximum);
    spin->setDecimals(1);
    spin->setSingleStep(15);
    spin->setSuffix(suffix);
    m_propertyForm->addRow(label, spin);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, set, label, mergeKey](double value) {
        if (m_applying)
            return;
        edit([set, value](ComponentDefinition &definition) { set(definition, value); },
             tr("Edit %1").arg(QString(label).remove(QLatin1Char(':')).toLower()), mergeKey);
    });
    m_propertyRefreshers.append([this, spin, get] {
        const QSignalBlocker blocker(spin);
        spin->setValue(get(m_definition));
    });
}

QLineEdit *SymbolEditor::addTextRow(const QString &label, const std::function<QString(const ComponentDefinition &)> &get,
                                    const std::function<void(ComponentDefinition &, const QString &)> &set,
                                    const QString &mergeKey, bool commitOnFinish)
{
    auto *line = new QLineEdit(m_propertyForm->parentWidget());
    m_propertyForm->addRow(label, line);
    const QString description = tr("Edit %1").arg(QString(label).remove(QLatin1Char(':')).toLower());
    if (commitOnFinish) {
        connect(line, &QLineEdit::editingFinished, this, [this, line, set, description] {
            if (m_applying)
                return;
            const QString text = line->text();
            edit([set, text](ComponentDefinition &definition) { set(definition, text); }, description);
        });
    } else {
        connect(line, &QLineEdit::textEdited, this, [this, set, description, mergeKey](const QString &text) {
            edit([set, text](ComponentDefinition &definition) { set(definition, text); }, description, mergeKey);
        });
    }
    m_propertyRefreshers.append([this, line, get] { setTextIfChanged(line, get(m_definition)); });
    if (!m_firstTextField)
        m_firstTextField = line;
    return line;
}

void SymbolEditor::addChoiceRow(const QString &label, const QStringList &choices,
                                const std::function<int(const ComponentDefinition &)> &get,
                                const std::function<void(ComponentDefinition &, int)> &set)
{
    auto *combo = new QComboBox(m_propertyForm->parentWidget());
    combo->addItems(choices);
    m_propertyForm->addRow(label, combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [this, set, label](int value) {
        if (m_applying || value < 0)
            return;
        edit([set, value](ComponentDefinition &definition) { set(definition, value); },
             tr("Edit %1").arg(QString(label).remove(QLatin1Char(':')).toLower()));
    });
    m_propertyRefreshers.append([this, combo, get] {
        const QSignalBlocker blocker(combo);
        combo->setCurrentIndex(get(m_definition));
    });
}

void SymbolEditor::addPointsRow(const QString &label, int graphicIndex)
{
    auto *line = new QLineEdit(m_propertyForm->parentWidget());
    line->setToolTip(tr("Points as \"x, y; x, y; ...\""));
    m_propertyForm->addRow(label, line);
    const auto format = [](const QList<QPoint> &points) {
        QStringList parts;
        for (const QPoint &point : points)
            parts.append(QStringLiteral("%1, %2").arg(point.x()).arg(point.y()));
        return parts.join(QStringLiteral("; "));
    };
    connect(line, &QLineEdit::editingFinished, this, [this, line, graphicIndex] {
        if (m_applying)
            return;
        QList<QPoint> points;
        static const QRegularExpression pointPattern(QStringLiteral("^\\s*(-?\\d+)\\s*[, ]\\s*(-?\\d+)\\s*$"));
        for (const QString &part : line->text().split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const QRegularExpressionMatch match = pointPattern.match(part);
            if (!match.hasMatch()) {
                statusBar()->showMessage(tr("Could not read \"%1\" as a point x, y").arg(part.trimmed()), 5000);
                refreshProperties();
                return;
            }
            points.append(QPoint(match.captured(1).toInt(), match.captured(2).toInt()));
        }
        edit([graphicIndex, points](ComponentDefinition &definition) { definition.symbol.graphics[graphicIndex].points = points; },
             tr("Edit points"));
    });
    m_propertyRefreshers.append([this, line, graphicIndex, format] {
        if (!line->hasFocus())
            setTextIfChanged(line, format(m_definition.symbol.graphics.at(graphicIndex).points));
    });
}

// --- validation ---

QList<SymbolEditor::Problem> SymbolEditor::problems() const
{
    QList<Problem> list;
    const QString context = QStringLiteral("component \"%1\": ").arg(m_definition.id);
    static const QRegularExpression pinPattern(QStringLiteral("pin \"([^\"]*)\""));
    static const QRegularExpression parameterPattern(
        QStringLiteral("(?:parameter \"([^\"]*)\")|(?:\"([^\"]*)\" is not a valid parameter name)|(?:default of \"([^\"]*)\")"));
    for (QString text : m_definition.validate()) {
        if (text.startsWith(context))
            text = text.mid(context.size());
        Problem problem;
        problem.text = text.left(1).toUpper() + text.mid(1);
        if (text.contains(QLatin1String("the id ")))
            problem.target = Problem::Target::Id;
        else if (text.contains(QLatin1String("reference prefix")))
            problem.target = Problem::Target::Prefix;
        else if (const auto match = parameterPattern.match(text); match.hasMatch()) {
            problem.target = Problem::Target::Parameter;
            for (int group = 1; group <= 3; ++group) {
                if (!match.captured(group).isNull())
                    problem.name = match.captured(group);
            }
        } else if (const auto pinMatch = pinPattern.match(text); pinMatch.hasMatch()) {
            problem.target = Problem::Target::Pin;
            problem.name = pinMatch.captured(1);
        } else if (text.contains(QLatin1String("net name")))
            problem.target = Problem::Target::NetName;
        else if (text.contains(QLatin1String("sheet port")) || text.contains(QLatin1String("role")))
            problem.target = Problem::Target::Role;
        else if (text.contains(QLatin1String("model")))
            problem.target = Problem::Target::Model;
        else if (text.contains(QLatin1String("pin")))
            problem.target = Problem::Target::Pin;
        list.append(problem);
    }

    if (m_definition.name.trimmed().isEmpty())
        list.append(Problem{tr("Enter a name."), Problem::Target::Name, {}, -1});

    // Problems that would make the saved file fail to load.
    for (int i = 0; i < m_definition.symbol.graphics.size(); ++i) {
        const SymbolGraphic &graphic = m_definition.symbol.graphics.at(i);
        QString problem;
        switch (graphic.type) {
        case SymbolGraphic::Type::Line:
            if (graphic.points.size() < 2)
                problem = tr("A line needs at least 2 points.");
            break;
        case SymbolGraphic::Type::Polygon:
            if (graphic.points.size() < 3)
                problem = tr("A polygon needs at least 3 points.");
            break;
        case SymbolGraphic::Type::Circle:
        case SymbolGraphic::Type::Arc:
            if (graphic.radius <= 0)
                problem = tr("A %1 needs a positive radius.").arg(graphicTypeName(graphic.type).toLower());
            break;
        case SymbolGraphic::Type::Text:
            if (graphic.size <= 0)
                problem = tr("A text needs a positive size.");
            break;
        case SymbolGraphic::Type::Rect:
            break;
        }
        if (!problem.isEmpty())
            list.append(Problem{problem, Problem::Target::Graphic, {}, i});
    }

    for (int i = 0; i < m_definition.parameters.size(); ++i) {
        const ParameterDefinition &parameter = m_definition.parameters.at(i);
        if (!parameter.name.isEmpty() && isReservedName(parameter.name))
            list.append(Problem{tr("\"%1\" is a reserved name (a constant or function); rename the parameter.")
                                    .arg(parameter.name),
                                Problem::Target::Parameter, parameter.name, i});
        if (parameter.isExpression() && !parameter.defaultValue.trimmed().isEmpty()) {
            const Expression expression = Expression::parse(parameter.defaultValue);
            if (!expression.isValid())
                list.append(Problem{tr("The default of \"%1\" is not a valid expression: %2")
                                        .arg(parameter.name, expression.error()),
                                    Problem::Target::Parameter, parameter.name, i});
        }
    }

    for (int i = 0; i < m_definition.models.size(); ++i) {
        const ModelDefinition &model = m_definition.models.at(i);
        if (!model.kind.isEmpty() && model.data.value(QStringLiteral("kind")).toString() != model.kind
            && model.data.contains(QStringLiteral("kind")))
            list.append(Problem{tr("Model %1 has two different kinds.").arg(i + 1), Problem::Target::Model, {}, i});
    }

    if (!m_targetLibrary.isEmpty()) {
        if (const LibraryManager::Entry *entry = entryInScope(*m_libraries, m_targetLibrary, m_targetScope)) {
            if (m_definition.id != m_originalId && entry->library.component(m_definition.id))
                list.append(Problem{tr("Library \"%1\" already has a component with the id \"%2\".")
                                        .arg(m_targetLibrary, m_definition.id),
                                    Problem::Target::Id, {}, -1});
        }
    }
    return list;
}

QStringList SymbolEditor::problemTexts() const
{
    QStringList texts;
    for (const Problem &problem : problems())
        texts.append(problem.text);
    return texts;
}

void SymbolEditor::revalidate()
{
    const QList<Problem> list = problems();
    m_problemList->clear();
    const QIcon errorIcon = style()->standardIcon(QStyle::SP_MessageBoxCritical);
    for (const Problem &problem : list) {
        auto *item = new QListWidgetItem(errorIcon, problem.text, m_problemList);
        item->setData(Qt::UserRole, static_cast<int>(problem.target));
        item->setData(Qt::UserRole + 1, problem.name);
        item->setData(Qt::UserRole + 2, problem.index);
    }
    if (list.isEmpty()) {
        auto *item = new QListWidgetItem(style()->standardIcon(QStyle::SP_DialogApplyButton),
                                         tr("No problems: the component can be saved."), m_problemList);
        item->setFlags(Qt::ItemIsEnabled);
    }
    if (auto *dock = qobject_cast<QDockWidget *>(m_problemList->parentWidget()))
        dock->setWindowTitle(list.isEmpty() ? tr("Problems") : tr("Problems (%1)").arg(list.size()));

    const bool valid = list.isEmpty();
    m_saveAction->setEnabled(valid);
    m_saveAction->setText(isReadOnlySource() ? tr("&Save a Copy to Library...") : tr("&Save to Library"));
    m_saveAction->setToolTip(valid ? (isReadOnlySource() ? tr("Save a copy into a user or project library")
                                                         : tr("Save into library \"%1\"").arg(m_targetLibrary))
                                   : tr("Fix the listed problems to save"));
    m_saveCopyAction->setEnabled(valid);
    m_bannerButton->setEnabled(valid);
}

void SymbolEditor::showProblem(QListWidgetItem *item)
{
    if (!item || !(item->flags() & Qt::ItemIsSelectable))
        return;
    const auto target = static_cast<Problem::Target>(item->data(Qt::UserRole).toInt());
    const QString name = item->data(Qt::UserRole + 1).toString();
    const int index = item->data(Qt::UserRole + 2).toInt();
    const auto focusGeneral = [this](QWidget *field) {
        m_tabs->setCurrentIndex(0);
        field->setFocus();
        if (auto *line = qobject_cast<QLineEdit *>(field))
            line->selectAll();
    };
    switch (target) {
    case Problem::Target::Id: focusGeneral(m_idEdit); break;
    case Problem::Target::Name: focusGeneral(m_nameEdit); break;
    case Problem::Target::Category: focusGeneral(m_categoryEdit); break;
    case Problem::Target::Prefix: focusGeneral(m_prefixEdit); break;
    case Problem::Target::NetName: focusGeneral(m_netNameEdit); break;
    case Problem::Target::Role: focusGeneral(m_roleCombo); break;
    case Problem::Target::Library: m_tabs->setCurrentIndex(0); break;
    case Problem::Target::Pin: {
        m_tabs->setCurrentIndex(1);
        const int row = static_cast<int>(m_definition.pins.indexOf(name));
        if (row >= 0)
            m_pinList->setCurrentRow(row);
        for (int i = 0; i < m_definition.symbol.pins.size(); ++i) {
            if (m_definition.symbol.pins.at(i).pin == name)
                m_canvas->setSelection({ElementRef{ElementRef::Kind::Pin, i}});
        }
        break;
    }
    case Problem::Target::Parameter: {
        m_tabs->setCurrentIndex(2);
        int row = index;
        if (row < 0) {
            for (int i = 0; i < m_definition.parameters.size(); ++i) {
                if (m_definition.parameters.at(i).name == name)
                    row = i;
            }
        }
        if (row >= 0) {
            m_parameterTable->setCurrentCell(row, NameColumn);
            m_parameterTable->setFocus();
        }
        break;
    }
    case Problem::Target::Graphic:
        if (index >= 0)
            m_canvas->setSelection({ElementRef{ElementRef::Kind::Graphic, index}});
        m_canvas->setFocus();
        break;
    case Problem::Target::Model:
        m_tabs->setCurrentIndex(3);
        if (index >= 0)
            m_modelList->setCurrentRow(index);
        break;
    case Problem::Target::None:
        break;
    }
}

// --- window state ---

void SymbolEditor::updateTitle()
{
    QString title;
    if (!definitionKey().isEmpty())
        title = QStringLiteral("%1 (%2)").arg(m_definition.name, definitionKey());
    else if (!m_sourceKey.isEmpty())
        title = tr("%1 (%2, read-only)").arg(m_definition.name, m_sourceKey);
    else
        title = m_definition.name;
    setWindowTitle(tr("%1[*] - Symbol Editor").arg(title));
    setWindowModified(isModified());

    if (m_targetLibrary.isEmpty()) {
        QString sourceLibrary, sourceId;
        LibraryManager::splitKey(m_sourceKey, &sourceLibrary, &sourceId);
        m_targetLabel->setText(tr("%1 (built-in, read-only)").arg(sourceLibrary));
    } else if (const LibraryManager::Entry *entry = entryInScope(*m_libraries, m_targetLibrary, m_targetScope)) {
        m_targetLabel->setText(tr("%1 (%2)\n%3")
                                   .arg(m_targetLibrary, scopeDisplayName(m_targetScope),
                                        QDir::toNativeSeparators(entry->library.filePath)));
    } else {
        m_targetLabel->setText(tr("%1 (%2, new: created on save)").arg(m_targetLibrary, scopeDisplayName(m_targetScope)));
    }
}

void SymbolEditor::updateBanner()
{
    if (isReadOnlySource() && !m_sourceKey.isEmpty()) {
        QString sourceLibrary, sourceId;
        LibraryManager::splitKey(m_sourceKey, &sourceLibrary, &sourceId);
        m_bannerText->setText(tr("<b>%1</b> belongs to the built-in library \"%2\", which is read-only. You can change "
                                 "it here and save a copy into a user or project library; schematics keep using the "
                                 "built-in part until you place the copy.")
                                  .arg(m_sourceKey.toHtmlEscaped(), sourceLibrary.toHtmlEscaped()));
        m_bannerButton->show();
        m_banner->show();
    } else if (!m_targetLibrary.isEmpty() && !entryInScope(*m_libraries, m_targetLibrary, m_targetScope)) {
        m_bannerText->setText(tr("The %1 library \"%2\" does not exist yet; it is created when you save.")
                                  .arg(scopeDisplayName(m_targetScope), m_targetLibrary.toHtmlEscaped()));
        m_bannerButton->hide();
        m_banner->show();
    } else if (m_targetLibrary.isEmpty()) {
        m_bannerText->setText(tr("No library is chosen for this component; use Save a Copy to choose one."));
        m_bannerButton->show();
        m_banner->show();
    } else {
        m_banner->hide();
    }
    if (m_saveAction)
        revalidate();
}

void SymbolEditor::updateCategoryCompletion()
{
    QStringList categories;
    for (const LibraryManager::Entry &entry : m_libraries->entries()) {
        for (const DefinitionPtr &definition : entry.library.components) {
            QString path;
            for (const QString &part : definition->category.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                path = path.isEmpty() ? part : path + QLatin1Char('/') + part;
                if (!categories.contains(path))
                    categories.append(path);
            }
        }
    }
    categories.sort(Qt::CaseInsensitive);
    static_cast<QStringListModel *>(m_categoryCompleter->model())->setStringList(categories);
}

} // namespace lvd
