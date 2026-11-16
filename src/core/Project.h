#pragma once

#include "core/Connectivity.h"
#include "core/Document.h"
#include "core/Library.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace lvd {

class Schematic;

// Where a placed component's definition came from: docs/file-format.md, section 5.
struct Resolution {
    enum class Source { Library, Cache, Sheet, Missing };

    DefinitionPtr definition; // null when Missing
    Source source = Source::Missing;
    bool changedInLibrary = false; // the library copy differs from the cached copy
    QStringList removedPins;       // pins of the cached copy the library copy no longer has
};

QString resolutionSourceName(Resolution::Source source);

// A folder holding a project file and the documents it references.
// The project name is the base name of the project file.
class Project : public QObject
{
    Q_OBJECT
public:
    static constexpr int FormatVersion = 1;
    static QString formatId();
    static QString fileExtension(); // without the dot

    // Creates the folder if needed and writes an empty project file into it.
    static std::unique_ptr<Project> create(const QString &directory, const QString &name, QString *error = nullptr);
    static std::unique_ptr<Project> load(const QString &projectFile, QString *error = nullptr);

    // Names become file names, so they must be valid on every platform.
    static bool validateName(const QString &name, QString *error = nullptr);

    QString name() const;
    QString directory() const;
    QString filePath() const { return m_filePath; }

    QList<Document *> documents() const;
    Document *document(const QString &name) const;

    // Creates the document file and registers it in the project file.
    Document *createDocument(DocumentType type, const QString &name, QString *error = nullptr);
    bool removeDocument(Document *document, bool deleteFile, QString *error = nullptr);

    bool hasModifiedDocuments() const;
    bool save(QString *error = nullptr);    // project file only
    bool saveAll(QString *error = nullptr); // project file and modified documents

    // Problems found while loading, such as referenced documents that are missing.
    QStringList loadWarnings() const { return m_loadWarnings; }

    // Component libraries. May be null, for example in tests; then only
    // cached definitions and sheets resolve.
    LibraryManager *libraryManager() const { return m_libraries; }
    void setLibraryManager(LibraryManager *libraries) { m_libraries = libraries; }

    // Resolves a definition key used in a schematic of this project:
    // "sheet:<document>" from the project, anything else from the libraries,
    // falling back to the schematic's cache.
    Resolution resolve(const Schematic &schematic, const QString &key) const;
    DefinitionLookup lookupFor(const Schematic &schematic) const;

    // The block symbol of a schematic document of this project (section 4.5).
    DefinitionPtr sheetDefinition(const QString &documentName) const;
    static QString sheetKey(const QString &documentName);

    // Copies the current library definition of every library component the
    // schematic uses into its cache, and drops unused entries.
    void refreshCache(Schematic &schematic) const;

    // An in-memory copy for background work (tuning, optimisation): every
    // document is cloned, schematic caches hold the definitions resolved
    // now, and the copy has no library manager, so it never touches shared
    // state. Saving the copy is not possible (it has no files). Create it
    // on the thread that owns this project; afterwards it may be used on
    // one other thread.
    std::unique_ptr<Project> snapshot() const;
    bool isSnapshot() const { return m_snapshot; }

Q_SIGNALS:
    void documentAdded(lvd::Document *document);
    void documentAboutToBeRemoved(lvd::Document *document);

private:
    explicit Project(const QString &filePath);

    QString m_filePath;
    std::vector<std::unique_ptr<Document>> m_documents;
    QStringList m_unresolvedPaths; // kept so that saving does not drop them
    QStringList m_loadWarnings;
    LibraryManager *m_libraries = nullptr;
    bool m_snapshot = false;
};

} // namespace lvd
