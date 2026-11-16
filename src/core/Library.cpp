#include "core/Library.h"

#include "core/JsonFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>

namespace lvd {

QString Library::formatId()
{
    return QStringLiteral("lvdrfcad-library");
}

QString Library::fileExtension()
{
    return QStringLiteral("lvdlib");
}

std::optional<Library> Library::load(const QString &path, QString *error, QStringList *warnings)
{
    const auto object = readJsonFile(path, error);
    if (!object)
        return std::nullopt;
    if (!checkFileHeader(*object, formatId(), FormatVersion, path, error))
        return std::nullopt;

    Library library;
    library.filePath = QFileInfo(path).absoluteFilePath();
    library.name = object->value(QStringLiteral("name")).toString();
    library.description = object->value(QStringLiteral("description")).toString();
    if (library.name.isEmpty() || library.name.contains(QLatin1Char(':'))) {
        setError(error, QStringLiteral("%1 has no valid library name").arg(path));
        return std::nullopt;
    }

    for (const QJsonValue &value : object->value(QStringLiteral("components")).toArray()) {
        QString componentError;
        auto definition = ComponentDefinition::fromJson(value.toObject(), &componentError);
        if (!definition) {
            if (warnings)
                warnings->append(QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(path), componentError));
            continue;
        }
        if (library.component(definition->id)) {
            if (warnings)
                warnings->append(QStringLiteral("%1: component \"%2\" is defined twice; the first is used")
                                     .arg(QDir::toNativeSeparators(path), definition->id));
            continue;
        }
        library.components.push_back(std::make_shared<const ComponentDefinition>(std::move(*definition)));
    }
    return library;
}

QJsonObject Library::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("format"), formatId());
    object.insert(QStringLiteral("version"), FormatVersion);
    object.insert(QStringLiteral("name"), name);
    if (!description.isEmpty())
        object.insert(QStringLiteral("description"), description);
    QJsonArray array;
    for (const DefinitionPtr &component : components)
        array.append(component->toJson());
    object.insert(QStringLiteral("components"), array);
    return object;
}

bool Library::save(QString *error) const
{
    if (filePath.isEmpty()) {
        setError(error, QStringLiteral("Library \"%1\" has no file").arg(name));
        return false;
    }
    return writeJsonFile(filePath, toJson(), error);
}

DefinitionPtr Library::component(const QString &id) const
{
    for (const DefinitionPtr &candidate : components) {
        if (candidate->id == id)
            return candidate;
    }
    return nullptr;
}

void Library::setComponent(const ComponentDefinition &definition)
{
    auto replacement = std::make_shared<const ComponentDefinition>(definition);
    for (DefinitionPtr &candidate : components) {
        if (candidate->id == definition.id) {
            candidate = std::move(replacement);
            return;
        }
    }
    components.push_back(std::move(replacement));
}

bool Library::removeComponent(const QString &id)
{
    const auto it = std::find_if(components.begin(), components.end(),
                                 [&id](const DefinitionPtr &candidate) { return candidate->id == id; });
    if (it == components.end())
        return false;
    components.erase(it);
    return true;
}

QString libraryScopeName(LibraryScope scope)
{
    switch (scope) {
    case LibraryScope::Project: return QStringLiteral("project");
    case LibraryScope::User: return QStringLiteral("user");
    case LibraryScope::BuiltIn: return QStringLiteral("built-in");
    }
    return {};
}

LibraryManager::LibraryManager(QObject *parent) : QObject(parent)
{
}

QString LibraryManager::defaultBuiltInPath()
{
    if (QCoreApplication::instance()) {
        const QString installed = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("libraries"));
        if (QFileInfo(installed).isDir())
            return installed;
    }
    return QStringLiteral(LVD_SOURCE_DIR "/libraries");
}

QString LibraryManager::searchPath(LibraryScope scope) const
{
    return m_paths[static_cast<int>(scope)];
}

void LibraryManager::setSearchPath(LibraryScope scope, const QString &directory)
{
    QString &path = m_paths[static_cast<int>(scope)];
    if (path == directory)
        return;
    path = directory;
    reload();
}

void LibraryManager::reload()
{
    m_entries.clear();
    m_warnings.clear();
    QStringList seen;
    for (const LibraryScope scope : {LibraryScope::Project, LibraryScope::User, LibraryScope::BuiltIn}) {
        const QString directory = searchPath(scope);
        if (directory.isEmpty() || !QFileInfo(directory).isDir())
            continue;
        const QStringList files =
            QDir(directory).entryList({QStringLiteral("*.") + Library::fileExtension()}, QDir::Files, QDir::Name);
        for (const QString &file : files) {
            QString error;
            auto library = Library::load(QDir(directory).filePath(file), &error, &m_warnings);
            if (!library) {
                m_warnings.append(error);
                continue;
            }
            const bool shadowed = seen.contains(library->name);
            if (!shadowed)
                seen.append(library->name);
            else if (std::any_of(m_entries.begin(), m_entries.end(), [&](const Entry &entry) {
                         return entry.library.name == library->name && entry.scope == scope;
                     }))
                m_warnings.append(QStringLiteral("Library \"%1\" is defined twice in %2; %3 is ignored")
                                      .arg(library->name, QDir::toNativeSeparators(directory), file));
            m_entries.push_back(Entry{std::move(*library), scope, shadowed});
        }
    }
    Q_EMIT librariesChanged();
}

const Library *LibraryManager::library(const QString &name) const
{
    for (const Entry &entry : m_entries) {
        if (!entry.shadowed && entry.library.name == name)
            return &entry.library;
    }
    return nullptr;
}

LibraryScope LibraryManager::scopeOf(const QString &libraryName) const
{
    for (const Entry &entry : m_entries) {
        if (!entry.shadowed && entry.library.name == libraryName)
            return entry.scope;
    }
    return LibraryScope::BuiltIn;
}

QStringList LibraryManager::libraryNames() const
{
    QStringList names;
    for (const Entry &entry : m_entries) {
        if (!entry.shadowed)
            names.append(entry.library.name);
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

DefinitionPtr LibraryManager::find(const QString &key) const
{
    QString libraryName, componentId;
    if (!splitKey(key, &libraryName, &componentId))
        return nullptr;
    const Library *found = library(libraryName);
    return found ? found->component(componentId) : nullptr;
}

bool LibraryManager::saveLibrary(Library library, LibraryScope scope, QString *error)
{
    if (library.filePath.isEmpty()) {
        const QString directory = searchPath(scope);
        if (directory.isEmpty()) {
            setError(error, QStringLiteral("No %1 library folder is set").arg(libraryScopeName(scope)));
            return false;
        }
        if (!QDir().mkpath(directory)) {
            setError(error, QStringLiteral("Cannot create folder %1").arg(directory));
            return false;
        }
        library.filePath = QDir(directory).filePath(library.name + QLatin1Char('.') + Library::fileExtension());
    }
    if (!library.save(error))
        return false;
    reload();
    return true;
}

QString LibraryManager::makeKey(const QString &library, const QString &component)
{
    return library + QLatin1Char(':') + component;
}

bool LibraryManager::splitKey(const QString &key, QString *library, QString *component)
{
    const qsizetype colon = key.indexOf(QLatin1Char(':'));
    if (colon <= 0 || colon == key.size() - 1)
        return false;
    *library = key.left(colon);
    *component = key.mid(colon + 1);
    return true;
}

} // namespace lvd
