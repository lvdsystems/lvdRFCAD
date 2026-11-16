#include "plot3d/Scene3DRender.h"

#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace lvd {

RenderVertex renderVertex(const QVector3D &position, const QVector3D &normal, const QColor &color, float alpha)
{
    return RenderVertex{position.x(),
                        position.y(),
                        position.z(),
                        normal.x(),
                        normal.y(),
                        normal.z(),
                        static_cast<float>(color.redF()),
                        static_cast<float>(color.greenF()),
                        static_cast<float>(color.blueF()),
                        static_cast<float>(color.alphaF()) * alpha};
}

namespace {

struct Projected {
    QPointF pixel;
    QVector3D viewPos;
    bool visible = false;
};

struct Primitive {
    float depth = 0; // distance from the eye; painted far to near
    RenderPrimitive kind = RenderPrimitive::Triangles;
    int batch = 0;
    quint32 a = 0, b = 0, c = 0;
    QColor color;
};

QColor vertexColor(const RenderVertex &v)
{
    return QColor::fromRgbF(v.r, v.g, v.b, v.a);
}

QColor averageColor(const RenderVertex &a, const RenderVertex &b, const RenderVertex &c)
{
    return QColor::fromRgbF((a.r + b.r + c.r) / 3.0f, (a.g + b.g + c.g) / 3.0f, (a.b + b.b + c.b) / 3.0f, (a.a + b.a + c.a) / 3.0f);
}

QColor averageColor(const RenderVertex &a, const RenderVertex &b)
{
    return QColor::fromRgbF((a.r + b.r) / 2.0f, (a.g + b.g) / 2.0f, (a.b + b.b) / 2.0f, (a.a + b.a) / 2.0f);
}

// The same headlight term as the fragment shader, evaluated once per triangle.
QColor shade(const QColor &color, const QVector3D &viewNormal, const QVector3D &viewPos)
{
    const QVector3D n = viewNormal.normalized();
    const QVector3D l = (-viewPos).normalized();
    const float d = std::abs(QVector3D::dotProduct(n, l));
    const float k = kAmbient + kDiffuse * d;
    const float s = kSpecular * std::pow(d, kShininess);
    auto channel = [&](float c) { return std::clamp(c * k + s, 0.0f, 1.0f); };
    return QColor::fromRgbF(channel(static_cast<float>(color.redF())), channel(static_cast<float>(color.greenF())),
                            channel(static_cast<float>(color.blueF())), static_cast<float>(color.alphaF()));
}

} // namespace

void paintSceneSoftware(QPainter &painter, const RenderScene &scene, const Camera3D &camera, const QRectF &viewport)
{
    if (viewport.width() <= 0 || viewport.height() <= 0)
        return;
    const double aspect = viewport.width() / viewport.height();
    const QMatrix4x4 view = camera.viewMatrix();
    const QMatrix4x4 projection = camera.projectionMatrix(aspect);
    // Lines and points sit on surfaces; pull them towards the eye a little so
    // the depth sort does not bury them (the GL path uses polygon offset).
    const float lineBias = static_cast<float>(0.01 * camera.sceneRadius);

    std::vector<std::vector<Projected>> projected(scene.batches.size());
    std::vector<Primitive> primitives;
    for (std::size_t bi = 0; bi < scene.batches.size(); ++bi) {
        const RenderBatch &batch = scene.batches[bi];
        std::vector<Projected> &points = projected[bi];
        points.resize(batch.vertices.size());
        for (std::size_t i = 0; i < batch.vertices.size(); ++i) {
            const RenderVertex &v = batch.vertices[i];
            const QVector4D eyeSpace = view * QVector4D(v.x, v.y, v.z, 1.0f);
            Projected &p = points[i];
            p.viewPos = eyeSpace.toVector3D();
            const QVector4D clip = projection * eyeSpace;
            if (eyeSpace.z() >= 0 || clip.w() <= 0)
                continue;
            p.visible = true;
            p.pixel = QPointF(viewport.left() + (clip.x() / clip.w() + 1.0) * 0.5 * viewport.width(),
                              viewport.top() + (1.0 - clip.y() / clip.w()) * 0.5 * viewport.height());
        }

        const bool indexed = !batch.indices.empty();
        const std::size_t count = indexed ? batch.indices.size() : batch.vertices.size();
        auto index = [&](std::size_t k) { return indexed ? batch.indices[k] : static_cast<quint32>(k); };
        switch (batch.primitive) {
        case RenderPrimitive::Triangles:
            for (std::size_t k = 0; k + 2 < count; k += 3) {
                const quint32 a = index(k), b = index(k + 1), c = index(k + 2);
                if (!points[a].visible || !points[b].visible || !points[c].visible)
                    continue;
                const RenderVertex &va = batch.vertices[a];
                const RenderVertex &vb = batch.vertices[b];
                const RenderVertex &vc = batch.vertices[c];
                const QVector3D centre = (points[a].viewPos + points[b].viewPos + points[c].viewPos) / 3.0f;
                QColor color = averageColor(va, vb, vc);
                if (batch.lit) {
                    const QVector3D normal(va.nx + vb.nx + vc.nx, va.ny + vb.ny + vc.ny, va.nz + vb.nz + vc.nz);
                    color = shade(color, view.mapVector(normal), centre);
                }
                primitives.push_back({-centre.z(), RenderPrimitive::Triangles, static_cast<int>(bi), a, b, c, color});
            }
            break;
        case RenderPrimitive::Lines:
            for (std::size_t k = 0; k + 1 < count; k += 2) {
                const quint32 a = index(k), b = index(k + 1);
                if (!points[a].visible || !points[b].visible)
                    continue;
                const float depth = -0.5f * (points[a].viewPos.z() + points[b].viewPos.z()) - lineBias;
                primitives.push_back({depth, RenderPrimitive::Lines, static_cast<int>(bi), a, b, 0,
                                      averageColor(batch.vertices[a], batch.vertices[b])});
            }
            break;
        case RenderPrimitive::Points:
            for (std::size_t k = 0; k < count; ++k) {
                const quint32 a = index(k);
                if (!points[a].visible)
                    continue;
                primitives.push_back({-points[a].viewPos.z() - lineBias, RenderPrimitive::Points, static_cast<int>(bi), a, 0, 0,
                                      vertexColor(batch.vertices[a])});
            }
            break;
        }
    }
    std::stable_sort(primitives.begin(), primitives.end(), [](const Primitive &l, const Primitive &r) { return l.depth > r.depth; });

    painter.save();
    painter.setClipRect(viewport);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPolygonF triangle(3);
    for (const Primitive &p : primitives) {
        const RenderBatch &batch = scene.batches[static_cast<std::size_t>(p.batch)];
        const std::vector<Projected> &points = projected[static_cast<std::size_t>(p.batch)];
        switch (p.kind) {
        case RenderPrimitive::Triangles:
            triangle[0] = points[p.a].pixel;
            triangle[1] = points[p.b].pixel;
            triangle[2] = points[p.c].pixel;
            // An outline in the fill colour hides the antialiasing seams
            // between neighbouring triangles; translucent ones would double up.
            if (p.color.alpha() == 255)
                painter.setPen(QPen(p.color, 0.8));
            else
                painter.setPen(Qt::NoPen);
            painter.setBrush(p.color);
            painter.drawPolygon(triangle);
            break;
        case RenderPrimitive::Lines:
            painter.setPen(QPen(p.color, batch.width, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(points[p.a].pixel, points[p.b].pixel);
            break;
        case RenderPrimitive::Points:
            painter.setPen(Qt::NoPen);
            painter.setBrush(p.color);
            painter.drawEllipse(points[p.a].pixel, batch.width * 0.5, batch.width * 0.5);
            break;
        }
    }
    painter.restore();
}

} // namespace lvd
