#include "plot3d/Scene3DRender.h"

#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

#include <algorithm>
#include <cstddef>

namespace lvd {

namespace {

// Enums missing from the ES2-level QOpenGLFunctions headers.
constexpr GLenum kProgramPointSize = 0x8642; // GL_VERTEX_PROGRAM_POINT_SIZE in 2.1
constexpr GLenum kPointSprite = 0x8861;      // compatibility only

// One shader for everything: lit triangles, unlit lines and round points.
// Written once and specialised by version macros, because a 3.3 core context
// need not accept GLSL 1.20 and a 2.1 context has no GLSL 3.30.
const char *kVertexBody = R"(
IN vec3 position;
IN vec3 normal;
IN vec4 color;
uniform mat4 mvp;
uniform mat4 modelView;
uniform float pointSize;
OUT vec4 vColor;
OUT vec3 vNormal;
OUT vec3 vViewPos;
void main()
{
    vec4 viewPos = modelView * vec4(position, 1.0);
    vViewPos = viewPos.xyz;
    vNormal = mat3(modelView) * normal;
    vColor = color;
    gl_PointSize = pointSize;
    gl_Position = mvp * vec4(position, 1.0);
}
)";

const char *kFragmentBody = R"(
IN vec4 vColor;
IN vec3 vNormal;
IN vec3 vViewPos;
uniform float lit;
uniform float roundPoints;
uniform float pointSize;
void main()
{
    vec4 c = vColor;
    if (roundPoints > 0.5) {
        // Round with a one-pixel soft rim: multisampling does not smooth
        // discarded fragments.
        float r = length(gl_PointCoord - vec2(0.5));
        if (r > 0.5)
            discard;
        c.a *= 1.0 - smoothstep(0.5 - 1.0 / max(pointSize, 1.0), 0.5, r);
    }
    if (lit > 0.5) {
        vec3 n = normalize(vNormal);
        vec3 l = normalize(-vViewPos);
        float d = abs(dot(n, l));
        c.rgb = c.rgb * (AMBIENT + DIFFUSE * d) + vec3(SPECULAR * pow(d, SHININESS));
    }
    FRAG_COLOR = c;
}
)";

// Bounded: a lost context can report errors forever.
void clearErrors(QOpenGLFunctions *f)
{
    for (int i = 0; i < 32 && f->glGetError() != GL_NO_ERROR; ++i) {
    }
}

QString constants()
{
    return QStringLiteral("#define AMBIENT %1\n#define DIFFUSE %2\n#define SPECULAR %3\n#define SHININESS %4\n")
        .arg(QString::number(kAmbient, 'f', 4), QString::number(kDiffuse, 'f', 4), QString::number(kSpecular, 'f', 4),
             QString::number(kShininess, 'f', 2));
}

} // namespace

struct SceneGLRenderer::GpuBatch {
    QOpenGLBuffer vertices{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer indices{QOpenGLBuffer::IndexBuffer};
    GLsizei count = 0;
    bool indexed = false;
    RenderPrimitive primitive = RenderPrimitive::Triangles;
    float width = 1.0f;
    bool lit = true;
    bool translucent = false;
    bool pushedBack = false;
};

SceneGLRenderer::SceneGLRenderer() = default;
SceneGLRenderer::~SceneGLRenderer() = default;

bool SceneGLRenderer::initialize()
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context || context->isOpenGLES())
        return false;
    const QSurfaceFormat format = context->format();
    m_core = format.profile() == QSurfaceFormat::CoreProfile;
    if (format.majorVersion() < 2)
        return false;

    const QString vertexHeader = m_core ? QStringLiteral("#version 330 core\n#define IN in\n#define OUT out\n")
                                        : QStringLiteral("#version 120\n#define IN attribute\n#define OUT varying\n");
    const QString fragmentHeader = m_core
        ? QStringLiteral("#version 330 core\n#define IN in\nout vec4 fragColor;\n#define FRAG_COLOR fragColor\n")
        : QStringLiteral("#version 120\n#define IN varying\n#define FRAG_COLOR gl_FragColor\n");

    m_program = std::make_unique<QOpenGLShaderProgram>();
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexHeader + QLatin1String(kVertexBody))
        || !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentHeader + constants() + QLatin1String(kFragmentBody))) {
        m_program.reset();
        return false;
    }
    m_program->bindAttributeLocation("position", 0);
    m_program->bindAttributeLocation("normal", 1);
    m_program->bindAttributeLocation("color", 2);
    if (!m_program->link()) {
        m_program.reset();
        return false;
    }

    // A core profile has no default vertex array object.
    m_vao = std::make_unique<QOpenGLVertexArrayObject>();
    if (!m_vao->create()) {
        m_vao.reset();
        if (m_core) {
            m_program.reset();
            return false;
        }
    }
    m_valid = true;
    return true;
}

void SceneGLRenderer::upload(const RenderScene &scene)
{
    for (GpuBatch &batch : m_batches) {
        batch.vertices.destroy();
        batch.indices.destroy();
    }
    m_batches.clear();
    if (!m_valid)
        return;
    for (const RenderBatch &source : scene.batches) {
        if (source.vertices.empty())
            continue;
        GpuBatch batch;
        batch.primitive = source.primitive;
        batch.width = source.width;
        batch.lit = source.lit;
        batch.translucent = source.translucent;
        batch.pushedBack = source.pushedBack;
        batch.vertices.create();
        batch.vertices.bind();
        batch.vertices.allocate(source.vertices.data(), static_cast<int>(source.vertices.size() * sizeof(RenderVertex)));
        batch.vertices.release();
        if (!source.indices.empty()) {
            batch.indexed = true;
            batch.indices.create();
            batch.indices.bind();
            batch.indices.allocate(source.indices.data(), static_cast<int>(source.indices.size() * sizeof(quint32)));
            batch.indices.release();
            batch.count = static_cast<GLsizei>(source.indices.size());
        } else {
            batch.count = static_cast<GLsizei>(source.vertices.size());
        }
        m_batches.push_back(std::move(batch));
    }
}

void SceneGLRenderer::render(const Camera3D &camera, const QRect &deviceViewport, double aspect, float pixelScale)
{
    if (!m_valid)
        return;
    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
    f->glViewport(deviceViewport.x(), deviceViewport.y(), deviceViewport.width(), deviceViewport.height());
    f->glEnable(GL_DEPTH_TEST);
    f->glDepthFunc(GL_LEQUAL);
    f->glDepthMask(GL_TRUE);
    f->glDisable(GL_CULL_FACE);
    f->glEnable(GL_BLEND);
    // Keep destination alpha at 1: the frame is composited as premultiplied,
    // and a translucent mesh would otherwise let the window show through.
    f->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    f->glEnable(kProgramPointSize);
    if (!m_core)
        f->glEnable(kPointSprite);
    // Old drivers reject some of the enables above; do not leave the error
    // for QPainter's GL engine to trip over.
    clearErrors(f);

    const QMatrix4x4 view = camera.viewMatrix();
    const QMatrix4x4 mvp = camera.projectionMatrix(aspect) * view;
    m_program->bind();
    m_program->setUniformValue("mvp", mvp);
    m_program->setUniformValue("modelView", view);
    if (m_vao)
        m_vao->bind();
    for (int attribute = 0; attribute < 3; ++attribute)
        m_program->enableAttributeArray(attribute);

    auto draw = [&](GpuBatch &batch) {
        batch.vertices.bind();
        const int stride = static_cast<int>(sizeof(RenderVertex));
        m_program->setAttributeBuffer(0, GL_FLOAT, static_cast<int>(offsetof(RenderVertex, x)), 3, stride);
        m_program->setAttributeBuffer(1, GL_FLOAT, static_cast<int>(offsetof(RenderVertex, nx)), 3, stride);
        m_program->setAttributeBuffer(2, GL_FLOAT, static_cast<int>(offsetof(RenderVertex, r)), 4, stride);
        m_program->setUniformValue("lit", batch.lit && batch.primitive == RenderPrimitive::Triangles ? 1.0f : 0.0f);
        m_program->setUniformValue("roundPoints", batch.primitive == RenderPrimitive::Points ? 1.0f : 0.0f);
        m_program->setUniformValue("pointSize", batch.width * pixelScale);
        GLenum mode = GL_TRIANGLES;
        if (batch.primitive == RenderPrimitive::Lines) {
            mode = GL_LINES;
            // Wide lines are not part of the core profile.
            f->glLineWidth(m_core ? 1.0f : std::max(1.0f, batch.width * pixelScale));
        } else if (batch.primitive == RenderPrimitive::Points) {
            mode = GL_POINTS;
        }
        if (batch.pushedBack) {
            f->glEnable(GL_POLYGON_OFFSET_FILL);
            f->glPolygonOffset(1.0f, 1.0f);
        }
        if (batch.indexed) {
            batch.indices.bind();
            f->glDrawElements(mode, batch.count, GL_UNSIGNED_INT, nullptr);
            batch.indices.release();
        } else {
            f->glDrawArrays(mode, 0, batch.count);
        }
        if (batch.pushedBack)
            f->glDisable(GL_POLYGON_OFFSET_FILL);
        batch.vertices.release();
    };
    for (GpuBatch &batch : m_batches)
        if (!batch.translucent)
            draw(batch);
    // Translucent meshes last, without depth writes, so what is behind them
    // still shows. They are not sorted among themselves.
    f->glDepthMask(GL_FALSE);
    for (GpuBatch &batch : m_batches)
        if (batch.translucent)
            draw(batch);
    f->glDepthMask(GL_TRUE);

    for (int attribute = 0; attribute < 3; ++attribute)
        m_program->disableAttributeArray(attribute);
    if (m_vao)
        m_vao->release();
    m_program->release();
    f->glLineWidth(1.0f);
    f->glDisable(GL_DEPTH_TEST);
    clearErrors(f);
}

void SceneGLRenderer::release()
{
    for (GpuBatch &batch : m_batches) {
        batch.vertices.destroy();
        batch.indices.destroy();
    }
    m_batches.clear();
    if (m_vao)
        m_vao->destroy();
    m_vao.reset();
    m_program.reset();
    m_valid = false;
}

} // namespace lvd
