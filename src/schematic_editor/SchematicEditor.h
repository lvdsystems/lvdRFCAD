#pragma once

#include "core/Connectivity.h"
#include "core/RuleCheck.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QWidget>

class QAction;
class QActionGroup;

namespace lvd {

class Document;
class Project;
class PropertiesPanel;
class Schematic;
class SchematicScene;
class SchematicView;
class VariablesPanel;

// Editor for RF and SPICE schematics (requirement PL-3): a canvas plus a
// properties panel and a variables panel that the main window puts in docks.
// All changes go through the Schematic API and its undo stack.
class SchematicEditor : public QWidget
{
    Q_OBJECT
public:
    SchematicEditor(Project *project, Document *document, QWidget *parent = nullptr);
    ~SchematicEditor() override;

    Project *project() const { return m_project; }
    Document *document() const { return m_document; }
    Schematic *schematic() const;

    // Placement mode with this component ("library:component" or
    // "sheet:<document>"); Esc leaves it.
    void startPlacing(const QString &definitionKey);

    // Undo, redo, cut, copy, paste, duplicate, delete, rotate, mirror, select all.
    QList<QAction *> editActions() const;
    // Select, wire, label, text, net highlight, zoom to fit, rule check.
    QList<QAction *> toolActions() const;
    // An action by object name: "undo", "redo", "cut", "copy", "paste",
    // "duplicate", "delete", "rotate", "mirror", "selectAll", "selectTool",
    // "wireTool", "labelTool", "textTool", "highlightTool", "zoomToFit",
    // "ruleCheck".
    QAction *action(const QString &name) const;

    // Panels owned by the editor; the main window may put them in docks.
    QWidget *propertiesWidget() const;
    QWidget *variablesWidget() const;

    // Marks the items of this document named by the violations.
    void showViolations(const QList<RuleViolation> &violations);
    void zoomToItem(const QString &itemId);
    // Runs the rule check now; also emits ruleCheckFinished.
    QList<RuleViolation> runRuleCheck();

    QStringList selectedItemIds() const;
    void selectItems(const QStringList &ids);
    void zoomToFit();

    // Highlights the net of an item id or "componentId/pin"; empty clears.
    void highlightNet(const QString &seed);
    // Name of the highlighted net, or empty.
    QString highlightedNet() const;

    // Up-to-date connectivity of the sheet.
    const Connectivity &connectivity() const;

    SchematicView *view() const { return m_view; }
    SchematicScene *scene() const { return m_scene; }

Q_SIGNALS:
    void statusMessage(const QString &text);
    void selectionChanged();
    void ruleCheckFinished(const QList<lvd::RuleViolation> &violations);

protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    QAction *makeAction(const QString &name, const QString &text, const QString &tip, int icon,
                        const QList<QKeySequence> &shortcuts);
    void updateActions();
    void updateIcons();
    void applyPalette();

    Project *m_project;
    Document *m_document;
    SchematicScene *m_scene;
    SchematicView *m_view;
    QPointer<PropertiesPanel> m_properties;
    QPointer<VariablesPanel> m_variables;

    QHash<QString, QAction *> m_actions;
    QHash<QAction *, int> m_actionIcons;
    QList<QAction *> m_editActions;
    QList<QAction *> m_toolActions;
    QActionGroup *m_toolGroup;
    bool m_fitted = false;
};

} // namespace lvd
