// Element models, pattern files and taper functions against analytic results.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "antenna/ElementModel.h"
#include "antenna/Excitation.h"
#include "antenna/PatternFile.h"

#include <QTemporaryDir>

using namespace lvd::ant;
using Catch::Approx;

namespace {

ElementSpec spec(ElementModelKind model)
{
    ElementSpec s;
    s.model = model;
    s.frequency = 10e9;
    return s;
}

// Array factor of an equally spaced line (half wavelength) in dB relative to its peak.
double lineSideLobeDb(const std::vector<double> &w)
{
    const int samples = 20001;
    std::vector<double> db(samples);
    double peak = 0;
    for (int i = 0; i < samples; ++i) {
        const double u = -1.0 + 2.0 * i / (samples - 1);
        cplx af = 0;
        for (std::size_t n = 0; n < w.size(); ++n)
            af += w[n] * std::polar(1.0, kPi * u * double(n));
        db[std::size_t(i)] = std::norm(af);
        peak = std::max(peak, db[std::size_t(i)]);
    }
    // Walk out of the main lobe from u = 0, then take the largest value.
    std::size_t c = samples / 2, r = c;
    while (r + 1 < std::size_t(samples) && db[r + 1] < db[r])
        ++r;
    double sll = 0;
    for (std::size_t i = r; i < std::size_t(samples); ++i)
        sll = std::max(sll, db[i]);
    return 10 * std::log10(sll / peak);
}

} // namespace

TEST_CASE("Analytic elements have their textbook directivity")
{
    CHECK(ElementPattern(spec(ElementModelKind::Isotropic)).peakDirectivityDbi(10e9) == Approx(0.0).margin(0.01));

    ElementSpec cosn = spec(ElementModelKind::CosN);
    cosn.exponent = 2;
    cosn.backLobeDb = -100;
    CHECK(ElementPattern(cosn).peakDirectivityDbi(10e9) == Approx(10 * std::log10(6.0)).margin(0.02));

    ElementSpec shortDipole = spec(ElementModelKind::Dipole);
    shortDipole.length = 1e-4;
    CHECK(ElementPattern(shortDipole).peakDirectivityDbi(10e9) == Approx(1.76).margin(0.02));

    CHECK(ElementPattern(spec(ElementModelKind::Dipole)).peakDirectivityDbi(10e9) == Approx(2.15).margin(0.02));
    double theta = -1;
    CHECK(ElementPattern(spec(ElementModelKind::Monopole)).peakDirectivityDbi(10e9, &theta) == Approx(5.16).margin(0.03));
    CHECK(theta == Approx(90).margin(0.6));

    // Half-wave dipole a quarter wavelength over ground: maximum at broadside.
    ElementPattern overGround(spec(ElementModelKind::DipoleOverGround));
    CHECK(overGround.peakDirectivityDbi(10e9, &theta) > 6.0);
    CHECK(theta == Approx(0).margin(0.6));
    CHECK(overGround.field(kPi * 0.75, 0.3, 10e9).power() == 0.0);
}

TEST_CASE("Patch and aperture models are plausible")
{
    ElementSpec patch = spec(ElementModelKind::Patch);
    const ElementSpec::PatchSize size = patch.patchSize();
    CHECK(size.width * 1e3 == Approx(10.14).margin(0.02));
    CHECK(size.length > 7.0e-3);
    CHECK(size.length < 9.0e-3);
    const double d = ElementPattern(patch).peakDirectivityDbi(10e9);
    CHECK(d > 6.0);
    CHECK(d < 9.0);

    ElementSpec waveguide = spec(ElementModelKind::Aperture);
    waveguide.apertureA = 22.86e-3;
    waveguide.apertureB = 10.16e-3;
    const double g = ElementPattern(waveguide).peakDirectivityDbi(10e9);
    CHECK(g > 5.0);
    CHECK(g < 8.0);

    // A larger horn aperture gives more directivity.
    waveguide.apertureA = 0.1;
    waveguide.apertureB = 0.08;
    CHECK(ElementPattern(waveguide).peakDirectivityDbi(10e9) > 18.0);
}

TEST_CASE("Polarisation options rotate or combine the element")
{
    ElementSpec s = spec(ElementModelKind::CosN);
    s.polarisation = Polarisation::LinearY;
    const Field y = ElementPattern(s).field(0, 0, 10e9);
    CHECK(std::abs(y.eTheta) < 1e-9); // at phi = 0, y is phi_hat
    CHECK(std::abs(y.ePhi) > 0.5);

    s.polarisation = Polarisation::Rhcp;
    const ElementPattern rhcpElement(s);
    const Field f = rhcpElement.field(0.2, 0.7, 10e9);
    CHECK(axialRatioDb(rhcpElement.field(0, 0, 10e9)) == Approx(0).margin(1e-6));
    CHECK(std::abs(lhcp(rhcpElement.field(0, 0, 10e9))) < 1e-9);
    CHECK(std::abs(rhcp(f)) > 10 * std::abs(lhcp(f)));

    s.polarisation = Polarisation::Lhcp;
    CHECK(std::abs(rhcp(ElementPattern(s).field(0, 0, 10e9))) < 1e-9);
}

TEST_CASE("Pattern files are read in the supported formats")
{
    SECTION("CST ASCII farfield")
    {
        const QString text = QStringLiteral(
            "Theta [deg.]  Phi   [deg.]  Abs(Dir.)[dBi   ]   Abs(Theta)[dBi   ]  Phase(Theta)[deg.]  Abs(Phi  )[dBi   ]  "
            "Phase(Phi  )[deg.]  Ax.Ratio[dB    ]\n"
            "------------------------------------------------------------------------------------------------------\n"
            "0.000 0.000 6.000 6.000 0.000 -100.0 0.000 40.0\n"
            "90.000 0.000 0.000 0.000 10.000 -100.0 0.000 40.0\n"
            "180.000 0.000 -20.000 -20.000 0.000 -100.0 0.000 40.0\n"
            "0.000 90.000 6.000 -100.0 0.000 6.000 0.000 40.0\n"
            "90.000 90.000 0.000 -100.0 0.000 0.000 0.000 40.0\n"
            "180.000 90.000 -20.000 -100.0 0.000 -20.000 0.000 40.0\n");
        QString error;
        const auto r = parsePatternText(text, QStringLiteral("cst.txt"), &error, 10e9);
        INFO(error.toStdString());
        REQUIRE(r);
        CHECK(r->table.theta.size() == 3);
        CHECK(r->table.phi.size() == 2);
        CHECK(r->table.hasPhase);
        CHECK(r->peakDbi == Approx(6.0).margin(1e-6));
        const Field f = r->table.sample(90, 0, 10e9);
        CHECK(std::arg(f.eTheta) == Approx(toRadians(10)).margin(1e-9));
    }
    SECTION("FEKO ffe with two frequencies")
    {
        const QString text = QStringLiteral(
            "##File Type: Far field\n"
            "#Frequency:   1.00000000E+010\n"
            "#No. of Theta Samples: 2\n"
            "#\"Theta\" \"Phi\" \"Re(Etheta)\" \"Im(Etheta)\" \"Re(Ephi)\" \"Im(Ephi)\" \"Directivity(Theta)\" "
            "\"Directivity(Phi)\" \"Directivity(Total)\"\n"
            "0 0 1 0 0 0 3 -100 3\n"
            "90 0 0.5 0 0 0 -3 -100 -3\n"
            "#Frequency:   1.10000000E+010\n"
            "#\"Theta\" \"Phi\" \"Re(Etheta)\" \"Im(Etheta)\" \"Re(Ephi)\" \"Im(Ephi)\" \"Directivity(Theta)\" "
            "\"Directivity(Phi)\" \"Directivity(Total)\"\n"
            "0 0 2 0 0 0 3 -100 3\n"
            "90 0 1 0 0 0 -3 -100 -3\n");
        QString error;
        const auto r = parsePatternText(text, QStringLiteral("a.ffe"), &error);
        INFO(error.toStdString());
        REQUIRE(r);
        REQUIRE(r->table.frequencies.size() == 2);
        CHECK(r->table.frequencies[1] == Approx(11e9));
        CHECK(r->table.sample(0, 0, 10.5e9).eTheta.real() == Approx(1.5));
        CHECK(r->table.sample(45, 0, 10e9).eTheta.real() == Approx(0.75));
    }
    SECTION("Generic CSV with gain only and negative theta")
    {
        const QString text = QStringLiteral("theta,phi,gain_dbi\n-10,0,3\n0,0,6\n10,0,3\n");
        QString error;
        const auto r = parsePatternText(text, QStringLiteral("g.csv"), &error);
        REQUIRE(r);
        CHECK_FALSE(r->table.hasPhase);
        CHECK(r->table.phi.size() == 2); // 0 and 180
        CHECK(r->table.sample(10, 180, 0).power() == Approx(fromDb(3)));
        CHECK_FALSE(r->warnings.isEmpty());
    }
    SECTION("A file without a header naming the columns is rejected")
    {
        QString error;
        CHECK_FALSE(parsePatternText(QStringLiteral("1 2 3 4\n"), QStringLiteral("x"), &error));
        CHECK_FALSE(error.isEmpty());
    }
}

TEST_CASE("An exported pattern reads back to the same element")
{
    ElementSpec s = spec(ElementModelKind::CosN);
    s.exponent = 3;
    s.polarisation = Polarisation::Slant45;
    const ElementPattern source(s);
    PatternTable table;
    table.frequencies = {10e9};
    for (int i = 0; i <= 180; ++i)
        table.theta.push_back(i);
    for (int j = 0; j < 360; j += 2)
        table.phi.push_back(j);
    table.fields.emplace_back();
    for (double t : table.theta)
        for (double p : table.phi)
            table.fields[0].push_back(source.field(toRadians(t), toRadians(p), 10e9));

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("p.csv"));
    QString error;
    REQUIRE(writePatternCsv(table, path, &error));
    const auto read = readPatternFile(path, &error);
    REQUIRE(read);
    ElementSpec imported = spec(ElementModelKind::Imported);
    imported.pattern = std::make_shared<PatternTable>(read->table);
    const ElementPattern copy(imported);
    CHECK(copy.peakDirectivityDbi(10e9) == Approx(source.peakDirectivityDbi(10e9)).margin(0.05));
    const Field a = source.field(0.4, 1.0, 10e9), b = copy.field(0.4, 1.0, 10e9);
    CHECK(std::abs(a.eTheta - b.eTheta) < 0.02 * std::sqrt(a.power()));
    CHECK(std::abs(a.ePhi - b.ePhi) < 0.02 * std::sqrt(a.power()));
}

TEST_CASE("Dolph-Chebyshev weights give equal side lobes")
{
    const std::vector<double> w = dolphChebyshev(16, 30);
    REQUIRE(w.size() == 16);
    CHECK(w.front() == Approx(w.back()));
    CHECK(lineSideLobeDb(w) == Approx(-30).margin(0.05));
    CHECK(lineSideLobeDb(dolphChebyshev(9, 40)) == Approx(-40).margin(0.05));
}

TEST_CASE("A Taylor line taper reaches its design side-lobe level")
{
    TaperSpec t;
    t.kind = TaperKind::Taylor;
    t.sllDb = 30;
    t.nbar = 5;
    std::vector<double> w;
    const int n = 40;
    for (int i = 0; i < n; ++i)
        w.push_back(lineTaper(t, (i - (n - 1) / 2.0) / (n / 2.0)));
    CHECK(w[0] < w[n / 2]);
    CHECK(lineSideLobeDb(w) == Approx(-30).margin(0.8));

    t.kind = TaperKind::Uniform;
    CHECK(lineTaper(t, 0.3) == 1.0);
    t.kind = TaperKind::CosinePedestal;
    t.pedestalDb = -20;
    CHECK(lineTaper(t, 1.0) == Approx(0.1));
}

TEST_CASE("The Bessel function and circular Taylor taper behave")
{
    CHECK(besselJ0(0) == Approx(1.0));
    CHECK(besselJ0(2.404825557695773) == Approx(0).margin(1e-7));
    CHECK(besselJ0(10.0) == Approx(-0.2459357645).margin(1e-7));
    const double centre = circularTaylor(30, 5, 0);
    const double edge = circularTaylor(30, 5, 1);
    CHECK(edge > 0);
    CHECK(edge < centre);
}
