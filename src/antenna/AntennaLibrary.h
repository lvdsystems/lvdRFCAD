#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace lvd {
class LibraryManager;
}

namespace lvd::ant {

// Antenna libraries (decision 9.3): .lvdantlib files next to the component
// libraries, in the same project, user and built-in folders.
enum class AntennaEntryKind { Element, Pattern, TrModule, Array };

QString antennaEntryKindId(AntennaEntryKind kind);
QString antennaEntryKindName(AntennaEntryKind kind);

struct AntennaLibraryEntry {
    QString id;
    QString name;
    QString description;
    AntennaEntryKind kind = AntennaEntryKind::Element;
    // element and pattern: an ElementSpec object (a pattern's "patternFile"
    // is relative to the library file); tr-module: tx, rx and AESA values;
    // array: a partial antenna array document.
    QJsonObject data;
};

struct AntennaLibrary {
    static constexpr int FormatVersion = 1;
    static QString formatId();
    static QString fileExtension(); // without the dot

    QString name;
    QString description;
    QString filePath;
    std::vector<AntennaLibraryEntry> entries;

    static std::optional<AntennaLibrary> load(const QString &path, QString *error = nullptr);
    bool save(QString *error = nullptr) const;
    QJsonObject toJson() const;
    const AntennaLibraryEntry *entry(const QString &id) const;
    void setEntry(const AntennaLibraryEntry &entry); // adds or replaces by id
    QString directory() const;
};

// Loads the antenna libraries from the component library manager's search
// folders and reloads when those change. Keys are "library:entry".
class AntennaLibraryManager : public QObject
{
    Q_OBJECT
public:
    explicit AntennaLibraryManager(LibraryManager *components, QObject *parent = nullptr);

    void reload();
    const std::vector<AntennaLibrary> &libraries() const { return m_libraries; }
    const AntennaLibrary *library(const QString &name) const;

    struct Found {
        const AntennaLibraryEntry *entry = nullptr;
        const AntennaLibrary *library = nullptr;
    };
    Found find(const QString &key) const;
    static QString makeKey(const QString &library, const QString &entry);

    // Writes the library into the user folder (or its own path) and reloads.
    bool saveLibrary(const AntennaLibrary &library, QString *error = nullptr);
    QStringList warnings() const { return m_warnings; }

Q_SIGNALS:
    void librariesChanged();

private:
    LibraryManager *m_components;
    std::vector<AntennaLibrary> m_libraries;
    QStringList m_warnings;
};

} // namespace lvd::ant
