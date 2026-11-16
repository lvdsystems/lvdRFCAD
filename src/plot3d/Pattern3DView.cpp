#include "plot3d/Pattern3DView.h"

#include "plot3d/Scene3DRender.h"

#include <QEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace lvd {

namespace {

constexpr double kColorBarArea = 84.0;   // strip on the right reserved for the colour bar
constexpr double kAxisLengthFactor = 1.2; // axes reach past the content
constexpr double kOrbitDegPerPixel = 0.4;

const QColor kAxisColors[3] = {QColor(214, 39, 40), QColor(44, 160, 44), QColor(31, 100, 220)};

// Nice dB step for a colour bar with at most about seven ticks.
double niceStep(double range)
{
    for (double step : {1.0, 2.0, 2.5, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0})
        if (range / step <= 7.0)
            return step;
    return std::pow(10.0, std::ceil(std::log10(range / 7.0)));
}

// Moller-Trumbore; returns the ray parameter or a negative value.
float intersect(const QVector3D &origin, const QVector3D &dir, const QVector3D &a, const QVector3D &b, const QVector3D &c)
{
    const QVector3D e1 = b - a;
    const QVector3D e2 = c - a;
    const QVector3D p = QVector3D::crossProduct(dir, e2);
    const float det = QVector3D::dotProduct(e1, p);
    if (std::abs(det) < 1e-12f)
        return -1.0f;
    const float inv = 1.0f / det;
    const QVector3D s = origin - a;
    const float u = QVector3D::dotProduct(s, p) * inv;
    if (u < 0.0f || u > 1.0f)
        return -1.0f;
    const QVector3D q = QVector3D::crossProduct(s, e1);
    const float v = QVector3D::dotProduct(dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return -1.0f;
    return QVector3D::dotProduct(e2, q) * inv;
}

void addLineBatch(RenderScene &scene, const std::vector<QVector3D> &points, const QColor &color, float width)
{
    if (points.size() < 2)
        return;
    RenderBatch batch;
    batch.primitive = RenderPrimitive::Lines;
    batch.lit = false;
    batch.width = width;
    for (const QVector3D &p : points)
        batch.vertices.push_back(renderVertex(p, QVector3D(0, 0, 1), color));
    for (std::size_t i = 0; i + 1 < points.size(); ++i)
        batch.indices.insert(batch.indices.end(), {static_cast<quint32>(i), static_cast<quint32>(i + 1)});
    scene.batches.push_back(std::move(batch));
}

// Arrow head along an axis: a cone from base centre to tip.
void addCone(RenderBatch &batch, const QVector3D &base, const QVector3D &tip, float radius, const QColor &color)
{
    const QVector3D axis = (tip - base).normalized();
    QVector3D u = QVector3D::crossProduct(axis, std::abs(axis.z()) < 0.9f ? QVector3D(0, 0, 1) : QVector3D(1, 0, 0)).normalized();
    const QVector3D v = QVector3D::crossProduct(axis, u);
    constexpr int kSegments = 18;
    for (int i = 0; i < kSegments; ++i) {
        const double a0 = 2.0 * std::numbers::pi * i / kSegments;
        const double a1 = 2.0 * std::numbers::pi * (i + 1) / kSegments;
        const QVector3D r0 = u * static_cast<float>(std::cos(a0)) + v * static_cast<float>(std::sin(a0));
        const QVector3D r1 = u * static_cast<float>(std::cos(a1)) + v * static_cast<float>(std::sin(a1));
        const QVector3D p0 = base + r0 * radius;
        const QVector3D p1 = base + r1 * radius;
        const QVector3D side = QVector3D::crossProduct(p0 - tip, p1 - tip).normalized();
        for (const QVector3D &p : {tip, p0, p1})
            batch.vertices.push_back(renderVertex(p, side, color));
        for (const QVector3D &p : {base, p1, p0})
            batch.vertices.push_back(renderVertex(p, -axis, color));
    }
}

QString formatDb(double value)
{
    return QString::number(value, 'f', std::abs(value - std::round(value)) < 1e-9 ? 0 : 1);
}

} // namespace

Pattern3DView::Pattern3DView(QWidget *parent)
    : QOpenGLWidget(parent)
{
    QSurfaceFormat surfaceFormat = format();
    surfaceFormat.setDepthBufferSize(24);
    surfaceFormat.setSamples(4);
    setFormat(surfaceFormat);
    setMouseTracking(true);
    m_theme = PlotTheme::fromPalette(palette());
}

Pattern3DView::~Pattern3DView()
{
    if (m_gl && context()) {
        makeCurrent();
        releaseGL();
        doneCurrent();
    }
}

// ---- Pattern

void Pattern3DView::setPattern(const SphericalPattern &pattern, double dynamicRangeDb)
{
    m_pattern = pattern;
    m_dynamicRangeDb = dynamicRangeDb > 0 ? dynamicRangeDb : 40.0;
    rebuildMesh();
}

void Pattern3DView::clearPattern()
{
    m_pattern = {};
    rebuildMesh();
}

void Pattern3DView::setColorMap(ColorMap map)
{
    if (map == m_colorMap)
        return;
    m_colorMap = map;
    rebuildMesh();
}

void Pattern3DView::setColorBar(const QString &label, const QString &unit)
{
    m_colorBarLabel = label;
    m_colorBarUnit = unit;
    update();
}

void Pattern3DView::setColorBarVisible(bool visible)
{
    if (visible == m_colorBarVisible)
        return;
    m_colorBarVisible = visible;
    update();
}

void Pattern3DView::setPatternScale(double sceneRadius)
{
    m_patternScale = std::isfinite(sceneRadius) && sceneRadius > 0 ? sceneRadius : 0.0;
    rebuildMesh();
}

double Pattern3DView::effectivePatternScale() const
{
    if (m_patternScale > 0)
        return m_patternScale;
    const double geometry = geometryRadius();
    return geometry > 0 ? 1.5 * geometry : 1.0;
}

void Pattern3DView::rebuildMesh()
{
    m_mesh = buildPatternMesh(m_pattern, m_dynamicRangeDb, effectivePatternScale(), m_colorMap);
    setHover({}, {}, false);
    sceneChanged();
}

// ---- Geometry

void Pattern3DView::setPoints(std::vector<Scene3DPoint> points)
{
    m_points = std::move(points);
    if (m_patternScale <= 0)
        rebuildMesh(); // the automatic pattern scale follows the geometry
    else
        sceneChanged();
}

void Pattern3DView::setLines(std::vector<Scene3DLine> lines)
{
    m_lines = std::move(lines);
    if (m_patternScale <= 0)
        rebuildMesh();
    else
        sceneChanged();
}

void Pattern3DView::addMesh(const Scene3DMesh &mesh)
{
    m_meshes.push_back(mesh);
    if (m_patternScale <= 0)
        rebuildMesh();
    else
        sceneChanged();
}

void Pattern3DView::clearGeometry()
{
    m_points.clear();
    m_lines.clear();
    m_meshes.clear();
    if (m_patternScale <= 0)
        rebuildMesh();
    else
        sceneChanged();
}

double Pattern3DView::geometryRadius() const
{
    float r = 0;
    for (const Scene3DPoint &p : m_points)
        r = std::max(r, p.position.length());
    for (const Scene3DLine &line : m_lines)
        for (const QVector3D &p : line.points)
            r = std::max(r, p.length());
    for (const Scene3DMesh &mesh : m_meshes)
        for (const QVector3D &p : mesh.vertices)
            r = std::max(r, p.length());
    return std::isfinite(r) ? r : 0.0;
}

double Pattern3DView::sceneRadius() const
{
    double content = std::max(hasPattern() ? effectivePatternScale() : 0.0, geometryRadius());
    if (!(content > 0))
        content = 1.0;
    // Leave room for the axis arrows and their labels.
    return m_axesVisible ? content * (kAxisLengthFactor + 0.1) : content;
}

// ---- Decorations

void Pattern3DView::setAxesVisible(bool visible)
{
    if (visible == m_axesVisible)
        return;
    m_axesVisible = visible;
    sceneChanged();
}

void Pattern3DView::setWireframe(bool visible)
{
    if (visible == m_wireframe)
        return;
    m_wireframe = visible;
    sceneChanged();
}

// ---- View

void Pattern3DView::setViewAngles(double azimuthDeg, double elevationDeg)
{
    m_camera.azimuthDeg = std::remainder(azimuthDeg, 360.0);
    m_camera.elevationDeg = std::clamp(elevationDeg, -Camera3D::kMaxElevation, Camera3D::kMaxElevation);
    viewUpdated();
}

void Pattern3DView::setZoom(double zoom)
{
    if (!std::isfinite(zoom))
        return;
    m_camera.zoom = std::clamp(zoom, 0.05, 100.0);
    viewUpdated();
}

void Pattern3DView::resetView()
{
    m_camera = Camera3D{};
    viewUpdated();
}

Camera3D Pattern3DView::camera() const
{
    Camera3D c = m_camera;
    c.sceneRadius = sceneRadius();
    return c;
}

QRectF Pattern3DView::sceneRect(const QSizeF &size) const
{
    if (hasPattern() && m_colorBarVisible && size.width() > 3 * kColorBarArea)
        return QRectF(0, 0, size.width() - kColorBarArea, size.height());
    return QRectF(QPointF(0, 0), size);
}

QPointF Pattern3DView::projectToWidget(const QVector3D &point) const
{
    return camera().project(point, sceneRect(size()));
}

bool Pattern3DView::pickPattern(const QPointF &position, quint32 *vertex, QVector3D *point) const
{
    if (m_mesh.triangles.empty())
        return false;
    const QRectF viewport = sceneRect(size());
    if (!viewport.contains(position))
        return false;
    QVector3D origin, dir;
    camera().ray(position, viewport, &origin, &dir);
    float best = INFINITY;
    std::size_t hitTriangle = 0;
    for (std::size_t k = 0; k + 2 < m_mesh.triangles.size(); k += 3) {
        const float t = intersect(origin, dir, m_mesh.positions[m_mesh.triangles[k]], m_mesh.positions[m_mesh.triangles[k + 1]],
                                  m_mesh.positions[m_mesh.triangles[k + 2]]);
        if (t > 0 && t < best) {
            best = t;
            hitTriangle = k;
        }
    }
    if (!std::isfinite(best))
        return false;
    // Report the nearest sample rather than an interpolated value: it is what
    // the solver computed.
    const QVector3D hit = origin + dir * best;
    quint32 nearest = m_mesh.triangles[hitTriangle];
    for (std::size_t i = 1; i < 3; ++i) {
        const quint32 candidate = m_mesh.triangles[hitTriangle + i];
        if ((m_mesh.positions[candidate] - hit).lengthSquared() < (m_mesh.positions[nearest] - hit).lengthSquared())
            nearest = candidate;
    }
    if (vertex)
        *vertex = nearest;
    if (point)
        *point = hit;
    return true;
}

QString Pattern3DView::readoutAt(const QPointF &position) const
{
    quint32 vertex = 0;
    if (!pickPattern(position, &vertex, nullptr))
        return {};
    return readoutText(vertex);
}

QString Pattern3DView::readoutText(quint32 vertex) const
{
    return QStringLiteral("theta %1 deg, phi %2 deg: %3 %4")
        .arg(QString::number(m_mesh.thetaDeg[vertex], 'f', 1), QString::number(m_mesh.phiDeg[vertex], 'f', 1),
             QString::number(m_mesh.valuesDb[vertex], 'f', 1), m_colorBarUnit)
        .trimmed();
}

void Pattern3DView::setHover(const QString &text, const QVector3D &point, bool valid)
{
    const bool moved = valid != m_hoverValid || (valid && point != m_hoverPoint);
    m_hoverPoint = point;
    m_hoverValid = valid;
    if (moved)
        update();
    if (text == m_hoverText)
        return;
    m_hoverText = text;
    Q_EMIT hoverReadoutChanged(text);
}

void Pattern3DView::sceneChanged()
{
    m_scene.reset();
    ++m_sceneGeneration;
    update();
}

void Pattern3DView::viewUpdated()
{
    update();
    Q_EMIT viewChanged();
}

// ---- Scene

const RenderScene &Pattern3DView::renderScene()
{
    if (m_scene)
        return *m_scene;
    m_scene = std::make_unique<RenderScene>();
    RenderScene &scene = *m_scene;
    const QColor defaultColor = m_theme.text;

    if (!m_mesh.isEmpty()) {
        if (!m_mesh.triangles.empty()) {
            RenderBatch surface;
            surface.vertices.reserve(m_mesh.positions.size());
            for (std::size_t k = 0; k < m_mesh.positions.size(); ++k)
                surface.vertices.push_back(renderVertex(m_mesh.positions[k], m_mesh.normals[k], m_mesh.colors[k]));
            surface.indices = m_mesh.triangles;
            surface.pushedBack = m_wireframe;
            scene.batches.push_back(std::move(surface));
        }
        if (m_wireframe && !m_mesh.gridLines.empty()) {
            RenderBatch grid;
            grid.primitive = RenderPrimitive::Lines;
            grid.lit = false;
            grid.width = 1.0f;
            const QColor lineColor = m_theme.dark ? QColor(255, 255, 255, 110) : QColor(0, 0, 0, 110);
            for (const QVector3D &p : m_mesh.positions)
                grid.vertices.push_back(renderVertex(p, QVector3D(0, 0, 1), lineColor));
            grid.indices = m_mesh.gridLines;
            scene.batches.push_back(std::move(grid));
        }
        if (m_mesh.curve.size() >= 2) {
            RenderBatch curve;
            curve.primitive = RenderPrimitive::Lines;
            curve.lit = false;
            curve.width = 2.5f;
            for (std::size_t k = 0; k < m_mesh.positions.size(); ++k)
                curve.vertices.push_back(renderVertex(m_mesh.positions[k], QVector3D(0, 0, 1), m_mesh.colors[k]));
            for (std::size_t i = 0; i + 1 < m_mesh.curve.size(); ++i)
                curve.indices.insert(curve.indices.end(), {m_mesh.curve[i], m_mesh.curve[i + 1]});
            scene.batches.push_back(std::move(curve));
        }
    }

    for (const Scene3DMesh &mesh : m_meshes) {
        RenderBatch batch;
        const QColor color = mesh.color.isValid() ? mesh.color : defaultColor;
        const float opacity = std::clamp(mesh.opacity, 0.0f, 1.0f);
        batch.translucent = opacity < 1.0f;
        const std::size_t n = mesh.vertices.size();
        for (std::size_t k = 0; k + 2 < mesh.indices.size(); k += 3) {
            const quint32 a = mesh.indices[k], b = mesh.indices[k + 1], c = mesh.indices[k + 2];
            if (a >= n || b >= n || c >= n)
                continue;
            const QVector3D normal = QVector3D::crossProduct(mesh.vertices[b] - mesh.vertices[a], mesh.vertices[c] - mesh.vertices[a]).normalized();
            for (quint32 i : {a, b, c})
                batch.vertices.push_back(renderVertex(mesh.vertices[i], normal, color, opacity));
        }
        if (!batch.vertices.empty())
            scene.batches.push_back(std::move(batch));
    }

    for (const Scene3DLine &line : m_lines)
        addLineBatch(scene, line.points, line.color.isValid() ? line.color : defaultColor, line.width);

    std::map<float, RenderBatch> pointBatches; // one draw per marker size
    for (const Scene3DPoint &point : m_points) {
        RenderBatch &batch = pointBatches[point.size];
        batch.primitive = RenderPrimitive::Points;
        batch.lit = false;
        batch.width = point.size;
        batch.vertices.push_back(renderVertex(point.position, QVector3D(0, 0, 1), point.color.isValid() ? point.color : defaultColor));
    }
    for (auto &entry : pointBatches)
        scene.batches.push_back(std::move(entry.second));

    if (m_axesVisible) {
        const float length = static_cast<float>(sceneRadius() / (kAxisLengthFactor + 0.1) * kAxisLengthFactor);
        RenderBatch heads;
        for (int axis = 0; axis < 3; ++axis) {
            QVector3D direction;
            direction[axis] = 1.0f;
            const QVector3D base = direction * (length * 0.9f);
            // In pieces, so the fallback's depth sort hides the part inside the pattern.
            std::vector<QVector3D> pieces;
            constexpr int kPieces = 24;
            for (int i = 0; i <= kPieces; ++i)
                pieces.push_back(base * (static_cast<float>(i) / kPieces));
            addLineBatch(scene, pieces, kAxisColors[axis], 2.0f);
            addCone(heads, base, direction * length, length * 0.03f, kAxisColors[axis]);
        }
        scene.batches.push_back(std::move(heads));
    }
    return scene;
}

// ---- OpenGL

void Pattern3DView::initializeGL()
{
    // A new top-level window recreates the context: drop what belonged to
    // the old one (already released through aboutToBeDestroyed).
    m_gl = std::make_unique<SceneGLRenderer>();
    m_uploadedGeneration = 0;
    m_glActive = m_gl->initialize();
    if (!m_glActive) {
        m_gl.reset();
        qWarning("Pattern3DView: OpenGL shaders unavailable, using the software renderer");
        return;
    }
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        makeCurrent();
        releaseGL();
        doneCurrent();
    }, Qt::DirectConnection);
}

void Pattern3DView::releaseGL()
{
    if (m_gl)
        m_gl->release();
    m_gl.reset();
    m_glActive = false;
    m_uploadedGeneration = 0;
}

void Pattern3DView::paintGL()
{
    const QSizeF area = size();
    if (!m_glActive || !m_gl) {
        QPainter painter(this);
        painter.fillRect(rect(), m_theme.background);
        paintSoftware(painter, area);
        paintOverlay(painter, area);
        return;
    }
    QOpenGLFunctions *f = context()->functions();
    const QColor bg = m_theme.background;
    f->glClearColor(static_cast<float>(bg.redF()), static_cast<float>(bg.greenF()), static_cast<float>(bg.blueF()), 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (m_uploadedGeneration != m_sceneGeneration) {
        m_gl->upload(renderScene());
        m_uploadedGeneration = m_sceneGeneration;
    }
    const QRectF viewport = sceneRect(area);
    const double dpr = devicePixelRatioF();
    const QRect device(static_cast<int>(std::lround(viewport.left() * dpr)),
                       static_cast<int>(std::lround((area.height() - viewport.bottom()) * dpr)),
                       static_cast<int>(std::lround(viewport.width() * dpr)), static_cast<int>(std::lround(viewport.height() * dpr)));
    m_gl->render(camera(), device, viewport.width() / std::max(viewport.height(), 1.0), static_cast<float>(dpr));

    QPainter painter(this);
    paintOverlay(painter, area);
}

// ---- Painting

void Pattern3DView::paintSoftware(QPainter &painter, const QSizeF &size)
{
    paintSceneSoftware(painter, renderScene(), camera(), sceneRect(size));
}

void Pattern3DView::paintOverlay(QPainter &painter, const QSizeF &size) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QRectF viewport = sceneRect(size);
    const Camera3D cam = camera();
    if (m_axesVisible)
        paintAxisLabels(painter, cam, viewport);
    if (hasPattern() && m_colorBarVisible)
        paintColorBar(painter, size);
    // Hover marker (widget only: exports go through other paint devices).
    if (m_hoverValid && painter.device() == this) {
        QPointF p;
        if (cam.project(m_hoverPoint, viewport, &p)) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(m_theme.text, 1.5));
            painter.drawEllipse(p, 5, 5);
        }
    }
    painter.restore();
}

void Pattern3DView::paintAxisLabels(QPainter &painter, const Camera3D &camera, const QRectF &viewport) const
{
    const float length = static_cast<float>(sceneRadius() / (kAxisLengthFactor + 0.1) * kAxisLengthFactor);
    QFont font = painter.font();
    font.setBold(true);
    painter.setFont(font);
    const QFontMetricsF fm(font);
    static const char *const kNames[3] = {"x", "y", "z"};
    QPointF origin;
    const bool originVisible = camera.project(QVector3D(), viewport, &origin);
    for (int axis = 0; axis < 3; ++axis) {
        QVector3D tip;
        tip[axis] = length;
        QPointF p;
        if (!camera.project(tip, viewport, &p))
            continue;
        // Push the label beyond the arrow tip along its screen direction, so
        // it never sits on the arrow head.
        QPointF away = originVisible ? p - origin : QPointF(0, -1);
        const double n = std::hypot(away.x(), away.y());
        away = n > 1e-6 ? away / n : QPointF(0, -1);
        const QString name = QString::fromLatin1(kNames[axis]);
        const QSizeF box(fm.horizontalAdvance(name) + 4, fm.height());
        QRectF r(p + away * (box.height() * 0.8) - QPointF(box.width() / 2, box.height() / 2), box);
        // Keep labels inside the scene area.
        r.moveLeft(std::clamp(r.left(), viewport.left() + 2, viewport.right() - r.width() - 2));
        r.moveTop(std::clamp(r.top(), viewport.top() + 2, viewport.bottom() - r.height() - 2));
        painter.setPen(kAxisColors[axis]);
        painter.drawText(r, Qt::AlignCenter, name);
    }
    // The angle convention, since tools disagree on it.
    QFont small = painter.font();
    small.setBold(false);
    painter.setFont(small);
    painter.setPen(m_theme.mutedText);
    const QString hint = QStringLiteral("θ from +z, φ from +x towards +y");
    const QFontMetricsF sfm(small);
    painter.drawText(QPointF(viewport.left() + 8, viewport.bottom() - 8 - sfm.descent()), hint);
}

void Pattern3DView::paintColorBar(QPainter &painter, const QSizeF &size) const
{
    if (!std::isfinite(m_mesh.maxDb))
        return;
    const QFontMetricsF fm(painter.font());
    const double top = std::max(size.height() * 0.15, 2.5 * fm.height());
    const double bottom = size.height() - std::max(size.height() * 0.15, 2.5 * fm.height());
    if (bottom - top < 30)
        return;
    const QRectF bar(size.width() - kColorBarArea + 10, top, 14, bottom - top);

    constexpr int kSlices = 96;
    for (int i = 0; i < kSlices; ++i) {
        const double t0 = static_cast<double>(i) / kSlices;
        const double t1 = static_cast<double>(i + 1) / kSlices;
        const QRectF slice(bar.left(), bar.bottom() - t1 * bar.height(), bar.width(), (t1 - t0) * bar.height() + 0.6);
        painter.fillRect(slice, colorMapColor(m_colorMap, (t0 + t1) / 2));
    }
    painter.setPen(QPen(m_theme.axis, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(bar);

    const double range = m_mesh.maxDb - m_mesh.floorDb;
    const double step = niceStep(range);
    painter.setPen(m_theme.text);
    for (double v = std::ceil(m_mesh.floorDb / step - 1e-9) * step; v <= m_mesh.maxDb + 1e-9; v += step) {
        const double y = bar.bottom() - (v - m_mesh.floorDb) / range * bar.height();
        painter.drawLine(QPointF(bar.right(), y), QPointF(bar.right() + 4, y));
        const double shown = std::abs(v) < 1e-9 ? 0.0 : v;
        painter.drawText(QRectF(bar.right() + 6, y - fm.height() / 2, kColorBarArea - 32, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                         formatDb(shown));
    }

    // Title above, maximum below; both right-aligned so they stay inside.
    const QString title = m_colorBarUnit.isEmpty() ? m_colorBarLabel : QStringLiteral("%1 (%2)").arg(m_colorBarLabel, m_colorBarUnit);
    const QRectF titleRect(size.width() - 160, top - 1.6 * fm.height(), 154, fm.height());
    painter.drawText(titleRect, Qt::AlignRight | Qt::AlignVCenter, title);
    painter.setPen(m_theme.mutedText);
    const QRectF maxRect(size.width() - 160, bottom + 0.5 * fm.height(), 154, fm.height());
    painter.drawText(maxRect, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("max %1").arg(QString::number(m_mesh.maxDb, 'f', 1)));
}

// ---- Export

void Pattern3DView::setSoftwareRenderingForced(bool forced)
{
    m_forceSoftware = forced;
}

QImage Pattern3DView::renderImageGL(const QSize &size)
{
    QOpenGLContext *previous = QOpenGLContext::currentContext();
    QSurface *previousSurface = previous ? previous->surface() : nullptr;
    auto restore = [&] {
        if (previous && previousSurface)
            previous->makeCurrent(previousSurface);
    };

    QOpenGLContext context;
    context.setFormat(QSurfaceFormat::defaultFormat());
    if (!context.create())
        return {};
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface)) {
        restore();
        return {};
    }

    QImage image;
    {
        QOpenGLFramebufferObjectFormat fboFormat;
        fboFormat.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        fboFormat.setSamples(4);
        auto fbo = std::make_unique<QOpenGLFramebufferObject>(size, fboFormat);
        if (!fbo->isValid()) {
            fboFormat.setSamples(0);
            fbo = std::make_unique<QOpenGLFramebufferObject>(size, fboFormat);
        }
        SceneGLRenderer renderer;
        if (fbo->isValid() && fbo->bind() && renderer.initialize()) {
            QOpenGLFunctions *f = context.functions();
            const QColor bg = m_theme.background;
            f->glClearColor(static_cast<float>(bg.redF()), static_cast<float>(bg.greenF()), static_cast<float>(bg.blueF()), 1.0f);
            f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            renderer.upload(renderScene());
            const QRectF viewport = sceneRect(size);
            const QRect device(static_cast<int>(std::lround(viewport.left())),
                               static_cast<int>(std::lround(size.height() - viewport.bottom())),
                               static_cast<int>(std::lround(viewport.width())), static_cast<int>(std::lround(viewport.height())));
            renderer.render(camera(), device, viewport.width() / std::max(viewport.height(), 1.0));
            f->glFinish();
            fbo->release();
            image = fbo->toImage();
        }
        renderer.release();
        fbo.reset();
    }
    context.doneCurrent();
    restore();
    if (image.isNull())
        return {};
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage Pattern3DView::renderImage(const QSize &size)
{
    if (size.isEmpty())
        return {};
    QImage image;
    if (!m_forceSoftware)
        image = renderImageGL(size);
    m_lastImageGL = !image.isNull();
    QPainter painter;
    if (image.isNull()) {
        image = QImage(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(m_theme.background);
        painter.begin(&image);
        paintSoftware(painter, size);
    } else {
        painter.begin(&image);
    }
    paintOverlay(painter, size);
    painter.end();
    return image;
}

// ---- Events

QSize Pattern3DView::sizeHint() const
{
    return QSize(640, 480);
}

QSize Pattern3DView::minimumSizeHint() const
{
    return QSize(200, 160);
}

void Pattern3DView::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        m_theme = PlotTheme::fromPalette(palette());
        sceneChanged(); // default geometry and grid colours follow the theme
    }
    QOpenGLWidget::changeEvent(event);
}

void Pattern3DView::mousePressEvent(QMouseEvent *event)
{
    m_lastPos = event->position();
    if (event->button() == Qt::LeftButton)
        m_drag = Drag::Orbit;
    else if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton)
        m_drag = Drag::Pan;
    else
        m_drag = Drag::None;
    if (m_drag != Drag::None) {
        setHover({}, {}, false);
        event->accept();
    }
}

void Pattern3DView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    const QPointF delta = pos - m_lastPos;
    m_lastPos = pos;
    switch (m_drag) {
    case Drag::Orbit:
        // Grab-the-scene feel: dragging right turns the scene right.
        m_camera.orbit(-delta.x() * kOrbitDegPerPixel, delta.y() * kOrbitDegPerPixel);
        viewUpdated();
        return;
    case Drag::Pan: {
        Camera3D c = camera();
        c.pan(delta, sceneRect(size()));
        m_camera.target = c.target;
        viewUpdated();
        return;
    }
    case Drag::None:
        break;
    }
    QVector3D point;
    quint32 vertex = 0;
    if (pickPattern(pos, &vertex, &point))
        setHover(readoutText(vertex), point, true);
    else
        setHover({}, {}, false);
}

void Pattern3DView::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    m_drag = Drag::None;
}

void Pattern3DView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        resetView();
}

void Pattern3DView::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y();
    if (steps != 0)
        setZoom(m_camera.zoom * std::pow(1.0015, steps));
    event->accept();
}

void Pattern3DView::leaveEvent(QEvent *event)
{
    setHover({}, {}, false);
    QOpenGLWidget::leaveEvent(event);
}

} // namespace lvd
