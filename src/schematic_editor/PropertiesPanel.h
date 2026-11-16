#pragma once

#include "core/Project.h"
#include "core/StandardValues.h"

#include <QStringList>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace lvd {

class SchematicScene;

// Standard values (requirement RF-13): the text of a parameter value rounded
// to a value of the series, or nullopt when the text is not a positive
// constant (a number, or an expression without variables). The unit style
// of the original text is kept: the result is written with an SI prefix
// (formatEngineering), with the unit only when the original text had it,
// with a space between number and prefix only when the original had one,
// and with "u" for micro unless the original used "µ". `unit` is the
// parameter's unit, such as "F".
std::optional<QString> standardValueText(const QString &text, const QString &unit, ESeries series,
                                         Rounding rounding = Rounding::Nearest);

// Properties of the selection: one component (reference, definition, source
// and parameters), one net label, one text note, or a count for several
// items. All edits go through the Schematic API, so they are undoable.
class PropertiesPanel : public QWidget
{
    Q_OBJECT
public:
    explicit PropertiesPanel(SchematicScene *scene, QWidget *parent = nullptr);

    void setSelection(const QStringList &ids);
    QStringList selection() const { return m_ids; }
    // Shows the panel's dock, if any, and focuses the first field.
    void activate();

    // Rounds a numeric parameter of the shown component to a standard value
    // (RF-13), as the "Round to standard value" menu does: one undoable
    // edit. Returns false when the parameter has no constant value; true
    // when it was rounded or already was a standard value.
    bool roundToStandardValue(const QString &parameter, ESeries series, Rounding rounding = Rounding::Nearest);
    // The menu of the standard-value button of a parameter (E12 to E96,
    // nearest, up, down, with the resulting values); null if the row has none.
    QMenu *standardValueMenu(const QString &parameter) const;

Q_SIGNALS:
    void statusMessage(const QString &text);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void scheduleRefresh();
    void refresh();
    void showComponent(const QString &id);
    void buildParameterTable(const PlacedComponent &component, const ComponentDefinition &definition);
    void updateParameterRows(const PlacedComponent &component, const ComponentDefinition &definition);
    void applyParameter(const QString &name, const QString &text);
    void browseFile(const QString &name);
    void fillStandardValueMenu(QMenu *menu, const QString &name);
    QString currentParameterText(const QString &name) const;
    void applyText();

    SchematicScene *m_scene;
    QStringList m_ids;
    QTimer m_refreshTimer;

    QStackedWidget *m_pages;
    QLabel *m_emptyLabel;
    QLabel *m_multiLabel;

    // Component page
    QWidget *m_componentPage;
    QLineEdit *m_reference;
    QLabel *m_definition;
    QLabel *m_source;
    QTableWidget *m_parameters;
    QString m_tableComponent;      // component the table was built for
    DefinitionPtr m_tableDefinition;

    // Label page
    QWidget *m_labelPage;
    QLineEdit *m_net;

    // Text page
    QWidget *m_textPage;
    QPlainTextEdit *m_text;
    QSpinBox *m_textSize;
};

} // namespace lvd
