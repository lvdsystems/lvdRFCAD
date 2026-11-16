#pragma once

// Internal: the scene flattened into draw batches, consumed by both the OpenGL
// renderer and the QPainter fallback so the two paths draw the same thing.

#include "plot3d/Scene3D.h"

#include <QColor>
#include <QMatrix4x4>
#include <QRectF>

#include <memory>
#include <vector>

class QOpenGLShaderProgram;
class QOpenGLVertexArrayObject;
class QPainter;

namespace lvd {

struct RenderVertex {
    float x, y, z;
    float nx, ny, nz;
    float r, g, b, a;
};

enum class RenderPrimitive { Triangles, Lines, Points };

struct RenderBatch {
    RenderPrimitive primitive = RenderPrimitive::Triangles;
    std::vector<RenderVertex> vertices;
    std::vector<quint32> indices; // empty: the vertices in order
    float width = 1.0f;           // line width or point diameter, pixels
    bool lit = true;              // headlight shading (triangles)
    bool translucent = false;     // drawn after opaque batches, no depth writes
    bool pushedBack = false;      // polygon offset, so grid lines on it show
};

struct RenderScene {
    std::vector<RenderBatch> batches;
};

RenderVertex renderVertex(const QVector3D &position, const QVector3D &normal, const QColor &color, float alpha = 1.0f);

// Headlight shading shared by the shader and the fallback: two-sided Lambert
// (meshes and the inside of a pattern are lit alike) plus a faint highlight.
constexpr float kAmbient = 0.32f;
constexpr float kDiffuse = 0.68f;
constexpr float kSpecular = 0.10f;
constexpr float kShininess = 24.0f;

// Painter's algorithm renderer: projects every primitive, sorts by depth and
// paints flat-shaded polygons. Used when OpenGL is unavailable.
void paintSceneSoftware(QPainter &painter, const RenderScene &scene, const Camera3D &camera, const QRectF &viewport);

// OpenGL renderer for the current context (desktop GL 2.1 compatibility or
// 3.3 core). Owns buffers and the shader program of that context.
class SceneGLRenderer
{
public:
    SceneGLRenderer();
    ~SceneGLRenderer(); // release() must have run with the context current
    SceneGLRenderer(const SceneGLRenderer &) = delete;
    SceneGLRenderer &operator=(const SceneGLRenderer &) = delete;

    // Needs a current context; false when the shaders cannot be built.
    bool initialize();
    bool isValid() const { return m_valid; }
    void upload(const RenderScene &scene);
    // Viewport in device pixels, origin bottom-left (glViewport); pixelScale
    // (device pixel ratio) converts line widths and point sizes.
    void render(const Camera3D &camera, const QRect &deviceViewport, double aspect, float pixelScale = 1.0f);
    void release();

private:
    struct GpuBatch;
    bool m_valid = false;
    bool m_core = false;
    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::unique_ptr<QOpenGLVertexArrayObject> m_vao;
    std::vector<GpuBatch> m_batches;
};

} // namespace lvd
