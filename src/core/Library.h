#pragma once

#include "core/ComponentDefinition.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace lvd {

using DefinitionPtr = std::shared_ptr<const ComponentDefinition>;

// A .lvdlib file: a named set of component definitions.
class Library
{
public:
    static constexpr int FormatVersion = 1;
    static QString formatId();
    static QString fileExtension(); // without the dot

    // Components that fail to parse are skipped and reported in *warnings;
    // only an unreadable file or a bad header fails the whole library.
    static std::optional<Library> load(const QString &path, QString *error = nullptr, QStringList *warnings = nullptr);
    bool save(QString *error = nullptr) const;
    QJsonObject toJson() const;

    QString name;
    QString description;
    QString filePath;
    std::vector<DefinitionPtr> components;

    DefinitionPtr component(const QString &id) const;
    // Adds or replaces the component with the same id.
    void setComponent(const ComponentDefinition &definition);
    bool removeComponent(const QString &id);
};

enum class LibraryScope { Project, User, BuiltIn };

QString libraryScopeName(LibraryScope scope);

// Finds libraries in the project, user and built-in folders, in that order of
// precedence (docs/file-format.md, section 6).
class LibraryManager : public QObject
{
    Q_OBJECT
public:
    explicit LibraryManager(QObject *parent = nullptr);

    // The libraries folder of a source checkout or of an installation.
    static QString defaultBuiltInPath();

    QString searchPath(LibraryScope scope) const;
    // Changing a path reloads all libraries.
    void setSearchPath(LibraryScope scope, const QString &directory);
    void reload();

    struct Entry {
        Library library;
        LibraryScope scope;
        bool shadowed; // a library with the same name exists in a higher-precedence scope
    };
    const std::vector<Entry> &entries() const { return m_entries; }

    // The effective library of this name, or nullptr.
    const Library *library(const QString &name) const;
    LibraryScope scopeOf(const QString &libraryName) const;
    QStringList libraryNames() const; // effective libraries, sorted

    // Resolves "library:component". Returns nullptr if not found.
    DefinitionPtr find(const QString &key) const;

    // Writes the library to its filePath and reloads. A library without a
    // filePath is written to the folder of the given scope.
    bool saveLibrary(Library library, LibraryScope scope, QString *error = nullptr);

    QStringList warnings() const { return m_warnings; }

    static QString makeKey(const QString &library, const QString &component);
    // Splits "library:component"; returns false for a malformed key.
    static bool splitKey(const QString &key, QString *library, QString *component);

Q_SIGNALS:
    void librariesChanged();

private:
    QString m_paths[3];
    std::vector<Entry> m_entries;
    QStringList m_warnings;
};

} // namespace lvd
