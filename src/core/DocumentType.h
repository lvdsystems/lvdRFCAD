#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace lvd {

enum class DocumentType { RfSchematic, SpiceSchematic, AntennaArray, AntennaElement };

struct DocumentTypeInfo {
    DocumentType type;
    QString id;            // stable identifier used in files and in the Python API
    QString displayName;
    QString fileExtension; // without the dot
};

const std::vector<DocumentTypeInfo> &documentTypes();
const DocumentTypeInfo &documentTypeInfo(DocumentType type);
std::optional<DocumentType> documentTypeFromId(const QString &id);

} // namespace lvd
