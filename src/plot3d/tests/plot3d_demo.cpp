// Renders an example 3D pattern on the native platform through the widget's
// OpenGL path, the offscreen OpenGL export and the QPainter fallback, saves
// PNGs and quits. Not a test: used to check the look.
//
// Usage: plot3d_demo <output folder> [light|dark]

#include "plot3d/Pattern3DView.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QStyleHints>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <numbers>

using namespace lvd;

namespace {

void settle()
{
    for (int i = 0; i < 10; ++i) {
        QApplication::processEvents();
        QThread::msleep(20);
    }
}

bool save(const QImage &image, const QString &path)
{
    const bool ok = !image.isNull() && image.save(path);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", qPrintable(QDir::toNativeSeparators(path)));
    return ok;
}

// 4 x 4 broadside array of cos-like elements, lambda/2 spacing: a main lobe
// along +z with side lobes, and a weak back lobe.
SphericalPattern arrayPattern(double phiFrom, double phiTo)
{
    SphericalPattern p;
    for (int i = 0; i <= 90; ++i)
        p.theta.push_back(2.0 * i);
    for (int j = 0; j <= 72; ++j)
        p.phi.push_back(phiFrom + (phiTo - phiFrom) * j / 72.0);
    constexpr double kDeg = std::numbers::pi / 180.0;
    for (double theta : p.theta) {
        for (double phi : p.phi) {
            const double t = theta * kDeg;
            const double ph = phi * kDeg;
            const double u = std::numbers::pi * std::sin(t) * std::cos(ph);
            const double v = std::numbers::pi * std::sin(t) * std::sin(ph);
            auto af = [](double x) { return std::abs(std::sin(4 * x / 2) / (4 * std::sin(x / 2) + 1e-12)); };
            const double afx = std::abs(u) < 1e-9 ? 1.0 : af(u);
            const double afy = std::abs(v) < 1e-9 ? 1.0 : af(v);
            const double element = theta <= 90 ? std::pow(std::cos(t), 1.5) + 0.01 : 0.01;
            const double power = element * element * afx * afx * afy * afy;
            p.valuesDb.push_back(10 * std::log10(power + 1e-9) + 18.0);
        }
    }
    return p;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: plot3d_demo <output folder> [light|dark]\n");
        return 2;
    }
    const QString folder = QString::fromLocal8Bit(argv[1]);
    const QString theme = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("light");
    QGuiApplication::styleHints()->setColorScheme(theme == QLatin1String("dark") ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    QDir().mkpath(folder);

    Pattern3DView view;
    view.resize(900, 650);
    view.setPattern(arrayPattern(-180, 180), 40.0);
    view.setColorBar(QStringLiteral("Directivity"), QStringLiteral("dBi"));
    std::vector<Scene3DPoint> elements;
    const double spacing = 0.075; // lambda/2 at 2 GHz
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            elements.push_back({QVector3D(static_cast<float>((i - 1.5) * spacing), static_cast<float>((j - 1.5) * spacing), 0.0f),
                                QColor(230, 120, 20), 9.0f});
    view.setPoints(elements);
    // Ground plane: a translucent square under the elements.
    Scene3DMesh ground;
    const float g = 0.18f;
    ground.vertices = {QVector3D(-g, -g, -0.01f), QVector3D(g, -g, -0.01f), QVector3D(g, g, -0.01f), QVector3D(-g, g, -0.01f)};
    ground.indices = {0, 1, 2, 0, 2, 3};
    ground.color = QColor(150, 150, 150);
    ground.opacity = 0.6f;
    view.addMesh(ground);
    Scene3DLine outline;
    outline.points = {QVector3D(-g, -g, -0.01f), QVector3D(g, -g, -0.01f), QVector3D(g, g, -0.01f), QVector3D(-g, g, -0.01f),
                      QVector3D(-g, -g, -0.01f)};
    outline.color = QColor(90, 90, 90);
    view.setLines({outline});
    view.show();
    settle();

    std::printf("widget OpenGL active: %s\n", view.isOpenGLActive() ? "yes" : "no");
    save(view.grabFramebuffer(), folder + QStringLiteral("/widget_%1.png").arg(theme));

    QElapsedTimer timer;
    timer.start();
    const QImage gl = view.renderImage(QSize(900, 650));
    std::printf("renderImage: %s, %lld ms\n", view.lastImageUsedOpenGL() ? "OpenGL" : "software", static_cast<long long>(timer.elapsed()));
    save(gl, folder + QStringLiteral("/image_gl_%1.png").arg(theme));

    view.setSoftwareRenderingForced(true);
    timer.restart();
    const QImage soft = view.renderImage(QSize(900, 650));
    std::printf("software renderImage: %lld ms\n", static_cast<long long>(timer.elapsed()));
    save(soft, folder + QStringLiteral("/image_soft_%1.png").arg(theme));
    view.setSoftwareRenderingForced(false);

    view.setWireframe(true);
    view.setColorMap(ColorMap::Jet);
    view.setViewAngles(-60, 15);
    settle();
    save(view.grabFramebuffer(), folder + QStringLiteral("/widget_wire_%1.png").arg(theme));
    view.setSoftwareRenderingForced(true);
    save(view.renderImage(QSize(700, 700)), folder + QStringLiteral("/image_soft_wire_%1.png").arg(theme));
    view.setSoftwareRenderingForced(false);

    // A single phi cut: shown as a curve.
    SphericalPattern cut = arrayPattern(0, 0);
    cut.phi = {0.0};
    cut.valuesDb.clear();
    const SphericalPattern full = arrayPattern(0, 360);
    for (std::size_t i = 0; i < full.theta.size(); ++i)
        cut.valuesDb.push_back(full.valuesDb[i * full.phi.size()]);
    view.clearGeometry();
    view.setPattern(cut);
    view.resetView();
    settle();
    save(view.grabFramebuffer(), folder + QStringLiteral("/widget_cut_%1.png").arg(theme));
    return 0;
}
