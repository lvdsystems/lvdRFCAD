#pragma once

#include "core/Library.h"

#include <QDialog>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QRadioButton;

namespace lvd {

// Chooses where a component is saved: an existing user or project library,
// or a new library file in the user or project folder.
class LibraryTargetDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Mode { ExistingOrNew, NewOnly };

    explicit LibraryTargetDialog(LibraryManager *libraries, Mode mode = Mode::ExistingOrNew, QWidget *parent = nullptr);

    void setExplanation(const QString &text);
    // Preselects an existing writable library of this name, if there is one.
    void setPreferredLibrary(const QString &name);
    void setSuggestedNewLibrary(const QString &name);

    QString libraryName() const;
    LibraryScope scope() const;
    bool isNewLibrary() const;
    QString description() const; // of a new library

    // Checks a new library name and scope. Returns false with *error set when
    // it cannot be created; *warning is set for allowed but notable cases
    // (it hides a library of a lower-precedence scope).
    static bool checkNewLibrary(const LibraryManager &libraries, const QString &name, LibraryScope scope,
                                QString *error, QString *warning = nullptr);

private:
    void validate();

    LibraryManager *m_libraries;
    Mode m_mode;
    QLabel *m_explanation;
    QRadioButton *m_existingButton;
    QRadioButton *m_newButton;
    QComboBox *m_existing;
    QLineEdit *m_name;
    QComboBox *m_scope;
    QLineEdit *m_description;
    QLabel *m_status;
    QDialogButtonBox *m_buttons;
};

} // namespace lvd
