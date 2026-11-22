#pragma once

#include "fullwave/FwModel.h"
#include "fullwave/Mesher.h"
#include "fullwave/Simulation.h"
#include "antenna/ElementModel.h"
#include "fullwave/Templates.h"

#include <QColor>
#include <QJsonObject>
#include <QString>

#include <vector>

namespace lvd::fw {

// An imported STL body of the "stl" template.
struct FwStlBody {
    QString file;          // relative to the project folder, or absolute
    double scale = 1;      // file units to millimetres (1 mm, 1000 m, 25.4 inch)
    bool metal = true;
    double epsR = 1, tanD = 0;
    QString name;
};

// The full-wave part of an antenna element (ElementSpec::fullWave): the
// template, its parameter values and the run settings.
struct FwElementSetup {
    QString templateId = QStringLiteral("patch-probe");
    ParameterValues values;   // only the ones that differ from the defaults need be present
    FwSettings settings;
    std::vector<FwStlBody> bodies; // template "stl"
    // Embedded element pattern (requirement AN-10): the element repeated in an
    // nx by ny array (spacing in millimetres), the centre one excited and the
    // others terminated in the port resistance.
    int arrayNx = 1, arrayNy = 1;
    double arrayDx = 0, arrayDy = 0;

    bool isArray() const { return arrayNx * arrayNy > 1; }

    QJsonObject toJson() const;
    static FwElementSetup fromJson(const QJsonObject &object);
    // A setup with the template's default band.
    static FwElementSetup forTemplate(const QString &templateId);
};

QJsonObject settingsToJson(const FwSettings &s);
FwSettings settingsFromJson(const QJsonObject &o);

// The model of a setup: the template (or the STL bodies, read relative to
// baseDirectory), repeated into the array when one is set.
FwModel buildModel(const FwElementSetup &setup, const QString &baseDirectory, QString *error = nullptr);
// Repeats a single-element model nx by ny times; the centre copy keeps port 1
// and is the only one excited. Exposed for tests.
FwModel repeatModel(const FwModel &element, int nx, int ny, double dx, double dy);

// Where an element's runs live: <project>/simulations/<element name>.
QString simulationFolder(const QString &projectDirectory, const QString &elementName);
// The pattern file path stored in the element (relative to the project).
QString simulationPatternFile(const QString &elementName);

// After a successful run: the element uses the simulated pattern
// (simulationPatternFile), its frequency becomes the resonance and its
// efficiency the radiation efficiency nearest the resonance.
void applyResults(ant::ElementSpec &spec, const FwResults &results, const QString &elementName);

// Triangles for the 3D view (millimetres): one mesh per material.
struct FwDisplayMesh {
    QString material;
    QColor color;
    bool metal = true;
    std::vector<Vec3> vertices;
    std::vector<unsigned> indices;
};
std::vector<FwDisplayMesh> displayMeshes(const FwModel &model);
// Polygon triangulation (ear clipping); indices into the outline.
std::vector<unsigned> triangulate(const std::vector<std::array<double, 2>> &outline);

} // namespace lvd::fw
