#include "gui/NewProjectDialog.h"

#include "core/Project.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace lvd {

NewProjectDialog::NewProjectDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("New Project"));

    m_name = new QLineEdit(this);
    m_location = new QLineEdit(this);
    m_location->setMinimumWidth(360);
    const QString defaultLocation = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    m_location->setText(QDir::toNativeSeparators(
        QSettings().value(QStringLiteral("newProjectLocation"), defaultLocation).toString()));

    auto *browseButton = new QPushButton(tr("Browse..."), this);
    auto *locationRow = new QHBoxLayout;
    locationRow->addWidget(m_location);
    locationRow->addWidget(browseButton);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));

    auto *form = new QFormLayout;
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Location:"), locationRow);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_status);
    layout->addWidget(m_buttons);

    connect(browseButton, &QPushButton::clicked, this, &NewProjectDialog::browse);
    connect(m_name, &QLineEdit::textChanged, this, &NewProjectDialog::validate);
    connect(m_location, &QLineEdit::textChanged, this, &NewProjectDialog::validate);
    connect(m_buttons, &QDialogButtonBox::accepted, this, [this] {
        QSettings().setValue(QStringLiteral("newProjectLocation"), QDir::fromNativeSeparators(m_location->text()));
        accept();
    });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    validate();
}

QString NewProjectDialog::projectName() const
{
    return m_name->text();
}

QString NewProjectDialog::projectDirectory() const
{
    return QDir(QDir::fromNativeSeparators(m_location->text())).filePath(projectName());
}

void NewProjectDialog::browse()
{
    const QString directory = QFileDialog::getExistingDirectory(this, tr("Project Location"), m_location->text());
    if (!directory.isEmpty())
        m_location->setText(QDir::toNativeSeparators(directory));
}

void NewProjectDialog::validate()
{
    QString problem;
    bool valid = true;
    if (m_name->text().isEmpty()) {
        valid = false; // nothing typed yet: no message
    } else if (!Project::validateName(m_name->text(), &problem)) {
        valid = false;
    } else if (m_location->text().trimmed().isEmpty()) {
        valid = false;
        problem = tr("Choose a location.");
    } else if (QFileInfo::exists(projectDirectory())) {
        valid = false;
        problem = tr("%1 already exists.").arg(QDir::toNativeSeparators(projectDirectory()));
    }

    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
    if (valid)
        m_status->setText(tr("The project is created in %1").arg(QDir::toNativeSeparators(projectDirectory())));
    else
        m_status->setText(problem);
}

} // namespace lvd
