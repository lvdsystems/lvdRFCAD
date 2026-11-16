#include "library_ui/LibraryTargetDialog.h"

#include "library_ui/LibraryUiCommon.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace lvd {

LibraryTargetDialog::LibraryTargetDialog(LibraryManager *libraries, Mode mode, QWidget *parent)
    : QDialog(parent), m_libraries(libraries), m_mode(mode)
{
    setWindowTitle(mode == Mode::NewOnly ? tr("New Library") : tr("Choose Library"));

    m_explanation = new QLabel(this);
    m_explanation->setWordWrap(true);
    m_explanation->hide();

    m_existingButton = new QRadioButton(tr("Add to an existing library:"), this);
    m_newButton = new QRadioButton(tr("Create a new library:"), this);
    m_existing = new QComboBox(this);
    for (const LibraryManager::Entry &entry : libraries->entries()) {
        if (entry.shadowed || !isWritableScope(entry.scope))
            continue;
        m_existing->addItem(tr("%1 (%2)").arg(entry.library.name, scopeDisplayName(entry.scope)),
                            QVariant::fromValue(QStringList{entry.library.name, QString::number(static_cast<int>(entry.scope))}));
    }

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("library name, for example my-parts"));
    m_scope = new QComboBox(this);
    for (const LibraryScope scope : {LibraryScope::User, LibraryScope::Project}) {
        const QString folder = libraries->searchPath(scope);
        const QString label = scope == LibraryScope::User ? tr("User library folder") : tr("Project library folder");
        m_scope->addItem(folder.isEmpty() ? tr("%1 (not set)").arg(label)
                                          : QStringLiteral("%1 - %2").arg(label, QDir::toNativeSeparators(folder)),
                         static_cast<int>(scope));
        if (folder.isEmpty()) {
            if (auto *model = qobject_cast<QStandardItemModel *>(m_scope->model()))
                model->item(m_scope->count() - 1)->setEnabled(false);
        }
    }
    if (libraries->searchPath(LibraryScope::User).isEmpty() && !libraries->searchPath(LibraryScope::Project).isEmpty())
        m_scope->setCurrentIndex(1);
    m_description = new QLineEdit(this);
    m_description->setPlaceholderText(tr("optional"));

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(m_status->fontMetrics().height() * 3);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto *newForm = new QFormLayout;
    newForm->setContentsMargins(20, 0, 0, 0);
    newForm->addRow(tr("Name:"), m_name);
    newForm->addRow(tr("Location:"), m_scope);
    newForm->addRow(tr("Description:"), m_description);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_explanation);
    if (mode == Mode::ExistingOrNew) {
        auto *existingRow = new QHBoxLayout;
        existingRow->addWidget(m_existingButton);
        existingRow->addWidget(m_existing, 1);
        layout->addLayout(existingRow);
        layout->addWidget(m_newButton);
    } else {
        m_existingButton->hide();
        m_existing->hide();
        m_newButton->hide();
    }
    layout->addLayout(newForm);
    layout->addWidget(m_status);
    layout->addWidget(m_buttons);
    setMinimumWidth(480);

    const bool haveExisting = m_existing->count() > 0;
    m_existingButton->setEnabled(haveExisting);
    m_existing->setEnabled(haveExisting);
    if (haveExisting && mode == Mode::ExistingOrNew)
        m_existingButton->setChecked(true);
    else
        m_newButton->setChecked(true);

    connect(m_existingButton, &QRadioButton::toggled, this, &LibraryTargetDialog::validate);
    connect(m_newButton, &QRadioButton::toggled, this, &LibraryTargetDialog::validate);
    connect(m_existing, &QComboBox::currentIndexChanged, this, &LibraryTargetDialog::validate);
    connect(m_name, &QLineEdit::textChanged, this, [this] {
        if (!m_name->text().isEmpty())
            m_newButton->setChecked(true);
        validate();
    });
    connect(m_scope, &QComboBox::currentIndexChanged, this, &LibraryTargetDialog::validate);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    validate();
}

void LibraryTargetDialog::setExplanation(const QString &text)
{
    m_explanation->setText(text);
    m_explanation->setVisible(!text.isEmpty());
}

void LibraryTargetDialog::setPreferredLibrary(const QString &name)
{
    for (int i = 0; i < m_existing->count(); ++i) {
        if (m_existing->itemData(i).toStringList().value(0) == name) {
            m_existing->setCurrentIndex(i);
            m_existingButton->setChecked(true);
            return;
        }
    }
}

void LibraryTargetDialog::setSuggestedNewLibrary(const QString &name)
{
    const QSignalBlocker blocker(m_name);
    m_name->setText(name);
    validate();
}

bool LibraryTargetDialog::isNewLibrary() const
{
    return m_mode == Mode::NewOnly || m_newButton->isChecked();
}

QString LibraryTargetDialog::libraryName() const
{
    if (isNewLibrary())
        return m_name->text().trimmed();
    return m_existing->currentData().toStringList().value(0);
}

LibraryScope LibraryTargetDialog::scope() const
{
    if (isNewLibrary())
        return static_cast<LibraryScope>(m_scope->currentData().toInt());
    return static_cast<LibraryScope>(m_existing->currentData().toStringList().value(1).toInt());
}

QString LibraryTargetDialog::description() const
{
    return m_description->text().trimmed();
}

bool LibraryTargetDialog::checkNewLibrary(const LibraryManager &libraries, const QString &name, LibraryScope scope,
                                          QString *error, QString *warning)
{
    if (!validateLibraryName(name, error))
        return false;
    const QString folder = libraries.searchPath(scope);
    if (folder.isEmpty()) {
        *error = scope == LibraryScope::User
                     ? tr("No user library folder is set. Set one under Manage Libraries.")
                     : tr("No project is open, so there is no project library folder.");
        return false;
    }
    if (entryInScope(libraries, name, scope)) {
        *error = tr("A %1 library named \"%2\" already exists.").arg(scopeDisplayName(scope), name);
        return false;
    }
    if (QFileInfo::exists(QDir(folder).filePath(name + QLatin1Char('.') + Library::fileExtension()))) {
        *error = tr("The file %1 already exists.")
                     .arg(QDir::toNativeSeparators(QDir(folder).filePath(name + QLatin1Char('.') + Library::fileExtension())));
        return false;
    }
    for (const LibraryManager::Entry &entry : libraries.entries()) {
        if (entry.library.name != name)
            continue;
        if (static_cast<int>(entry.scope) < static_cast<int>(scope)) {
            *error = tr("The %1 library \"%2\" has the same name and takes precedence, so the new library would be "
                        "hidden.")
                         .arg(scopeDisplayName(entry.scope), name);
            return false;
        }
        if (warning)
            *warning = tr("Warning: this hides the whole %1 library \"%2\" (all %n component(s)), not just the "
                          "components you add.",
                          nullptr, static_cast<int>(entry.library.components.size()))
                           .arg(scopeDisplayName(entry.scope), name);
    }
    return true;
}

void LibraryTargetDialog::validate()
{
    m_name->setEnabled(isNewLibrary());
    m_scope->setEnabled(isNewLibrary());
    m_description->setEnabled(isNewLibrary());

    bool valid = true;
    QString message;
    if (isNewLibrary()) {
        QString error, warning;
        if (m_name->text().trimmed().isEmpty()) {
            valid = false;
        } else if (!checkNewLibrary(*m_libraries, libraryName(), scope(), &error, &warning)) {
            valid = false;
            message = error;
        } else {
            const QString folder = m_libraries->searchPath(scope());
            message = tr("Creates %1")
                          .arg(QDir::toNativeSeparators(QDir(folder).filePath(libraryName() + QLatin1Char('.')
                                                                              + Library::fileExtension())));
            if (!warning.isEmpty())
                message += QStringLiteral("\n") + warning;
        }
    } else {
        valid = m_existing->currentIndex() >= 0;
        if (const LibraryManager::Entry *entry = entryInScope(*m_libraries, libraryName(), scope()))
            message = tr("Adds to %1").arg(QDir::toNativeSeparators(entry->library.filePath));
    }
    m_status->setText(message);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
}

} // namespace lvd
