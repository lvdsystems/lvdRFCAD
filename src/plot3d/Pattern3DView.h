#pragma once

#include "plot/ColorMaps.h"
#include "plot/PlotTheme.h"
#include "plot3d/Scene3D.h"

#include <QImage>
#include <QOpenGLWidget>
#include <QString>

#include <memory>
#include <vector>

class QPainter;

namespace lvd {

class SceneGLRenderer;
struct RenderScene;

// 3D view of an antenna radiation pattern with the antenna geometry: an OpenGL
// scene of primitives (pattern surface, points, polylines, triangle meshes)
// with QPainter overlays for labels and the colour bar. Without OpenGL the same
// view is painted by a QPainter fallback, so export and tests always work.
//
// Interaction: left drag orbits, wheel zooms, right or middle drag pans,
// double-click resets the view. z is up; the default view is isometric.
class Pattern3DView : public QOpenGLWidget
{
    Q_OBJECT
public:
    explicit Pattern3DView(QWidget *parent = nullptr);
    ~Pattern3DView() override;

    // ---- Pattern
    // Radius = (dB - (max - range)) / range, clamped at 0 and scaled to the
    // pattern scale; colour = the colour map at the same fraction.
    void setPattern(const SphericalPattern &pattern, double dynamicRangeDb = 40.0);
    void clearPattern();
    bool hasPattern() const { return !m_mesh.isEmpty(); }
    const PatternMesh &patternMesh() const { return m_mesh; }
    double dynamicRange() const { return m_dynamicRangeDb; }
    void setColorMap(ColorMap map);
    ColorMap colorMap() const { return m_colorMap; }
    void setColorBar(const QString &label, const QString &unit); // e.g. "Gain", "dBi"
    void setColorBarVisible(bool visible);
    bool isColorBarVisible() const { return m_colorBarVisible; }
    // Radius of the pattern maximum in scene units (metres), so it can be
    // sized against the geometry. 0 (default): 1.5 x the geometry's extent,
    // or 1 without geometry.
    void setPatternScale(double sceneRadius);
    double patternScale() const { return m_patternScale; }
    double effectivePatternScale() const;

    // ---- Geometry, in metres
    void setPoints(std::vector<Scene3DPoint> points);
    void setLines(std::vector<Scene3DLine> lines);
    void addMesh(const Scene3DMesh &mesh);
    void clearGeometry();

    // ---- Decorations
    void setAxesVisible(bool visible); // x red, y green, z blue, with the theta/phi convention
    bool areAxesVisible() const { return m_axesVisible; }
    void setWireframe(bool visible);   // grid lines over the pattern surface
    bool isWireframe() const { return m_wireframe; }

    // ---- View
    void setViewAngles(double azimuthDeg, double elevationDeg);
    double azimuth() const { return m_camera.azimuthDeg; }
    double elevation() const { return m_camera.elevationDeg; }
    void setZoom(double zoom);
    double zoom() const { return m_camera.zoom; }
    void resetView();
    // The camera as used for the widget (scene radius up to date).
    Camera3D camera() const;
    // Area of the 3D scene in a target of the given size (the colour bar
    // takes a strip on the right).
    QRectF sceneRect(const QSizeF &size) const;
    QPointF projectToWidget(const QVector3D &point) const;
    // Pattern value under a widget position, empty when none.
    QString readoutAt(const QPointF &position) const;
    QString hoverReadout() const { return m_hoverText; }

    // ---- Export
    // Offscreen rendering of the current view: OpenGL through an offscreen
    // surface when available, else the QPainter fallback.
    QImage renderImage(const QSize &size);
    // Always use the QPainter renderer (tests, broken drivers).
    void setSoftwareRenderingForced(bool forced);
    bool isSoftwareRenderingForced() const { return m_forceSoftware; }
    bool lastImageUsedOpenGL() const { return m_lastImageGL; }
    // False when the widget's own context could not run the shaders; it then
    // paints with the fallback.
    bool isOpenGLActive() const { return m_glActive; }

    const PlotTheme &theme() const { return m_theme; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void hoverReadoutChanged(const QString &text);
    void viewChanged();

protected:
    void initializeGL() override;
    void paintGL() override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void rebuildMesh();
    void sceneChanged(); // the render scene must be rebuilt
    void viewUpdated();
    const RenderScene &renderScene();
    void releaseGL();
    double geometryRadius() const;
    double sceneRadius() const;
    void paintSoftware(QPainter &painter, const QSizeF &size);
    void paintOverlay(QPainter &painter, const QSizeF &size) const;
    void paintAxisLabels(QPainter &painter, const Camera3D &camera, const QRectF &viewport) const;
    void paintColorBar(QPainter &painter, const QSizeF &size) const;
    QImage renderImageGL(const QSize &size);
    // Ray cast against the pattern surface: nearest sample and hit point.
    bool pickPattern(const QPointF &position, quint32 *vertex, QVector3D *point) const;
    QString readoutText(quint32 vertex) const;
    void setHover(const QString &text, const QVector3D &point, bool valid);

    SphericalPattern m_pattern;
    double m_dynamicRangeDb = 40.0;
    PatternMesh m_mesh;
    ColorMap m_colorMap = ColorMap::Viridis;
    QString m_colorBarLabel = QStringLiteral("Gain");
    QString m_colorBarUnit = QStringLiteral("dBi");
    bool m_colorBarVisible = true;
    double m_patternScale = 0;

    std::vector<Scene3DPoint> m_points;
    std::vector<Scene3DLine> m_lines;
    std::vector<Scene3DMesh> m_meshes;

    bool m_axesVisible = true;
    bool m_wireframe = false;

    Camera3D m_camera; // angles, zoom and pan; the scene radius is set on use

    PlotTheme m_theme;
    std::unique_ptr<RenderScene> m_scene; // null: rebuild
    std::unique_ptr<SceneGLRenderer> m_gl;
    quint64 m_sceneGeneration = 1;
    quint64 m_uploadedGeneration = 0;
    bool m_glActive = false;
    bool m_forceSoftware = false;
    bool m_lastImageGL = false;

    enum class Drag { None, Orbit, Pan };
    Drag m_drag = Drag::None;
    QPointF m_lastPos;
    QString m_hoverText;
    QVector3D m_hoverPoint;
    bool m_hoverValid = false;
};

} // namespace lvd
