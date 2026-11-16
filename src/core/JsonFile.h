#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace lvd {

// Every lvd RF cad file is a JSON object carrying "format" and "version".

std::optional<QJsonObject> readJsonFile(const QString &path, QString *error);

// Written atomically: the target is replaced only once the data is on disk.
bool writeJsonFile(const QString &path, const QJsonObject &object, QString *error);

// Checks "format" and rejects files written by a newer version of the application.
bool checkFileHeader(const QJsonObject &object, const QString &expectedFormat, int supportedVersion,
                     const QString &path, QString *error);

void setError(QString *error, const QString &message);

} // namespace lvd
