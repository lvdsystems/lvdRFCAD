#include "antenna/AntennaLibrary.h"

#include "core/JsonFile.h"
#include "core/Library.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace lvd::ant {

namespace {

struct NamedKind {
    AntennaEntryKind kind;
    const char *id;
    const char *name;
};

constexpr NamedKind kKinds[] = {
    {AntennaEntryKind::Element, "element", "Element models"},
    {AntennaEntryKind::Pattern, "pattern", "Measured and vendor patterns"},
    {AntennaEntryKind::TrModule, "tr-module", "T/R modules"},
    {AntennaEntryKind::Array, "array", "Array presets"},
};

} // namespace

QString antennaEntryKindId(AntennaEntryKind kind)
{
    for (const auto &k : kKinds)
        if (k.kind == kind)
            return QString::fromLatin1(k.id);
    return QStringLiteral("element");
}

QString antennaEntryKindName(AntennaEntryKind kind)
{
    for (const auto &k : kKinds)
        if (k.kind == kind)
            return QString::fromLatin1(k.name);
    return {};
}

QString AntennaLibrary::formatId()
{
    return QStringLiteral("lvdrfcad-antenna-library");
}

QString AntennaLibrary::fileExtension()
{
    return QStringLiteral("lvdantlib");
}

std::optional<AntennaLibrary> AntennaLibrary::load(const QString &path, QString *error)
{
    const auto object = readJsonFile(path, error);
    if (!object)
        return std::nullopt;
    if (!checkFileHeader(*object, formatId(), FormatVersion, path, error))
        return std::nullopt;
    AntennaLibrary library;
    library.filePath = QFileInfo(path).absoluteFilePath();
    library.name = object->value(QStringLiteral("name")).toString(QFileInfo(path).completeBaseName());
    library.description = object->value(QStringLiteral("description")).toString();
    for (const QJsonValue &v : object->value(QStringLiteral("entries")).toArray()) {
        const QJsonObject e = v.toObject();
        AntennaLibraryEntry entry;
        entry.id = e.value(QStringLiteral("id")).toString();
        if (entry.id.isEmpty())
            continue;
        entry.name = e.value(QStringLiteral("name")).toString(entry.id);
        entry.description = e.value(QStringLiteral("description")).toString();
        const QString kind = e.value(QStringLiteral("kind")).toString();
        for (const auto &k : kKinds)
            if (kind == QLatin1String(k.id))
                entry.kind = k.kind;
        entry.data = e.value(QStringLiteral("data")).toObject();
        library.entries.push_back(entry);
    }
    return library;
}

QJsonObject AntennaLibrary::toJson() const
{
    QJsonArray list;
    for (const AntennaLibraryEntry &e : entries)
        list.append(QJsonObject{{QStringLiteral("id"), e.id},
                                {QStringLiteral("name"), e.name},
                                {QStringLiteral("description"), e.description},
                                {QStringLiteral("kind"), antennaEntryKindId(e.kind)},
                                {QStringLiteral("data"), e.data}});
    return QJsonObject{{QStringLiteral("format"), formatId()},
                       {QStringLiteral("version"), FormatVersion},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("entries"), list}};
}

bool AntennaLibrary::save(QString *error) const
{
    return writeJsonFile(filePath, toJson(), error);
}

const AntennaLibraryEntry *AntennaLibrary::entry(const QString &id) const
{
    for (const AntennaLibraryEntry &e : entries)
        if (e.id == id)
            return &e;
    return nullptr;
}

void AntennaLibrary::setEntry(const AntennaLibraryEntry &entry)
{
    for (AntennaLibraryEntry &e : entries)
        if (e.id == entry.id) {
            e = entry;
            return;
        }
    entries.push_back(entry);
}

QString AntennaLibrary::directory() const
{
    return QFileInfo(filePath).absolutePath();
}

AntennaLibraryManager::AntennaLibraryManager(LibraryManager *components, QObject *parent)
    : QObject(parent), m_components(components)
{
    if (m_components)
        connect(m_components, &LibraryManager::librariesChanged, this, &AntennaLibraryManager::reload);
    reload();
}

void AntennaLibraryManager::reload()
{
    m_libraries.clear();
    m_warnings.clear();
    QStringList seen;
    if (m_components) {
        for (const LibraryScope scope : {LibraryScope::Project, LibraryScope::User, LibraryScope::BuiltIn}) {
            const QString directory = m_components->searchPath(scope);
            if (directory.isEmpty() || !QFileInfo(directory).isDir())
                continue;
            for (const QString &file : QDir(directory).entryList({QStringLiteral("*.") + AntennaLibrary::fileExtension()},
                                                                 QDir::Files, QDir::Name)) {
                QString error;
                auto library = AntennaLibrary::load(QDir(directory).filePath(file), &error);
                if (!library) {
                    m_warnings << error;
                    continue;
                }
                if (seen.contains(library->name))
                    continue; // shadowed by a higher-precedence folder
                seen << library->name;
                m_libraries.push_back(std::move(*library));
            }
        }
    }
    Q_EMIT librariesChanged();
}

const AntennaLibrary *AntennaLibraryManager::library(const QString &name) const
{
    for (const AntennaLibrary &l : m_libraries)
        if (l.name == name)
            return &l;
    return nullptr;
}

AntennaLibraryManager::Found AntennaLibraryManager::find(const QString &key) const
{
    const qsizetype colon = key.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return {};
    const AntennaLibrary *l = library(key.left(colon));
    if (!l)
        return {};
    return {l->entry(key.mid(colon + 1)), l};
}

QString AntennaLibraryManager::makeKey(const QString &library, const QString &entry)
{
    return library + QLatin1Char(':') + entry;
}

bool AntennaLibraryManager::saveLibrary(const AntennaLibrary &library, QString *error)
{
    AntennaLibrary copy = library;
    if (copy.filePath.isEmpty()) {
        const QString directory = m_components ? m_components->searchPath(LibraryScope::User) : QString();
        if (directory.isEmpty() || !QDir().mkpath(directory)) {
            setError(error, QStringLiteral("No user library folder is available"));
            return false;
        }
        copy.filePath = QDir(directory).filePath(copy.name + QLatin1Char('.') + AntennaLibrary::fileExtension());
    }
    if (!copy.save(error))
        return false;
    reload();
    return true;
}

} // namespace lvd::ant
