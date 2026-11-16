#include "core/DocumentType.h"

#include <stdexcept>

namespace lvd {

const std::vector<DocumentTypeInfo> &documentTypes()
{
    static const std::vector<DocumentTypeInfo> types = {
        {DocumentType::RfSchematic, QStringLiteral("rf"), QStringLiteral("RF schematic"), QStringLiteral("lvdrf")},
        {DocumentType::SpiceSchematic, QStringLiteral("spice"), QStringLiteral("SPICE schematic"), QStringLiteral("lvdspice")},
        {DocumentType::AntennaArray, QStringLiteral("antenna"), QStringLiteral("Antenna array"), QStringLiteral("lvdant")},
        {DocumentType::AntennaElement, QStringLiteral("antenna-element"), QStringLiteral("Antenna element"), QStringLiteral("lvdelem")},
    };
    return types;
}

const DocumentTypeInfo &documentTypeInfo(DocumentType type)
{
    for (const auto &info : documentTypes()) {
        if (info.type == type)
            return info;
    }
    throw std::logic_error("unknown document type");
}

std::optional<DocumentType> documentTypeFromId(const QString &id)
{
    for (const auto &info : documentTypes()) {
        if (info.id == id)
            return info.type;
    }
    return std::nullopt;
}

} // namespace lvd
