#include "core/JsonFile.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace lvd {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

std::optional<QJsonObject> readJsonFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot open %1: %2").arg(path, file.errorString()));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(error, QStringLiteral("%1 is not valid JSON: %2 (offset %3)")
                            .arg(path, parseError.errorString())
                            .arg(parseError.offset));
        return std::nullopt;
    }
    if (!document.isObject()) {
        setError(error, QStringLiteral("%1 does not contain a JSON object").arg(path));
        return std::nullopt;
    }
    return document.object();
}

bool writeJsonFile(const QString &path, const QJsonObject &object, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Cannot write %1: %2").arg(path, file.errorString()));
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setError(error, QStringLiteral("Cannot write %1: %2").arg(path, file.errorString()));
        return false;
    }
    return true;
}

bool checkFileHeader(const QJsonObject &object, const QString &expectedFormat, int supportedVersion,
                     const QString &path, QString *error)
{
    const QString format = object.value(QStringLiteral("format")).toString();
    if (format != expectedFormat) {
        setError(error, QStringLiteral("%1 is not a %2 file").arg(path, expectedFormat));
        return false;
    }
    const int version = object.value(QStringLiteral("version")).toInt(0);
    if (version < 1) {
        setError(error, QStringLiteral("%1 has no valid format version").arg(path));
        return false;
    }
    if (version > supportedVersion) {
        setError(error, QStringLiteral("%1 uses format version %2; this application reads up to version %3")
                            .arg(path)
                            .arg(version)
                            .arg(supportedVersion));
        return false;
    }
    return true;
}

} // namespace lvd
