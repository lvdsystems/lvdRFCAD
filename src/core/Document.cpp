#include "core/Document.h"

#include "core/JsonFile.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QFileInfo>

namespace lvd {

QString Document::formatId()
{
    return QStringLiteral("lvdrfcad-document");
}

Document::Document(DocumentType type, const QString &filePath, QObject *parent)
    : QObject(parent), m_type(type), m_filePath(QFileInfo(filePath).absoluteFilePath())
{
    if (isSchematic())
        attachSchematic(std::make_unique<Schematic>());
}

Document::~Document() = default;

std::unique_ptr<Document> Document::load(const QString &filePath, QString *error)
{
    const auto object = readJsonFile(filePath, error);
    if (!object)
        return nullptr;
    if (!checkFileHeader(*object, formatId(), FormatVersion, filePath, error))
        return nullptr;

    const QString typeId = object->value(QStringLiteral("type")).toString();
    const auto type = documentTypeFromId(typeId);
    if (!type) {
        setError(error, QStringLiteral("%1 has unknown document type \"%2\"").arg(filePath, typeId));
        return nullptr;
    }

    auto document = std::make_unique<Document>(*type, filePath);
    const QJsonObject content = object->value(QStringLiteral("content")).toObject();
    if (document->isSchematic()) {
        QString schematicError;
        auto schematic = Schematic::fromJson(content, &schematicError);
        if (!schematic) {
            setError(error, QStringLiteral("%1: %2").arg(filePath, schematicError));
            return nullptr;
        }
        document->attachSchematic(std::move(schematic));
    } else {
        document->m_content = content;
    }
    return document;
}

void Document::attachSchematic(std::unique_ptr<Schematic> schematic)
{
    m_schematic = std::move(schematic);
    connect(m_schematic->undoStack(), &UndoStack::cleanChanged, this, [this](bool clean) { setModified(!clean); });
}

QString Document::name() const
{
    return QFileInfo(m_filePath).completeBaseName();
}

bool Document::isSchematic() const
{
    return m_type == DocumentType::RfSchematic || m_type == DocumentType::SpiceSchematic;
}

void Document::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    if (!modified && m_schematic)
        m_schematic->undoStack()->setClean();
    Q_EMIT modifiedChanged(m_modified);
}

QJsonObject Document::content() const
{
    return m_schematic ? m_schematic->toJson() : m_content;
}

bool Document::setContent(const QJsonObject &content)
{
    if (m_schematic)
        return false;
    if (m_content != content) {
        m_content = content;
        setModified(true);
        Q_EMIT contentChanged();
    }
    return true;
}

QJsonObject Document::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("format"), formatId());
    object.insert(QStringLiteral("version"), FormatVersion);
    object.insert(QStringLiteral("type"), documentTypeInfo(m_type).id);
    object.insert(QStringLiteral("content"), content());
    return object;
}

bool Document::restoreFromJson(const QJsonObject &object, QString *error)
{
    if (!checkFileHeader(object, formatId(), FormatVersion, m_filePath, error))
        return false;
    if (object.value(QStringLiteral("type")).toString() != documentTypeInfo(m_type).id) {
        setError(error, QStringLiteral("%1: the copy is of another document type").arg(name()));
        return false;
    }
    const QJsonObject content = object.value(QStringLiteral("content")).toObject();
    if (isSchematic()) {
        QString schematicError;
        auto schematic = Schematic::fromJson(content, &schematicError);
        if (!schematic) {
            setError(error, QStringLiteral("%1: %2").arg(name(), schematicError));
            return false;
        }
        attachSchematic(std::move(schematic));
        m_schematic->undoStack()->setCleanUnreachable();
        m_modified = false; // so that setModified(true) signals
        setModified(true);
    } else {
        m_content = content;
        m_modified = false;
        setModified(true);
        Q_EMIT contentChanged();
    }
    return true;
}

void Document::replaceSchematic(std::unique_ptr<Schematic> schematic)
{
    if (isSchematic() && schematic)
        attachSchematic(std::move(schematic));
}

bool Document::save(QString *error)
{
    if (m_project && m_project->isSnapshot()) {
        setError(error, QStringLiteral("%1 is a copy for simulation and cannot be saved").arg(name()));
        return false;
    }
    if (m_schematic && m_project)
        m_project->refreshCache(*m_schematic);
    if (!writeJsonFile(m_filePath, toJson(), error))
        return false;
    setModified(false);
    return true;
}

} // namespace lvd
