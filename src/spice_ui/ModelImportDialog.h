#pragma once

#include "spice/SpiceImport.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QCheckBox;
class QTreeWidget;

namespace lvd {
class LibraryManager;
class Project;
} // namespace lvd

namespace lvd::spiceui {

// Imports vendor SPICE models (requirement SP-7): reads a .lib, .mod, .cir
// or .subckt file, lists its models and subcircuits, and adds a component
// for each chosen one to a user or project library, with the symbol of the
// matching generic part or a box with the subcircuit's pins. With a project
// open, the model file is copied into the project's "models" folder so the
// project stays self-contained.
class ModelImportDialog : public QDialog
{
    Q_OBJECT
public:
    ModelImportDialog(LibraryManager *libraries, Project *project, QWidget *parent = nullptr);

    // Reads a file into the list (also used by the Browse button).
    bool openFile(const QString &path, QString *error = nullptr);
    QTreeWidget *entryList() const { return m_entries; }
    void setLibraryName(const QString &name);
    void setScopeIsProject(bool project);
    // Imports the checked entries; false with *error.
    bool import(QString *error = nullptr);
    QString summary() const { return m_summary; }

    void accept() override;

private:
    QMap<QString, ComponentDefinition> templates() const;

    LibraryManager *m_libraries;
    Project *m_project;
    QString m_path;
    spice::SpiceModelFile m_file;
    QLineEdit *m_pathEdit;
    QTreeWidget *m_entries;
    QComboBox *m_library;
    QComboBox *m_scope;
    QCheckBox *m_copy;
    QLabel *m_message;
    QString m_summary;
};

} // namespace lvd::spiceui
