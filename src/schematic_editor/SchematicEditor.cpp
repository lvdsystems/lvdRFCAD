#include "schematic_editor/SchematicEditor.h"

#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "schematic_editor/EditorIcons.h"
#include "schematic_editor/EditorSupport.h"
#include "schematic_editor/PropertiesPanel.h"
#include "schematic_editor/SchematicScene.h"
#include "schematic_editor/SchematicView.h"
#include "schematic_editor/VariablesPanel.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QMimeData>
#include <QVBoxLayout>

namespace lvd {

SchematicEditor::SchematicEditor(Project *project, Document *document, QWidget *parent)
    : QWidget(parent), m_project(project), m_document(document)
{
    Q_ASSERT(project && document && document->schematic());
    m_scene = new SchematicScene(project, document, this);
    m_scene->setStyle(SchematicStyle::fromPalette(palette()));
    m_view = new SchematicView(m_scene, this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
    setFocusProxy(m_view);

    m_properties = new PropertiesPanel(m_scene);
    m_variables = new VariablesPanel(document->schematic());

    // Edit actions
    m_editActions = {
        makeAction(QStringLiteral("undo"), tr("&Undo"), tr("Undo the last change"), int(EditorIcon::Undo),
                   QKeySequence::keyBindings(QKeySequence::Undo)),
        makeAction(QStringLiteral("redo"), tr("&Redo"), tr("Redo the last undone change"), int(EditorIcon::Redo),
                   QKeySequence::keyBindings(QKeySequence::Redo)),
        makeAction(QStringLiteral("cut"), tr("Cu&t"), tr("Cut the selection to the clipboard"), int(EditorIcon::Cut),
                   QKeySequence::keyBindings(QKeySequence::Cut)),
        makeAction(QStringLiteral("copy"), tr("&Copy"), tr("Copy the selection to the clipboard"),
                   int(EditorIcon::Copy), QKeySequence::keyBindings(QKeySequence::Copy)),
        makeAction(QStringLiteral("paste"), tr("&Paste"), tr("Paste items from the clipboard"), int(EditorIcon::Paste),
                   QKeySequence::keyBindings(QKeySequence::Paste)),
        makeAction(QStringLiteral("duplicate"), tr("D&uplicate"), tr("Duplicate the selection (Ctrl+D)"),
                   int(EditorIcon::Duplicate), {QKeySequence(Qt::CTRL | Qt::Key_D)}),
        makeAction(QStringLiteral("delete"), tr("&Delete"), tr("Delete the selection (Del)"), int(EditorIcon::Delete),
                   QKeySequence::keyBindings(QKeySequence::Delete)),
        makeAction(QStringLiteral("rotate"), tr("R&otate"), tr("Rotate 90 degrees counter-clockwise (R)"),
                   int(EditorIcon::Rotate), {QKeySequence(Qt::Key_R)}),
        makeAction(QStringLiteral("mirror"), tr("&Mirror"), tr("Mirror left to right (X)"), int(EditorIcon::Mirror),
                   {QKeySequence(Qt::Key_X)}),
        makeAction(QStringLiteral("selectAll"), tr("Select &All"), tr("Select all items"), int(EditorIcon::SelectAll),
                   QKeySequence::keyBindings(QKeySequence::SelectAll)),
    };
    UndoStack *undo = schematic()->undoStack();
    connect(action(QStringLiteral("undo")), &QAction::triggered, this, [this, undo] {
        m_view->cancelTransient();
        undo->undo();
    });
    connect(action(QStringLiteral("redo")), &QAction::triggered, this, [this, undo] {
        m_view->cancelTransient();
        undo->redo();
    });
    connect(action(QStringLiteral("cut")), &QAction::triggered, m_view, &SchematicView::cutSelection);
    connect(action(QStringLiteral("copy")), &QAction::triggered, m_view, &SchematicView::copySelection);
    connect(action(QStringLiteral("paste")), &QAction::triggered, m_view, &SchematicView::paste);
    connect(action(QStringLiteral("duplicate")), &QAction::triggered, m_view, &SchematicView::duplicateSelection);
    connect(action(QStringLiteral("delete")), &QAction::triggered, m_view, &SchematicView::deleteSelection);
    connect(action(QStringLiteral("rotate")), &QAction::triggered, m_view, &SchematicView::rotateSelection);
    connect(action(QStringLiteral("mirror")), &QAction::triggered, m_view, &SchematicView::mirrorSelection);
    connect(action(QStringLiteral("selectAll")), &QAction::triggered, m_view, &SchematicView::selectAll);

    // Tool actions
    m_toolGroup = new QActionGroup(this);
    const auto tool = [this](const QString &name, const QString &text, const QString &tip, EditorIcon icon,
                             QKeySequence key, SchematicView::Tool which) {
        QAction *a = makeAction(name, text, tip, int(icon), {key});
        a->setCheckable(true);
        a->setData(int(which));
        m_toolGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, which] { m_view->setTool(which); });
        return a;
    };
    m_toolActions = {
        tool(QStringLiteral("selectTool"), tr("&Select"), tr("Select and move items (S, Esc)"), EditorIcon::Select,
             QKeySequence(Qt::Key_S), SchematicView::Tool::Select),
        tool(QStringLiteral("wireTool"), tr("&Wire"), tr("Draw wires (W)"), EditorIcon::Wire, QKeySequence(Qt::Key_W),
             SchematicView::Tool::Wire),
        tool(QStringLiteral("labelTool"), tr("Net &Label"), tr("Place net labels (L)"), EditorIcon::Label,
             QKeySequence(Qt::Key_L), SchematicView::Tool::Label),
        tool(QStringLiteral("textTool"), tr("&Text"), tr("Place text notes (T)"), EditorIcon::Text,
             QKeySequence(Qt::Key_T), SchematicView::Tool::Text),
        tool(QStringLiteral("highlightTool"), tr("Net &Highlight"),
             tr("Highlight a net (H); Ctrl+click a wire or pin also highlights its net"), EditorIcon::Highlight,
             QKeySequence(Qt::Key_H), SchematicView::Tool::Highlight),
        makeAction(QStringLiteral("zoomToFit"), tr("Zoom to &Fit"), tr("Show the whole schematic (Home)"),
                   int(EditorIcon::ZoomFit), {QKeySequence(Qt::Key_Home)}),
        makeAction(QStringLiteral("ruleCheck"), tr("&Rule Check"), tr("Run the electrical rule check (Ctrl+E)"),
                   int(EditorIcon::RuleCheck), {QKeySequence(Qt::CTRL | Qt::Key_E)}),
    };
    action(QStringLiteral("selectTool"))->setChecked(true);
    connect(action(QStringLiteral("zoomToFit")), &QAction::triggered, m_view, &SchematicView::zoomToFit);
    connect(action(QStringLiteral("ruleCheck")), &QAction::triggered, this, &SchematicEditor::runRuleCheck);
    updateIcons();

    QList<QAction *> context = {action(QStringLiteral("cut")), action(QStringLiteral("copy")),
                                action(QStringLiteral("paste")), action(QStringLiteral("duplicate")),
                                action(QStringLiteral("delete"))};
    auto *separator = new QAction(this);
    separator->setSeparator(true);
    context << separator << action(QStringLiteral("rotate")) << action(QStringLiteral("mirror"));
    m_view->setContextActions(context);

    connect(m_view, &SchematicView::toolChanged, this, [this](SchematicView::Tool which) {
        for (QAction *a : m_toolGroup->actions()) {
            if (a->data().toInt() == int(which))
                a->setChecked(true);
        }
        updateActions();
    });
    connect(m_view, &SchematicView::modeChanged, this, &SchematicEditor::updateActions);
    connect(m_view, &SchematicView::statusMessage, this, &SchematicEditor::statusMessage);
    connect(m_view, &SchematicView::componentActivated, this, [this](const QString &id) {
        m_scene->selectIds({id});
        m_properties->activate();
    });
    connect(m_properties, &PropertiesPanel::statusMessage, this, &SchematicEditor::statusMessage);
    connect(m_scene, &QGraphicsScene::selectionChanged, this, [this] {
        m_properties->setSelection(m_scene->selectedIds());
        updateActions();
        Q_EMIT selectionChanged();
    });
    connect(undo, &UndoStack::changed, this, &SchematicEditor::updateActions);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &SchematicEditor::updateActions);
    updateActions();
}

SchematicEditor::~SchematicEditor()
{
    disconnect(m_view, nullptr, this, nullptr);
    disconnect(m_scene, nullptr, this, nullptr);
    // The view removes its ghost items from the scene, so it goes first.
    delete m_view;
    delete m_properties.data();
    delete m_variables.data();
    delete m_scene;
}

Schematic *SchematicEditor::schematic() const
{
    return m_document->schematic();
}

QAction *SchematicEditor::makeAction(const QString &name, const QString &text, const QString &tip, int icon,
                                     const QList<QKeySequence> &shortcuts)
{
    auto *a = new QAction(text, this);
    a->setObjectName(name);
    a->setShortcuts(shortcuts);
    a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    a->setToolTip(tip);
    a->setStatusTip(tip);
    addAction(a);
    m_actions.insert(name, a);
    m_actionIcons.insert(a, icon);
    return a;
}

void SchematicEditor::updateIcons()
{
    for (auto it = m_actionIcons.cbegin(); it != m_actionIcons.cend(); ++it)
        it.key()->setIcon(editorIcon(EditorIcon(it.value()), palette()));
}

void SchematicEditor::updateActions()
{
    const UndoStack *undo = schematic()->undoStack();
    QAction *undoAction = action(QStringLiteral("undo"));
    QAction *redoAction = action(QStringLiteral("redo"));
    undoAction->setEnabled(undo->canUndo());
    redoAction->setEnabled(undo->canRedo());
    undoAction->setText(undo->canUndo() ? tr("&Undo %1").arg(undo->undoText()) : tr("&Undo"));
    redoAction->setText(undo->canRedo() ? tr("&Redo %1").arg(undo->redoText()) : tr("&Redo"));

    const bool selection = m_view->hasSelection();
    const bool transforming = m_view->isPlacing() || m_view->isFloating();
    for (const char *name : {"cut", "copy", "duplicate", "delete"})
        action(QString::fromLatin1(name))->setEnabled(selection && !transforming);
    action(QStringLiteral("rotate"))->setEnabled(selection || transforming);
    action(QStringLiteral("mirror"))->setEnabled(selection || transforming);
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    action(QStringLiteral("paste"))->setEnabled(mime && mime->hasFormat(itemsMimeType()));
}

QAction *SchematicEditor::action(const QString &name) const
{
    return m_actions.value(name);
}

void SchematicEditor::startPlacing(const QString &definitionKey)
{
    m_view->startPlacing(definitionKey);
}

QList<QAction *> SchematicEditor::editActions() const
{
    return m_editActions;
}

QList<QAction *> SchematicEditor::toolActions() const
{
    return m_toolActions;
}

QWidget *SchematicEditor::propertiesWidget() const
{
    return m_properties;
}

QWidget *SchematicEditor::variablesWidget() const
{
    return m_variables;
}

void SchematicEditor::showViolations(const QList<RuleViolation> &violations)
{
    m_scene->setViolations(violations);
}

void SchematicEditor::zoomToItem(const QString &itemId)
{
    m_view->zoomToItem(itemId);
}

QList<RuleViolation> SchematicEditor::runRuleCheck()
{
    const QList<RuleViolation> violations = checkSchematic(*m_project, *m_document);
    showViolations(violations);
    int errors = 0, warnings = 0;
    for (const RuleViolation &violation : violations) {
        if (violation.severity == RuleViolation::Severity::Error)
            ++errors;
        else if (violation.severity == RuleViolation::Severity::Warning)
            ++warnings;
    }
    Q_EMIT statusMessage(violations.isEmpty()
                             ? tr("Rule check: no problems found")
                             : tr("Rule check: %1, %2").arg(tr("%n error(s)", nullptr, errors),
                                                           tr("%n warning(s)", nullptr, warnings)));
    Q_EMIT ruleCheckFinished(violations);
    return violations;
}

QStringList SchematicEditor::selectedItemIds() const
{
    return m_scene->selectedIds();
}

void SchematicEditor::selectItems(const QStringList &ids)
{
    m_scene->selectIds(ids);
}

void SchematicEditor::zoomToFit()
{
    m_view->zoomToFit();
}

void SchematicEditor::highlightNet(const QString &seed)
{
    if (seed.isEmpty())
        m_view->clearHighlight();
    else
        m_scene->setHighlightSeed(seed);
}

QString SchematicEditor::highlightedNet() const
{
    const int index = m_scene->highlightedNet();
    return index < 0 ? QString() : m_scene->connectivity().nets.at(index).name;
}

const Connectivity &SchematicEditor::connectivity() const
{
    return m_scene->connectivity();
}

void SchematicEditor::applyPalette()
{
    m_scene->setStyle(SchematicStyle::fromPalette(palette()));
    m_view->resetCachedContent();
    updateIcons();
}

void SchematicEditor::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange)
        applyPalette();
    QWidget::changeEvent(event);
}

void SchematicEditor::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_fitted) {
        m_fitted = true;
        if (!m_scene->schematicItems().isEmpty())
            m_view->zoomToFit();
    }
}

} // namespace lvd
