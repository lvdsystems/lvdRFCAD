#pragma once

#include "core/Library.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

namespace lvd {

// Lists every library found in the project, user and built-in folders, and
// creates new libraries. The user library folder can be changed here; the
// main window stores it in the settings when userLibraryPathChanged() is emitted.
class LibraryManagerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LibraryManagerDialog(LibraryManager *libraries, QWidget *parent = nullptr);

    // Creates an empty library file. Returns false with *error set.
    bool createLibrary(const QString &name, LibraryScope scope, const QString &description = QString(),
                       QString *error = nullptr);
    // Sets the user library folder and reloads the libraries.
    void setUserLibraryPath(const QString &path);

    int libraryCount() const;

Q_SIGNALS:
    void userLibraryPathChanged(const QString &path);

private:
    void refresh();
    void newLibrary();
    void openSelectedFolder();
    void browseUserFolder();
    void updateButtons();

    LibraryManager *m_libraries;
    QTreeWidget *m_table;
    QLineEdit *m_userPath;
    QPushButton *m_applyUserPath;
    QLabel *m_projectPath;
    QLabel *m_builtInPath;
    QPlainTextEdit *m_warnings;
    QLabel *m_warningsLabel;
    QPushButton *m_openFolder;
};

} // namespace lvd
