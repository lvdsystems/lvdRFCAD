#include "core/Substrates.h"

#include "core/ComponentDefinition.h"
#include "core/Library.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace lvd {

namespace {

struct BuiltIn {
    SubstrateCatalog catalog;
    QString error;
};

const BuiltIn &builtInCatalog()
{
    static const BuiltIn result = [] {
        BuiltIn loaded;
        const QString path = QDir(LibraryManager::defaultBuiltInPath()).filePath(QStringLiteral("substrates.json"));
        if (auto catalog = SubstrateCatalog::load(path, &loaded.error))
            loaded.catalog = std::move(*catalog);
        return loaded;
    }();
    return result;
}

} // namespace

QString SubstrateCatalog::formatId()
{
    return QStringLiteral("lvdrfcad-substrates");
}

QString SubstrateCatalog::customId()
{
    return QStringLiteral("custom");
}

std::optional<SubstrateCatalog> SubstrateCatalog::load(const QString &path, QString *error)
{
    const auto fail = [&](const QString &message) -> std::optional<SubstrateCatalog> {
        if (error)
            *error = QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(path), message);
        return std::nullopt;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot be read"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("not a JSON object (%1)").arg(parseError.errorString()));
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != formatId())
        return fail(QStringLiteral("format is not \"%1\"").arg(formatId()));
    if (root.value(QStringLiteral("version")).toInt() != FormatVersion)
        return fail(QStringLiteral("unsupported version"));
    SubstrateCatalog catalog;
    for (const QJsonValue &value : root.value(QStringLiteral("materials")).toArray()) {
        const QJsonObject object = value.toObject();
        SubstrateMaterial material;
        material.id = object.value(QStringLiteral("id")).toString();
        material.name = object.value(QStringLiteral("name")).toString(material.id);
        material.er = object.value(QStringLiteral("er")).toDouble(NAN);
        material.tand = object.value(QStringLiteral("tand")).toDouble(NAN);
        material.frequency = object.value(QStringLiteral("frequency")).toDouble(0);
        for (const QJsonValue &thickness : object.value(QStringLiteral("thicknesses")).toArray())
            material.thicknesses.append(thickness.toDouble());
        material.note = object.value(QStringLiteral("note")).toString();
        material.source = object.value(QStringLiteral("source")).toString();
        if (material.id.isEmpty() || material.id == customId())
            return fail(QStringLiteral("a material has no id or uses the reserved id \"custom\""));
        if (!(material.er >= 1) || !(material.tand >= 0))
            return fail(QStringLiteral("material \"%1\" needs er >= 1 and tand >= 0").arg(material.id));
        if (catalog.material(material.id))
            return fail(QStringLiteral("material \"%1\" is defined twice").arg(material.id));
        catalog.materials.append(material);
    }
    return catalog;
}

const SubstrateCatalog &SubstrateCatalog::builtIn()
{
    return builtInCatalog().catalog;
}

QString SubstrateCatalog::builtInError()
{
    return builtInCatalog().error;
}

const SubstrateMaterial *SubstrateCatalog::material(const QString &id) const
{
    for (const SubstrateMaterial &material : materials) {
        if (material.id == id)
            return &material;
    }
    return nullptr;
}

QStringList SubstrateCatalog::ids() const
{
    QStringList list;
    for (const SubstrateMaterial &material : materials)
        list.append(material.id);
    return list;
}

bool isSubstrateDefinition(const ComponentDefinition &definition)
{
    const ModelDefinition *model = definition.model(QStringLiteral("rf-element"));
    return model && model->data.value(QStringLiteral("element")).toString() == QLatin1String("substrate");
}

} // namespace lvd
