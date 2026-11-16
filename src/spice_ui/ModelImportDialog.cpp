#include "spice_ui/ModelImportDialog.h"

#include "core/Library.h"
#include "core/Project.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lvd::spiceui {

ModelImportDialog::ModelImportDialog(LibraryManager *libraries, Project *project, QWidget *parent)
    : QDialog(parent), m_libraries(libraries), m_project(project)
{
    setWindowTitle(tr("Import SPICE Models"));
    resize(720, 520);
    auto *layout = new QVBoxLayout(this);

    auto *fileRow = new QHBoxLayout;
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(tr("A .lib, .mod, .cir or .subckt file from a vendor"));
    auto *browse = new QPushButton(tr("Browse…"), this);
    fileRow->addWidget(m_pathEdit, 1);
    fileRow->addWidget(browse);
    layout->addLayout(fileRow);

    m_entries = new QTreeWidget(this);
    m_entries->setHeaderLabels({tr("Name"), tr("Kind"), tr("Pins / type"), tr("Section")});
    m_entries->setRootIsDecorated(false);
    m_entries->header()->setStretchLastSection(true);
    layout->addWidget(m_entries, 1);

    auto *form = new QFormLayout;
    m_library = new QComboBox(this);
    m_library->setEditable(true);
    for (const LibraryManager::Entry &entry : libraries->entries())
        if (entry.scope != LibraryScope::BuiltIn && !entry.shadowed)
            m_library->addItem(entry.library.name);
    if (m_library->findText(QStringLiteral("imported")) < 0)
        m_library->addItem(QStringLiteral("imported"));
    m_library->setCurrentText(QStringLiteral("imported"));
    m_library->setToolTip(tr("An existing user or project library, or the name of a new one."));
    form->addRow(tr("Into library:"), m_library);
    m_scope = new QComboBox(this);
    m_scope->addItem(tr("Project library"), int(LibraryScope::Project));
    m_scope->addItem(tr("User library (all projects)"), int(LibraryScope::User));
    if (!project)
        m_scope->setCurrentIndex(1);
    m_scope->setItemData(0, project ? QVariant() : QVariant(0), Qt::UserRole - 1); // disabled without a project
    form->addRow(tr("New library in:"), m_scope);
    m_copy = new QCheckBox(tr("Copy the model file into the project's \"models\" folder"), this);
    m_copy->setChecked(project != nullptr);
    m_copy->setEnabled(project != nullptr);
    form->addRow(QString(), m_copy);
    layout->addLayout(form);

    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    layout->addWidget(m_message);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Import"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ModelImportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("SPICE Model File"), m_project ? m_project->directory() : QString(),
            tr("SPICE models (*.lib *.mod *.cir *.sub *.subckt *.sp *.spi *.txt);;All files (*)"));
        QString error;
        if (!path.isEmpty() && !openFile(path, &error))
            m_message->setText(error);
    });
}

bool ModelImportDialog::openFile(const QString &path, QString *error)
{
    QString readError;
    const auto file = spice::readSpiceModelFile(path, &readError);
    if (!file) {
        if (error)
            *error = readError;
        return false;
    }
    m_path = path;
    m_file = *file;
    m_pathEdit->setText(QDir::toNativeSeparators(path));
    m_entries->clear();
    for (const spice::SpiceModelEntry &entry : m_file.entries) {
        auto *item = new QTreeWidgetItem(m_entries);
        item->setText(0, entry.name);
        item->setText(1, entry.kind == spice::SpiceModelEntry::Kind::Model ? tr("model") : tr("subcircuit"));
        item->setText(2, entry.kind == spice::SpiceModelEntry::Kind::Model ? entry.type : entry.pins.join(QLatin1Char(' ')));
        item->setText(3, entry.section);
        item->setCheckState(0, Qt::Checked);
    }
    for (int c = 0; c < 3; ++c)
        m_entries->resizeColumnToContents(c);
    QStringList notes = m_file.errors + m_file.warnings;
    notes.prepend(tr("%n model(s) and subcircuit(s) found.", nullptr, int(m_file.entries.size())));
    m_message->setText(notes.join(QLatin1Char('\n')));
    if (!m_file.errors.isEmpty()) {
        if (error)
            *error = m_file.errors.join(QLatin1Char('\n'));
        return false;
    }
    return true;
}

void ModelImportDialog::setLibraryName(const QString &name)
{
    m_library->setCurrentText(name);
}

void ModelImportDialog::setScopeIsProject(bool project)
{
    m_scope->setCurrentIndex(project ? 0 : 1);
}

QMap<QString, ComponentDefinition> ModelImportDialog::templates() const
{
    QMap<QString, ComponentDefinition> map;
    const QList<QPair<QString, QString>> keys{{QStringLiteral("D"), QStringLiteral("spice-semiconductors:diode")},
                                              {QStringLiteral("NPN"), QStringLiteral("spice-semiconductors:npn")},
                                              {QStringLiteral("PNP"), QStringLiteral("spice-semiconductors:pnp")},
                                              {QStringLiteral("NMOS"), QStringLiteral("spice-semiconductors:nmos")},
                                              {QStringLiteral("PMOS"), QStringLiteral("spice-semiconductors:pmos")},
                                              {QStringLiteral("NJF"), QStringLiteral("spice-semiconductors:njf")},
                                              {QStringLiteral("PJF"), QStringLiteral("spice-semiconductors:pjf")}};
    for (const auto &[type, key] : keys)
        if (const DefinitionPtr definition = m_libraries->find(key))
            map.insert(type, *definition);
    return map;
}

bool ModelImportDialog::import(QString *error)
{
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (m_path.isEmpty())
        return fail(tr("Choose a model file first."));
    const QString libraryName = m_library->currentText().trimmed();
    if (libraryName.isEmpty())
        return fail(tr("Name the library."));

    // The include path: a copy inside the project (relative), or the file itself.
    QString includePath = QDir::fromNativeSeparators(QFileInfo(m_path).absoluteFilePath());
    if (m_project && m_copy->isChecked()) {
        const QDir models(QDir(m_project->directory()).filePath(QStringLiteral("models")));
        if (!models.exists() && !QDir().mkpath(models.path()))
            return fail(tr("The folder %1 cannot be created.").arg(QDir::toNativeSeparators(models.path())));
        const QString target = models.filePath(QFileInfo(m_path).fileName());
        if (QFileInfo(target).absoluteFilePath() != QFileInfo(m_path).absoluteFilePath()) {
            QFile::remove(target);
            if (!QFile::copy(m_path, target))
                return fail(tr("%1 cannot be copied into the project.").arg(QFileInfo(m_path).fileName()));
        }
        includePath = QStringLiteral("models/") + QFileInfo(m_path).fileName();
    }

    Library library;
    LibraryScope scope = LibraryScope(m_scope->currentData().toInt());
    if (const Library *existing = m_libraries->library(libraryName)) {
        scope = m_libraries->scopeOf(libraryName);
        if (scope == LibraryScope::BuiltIn)
            return fail(tr("\"%1\" is a built-in library; choose another name.").arg(libraryName));
        library = *existing;
    } else {
        library.name = libraryName;
        library.description = tr("SPICE models imported from vendor files");
    }
    if (scope == LibraryScope::Project && !m_project)
        return fail(tr("Open a project to use a project library."));

    // A user library serves every project: it names the file absolutely.
    if (scope == LibraryScope::User && QFileInfo(includePath).isRelative() && m_project)
        includePath = QDir::fromNativeSeparators(QDir(m_project->directory()).absoluteFilePath(includePath));

    const QMap<QString, ComponentDefinition> base = templates();
    int count = 0;
    for (int i = 0; i < m_entries->topLevelItemCount(); ++i) {
        if (m_entries->topLevelItem(i)->checkState(0) != Qt::Checked || i >= m_file.entries.size())
            continue;
        ComponentDefinition definition = spice::proposeComponent(m_file.entries.at(i), includePath, base);
        const QStringList problems = definition.validate();
        if (!problems.isEmpty())
            return fail(tr("%1: %2").arg(definition.name, problems.join(QStringLiteral("; "))));
        library.setComponent(definition);
        ++count;
    }
    if (count == 0)
        return fail(tr("Nothing is checked."));
    QString saveError;
    if (!m_libraries->saveLibrary(library, scope, &saveError))
        return fail(saveError);
    m_summary = tr("%n component(s) imported into the library \"%1\".", nullptr, count).arg(libraryName);
    return true;
}

void ModelImportDialog::accept()
{
    QString error;
    if (!import(&error)) {
        m_message->setText(error);
        return;
    }
    QDialog::accept();
}

} // namespace lvd::spiceui
