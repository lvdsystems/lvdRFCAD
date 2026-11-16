#include "core/Project.h"

#include "core/JsonFile.h"
#include "core/Schematic.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>

namespace lvd {

QString Project::formatId()
{
    return QStringLiteral("lvdrfcad-project");
}

QString Project::fileExtension()
{
    return QStringLiteral("lvdproj");
}

Project::Project(const QString &filePath) : m_filePath(QFileInfo(filePath).absoluteFilePath())
{
}

bool Project::validateName(const QString &name, QString *error)
{
    if (name.trimmed().isEmpty()) {
        setError(error, QStringLiteral("The name is empty"));
        return false;
    }
    if (name != name.trimmed() || name.endsWith(QLatin1Char('.'))) {
        setError(error, QStringLiteral("\"%1\" must not start or end with a space or end with a dot").arg(name));
        return false;
    }
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || forbidden.contains(c)) {
            setError(error, QStringLiteral("\"%1\" contains a character that is not allowed in file names").arg(name));
            return false;
        }
    }
    return true;
}

std::unique_ptr<Project> Project::create(const QString &directory, const QString &name, QString *error)
{
    if (!validateName(name, error))
        return nullptr;
    if (!QDir().mkpath(directory)) {
        setError(error, QStringLiteral("Cannot create folder %1").arg(directory));
        return nullptr;
    }
    const QString filePath = QDir(directory).filePath(name + QLatin1Char('.') + fileExtension());
    if (QFileInfo::exists(filePath)) {
        setError(error, QStringLiteral("%1 already exists").arg(filePath));
        return nullptr;
    }
    std::unique_ptr<Project> project(new Project(filePath));
    if (!project->save(error))
        return nullptr;
    return project;
}

std::unique_ptr<Project> Project::load(const QString &projectFile, QString *error)
{
    const auto object = readJsonFile(projectFile, error);
    if (!object)
        return nullptr;
    if (!checkFileHeader(*object, formatId(), FormatVersion, projectFile, error))
        return nullptr;

    std::unique_ptr<Project> project(new Project(projectFile));
    const QDir dir(project->directory());
    const QJsonArray entries = object->value(QStringLiteral("documents")).toArray();
    for (const QJsonValue &entry : entries) {
        const QString relativePath = entry.toObject().value(QStringLiteral("path")).toString();
        if (relativePath.isEmpty())
            continue;
        QString documentError;
        auto document = Document::load(dir.absoluteFilePath(relativePath), &documentError);
        if (!document) {
            project->m_unresolvedPaths.append(relativePath);
            project->m_loadWarnings.append(documentError);
            continue;
        }
        document->setProject(project.get());
        project->m_documents.push_back(std::move(document));
    }
    return project;
}

QString Project::name() const
{
    return QFileInfo(m_filePath).completeBaseName();
}

QString Project::directory() const
{
    return QFileInfo(m_filePath).absolutePath();
}

QList<Document *> Project::documents() const
{
    QList<Document *> result;
    result.reserve(static_cast<qsizetype>(m_documents.size()));
    for (const auto &document : m_documents)
        result.append(document.get());
    return result;
}

Document *Project::document(const QString &name) const
{
    // Case-insensitive, because document names are file names.
    for (const auto &document : m_documents) {
        if (document->name().compare(name, Qt::CaseInsensitive) == 0)
            return document.get();
    }
    return nullptr;
}

Document *Project::createDocument(DocumentType type, const QString &name, QString *error)
{
    if (!validateName(name, error))
        return nullptr;
    if (document(name)) {
        setError(error, QStringLiteral("The project already has a document named \"%1\"").arg(name));
        return nullptr;
    }
    const QString filePath =
        QDir(directory()).filePath(name + QLatin1Char('.') + documentTypeInfo(type).fileExtension);
    if (QFileInfo::exists(filePath)) {
        setError(error, QStringLiteral("%1 already exists").arg(filePath));
        return nullptr;
    }

    auto created = std::make_unique<Document>(type, filePath);
    created->setProject(this);
    if (!created->save(error))
        return nullptr;

    Document *result = created.get();
    m_documents.push_back(std::move(created));
    if (!save(error)) {
        m_documents.pop_back();
        QFile::remove(filePath);
        return nullptr;
    }
    Q_EMIT documentAdded(result);
    return result;
}

bool Project::removeDocument(Document *document, bool deleteFile, QString *error)
{
    const auto it = std::find_if(m_documents.begin(), m_documents.end(),
                                 [document](const auto &owned) { return owned.get() == document; });
    if (it == m_documents.end()) {
        setError(error, QStringLiteral("The document does not belong to this project"));
        return false;
    }
    Q_EMIT documentAboutToBeRemoved(document);
    const QString filePath = document->filePath();
    m_documents.erase(it);

    if (!save(error))
        return false;
    if (deleteFile && QFileInfo::exists(filePath) && !QFile::remove(filePath)) {
        setError(error, QStringLiteral("Cannot delete %1").arg(filePath));
        return false;
    }
    return true;
}

bool Project::hasModifiedDocuments() const
{
    return std::any_of(m_documents.begin(), m_documents.end(),
                       [](const auto &document) { return document->isModified(); });
}

bool Project::save(QString *error)
{
    if (m_snapshot) {
        setError(error, QStringLiteral("a simulation copy of the project cannot be saved"));
        return false;
    }
    const QDir dir(directory());
    QJsonArray entries;
    const auto addEntry = [&entries](const QString &relativePath) {
        QJsonObject entry;
        entry.insert(QStringLiteral("path"), relativePath);
        entries.append(entry);
    };
    for (const auto &document : m_documents)
        addEntry(dir.relativeFilePath(document->filePath()));
    for (const QString &path : m_unresolvedPaths)
        addEntry(path);

    QJsonObject object;
    object.insert(QStringLiteral("format"), formatId());
    object.insert(QStringLiteral("version"), FormatVersion);
    object.insert(QStringLiteral("documents"), entries);
    return writeJsonFile(m_filePath, object, error);
}

QString resolutionSourceName(Resolution::Source source)
{
    switch (source) {
    case Resolution::Source::Library: return QStringLiteral("library");
    case Resolution::Source::Cache: return QStringLiteral("cache");
    case Resolution::Source::Sheet: return QStringLiteral("sheet");
    case Resolution::Source::Missing: return QStringLiteral("missing");
    }
    return {};
}

QString Project::sheetKey(const QString &documentName)
{
    return QStringLiteral("sheet:") + documentName;
}

Resolution Project::resolve(const Schematic &schematic, const QString &key) const
{
    Resolution resolution;
    static const QString sheetPrefix = QStringLiteral("sheet:");
    if (key.startsWith(sheetPrefix)) {
        resolution.definition = sheetDefinition(key.mid(sheetPrefix.size()));
        resolution.source = resolution.definition ? Resolution::Source::Sheet : Resolution::Source::Missing;
        return resolution;
    }
    const DefinitionPtr fromLibrary = m_libraries ? m_libraries->find(key) : nullptr;
    const DefinitionPtr cached = schematic.cachedDefinition(key);
    if (fromLibrary) {
        resolution.definition = fromLibrary;
        resolution.source = Resolution::Source::Library;
        if (cached && *cached != *fromLibrary) {
            resolution.changedInLibrary = true;
            for (const QString &pin : cached->pins) {
                if (!fromLibrary->pins.contains(pin))
                    resolution.removedPins.append(pin);
            }
        }
    } else if (cached) {
        resolution.definition = cached;
        resolution.source = Resolution::Source::Cache;
    }
    return resolution;
}

DefinitionLookup Project::lookupFor(const Schematic &schematic) const
{
    return [this, &schematic](const QString &key) { return resolve(schematic, key).definition; };
}

DefinitionPtr Project::sheetDefinition(const QString &documentName) const
{
    const Document *sheet = document(documentName);
    if (!sheet || !sheet->schematic())
        return nullptr;
    const Schematic &schematic = *sheet->schematic();

    // Ports left of the sheet's middle become pins on the left of the block.
    struct Port {
        QString name;
        QPoint at;
    };
    QList<Port> ports;
    int minX = 0, maxX = 0;
    bool first = true;
    for (const PlacedComponent &component : schematic.components()) {
        minX = first ? component.at.x() : std::min(minX, component.at.x());
        maxX = first ? component.at.x() : std::max(maxX, component.at.x());
        first = false;
        if (component.definition.startsWith(QStringLiteral("sheet:")))
            continue; // sheets cannot be ports; also avoids recursion
        const DefinitionPtr definition = resolve(schematic, component.definition).definition;
        if (!definition || !definition->isSheetPort())
            continue;
        const QString name = parameterText(component, *definition, QStringLiteral("name")).trimmed();
        if (!name.isEmpty() && std::none_of(ports.begin(), ports.end(), [&](const Port &p) { return p.name == name; }))
            ports.append(Port{name, component.at});
    }
    const int middle = (minX + maxX) / 2;
    QList<Port> left, right;
    for (const Port &port : ports)
        (port.at.x() <= middle ? left : right).append(port);
    const auto byPosition = [](const Port &a, const Port &b) {
        return a.at.y() != b.at.y() ? a.at.y() < b.at.y() : a.name < b.name;
    };
    std::sort(left.begin(), left.end(), byPosition);
    std::sort(right.begin(), right.end(), byPosition);

    ComponentDefinition definition;
    definition.id = documentName;
    definition.name = documentName;
    definition.category = QStringLiteral("Sheets");
    definition.description = QStringLiteral("Sub-schematic %1").arg(documentName);
    definition.referencePrefix = QStringLiteral("X");

    const int rows = static_cast<int>(std::max<qsizetype>({left.size(), right.size(), 1}));
    const int top = -(rows - 1) * 10;
    const int bottom = (rows - 1) * 10;
    SymbolGraphic body;
    body.type = SymbolGraphic::Type::Rect;
    body.from = QPoint(-40, top - 20);
    body.to = QPoint(40, bottom + 20);
    body.fill = Fill::Background;
    definition.symbol.graphics.append(body);

    const auto addSide = [&](const QList<Port> &side, bool isLeft) {
        for (qsizetype i = 0; i < side.size(); ++i) {
            const int y = top + static_cast<int>(i) * 20;
            definition.pins.append(side.at(i).name);
            definition.symbol.pins.append(SymbolPin{side.at(i).name, QPoint(isLeft ? -60 : 60, y), 20,
                                                    isLeft ? PinDirection::Right : PinDirection::Left});
            SymbolGraphic label;
            label.type = SymbolGraphic::Type::Text;
            label.at = QPoint(isLeft ? -36 : 36, y);
            label.text = side.at(i).name;
            label.size = 8;
            label.anchor = isLeft ? TextAnchor::Left : TextAnchor::Right;
            definition.symbol.graphics.append(label);
        }
    };
    addSide(left, true);
    addSide(right, false);

    SymbolGraphic title;
    title.type = SymbolGraphic::Type::Text;
    title.at = QPoint(0, bottom + 32);
    title.text = documentName;
    title.size = 10;
    title.anchor = TextAnchor::Center;
    definition.symbol.graphics.append(title);
    definition.symbol.referenceAt = QPoint(0, top - 30);
    definition.symbol.valueAt = QPoint(0, bottom + 46);

    for (const Variable &variable : schematic.variables()) {
        ParameterDefinition parameter;
        parameter.name = variable.name;
        parameter.label = variable.name;
        parameter.defaultValue = variable.value;
        definition.parameters.append(parameter);
    }
    definition.models.append(ModelDefinition{
        QStringLiteral("subschematic"),
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("subschematic")}, {QStringLiteral("document"), documentName}}});
    return std::make_shared<const ComponentDefinition>(std::move(definition));
}

void Project::refreshCache(Schematic &schematic) const
{
    if (m_libraries) {
        for (const PlacedComponent &component : schematic.components()) {
            if (const DefinitionPtr definition = m_libraries->find(component.definition))
                schematic.setCachedDefinition(component.definition, definition);
        }
    }
    schematic.pruneCache();
}

std::unique_ptr<Project> Project::snapshot() const
{
    std::unique_ptr<Project> copy(new Project(m_filePath));
    copy->m_snapshot = true;
    for (const auto &document : m_documents) {
        auto clone = std::make_unique<Document>(document->type(), document->filePath());
        if (document->schematic()) {
            auto schematic = Schematic::fromJson(document->schematic()->toJson());
            if (schematic) {
                // Freeze the definitions as they resolve now.
                for (const PlacedComponent &component : schematic->components()) {
                    if (component.definition.startsWith(QStringLiteral("sheet:")))
                        continue;
                    if (const DefinitionPtr definition = resolve(*document->schematic(), component.definition).definition)
                        schematic->setCachedDefinition(component.definition, definition);
                }
                clone->replaceSchematic(std::move(schematic));
            }
        } else {
            clone->setContent(document->content());
        }
        clone->setProject(copy.get());
        copy->m_documents.push_back(std::move(clone));
    }
    return copy;
}

bool Project::saveAll(QString *error)
{
    for (const auto &document : m_documents) {
        if (document->isModified() && !document->save(error))
            return false;
    }
    return save(error);
}

} // namespace lvd
