#pragma once

#include "core/Substrates.h"
#include "fullwave/FwModel.h"

#include <QString>
#include <QStringList>

#include <map>
#include <vector>

namespace lvd::fw {

// Parametric antenna templates (requirement AN-7). Lengths in millimetres.
struct TemplateParameter {
    QString id;
    QString label;
    QString unit;  // "mm", "", "ohm"
    double value = 0;
    double min = 0, max = 1e9;
    QString help;
};

struct TemplateInfo {
    QString id;
    QString name;
    QString description;
    std::vector<TemplateParameter> parameters;
    double fStart = 1e9, fStop = 3e9; // default band
    Boundary boundary = Boundary::Mur;
    // The template radiates its main beam towards +z with x polarisation,
    // like the analytic element models, unless noted in the description.
};

const std::vector<TemplateInfo> &fullWaveTemplates();
const TemplateInfo *findTemplate(const QString &id);

using ParameterValues = std::map<QString, double>;
// The defaults overridden by the given values.
ParameterValues templateValues(const TemplateInfo &info, const ParameterValues &values);
// Builds the model; checks the values and reports problems as warnings.
FwModel buildTemplate(const QString &id, const ParameterValues &values, QString *error = nullptr);

// Substrate picker (libraries/substrates.json, core/Substrates.h): true for
// templates with a dielectric permittivity parameter (epsR, feedEpsR or
// patchEpsR).
bool hasSubstrate(const TemplateInfo &info);
// Sets every permittivity parameter and the loss tangent tanD from the
// material; in a template with "epsR" and "height" the height becomes the
// material's standard thickness nearest to the current height. `values`
// holds the full set (templateValues()). Returns the ids that changed.
QStringList applySubstrate(const TemplateInfo &info, ParameterValues &values, const SubstrateMaterial &material);

} // namespace lvd::fw
