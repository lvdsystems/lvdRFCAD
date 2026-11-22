#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace lvd {

struct ComponentDefinition;

// Substrate materials (libraries/substrates.json, docs/file-format.md
// section 8): the dielectric constant and loss tangent the Substrate part
// and the full-wave templates take from a material name.
struct SubstrateMaterial {
    QString id;         // the name used by the Substrate part's "material" parameter
    QString name;       // display name
    double er = 1;      // relative permittivity (design value)
    double tand = 0;    // loss tangent
    double frequency = 0; // Hz at which er and tand are quoted
    QList<double> thicknesses; // standard thicknesses, metres
    QString note;
    QString source;
};

class SubstrateCatalog
{
public:
    static constexpr int FormatVersion = 1;
    static QString formatId(); // "lvdrfcad-substrates"
    static QString customId(); // "custom": er and tand come from the part

    static std::optional<SubstrateCatalog> load(const QString &path, QString *error = nullptr);
    // libraries/substrates.json of the built-in libraries folder, read once.
    // Empty when the file cannot be read; builtInError() tells why.
    static const SubstrateCatalog &builtIn();
    static QString builtInError();

    const SubstrateMaterial *material(const QString &id) const;
    QStringList ids() const;

    QList<SubstrateMaterial> materials;
};

// True for the rf-circuit Substrate part (an "rf-element" model with element
// "substrate"): it holds substrate data and is no circuit element.
bool isSubstrateDefinition(const ComponentDefinition &definition);

} // namespace lvd
