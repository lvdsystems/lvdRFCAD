#pragma once

#include "core/DocumentType.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <memory>

namespace lvd {

class Project;
class Schematic;

// One design file: an RF schematic, a SPICE schematic or an antenna design.
// The document name is the base name of its file.
class Document : public QObject
{
    Q_OBJECT
public:
    static constexpr int FormatVersion = 1;
    static QString formatId();

    Document(DocumentType type, const QString &filePath, QObject *parent = nullptr);
    ~Document() override;

    static std::unique_ptr<Document> load(const QString &filePath, QString *error = nullptr);

    DocumentType type() const { return m_type; }
    QString name() const;
    QString filePath() const { return m_filePath; }
    bool isSchematic() const;

    bool isModified() const { return m_modified; }
    void setModified(bool modified);

    // The schematic of RF and SPICE documents; nullptr for other types.
    Schematic *schematic() const { return m_schematic.get(); }

    // The type-specific payload as stored in the file.
    QJsonObject content() const;
    // For document types without a model (antenna arrays and elements). Returns false
    // for schematics, which change only through their editing interface.
    bool setContent(const QJsonObject &content);

    QJsonObject toJson() const;
    // Fails for documents of a project snapshot.
    bool save(QString *error = nullptr);

    // Replaces the content with a document object read from a file of the same
    // type (an autosaved copy) and marks the document modified. Not undoable:
    // call it before any editor shows the document.
    bool restoreFromJson(const QJsonObject &document, QString *error = nullptr);

    // Replaces the schematic (for snapshots); not undoable.
    void replaceSchematic(std::unique_ptr<Schematic> schematic);

    // Set by the owning project; used to refresh the definition cache on save.
    Project *project() const { return m_project; }
    void setProject(Project *project) { m_project = project; }

Q_SIGNALS:
    void modifiedChanged(bool modified);
    // setContent() changed the payload (antenna documents).
    void contentChanged();

private:
    void attachSchematic(std::unique_ptr<Schematic> schematic);

    DocumentType m_type;
    QString m_filePath;
    QJsonObject m_content;
    std::unique_ptr<Schematic> m_schematic;
    Project *m_project = nullptr;
    bool m_modified = false;
};

} // namespace lvd
