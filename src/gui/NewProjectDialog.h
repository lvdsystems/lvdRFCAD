#pragma once

#include <QDialog>

class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace lvd {

// Asks for a project name and the folder in which the project folder is created.
class NewProjectDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NewProjectDialog(QWidget *parent = nullptr);

    QString projectName() const;
    QString projectDirectory() const; // <location>/<name>

private:
    void browse();
    void validate();

    QLineEdit *m_name;
    QLineEdit *m_location;
    QLabel *m_status;
    QDialogButtonBox *m_buttons;
};

} // namespace lvd
