#include "library_ui/LibraryManagerDialog.h"

#include "library_ui/LibraryTargetDialog.h"
#include "library_ui/LibraryUiCommon.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace lvd {

namespace {

enum Column { NameColumn, ScopeColumn, CountColumn, ShadowColumn, WarningColumn, PathColumn };

} // namespace

LibraryManagerDialog::LibraryManagerDialog(LibraryManager *libraries, QWidget *parent)
    : QDialog(parent), m_libraries(libraries)
{
    setWindowTitle(tr("Manage Libraries"));

    auto *intro = new QLabel(tr("Libraries are found by name in this order; the first match wins: the project's "
                                "libraries folder, the user library folder, then the built-in libraries."),
                             this);
    intro->setWordWrap(true);

    m_table = new QTreeWidget(this);
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setHeaderLabels({tr("Library"), tr("Scope"), tr("Components"), tr("Hidden by"), tr("Warnings"), tr("File")});
    m_table->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->header()->setStretchLastSection(true);
    connect(m_table, &QTreeWidget::itemSelectionChanged, this, &LibraryManagerDialog::updateButtons);
    connect(m_table, &QTreeWidget::itemDoubleClicked, this, &LibraryManagerDialog::openSelectedFolder);

    auto *newButton = new QPushButton(tr("New Library..."), this);
    m_openFolder = new QPushButton(tr("Open Folder"), this);
    auto *reloadButton = new QPushButton(tr("Reload"), this);
    auto *tableButtons = new QHBoxLayout;
    tableButtons->addWidget(newButton);
    tableButtons->addWidget(m_openFolder);
    tableButtons->addStretch(1);
    tableButtons->addWidget(reloadButton);

    auto *folders = new QGroupBox(tr("Folders"), this);
    auto *folderForm = new QFormLayout(folders);
    m_userPath = new QLineEdit(folders);
    m_userPath->setPlaceholderText(tr("not set"));
    auto *browse = new QPushButton(tr("Browse..."), folders);
    m_applyUserPath = new QPushButton(tr("Apply"), folders);
    auto *userRow = new QHBoxLayout;
    userRow->addWidget(m_userPath, 1);
    userRow->addWidget(browse);
    userRow->addWidget(m_applyUserPath);
    folderForm->addRow(tr("User libraries:"), userRow);
    m_projectPath = new QLabel(folders);
    m_projectPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    folderForm->addRow(tr("Project libraries:"), m_projectPath);
    m_builtInPath = new QLabel(folders);
    m_builtInPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    folderForm->addRow(tr("Built-in libraries:"), m_builtInPath);

    m_warningsLabel = new QLabel(tr("Load warnings:"), this);
    m_warnings = new QPlainTextEdit(this);
    m_warnings->setReadOnly(true);
    m_warnings->setMaximumHeight(110);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_table, 1);
    layout->addLayout(tableButtons);
    layout->addWidget(folders);
    layout->addWidget(m_warningsLabel);
    layout->addWidget(m_warnings);
    layout->addWidget(buttons);
    resize(820, 560);

    connect(newButton, &QPushButton::clicked, this, &LibraryManagerDialog::newLibrary);
    connect(m_openFolder, &QPushButton::clicked, this, &LibraryManagerDialog::openSelectedFolder);
    connect(reloadButton, &QPushButton::clicked, m_libraries, &LibraryManager::reload);
    connect(browse, &QPushButton::clicked, this, &LibraryManagerDialog::browseUserFolder);
    connect(m_applyUserPath, &QPushButton::clicked, this,
            [this] { setUserLibraryPath(QDir::fromNativeSeparators(m_userPath->text().trimmed())); });
    connect(m_userPath, &QLineEdit::textChanged, this, &LibraryManagerDialog::updateButtons);
    connect(m_libraries, &LibraryManager::librariesChanged, this, &LibraryManagerDialog::refresh);
    refresh();
}

int LibraryManagerDialog::libraryCount() const
{
    return m_table->topLevelItemCount();
}

void LibraryManagerDialog::refresh()
{
    const QString selectedFile = m_table->currentItem() ? m_table->currentItem()->data(PathColumn, Qt::UserRole).toString()
                                                         : QString();
    m_table->clear();
    const QStringList warnings = m_libraries->warnings();
    const QColor greyed = palette().color(QPalette::Disabled, QPalette::Text);
    for (const LibraryManager::Entry &entry : m_libraries->entries()) {
        const QString nativePath = QDir::toNativeSeparators(entry.library.filePath);
        QStringList own;
        for (const QString &warning : warnings) {
            if (warning.contains(nativePath, Qt::CaseInsensitive) || warning.contains(entry.library.filePath, Qt::CaseInsensitive)
                || warning.contains(QFileInfo(entry.library.filePath).fileName()))
                own.append(warning);
        }
        auto *item = new QTreeWidgetItem(m_table);
        item->setText(NameColumn, entry.library.name);
        item->setText(ScopeColumn, scopeDisplayName(entry.scope) + (isWritableScope(entry.scope) ? QString() : tr(", read-only")));
        item->setText(CountColumn, QString::number(entry.library.components.size()));
        item->setTextAlignment(CountColumn, Qt::AlignRight | Qt::AlignVCenter);
        if (const LibraryManager::Entry *winner = shadowingEntry(*m_libraries, entry)) {
            item->setText(ShadowColumn, tr("%1 library").arg(scopeDisplayName(winner->scope)));
            item->setToolTip(ShadowColumn, QDir::toNativeSeparators(winner->library.filePath));
            for (int column = 0; column <= PathColumn; ++column)
                item->setForeground(column, greyed);
        }
        if (!own.isEmpty()) {
            item->setIcon(WarningColumn, style()->standardIcon(QStyle::SP_MessageBoxWarning));
            item->setText(WarningColumn, QString::number(own.size()));
            item->setToolTip(WarningColumn, own.join(QLatin1Char('\n')));
        }
        item->setText(PathColumn, nativePath);
        item->setData(PathColumn, Qt::UserRole, entry.library.filePath);
        item->setToolTip(NameColumn, entry.library.description);
        if (entry.library.filePath == selectedFile)
            m_table->setCurrentItem(item);
    }

    if (!m_userPath->hasFocus() || m_userPath->text().isEmpty())
        m_userPath->setText(QDir::toNativeSeparators(m_libraries->searchPath(LibraryScope::User)));
    const QString projectPath = m_libraries->searchPath(LibraryScope::Project);
    m_projectPath->setText(projectPath.isEmpty() ? tr("no project open") : QDir::toNativeSeparators(projectPath));
    m_builtInPath->setText(QDir::toNativeSeparators(m_libraries->searchPath(LibraryScope::BuiltIn)));

    m_warnings->setPlainText(warnings.join(QLatin1Char('\n')));
    m_warnings->setVisible(!warnings.isEmpty());
    m_warningsLabel->setVisible(!warnings.isEmpty());
    updateButtons();
}

void LibraryManagerDialog::updateButtons()
{
    m_openFolder->setEnabled(m_table->currentItem() != nullptr);
    m_applyUserPath->setEnabled(QDir::fromNativeSeparators(m_userPath->text().trimmed())
                                != m_libraries->searchPath(LibraryScope::User));
}

bool LibraryManagerDialog::createLibrary(const QString &name, LibraryScope scope, const QString &description,
                                         QString *error)
{
    QString problem;
    if (!LibraryTargetDialog::checkNewLibrary(*m_libraries, name, scope, &problem)) {
        if (error)
            *error = problem;
        return false;
    }
    Library library;
    library.name = name;
    library.description = description;
    return m_libraries->saveLibrary(library, scope, error);
}

void LibraryManagerDialog::newLibrary()
{
    LibraryTargetDialog dialog(m_libraries, LibraryTargetDialog::Mode::NewOnly, this);
    dialog.setExplanation(tr("A library is a .lvdlib file in the user or project library folder. Libraries are plain "
                             "files and can be shared."));
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    if (!createLibrary(dialog.libraryName(), dialog.scope(), dialog.description(), &error))
        QMessageBox::warning(this, tr("New Library"), error);
}

void LibraryManagerDialog::openSelectedFolder()
{
    const QTreeWidgetItem *item = m_table->currentItem();
    if (!item)
        return;
    const QString folder = QFileInfo(item->data(PathColumn, Qt::UserRole).toString()).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void LibraryManagerDialog::browseUserFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("User Library Folder"), m_userPath->text());
    if (folder.isEmpty())
        return;
    m_userPath->setText(QDir::toNativeSeparators(folder));
    setUserLibraryPath(folder);
}

void LibraryManagerDialog::setUserLibraryPath(const QString &path)
{
    const QString cleaned = path.isEmpty() ? QString() : QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (cleaned == m_libraries->searchPath(LibraryScope::User))
        return;
    if (!cleaned.isEmpty() && !QFileInfo(cleaned).isDir() && !QDir().mkpath(cleaned)) {
        QMessageBox::warning(this, tr("User Library Folder"), tr("Cannot create the folder %1").arg(QDir::toNativeSeparators(cleaned)));
        return;
    }
    m_libraries->setSearchPath(LibraryScope::User, cleaned);
    m_userPath->setText(QDir::toNativeSeparators(cleaned));
    updateButtons();
    Q_EMIT userLibraryPathChanged(cleaned);
}

} // namespace lvd
