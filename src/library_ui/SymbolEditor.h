#pragma once

#include "core/ComponentDefinition.h"
#include "core/Library.h"
#include "library_ui/SymbolCanvas.h"

#include <QList>
#include <QMainWindow>

#include <functional>

class QAction;
class QActionGroup;
class QComboBox;
class QCompleter;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QUndoStack;
class QWidget;

namespace lvd {

class SymbolPreview;
class DefinitionCommand;

// Window for creating and editing component definitions: symbol drawing,
// pins, parameters, models and general fields, with undo, live validation
// and saving into a user or project library. Components of built-in
// libraries are read-only and can be saved as a copy.
class SymbolEditor : public QMainWindow
{
    Q_OBJECT
public:
    explicit SymbolEditor(LibraryManager *libraries, QWidget *parent = nullptr);
    ~SymbolEditor() override;

    // Starts a new component that is saved into this library. A library that
    // does not exist in that scope yet is created on save.
    void newComponent(const QString &libraryName, LibraryScope scope);
    // Asks for the target library first; returns false if the user cancels.
    bool newComponentInteractive(const QString &preferredLibrary = {});
    // Opens a library component. Components of built-in libraries open
    // read-only and are saved with saveCopy().
    bool openComponent(const QString &definitionKey, QString *error = nullptr);

    // Key the component is saved under; empty while there is no target library.
    QString definitionKey() const;
    QString targetLibrary() const { return m_targetLibrary; }
    LibraryScope targetScope() const { return m_targetScope; }
    bool isReadOnlySource() const { return m_targetLibrary.isEmpty(); }
    bool isModified() const;

    ComponentDefinition definition() const { return m_definition; }
    // Replaces the whole definition as one undoable step.
    void setDefinition(const ComponentDefinition &definition, const QString &description = QString());

    struct Problem {
        enum class Target { None, Id, Name, Category, Prefix, NetName, Role, Pin, Parameter, Graphic, Model, Library };
        QString text;
        Target target = Target::None;
        QString name;  // pin or parameter name
        int index = -1; // graphic, parameter or model index
    };
    // Everything that prevents saving; empty when the component can be saved.
    QList<Problem> problems() const;
    QStringList problemTexts() const;
    bool canSave() const;

    bool save(QString *error = nullptr);
    // Saves into another library, which is created if needed, and continues
    // editing that copy. Used for built-in components.
    bool saveCopy(const QString &libraryName, LibraryScope scope, QString *error = nullptr);
    // Asks for the library, then saves a copy. Returns false if cancelled or failed.
    bool saveCopyInteractive();

    QUndoStack *undoStack() const { return m_undo; }
    SymbolCanvas *canvas() const { return m_canvas; }

Q_SIGNALS:
    void componentSaved(const QString &definitionKey);

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    friend class DefinitionCommand;

    void createActions();
    void createGeneralTab(QTabWidget *tabs);
    void createPinsTab(QTabWidget *tabs);
    void createParametersTab(QTabWidget *tabs);
    void createModelsTab(QTabWidget *tabs);
    void createDocks();
    void updateToolIcons();

    // Applies a definition from the undo stack and refreshes every view.
    void applyDefinition(const ComponentDefinition &definition);
    // Records a change made through one of the panels; consecutive changes
    // with the same non-empty mergeKey become one undo step.
    void edit(const std::function<void(ComponentDefinition &)> &change, const QString &description,
              const QString &mergeKey = QString());
    void resetDefinition(const ComponentDefinition &definition);

    void refreshGeneral();
    void refreshPins();
    void refreshParameters();
    void refreshModels();
    void refreshModelFields();
    void refreshProperties();
    void rebuildProperties();
    void revalidate();
    void updateTitle();
    void updateBanner();
    void updateCategoryCompletion();

    void renamePin(const QString &oldName, const QString &newName);
    void addPin();
    void removeSelectedPin();
    void movePin(int delta);
    void onPinRowsMoved();

    void addParameter();
    void removeSelectedParameter();
    void moveParameter(int delta);
    void onParameterItemChanged(int row, int column);
    void setParameterKind(int row, int kind);

    void addModel(const QString &kind);
    void removeSelectedModel();
    void moveModel(int delta);
    void onModelFieldChanged(int row, int column);
    void applyModelJson();

    void showProblem(QListWidgetItem *item);
    int currentModelRow() const;

    // Property rows for the selected element.
    using Refresher = std::function<void()>;
    void addPointRow(const QString &label, const std::function<QPoint(const ComponentDefinition &)> &get,
                     const std::function<void(ComponentDefinition &, QPoint)> &set, int step, const QString &mergeKey);
    void addIntRow(const QString &label, const std::function<int(const ComponentDefinition &)> &get,
                   const std::function<void(ComponentDefinition &, int)> &set, int minimum, int maximum, int step,
                   const QString &mergeKey, const QString &suffix = QString());
    void addDoubleRow(const QString &label, const std::function<double(const ComponentDefinition &)> &get,
                      const std::function<void(ComponentDefinition &, double)> &set, double minimum, double maximum,
                      const QString &mergeKey, const QString &suffix = QString());
    QLineEdit *addTextRow(const QString &label, const std::function<QString(const ComponentDefinition &)> &get,
                          const std::function<void(ComponentDefinition &, const QString &)> &set,
                          const QString &mergeKey, bool commitOnFinish = false);
    void addChoiceRow(const QString &label, const QStringList &choices,
                      const std::function<int(const ComponentDefinition &)> &get,
                      const std::function<void(ComponentDefinition &, int)> &set);
    void addPointsRow(const QString &label, int graphicIndex);

    LibraryManager *m_libraries;
    ComponentDefinition m_definition;
    QUndoStack *m_undo;
    bool m_applying = false;

    QString m_targetLibrary;
    LibraryScope m_targetScope = LibraryScope::User;
    QString m_sourceKey;   // the component as opened; empty for a new one
    QString m_originalId;  // id in the target library before editing; empty if not saved there yet
    bool m_neverSaved = true;

    SymbolCanvas *m_canvas;
    SymbolPreview *m_orientationPreview;
    QWidget *m_banner;
    QLabel *m_bannerText;
    QPushButton *m_bannerButton;
    QLabel *m_cursorLabel;
    QLabel *m_targetLabel;

    QActionGroup *m_toolGroup;
    QList<std::pair<SymbolCanvas::Tool, QAction *>> m_toolActions;
    QAction *m_saveAction = nullptr;
    QAction *m_saveCopyAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_duplicateAction = nullptr;
    QComboBox *m_gridCombo = nullptr;

    // General
    QLineEdit *m_idEdit;
    QLineEdit *m_nameEdit;
    QLineEdit *m_categoryEdit;
    QCompleter *m_categoryCompleter;
    QLineEdit *m_descriptionEdit;
    QLineEdit *m_prefixEdit;
    QLineEdit *m_netNameEdit;
    QComboBox *m_roleCombo;

    // Pins and parameters
    QListWidget *m_pinList;
    QTableWidget *m_parameterTable;

    // Models
    QListWidget *m_modelList;
    QComboBox *m_modelKind;
    QTableWidget *m_modelFields;
    QPlainTextEdit *m_modelJson;
    QLabel *m_modelHint;
    QTabWidget *m_modelTabs;

    QTabWidget *m_tabs;
    QListWidget *m_problemList;

    // Selected element properties
    QWidget *m_propertyHost;
    QFormLayout *m_propertyForm = nullptr;
    QList<Refresher> m_propertyRefreshers;
    QString m_propertyKey; // identifies what the property rows were built for
    QLineEdit *m_firstTextField = nullptr;
};

} // namespace lvd
