// Full-wave: result readers checked against a real openEMS 0.37 run of the
// Simple Patch Antenna tutorial (tests/data: probe files, nf2ff output in the
// legacy HDF5 layout, and the tutorial's own Python post-processing).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "fullwave/Hdf5File.h"
#include "fullwave/SimulationResults.h"
#include "fullwave/Templates.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace lvd;
using namespace lvd::fw;
using Catch::Approx;

namespace {

QString data(const char *name)
{
    return QStringLiteral(LVD_FULLWAVE_TEST_DATA "/") + QString::fromLatin1(name);
}

QJsonObject reference()
{
    QFile f(data("patch_ref.json"));
    REQUIRE(f.open(QIODevice::ReadOnly));
    return QJsonDocument::fromJson(f.readAll()).object();
}

std::vector<double> numbers(const QJsonValue &v)
{
    std::vector<double> out;
    for (const QJsonValue &x : v.toArray())
        out.push_back(x.toDouble());
    return out;
}

} // namespace

TEST_CASE("Port probes give the tutorial's S11 and input impedance")
{
    QString error;
    const auto u = readProbeFile(data("port_ut_1"), &error);
    const auto i = readProbeFile(data("port_it_1"), &error);
    REQUIRE(u);
    REQUIRE(i);
    CHECK(u->time.size() == 338);

    const QJsonObject ref = reference();
    const std::vector<double> f = numbers(ref[QStringLiteral("f")]);
    const PortResult port = lumpedPortResult(*u, *i, f, 50);
    REQUIRE(port.s11.size() == f.size());

    // The tutorial's own S11 minimum (Python, in-process run) and ours from the
    // openEMS.exe run of the same model: the same resonance.
    std::size_t best = 0;
    for (std::size_t k = 0; k < f.size(); ++k)
        if (std::abs(port.s11[k]) < std::abs(port.s11[best]))
            best = k;
    const std::vector<double> refS11 = numbers(ref[QStringLiteral("s11_db")]);
    std::size_t refBest = std::size_t(std::min_element(refS11.begin(), refS11.end()) - refS11.begin());
    CHECK(f[refBest] == Approx(2.435e9));
    CHECK(f[best] == Approx(f[refBest]).margin(10e6));
    CHECK(20 * std::log10(std::abs(port.s11[best])) < -10);
    // Away from resonance the two runs agree closely.
    const std::size_t k = f.size() / 8;
    CHECK(20 * std::log10(std::abs(port.s11[k])) == Approx(refS11[k]).margin(0.3));
    CHECK(port.zin[k].real() == Approx(ref[QStringLiteral("zin_re")].toArray()[int(k)].toDouble()).margin(2.0));
}

TEST_CASE("The nf2ff far field reads into a pattern table")
{
    Hdf5File file(data("ff_legacy.h5"));
    REQUIRE(file.isOpen());
    CHECK(file.exists(QStringLiteral("/nf2ff/E_theta/FD/f0_real")));
    CHECK_FALSE(file.exists(QStringLiteral("/nf2ff/none")));
    CHECK(file.members(QStringLiteral("/Mesh")) == std::vector<QString>{QStringLiteral("phi"), QStringLiteral("r"), QStringLiteral("theta")});

    QString error;
    const auto ff = readFarField(data("ff_legacy.h5"), &error);
    INFO(error.toStdString());
    REQUIRE(ff);
    REQUIRE(ff->frequency.size() == 1);
    CHECK(ff->frequency[0] == Approx(2.435e9)); // float32 in the file
    CHECK(ff->theta.size() == 180);
    CHECK(ff->phi.size() == 2);
    REQUIRE(ff->dmax.size() == 1);
    CHECK(ff->dmax[0] == Approx(5.10).margin(0.01));

    // Broadside maximum; the pattern table folds theta -180..178 to 0..180.
    const ant::PatternTable table = toPatternTable(*ff);
    CHECK(table.theta.front() == 0);
    CHECK(table.theta.back() == 180);
    CHECK(table.phi == std::vector<double>{0, 90, 180, 270});
    const double broadside = table.sample(0, 0, 2.435e9).power();
    CHECK(broadside > 10 * table.sample(90, 0, 2.435e9).power());
    // The same direction reached from both sides of a cut agrees.
    const ant::Field a = table.sample(30, 180, 2.435e9);
    double mx = 0;
    for (const ant::Field &v : table.fields[0])
        mx = std::max(mx, v.power());
    CHECK(broadside == Approx(mx).epsilon(0.05));
    CHECK(a.power() > 0);
}

#include "fullwave/Mesher.h"
#include "fullwave/OpenEmsXml.h"
#include "fullwave/Simulation.h"
#include "fullwave/Templates.h"

#include <QDir>

TEST_CASE("Mesh lines are graded and respect the material cell size")
{
    // Fixed lines 0, 1 and 100 with 5 mm cells allowed: the 1 mm cell grows
    // by at most 1.4 towards the coarse region.
    const auto lines = smoothLines({0, 1, 100}, [](double) { return 5.0; }, 1.4);
    REQUIRE(lines.front() == 0);
    REQUIRE(lines.back() == 100);
    CHECK(std::find(lines.begin(), lines.end(), 1.0) != lines.end());
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const double h = lines[i] - lines[i - 1];
        CHECK(h <= 5.0 + 1e-9);
        if (i >= 2) {
            const double prev = lines[i - 1] - lines[i - 2];
            CHECK(h <= 1.4 * prev * 1.05);
            CHECK(prev <= 1.4 * h * 1.05);
        }
    }

    // The patch template: substrate cells, patch edges with the thirds rule,
    // and cells of at most lambda/20 at 3 GHz (5 mm) in air.
    QString error;
    const FwModel model = buildTemplate(QStringLiteral("patch-probe"), {}, &error);
    REQUIRE(error.isEmpty());
    FwSettings settings;
    const FwMesh mesh = buildMesh(model, settings);
    const auto &z = mesh.lines[2];
    for (double v : {0.0, 0.381, 0.762, 1.143, 1.524})
        CHECK(std::any_of(z.begin(), z.end(), [v](double x) { return std::abs(x - v) < 1e-6; }));
    const auto &x = mesh.lines[0];
    const double third = 299792458.0 / 3e9 * 1e3 / 20 / 2; // half the air cell
    CHECK(std::any_of(x.begin(), x.end(), [&](double v) { return std::abs(v - (16 + third * 2 / 3)) < 1e-6; }));
    CHECK(std::any_of(x.begin(), x.end(), [&](double v) { return std::abs(v - (16 - third / 3)) < 1e-6; }));
    // Neighbouring cells never grow by much more than 1.4 anywhere.
    for (int a = 0; a < 3; ++a) {
        const auto &l = mesh.lines[std::size_t(a)];
        for (std::size_t i = 2; i < l.size(); ++i) {
            const double r = (l[i] - l[i - 1]) / (l[i - 1] - l[i - 2]);
            CHECK(r < 1.6);
            CHECK(r > 1 / 1.6);
        }
    }
    for (int a = 0; a < 3; ++a)
        for (std::size_t i = 1; i < mesh.lines[std::size_t(a)].size(); ++i)
            CHECK(mesh.lines[std::size_t(a)][i] - mesh.lines[std::size_t(a)][i - 1] <= 5.0 + 1e-9);
    CHECK(mesh.cells() > 50000);
    CHECK(mesh.cells() < 250000);
    INFO("cells " << mesh.cells());

    const QString xml = openEmsXml(model, mesh, settings);
    CHECK(xml.contains(QStringLiteral("<LumpedElement")));
    CHECK(xml.contains(QStringLiteral("Name=\"port_ut_1\"")));
    CHECK(xml.contains(QStringLiteral("DumpType=\"1\"")));
    CHECK(xml.count(QStringLiteral("<Box Priority=\"0\">")) >= 12 + 2);
    CHECK(xml.contains(QStringLiteral("Excitation Type=\"0\" f0=\"2000000000\" fc=\"1000000000\"")));
}

TEST_CASE("The probe-fed patch matches the openEMS tutorial", "[openems]")
{
    if (!openEmsAvailable())
        SKIP("openEMS not available");
    QString error;
    const FwModel model = buildTemplate(QStringLiteral("patch-probe"), {}, &error);
    FwSettings settings;
    settings.farFieldFrequencies = {2.435e9};
    const QString work = qEnvironmentVariable("LVD_FULLWAVE_WORK", QDir::tempPath() + QStringLiteral("/lvd_fullwave_test"));
    const FwResults r = runFullWave(model, settings, work);
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    const QJsonObject ref = reference();
    // Resonance within 1 % of the tutorial's 2.435 GHz, and matched.
    CHECK(r.resonance() == Approx(2.435e9).epsilon(0.01));
    double best = 0;
    for (const auto &s : r.port.s11)
        best = std::min(best, 20 * std::log10(std::abs(s)));
    CHECK(best < -10);
    // Directivity of a patch of this size, against the tutorial's full-sphere-free nf2ff value 6.8 dBi.
    REQUIRE(r.directivityDbi.size() == 1);
    CHECK(r.directivityDbi[0] == Approx(ref[QStringLiteral("Dmax_dBi")].toDouble()).margin(0.6));
    // tan delta 0.001: a few per cent of dielectric loss.
    CHECK(r.radiationEfficiency[0] > 0.85);
    CHECK(r.radiationEfficiency[0] < 0.99);
    CHECK(QFileInfo::exists(r.patternFile()));
    const auto loaded = loadResults(work, &error);
    REQUIRE(loaded);
    CHECK(loaded->resonance() == Approx(r.resonance()));
}

#include "fullwave/FwElement.h"

TEST_CASE("Every template builds, meshes and writes openEMS XML")
{
    for (const TemplateInfo &t : fullWaveTemplates()) {
        if (t.id == QLatin1String("stl"))
            continue; // geometry from files; tested separately
        QString error;
        const FwModel model = buildTemplate(t.id, {}, &error);
        INFO(t.id.toStdString() << ": " << error.toStdString() << " " << model.warnings.join(QLatin1Char(';')).toStdString());
        REQUIRE(error.isEmpty());
        CHECK(model.warnings.isEmpty());
        REQUIRE_FALSE(model.ports.empty());
        const FwElementSetup setup = FwElementSetup::forTemplate(t.id);
        const FwMesh mesh = buildMesh(model, setup.settings);
        std::printf("%-15s %8.0f k cells, smallest %.3f mm\n", t.id.toUtf8().constData(), mesh.cells() / 1e3, mesh.smallestCell());
        CHECK(mesh.cells() > 10000);
        CHECK(mesh.cells() < 6000000);
        const QString xml = openEmsXml(model, mesh, setup.settings);
        CHECK(xml.contains(QStringLiteral("port_ut_1")));
        // The display triangulation covers every material with primitives.
        CHECK_FALSE(displayMeshes(model).empty());
    }
    // The horn's walls are rotated plates.
    QString error;
    const FwModel horn = buildTemplate(QStringLiteral("horn"), {}, &error);
    const QString xml = openEmsXml(horn, buildMesh(horn, FwElementSetup::forTemplate(QStringLiteral("horn")).settings),
                                   FwElementSetup::forTemplate(QStringLiteral("horn")).settings);
    CHECK(xml.contains(QStringLiteral("<Rotate_X Argument=\"0.349065850399\" />")));
    CHECK(xml.contains(QStringLiteral("PML_8")));
    // Ear clipping handles the concave inset-fed patch outline: 8 corners, 6 triangles.
    const std::vector<std::array<double, 2>> notch = {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 6}, {3, 6}, {3, 4}, {0, 4}};
    CHECK(triangulate(notch).size() == 18);
}

#include "fullwave/StlFile.h"

#include <QTemporaryDir>

#include <cstring>

TEST_CASE("STL files read in binary and ASCII and build into a model")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // One triangle, ASCII, in metres.
    {
        QFile f(dir.filePath(QStringLiteral("tri.stl")));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("solid t\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 0.01 0 0\n   vertex 0 0.02 0\n"
                "  endloop\n endfacet\nendsolid t\n");
    }
    auto ascii = readStl(dir.filePath(QStringLiteral("tri.stl")), 1000);
    REQUIRE(ascii);
    REQUIRE(ascii->size() == 3);
    CHECK((*ascii)[1].x() == Approx(10));
    CHECK((*ascii)[2].y() == Approx(20));
    // Two triangles, binary, whose header starts with "solid" (as some exporters write).
    {
        QByteArray data(84 + 2 * 50, '\0');
        std::memcpy(data.data(), "solid binary", 12);
        const quint32 count = 2;
        std::memcpy(data.data() + 80, &count, 4);
        const float tris[2][9] = {{0, 0, 0, 1, 0, 0, 0, 1, 0}, {1, 0, 0, 1, 1, 0, 0, 1, 2.5f}};
        for (int t = 0; t < 2; ++t)
            std::memcpy(data.data() + 84 + t * 50 + 12, tris[t], 36);
        QFile f(dir.filePath(QStringLiteral("bin.stl")));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(data);
    }
    auto binary = readStl(dir.filePath(QStringLiteral("bin.stl")), 2);
    REQUIRE(binary);
    REQUIRE(binary->size() == 6);
    CHECK((*binary)[5].z() == Approx(5));
    QString error;
    CHECK_FALSE(readStl(dir.filePath(QStringLiteral("missing.stl")), 1, &error));
    CHECK_FALSE(error.isEmpty());

    // The "stl" template: the bodies come from the setup, relative to the project.
    FwElementSetup setup = FwElementSetup::forTemplate(QStringLiteral("stl"));
    FwStlBody body;
    body.file = QStringLiteral("bin.stl");
    body.scale = 2;
    setup.bodies.push_back(body);
    const FwElementSetup copy = FwElementSetup::fromJson(setup.toJson());
    REQUIRE(copy.bodies.size() == 1);
    CHECK(copy.bodies[0].scale == 2);
    error.clear();
    const FwModel model = buildModel(copy, dir.path(), &error);
    CHECK(error.isEmpty());
    REQUIRE(model.ports.size() == 1);
    int stlBodies = 0;
    for (const FwPrimitive &p : model.primitives)
        if (p.kind == PrimitiveKind::StlFile) {
            ++stlBodies;
            CHECK(p.triangles.size() == 6);
        }
    CHECK(stlBodies == 1);
    const QString xml = openEmsXml(model, buildMesh(model, copy.settings), copy.settings);
    CHECK(xml.contains(QStringLiteral("PolyhedronReader")));
}

TEST_CASE("An element repeats into an embedded array with the centre copy excited")
{
    FwElementSetup setup = FwElementSetup::forTemplate(QStringLiteral("dipole"));
    setup.arrayNx = 3;
    setup.arrayNy = 2;
    setup.arrayDx = 60;
    setup.arrayDy = 70;
    const FwElementSetup copy = FwElementSetup::fromJson(setup.toJson());
    CHECK(copy.arrayNx == 3);
    CHECK(copy.arrayDy == 70);
    const FwModel single = buildTemplate(QStringLiteral("dipole"), {});
    const FwModel array = buildModel(copy, QString());
    REQUIRE(array.ports.size() == 6 * single.ports.size());
    CHECK(array.primitives.size() == 6 * single.primitives.size());
    // Port 1 is the centre copy (index (1, 0)) and the only excited one.
    CHECK(array.ports[0].number == 1);
    CHECK(array.ports[0].excite);
    CHECK(array.ports[0].start.x() == Approx(single.ports[0].start.x()));
    CHECK(array.ports[0].start.y() == Approx(single.ports[0].start.y()));
    for (std::size_t k = 1; k < array.ports.size(); ++k) {
        CHECK_FALSE(array.ports[k].excite);
        CHECK(array.ports[k].number == int(k) + 1);
    }
    CHECK(array.farFieldCentre.x() == Approx(single.farFieldCentre.x()));
    const QString xml = openEmsXml(array, buildMesh(array, copy.settings), copy.settings);
    CHECK(xml.contains(QStringLiteral("port_ut_6")));
}

// Template tuning aid: LVD_TUNE_TEMPLATE=id, LVD_TUNE_VALUES="key=value,...".
TEST_CASE("Tune one template", "[.tune]")
{
    const QString id = qEnvironmentVariable("LVD_TUNE_TEMPLATE");
    if (id.isEmpty() || !openEmsAvailable())
        SKIP("set LVD_TUNE_TEMPLATE");
    ParameterValues values;
    for (const QString &kv : qEnvironmentVariable("LVD_TUNE_VALUES").split(QLatin1Char(','), Qt::SkipEmptyParts))
        values[kv.section(QLatin1Char('='), 0, 0).trimmed()] = kv.section(QLatin1Char('='), 1).toDouble();
    QString error;
    const FwModel model = buildTemplate(id, values, &error);
    REQUIRE(error.isEmpty());
    FwElementSetup setup = FwElementSetup::forTemplate(id);
    setup.settings.farFieldStepDeg = 5;
    const QString work = qEnvironmentVariable("LVD_FULLWAVE_WORK", QDir::tempPath() + QStringLiteral("/lvd_fullwave_test"));
    const FwResults r = runFullWave(model, setup.settings, work + QStringLiteral("/tune-") + id);
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    const auto &f = r.port.frequency;
    for (std::size_t k = 0; k < f.size(); k += std::max<std::size_t>(1, f.size() / 16))
        std::printf("%7.3f GHz  S11 %6.1f dB  Z %7.1f %+7.1fj\n", f[k] / 1e9, 20 * std::log10(std::abs(r.port.s11[k])),
                    r.port.zin[k].real(), r.port.zin[k].imag());
    for (std::size_t k = 0; k < r.farFieldFrequencies.size(); ++k)
        std::printf("ff %7.3f GHz  D %5.2f dBi  eff %5.1f %%\n", r.farFieldFrequencies[k] / 1e9, r.directivityDbi[k],
                    100 * r.radiationEfficiency[k]);
    double best = 0;
    for (const auto &s : r.port.s11)
        best = std::min(best, 20 * std::log10(std::abs(s)));
    std::printf("resonance %.3f GHz, best S11 %.1f dB, %zu k cells, %.0f s\n", r.resonance() / 1e9, best, r.cells / 1000, r.seconds);
}

TEST_CASE("Every template simulates to a working antenna", "[openems][templates]")
{
    if (!openEmsAvailable())
        SKIP("openEMS not available");
    const QString work = qEnvironmentVariable("LVD_FULLWAVE_WORK", QDir::tempPath() + QStringLiteral("/lvd_fullwave_test"));
    for (const TemplateInfo &t : fullWaveTemplates()) {
        if (t.id == QLatin1String("patch-probe") || t.id == QLatin1String("stl"))
            continue; // the reference test covers the patch; STL needs files
        QString error;
        const FwModel model = buildTemplate(t.id, {}, &error);
        FwElementSetup setup = FwElementSetup::forTemplate(t.id);
        setup.settings.farFieldStepDeg = 5;
        const FwResults r = runFullWave(model, setup.settings, work + QLatin1Char('/') + t.id);
        INFO(t.id.toStdString() << ": " << r.error.toStdString());
        REQUIRE(r.ok);
        double best = 0;
        for (const auto &s : r.port.s11)
            best = std::min(best, 20 * std::log10(std::abs(s)));
        std::size_t res = 0;
        for (std::size_t k = 0; k < r.farFieldFrequencies.size(); ++k)
            if (std::abs(r.farFieldFrequencies[k] - r.resonance()) < 1)
                res = k;
        std::printf("%-15s resonance %7.3f GHz, S11 %6.1f dB, D %5.2f dBi, eff %5.1f %%, %zu k cells, %.0f s\n",
                    t.id.toUtf8().constData(), r.resonance() / 1e9, best, r.directivityDbi[res], 100 * r.radiationEfficiency[res],
                    r.cells / 1000, r.seconds);
        std::fflush(stdout);
        CHECK(best < -6);
        CHECK(r.resonance() > t.fStart);
        CHECK(r.resonance() < t.fStop);
        CHECK(r.directivityDbi[res] > 1.5);
        // The openEMS tutorials give the directivity at the band centre (the
        // second far-field frequency): horn 16.87 dBi at 15 GHz, helix
        // 13.30 dBi at 2.4 GHz (scripts run with openEMS 0.37.0).
        const auto atCentre = [&](double f) {
            for (std::size_t k = 0; k < r.farFieldFrequencies.size(); ++k)
                if (std::abs(r.farFieldFrequencies[k] - f) < 1e6)
                    return r.directivityDbi[k];
            return std::nan("");
        };
        if (t.id == QLatin1String("horn"))
            CHECK(atCentre(15e9) == Approx(16.87).margin(0.3));
        if (t.id == QLatin1String("helix")) {
            CHECK(atCentre(2.4e9) == Approx(13.30).margin(0.3));
            CHECK(r.resonance() == Approx(2.544e9).epsilon(0.01));
        }
    }
}

TEST_CASE("the substrate picker fills the templates' dielectric from the catalog")
{
    const SubstrateCatalog &catalog = SubstrateCatalog::builtIn();
    INFO(SubstrateCatalog::builtInError().toStdString());
    REQUIRE_FALSE(catalog.materials.isEmpty());
    const SubstrateMaterial *ro4003 = catalog.material(QStringLiteral("RO4003C"));
    REQUIRE(ro4003);
    CHECK(ro4003->er == 3.55);
    CHECK(ro4003->tand == 0.0027);
    CHECK(catalog.material(QStringLiteral("custom")) == nullptr);

    const TemplateInfo *patch = findTemplate(QStringLiteral("patch-probe"));
    REQUIRE(patch);
    CHECK(hasSubstrate(*patch));
    ParameterValues values = templateValues(*patch, {});
    values[QStringLiteral("height")] = 0.5;
    const QStringList changed = applySubstrate(*patch, values, *ro4003);
    CHECK(changed.contains(QStringLiteral("epsR")));
    CHECK(values.at(QStringLiteral("epsR")) == Approx(3.55));
    CHECK(values.at(QStringLiteral("tanD")) == Approx(0.0027));
    CHECK(values.at(QStringLiteral("height")) == Approx(0.508)); // nearest standard thickness
    // Applying it again changes nothing.
    CHECK(applySubstrate(*patch, values, *ro4003).isEmpty());

    // Templates without a dielectric have no picker.
    const TemplateInfo *horn = findTemplate(QStringLiteral("horn"));
    REQUIRE(horn);
    CHECK_FALSE(hasSubstrate(*horn));
    // The aperture-coupled patch has two permittivities; both are set, the heights stay.
    for (const TemplateInfo &info : fullWaveTemplates()) {
        if (!hasSubstrate(info))
            continue;
        ParameterValues all = templateValues(info, {});
        applySubstrate(info, all, *catalog.material(QStringLiteral("RT/duroid 5880")));
        for (const char *id : {"epsR", "feedEpsR", "patchEpsR"}) {
            if (all.count(QString::fromLatin1(id)))
                CHECK(all.at(QString::fromLatin1(id)) == Approx(2.2));
        }
        QString error;
        const FwModel model = buildTemplate(info.id, all, &error);
        INFO(info.id.toStdString() << ": " << error.toStdString());
        CHECK(error.isEmpty());
    }
}
