#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "plot3d/Pattern3DView.h"
#include "plot3d/Scene3D.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QWheelEvent>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <set>

using namespace lvd;
using Catch::Approx;

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

std::vector<double> range(double from, double to, double step)
{
    std::vector<double> v;
    const int n = static_cast<int>(std::lround((to - from) / step));
    for (int i = 0; i <= n; ++i)
        v.push_back(from + step * i);
    return v;
}

// cos^n(theta) power pattern in dB, 10 dBi at theta = 0, nothing below the horizon.
SphericalPattern cosPattern(int n, double phiFrom = -180, double phiTo = 180, double step = 5)
{
    SphericalPattern p;
    p.theta = range(0, 180, step);
    p.phi = range(phiFrom, phiTo, step);
    for (double theta : p.theta)
        for (std::size_t j = 0; j < p.phi.size(); ++j) {
            const double c = std::max(std::cos(theta * kDeg), 0.0);
            p.valuesDb.push_back(10.0 + 10.0 * std::log10(std::pow(c, n) + 1e-12));
        }
    return p;
}

bool hasTriangleWith(const PatternMesh &mesh, std::size_t a, std::size_t b)
{
    for (std::size_t k = 0; k + 2 < mesh.triangles.size(); k += 3) {
        const std::set<std::size_t> t{mesh.triangles[k], mesh.triangles[k + 1], mesh.triangles[k + 2]};
        if (t.count(a) && t.count(b))
            return true;
    }
    return false;
}

struct ImageStats {
    int foreground = 0;   // pixels differing from the corner (background)
    int bright = 0;       // high end of Viridis (yellow): the main lobe
    double brightY = 0;   // mean row of the bright pixels
};

ImageStats stats(const QImage &image)
{
    ImageStats s;
    const QRgb background = image.pixel(2, 2);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb c = image.pixel(x, y);
            if (c == background)
                continue;
            ++s.foreground;
            if (qRed(c) > 180 && qGreen(c) > 170 && qBlue(c) < 110) {
                ++s.bright;
                s.brightY += y;
            }
        }
    }
    if (s.bright > 0)
        s.brightY /= s.bright;
    return s;
}

} // namespace

TEST_CASE("Spherical directions follow the physics convention")
{
    const QVector3D z = sphericalDirection(0, 123);
    CHECK(z.z() == Approx(1.0));
    const QVector3D x = sphericalDirection(90, 0);
    CHECK(x.x() == Approx(1.0));
    CHECK(std::abs(x.z()) < 1e-6);
    const QVector3D y = sphericalDirection(90, 90);
    CHECK(y.y() == Approx(1.0));
}

TEST_CASE("Pattern mesh maps dB to radius and closes the seam")
{
    const SphericalPattern p = cosPattern(4);
    const PatternMesh mesh = buildPatternMesh(p, 40.0, 2.0, ColorMap::Viridis);
    REQUIRE(!mesh.isEmpty());
    // -180..180 repeats the first column: dropped, and wrapped instead.
    CHECK(mesh.rows == 37);
    CHECK(mesh.columns == 72);
    CHECK(mesh.seamClosed);
    CHECK(mesh.positions.size() == 37u * 72u);
    CHECK(mesh.normals.size() == mesh.positions.size());
    CHECK(mesh.maxDb == Approx(10.0));
    CHECK(mesh.floorDb == Approx(-30.0));

    // Maximum at theta = 0 on +z at the full scale.
    const QVector3D top = mesh.positions[mesh.vertexIndex(0, 0)];
    CHECK(top.z() == Approx(2.0));
    CHECK(mesh.normals[mesh.vertexIndex(0, 5)].z() == Approx(1.0).margin(1e-3));
    // Below the floor (theta >= 90 deg): radius 0.
    CHECK(mesh.positions[mesh.vertexIndex(18, 3)].length() == Approx(0.0).margin(1e-6));
    CHECK(mesh.positions[mesh.vertexIndex(30, 40)].length() == Approx(0.0).margin(1e-6));
    // Half the dynamic range below the maximum: half the radius.
    {
        SphericalPattern q;
        q.theta = {0, 90};
        q.phi = {0, 90};
        q.valuesDb = {10, 10, -10, -10};
        const PatternMesh m = buildPatternMesh(q, 40.0, 3.0, ColorMap::Viridis);
        CHECK(m.positions[m.vertexIndex(1, 0)].length() == Approx(1.5));
        CHECK(m.positions[m.vertexIndex(1, 0)].x() == Approx(1.5));
        CHECK(m.colors[m.vertexIndex(0, 0)] == colorMapColor(ColorMap::Viridis, 1.0));
    }

    // Normals are unit and the lobe's normals point outwards.
    for (std::size_t k = 0; k < mesh.normals.size(); ++k)
        CHECK(mesh.normals[k].length() == Approx(1.0).margin(1e-4));
    const std::size_t side = mesh.vertexIndex(6, 0); // theta 30, phi -180: -x side
    CHECK(QVector3D::dotProduct(mesh.normals[side], mesh.positions[side]) > 0);

    // Every triangle index is valid, and the seam (last column to the first) is closed.
    for (quint32 i : mesh.triangles)
        CHECK(i < mesh.positions.size());
    CHECK(hasTriangleWith(mesh, mesh.vertexIndex(5, 71), mesh.vertexIndex(5, 0)));
}

TEST_CASE("Pattern mesh seam for 0..355, 0..360 and sectors")
{
    {
        const PatternMesh m = buildPatternMesh(cosPattern(2, 0, 355), 30.0, 1.0, ColorMap::Jet);
        CHECK(m.columns == 72);
        CHECK(m.seamClosed);
        CHECK(hasTriangleWith(m, m.vertexIndex(4, 71), m.vertexIndex(4, 0)));
    }
    {
        const PatternMesh m = buildPatternMesh(cosPattern(2, 0, 360), 30.0, 1.0, ColorMap::Jet);
        CHECK(m.columns == 72);
        CHECK(m.seamClosed);
    }
    {
        const PatternMesh m = buildPatternMesh(cosPattern(2, 0, 90), 30.0, 1.0, ColorMap::Jet);
        CHECK(m.columns == 19);
        CHECK(!m.seamClosed);
        CHECK(!hasTriangleWith(m, m.vertexIndex(4, 18), m.vertexIndex(4, 0)));
        // A quad strip per row pair, minus the degenerate pole triangles.
        CHECK(m.triangles.size() / 3 <= 36u * 18u * 2u);
        CHECK(!m.triangles.empty());
    }
}

TEST_CASE("Pattern mesh copes with empty, NaN and degenerate grids")
{
    CHECK(buildPatternMesh({}, 40, 1, ColorMap::Viridis).isEmpty());
    SphericalPattern bad = cosPattern(2);
    bad.valuesDb.pop_back();
    CHECK(buildPatternMesh(bad, 40, 1, ColorMap::Viridis).isEmpty());
    SphericalPattern allNan = cosPattern(2);
    for (double &v : allNan.valuesDb)
        v = NAN;
    CHECK(buildPatternMesh(allNan, 40, 1, ColorMap::Viridis).isEmpty());

    SphericalPattern holes = cosPattern(2);
    const std::size_t hole = 10 * holes.phi.size() + 7;
    holes.valuesDb[hole] = NAN;
    const PatternMesh m = buildPatternMesh(holes, 40, 1, ColorMap::Viridis);
    REQUIRE(!m.isEmpty());
    CHECK(m.maxDb == Approx(10.0));
    CHECK(std::isnan(m.valuesDb[m.vertexIndex(10, 7)]));
    for (quint32 i : m.triangles)
        CHECK(i != m.vertexIndex(10, 7));
    for (quint32 i : m.gridLines)
        CHECK(i != m.vertexIndex(10, 7));

    // One phi: a cut, drawn as a curve.
    SphericalPattern cut;
    cut.theta = range(0, 180, 10);
    cut.phi = {0};
    for (double t : cut.theta)
        cut.valuesDb.push_back(std::cos(t * kDeg) * 10);
    const PatternMesh c = buildPatternMesh(cut, 40, 1, ColorMap::Viridis);
    CHECK(c.columns == 1);
    CHECK(c.triangles.empty());
    CHECK(c.curve.size() == cut.theta.size());

    // One theta: a ring.
    SphericalPattern ring;
    ring.theta = {90};
    ring.phi = range(0, 350, 10);
    ring.valuesDb.assign(ring.phi.size(), 0.0);
    const PatternMesh r = buildPatternMesh(ring, 40, 1, ColorMap::Viridis);
    CHECK(r.seamClosed);
    CHECK(r.curve.size() == ring.phi.size() + 1);
}

TEST_CASE("Default camera: z up, x to the lower left, y to the lower right")
{
    Camera3D camera;
    const QRectF viewport(0, 0, 800, 600);
    const QPointF centre = camera.project(QVector3D(0, 0, 0), viewport);
    CHECK(centre.x() == Approx(400).margin(1e-3));
    CHECK(centre.y() == Approx(300).margin(1e-3));
    const QPointF z = camera.project(QVector3D(0, 0, 0.5f), viewport);
    CHECK(z.y() < centre.y() - 50);
    CHECK(z.x() == Approx(centre.x()).margin(1e-3));
    const QPointF x = camera.project(QVector3D(0.5f, 0, 0), viewport);
    CHECK(x.x() < centre.x());
    CHECK(x.y() > centre.y());
    const QPointF y = camera.project(QVector3D(0, 0.5f, 0), viewport);
    CHECK(y.x() > centre.x());
    CHECK(y.y() > centre.y());

    // The whole bounding sphere is visible.
    for (const QVector3D &p : {QVector3D(1, 0, 0), QVector3D(0, -1, 0), QVector3D(0, 0, 1), QVector3D(0, 0, -1)}) {
        const QPointF q = camera.project(p, viewport);
        CHECK(viewport.contains(q));
    }

    // Zoom magnifies about the centre.
    Camera3D zoomed = camera;
    zoomed.zoom = 2;
    CHECK(centre.y() - zoomed.project(QVector3D(0, 0, 0.5f), viewport).y() > 1.5 * (centre.y() - z.y()));

    // Elevation is clamped; from straight above, +x points down or up but z shrinks to a point.
    Camera3D top = camera;
    top.orbit(0, 500);
    CHECK(top.elevationDeg == Approx(Camera3D::kMaxElevation));
    const QPointF topZ = top.project(QVector3D(0, 0, 0.5f), viewport);
    CHECK(std::hypot(topZ.x() - 400, topZ.y() - 300) < 10);

    // A ray through a projected point passes through it.
    QVector3D origin, dir;
    const QVector3D target(0.3f, -0.2f, 0.4f);
    camera.ray(camera.project(target, viewport), viewport, &origin, &dir);
    const QVector3D toTarget = target - origin;
    const QVector3D closest = origin + dir * QVector3D::dotProduct(toTarget, dir);
    CHECK((closest - target).length() < 1e-3);

    // Panning right moves the scene right.
    Camera3D panned = camera;
    panned.pan(QPointF(40, 0), viewport);
    CHECK(panned.project(QVector3D(), viewport).x() == Approx(440).margin(0.5));
    CHECK(panned.project(QVector3D(), viewport).y() == Approx(300).margin(0.5));
}

TEST_CASE("Pattern3DView: a broadside lobe points up on screen")
{
    Pattern3DView view;
    view.resize(800, 600);
    view.setPattern(cosPattern(4), 40.0);
    REQUIRE(view.hasPattern());
    CHECK(view.effectivePatternScale() == Approx(1.0));
    const QPointF centre = view.projectToWidget(QVector3D());
    const QPointF tip = view.projectToWidget(QVector3D(0, 0, 1));
    CHECK(tip.y() < centre.y() - 100);
    // The colour bar takes a strip on the right.
    CHECK(view.sceneRect(QSizeF(800, 600)).width() < 800);

    // Hovering the lobe tip reads theta near 0 and the maximum.
    QSignalSpy spy(&view, &Pattern3DView::hoverReadoutChanged);
    const QString text = view.readoutAt(view.projectToWidget(QVector3D(0, 0, 0.9f)));
    INFO(text.toStdString());
    CHECK(text.startsWith(QStringLiteral("theta ")));
    CHECK(text.endsWith(QStringLiteral("dBi")));
    CHECK(view.readoutAt(QPointF(3, 3)).isEmpty());
}

TEST_CASE("Pattern3DView: software rendering draws the pattern")
{
    Pattern3DView view;
    view.setPattern(cosPattern(4), 40.0);
    view.setWireframe(true);
    view.setSoftwareRenderingForced(true);
    const QImage image = view.renderImage(QSize(640, 480));
    REQUIRE(!image.isNull());
    CHECK(image.size() == QSize(640, 480));
    CHECK(!view.lastImageUsedOpenGL());
    const ImageStats s = stats(image);
    CHECK(s.foreground > 640 * 480 / 20);
    // The yellow (maximum) part of the lobe is above the image centre.
    CHECK(s.bright > 100);
    CHECK(s.brightY < 240);
}

TEST_CASE("Pattern3DView: renderImage works with whatever path is available")
{
    Pattern3DView view;
    view.setPattern(cosPattern(4), 40.0);
    std::vector<Scene3DPoint> points;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            points.push_back({QVector3D((i - 1.5f) * 0.1f, (j - 1.5f) * 0.1f, 0), QColor(Qt::red)});
    view.setPoints(points);
    view.setPatternScale(1.0);
    const QImage image = view.renderImage(QSize(500, 400));
    REQUIRE(!image.isNull());
    CHECK(image.size() == QSize(500, 400));
    std::printf("plot3d renderImage path: %s\n", view.lastImageUsedOpenGL() ? "OpenGL" : "software");
    const ImageStats s = stats(image);
    CHECK(s.foreground > 500 * 400 / 20);
    CHECK(s.bright > 100);
    CHECK(s.brightY < 200);

    if (view.lastImageUsedOpenGL()) {
        // Both paths show the same view: the foreground masks mostly overlap.
        view.setSoftwareRenderingForced(true);
        const QImage soft = view.renderImage(QSize(500, 400));
        const QRgb bg = image.pixel(2, 2);
        int both = 0, either = 0;
        for (int y = 0; y < 400; ++y)
            for (int x = 0; x < 500; ++x) {
                const bool a = image.pixel(x, y) != bg;
                const bool b = soft.pixel(x, y) != bg;
                both += a && b;
                either += a || b;
            }
        CHECK(both > 0.8 * either);
    }
}

TEST_CASE("Pattern3DView: empty, cut and geometry-only scenes render")
{
    Pattern3DView view;
    view.setSoftwareRenderingForced(true);
    const QImage empty = view.renderImage(QSize(300, 200));
    CHECK(empty.size() == QSize(300, 200));
    CHECK(stats(empty).foreground > 0); // the axes

    SphericalPattern cut;
    cut.theta = range(0, 180, 5);
    cut.phi = {0};
    for (double t : cut.theta)
        cut.valuesDb.push_back(10 * std::log10(std::pow(std::max(std::cos(t * kDeg), 0.0), 2) + 1e-9));
    view.setPattern(cut);
    CHECK(view.hasPattern());
    CHECK(!view.renderImage(QSize(300, 200)).isNull());
    view.clearPattern();
    CHECK(!view.hasPattern());

    // Geometry sets the automatic pattern scale.
    Scene3DMesh mesh;
    mesh.vertices = {QVector3D(0, 0, 0), QVector3D(0.2f, 0, 0), QVector3D(0, 0.2f, 0)};
    mesh.indices = {0, 1, 2, 0, 1, 7}; // the second triangle is out of range: ignored
    mesh.color = Qt::gray;
    mesh.opacity = 0.5f;
    view.addMesh(mesh);
    view.setLines({Scene3DLine{{QVector3D(0, 0, 0), QVector3D(0, 0, 0.1f)}, QColor(Qt::black), 2.0f}});
    CHECK(view.effectivePatternScale() == Approx(0.3));
    view.setAxesVisible(false);
    CHECK(stats(view.renderImage(QSize(300, 200))).foreground > 0);
    view.clearGeometry();
    CHECK(view.effectivePatternScale() == Approx(1.0));
}

TEST_CASE("Pattern3DView: mouse orbits, zooms and resets")
{
    Pattern3DView view;
    view.resize(600, 400);
    view.setPattern(cosPattern(2));
    QSignalSpy viewSpy(&view, &Pattern3DView::viewChanged);
    const double az = view.azimuth();
    const double el = view.elevation();

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(300, 200), view.mapToGlobal(QPointF(300, 200)), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&view, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(350, 260), view.mapToGlobal(QPointF(350, 260)), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&view, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(350, 260), view.mapToGlobal(QPointF(350, 260)), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &release);
    CHECK(view.azimuth() == Approx(az - 50 * 0.4));
    CHECK(view.elevation() == Approx(el + 60 * 0.4));
    CHECK(viewSpy.count() > 0);

    QWheelEvent wheel(QPointF(300, 200), view.mapToGlobal(QPointF(300, 200)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(&view, &wheel);
    CHECK(view.zoom() > 1.0);

    view.setViewAngles(10, 200);
    CHECK(view.elevation() == Approx(Camera3D::kMaxElevation));

    QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(300, 200), view.mapToGlobal(QPointF(300, 200)), Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&view, &dbl);
    CHECK(view.azimuth() == Approx(45));
    CHECK(view.elevation() == Approx(30));
    CHECK(view.zoom() == Approx(1));
}

TEST_CASE("Pattern3DView: shown widget paints without OpenGL errors")
{
    Pattern3DView view;
    view.resize(400, 300);
    view.setPattern(cosPattern(4));
    view.show();
    QApplication::processEvents();
    const QImage grabbed = view.grabFramebuffer();
    std::printf("plot3d widget OpenGL active: %s\n", view.isOpenGLActive() ? "yes" : "no");
    // Without a context there is nothing to grab; with one, the frame is drawn.
    if (view.isOpenGLActive())
        CHECK(stats(grabbed).foreground > 0);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("plot3d_tests"));
    return Catch::Session().run(argc, argv);
}
