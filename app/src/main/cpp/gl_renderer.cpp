// gl_renderer.cpp — рендеринг OpenGL ES 3.0.
// Модель: 1 draw call (треугольники) + каркас (тот же EBO со смещением).
// Оси: отдельный мини-шейдер, рисуется 3 линии RGB.
#include "gl_renderer.h"

#include "math3d.h"

#include <android/log.h>

#include <vector>

#define TAG "3dwiv"
#define GLE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

GLuint GlRenderer::compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    if (!s) return 0;
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        GLE("shader error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint GlRenderer::linkProgram(const char* vs, const char* fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(v);
    glDeleteShader(f);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        GLE("link error: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

bool GlRenderer::initAxes() {
    const char* vs =
        "#version 300 es\n"
        "layout(location=0) in vec3 aPos;\n"
        "layout(location=1) in vec3 aColor;\n"
        "uniform mat4 uMvp;\n"
        "out vec3 vColor;\n"
        "void main() {\n"
        "  gl_Position = uMvp * vec4(aPos, 1.0);\n"
        "  vColor = aColor;\n"
        "}\n";
    const char* fs =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec3 vColor;\n"
        "out vec4 oColor;\n"
        "void main() { oColor = vec4(vColor, 1.0); }\n";
    axisProg_ = linkProgram(vs, fs);
    if (!axisProg_) return false;
    aMvp_ = glGetUniformLocation(axisProg_, "uMvp");

    // три отрезка вдоль X/Y/Z от -1.25 до +1.25
    const float L = 1.25f;
    const float data[6 * 6] = {
        // X
        -L, 0, 0,  0.85f, 0.25f, 0.25f,
         L, 0, 0,  0.85f, 0.25f, 0.25f,
        // Y
         0,-L, 0,  0.30f, 0.75f, 0.30f,
         0, L, 0,  0.30f, 0.75f, 0.30f,
        // Z
         0, 0,-L,  0.35f, 0.55f, 0.95f,
         0, 0, L,  0.35f, 0.55f, 0.95f,
    };
    glGenVertexArrays(1, &axisVao_);
    glGenBuffers(1, &axisVbo_);
    glBindVertexArray(axisVao_);
    glBindBuffer(GL_ARRAY_BUFFER, axisVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof data, data, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)12);
    glBindVertexArray(0);
    return true;
}

bool GlRenderer::init() {
    const char* vs =
        "#version 300 es\n"
        "layout(location=0) in vec3 aPos;\n"
        "layout(location=1) in vec3 aNormal;\n"
        "uniform mat4 uMvp;\n"
        "uniform mat4 uView;\n"
        "out vec3 vNormal;\n"
        "void main() {\n"
        "  gl_Position = uMvp * vec4(aPos, 1.0);\n"
        "  vNormal = mat3(uView) * aNormal;\n"
        "}\n";
    const char* fs =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec3 vNormal;\n"
        "uniform vec3 uColor;\n"
        "uniform float uAmbient;\n"
        "out vec4 oColor;\n"
        "void main() {\n"
        "  vec3 n = normalize(vNormal);\n"
        "  float d = max(dot(n, vec3(0.35, 0.5, 0.8)), 0.0);\n"
        "  float light = uAmbient + (1.0 - uAmbient) * d;\n"
        "  oColor = vec4(uColor * light, 1.0);\n"
        "}\n";

    prog_ = linkProgram(vs, fs);
    if (!prog_) return false;

    uMvp_ = glGetUniformLocation(prog_, "uMvp");
    uView_ = glGetUniformLocation(prog_, "uView");
    uColor_ = glGetUniformLocation(prog_, "uColor");
    uAmbient_ = glGetUniformLocation(prog_, "uAmbient");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    initAxes();
    return true;
}

void GlRenderer::setMesh(const MeshData& mesh) {
    const uint32_t vc = static_cast<uint32_t>(mesh.vertexCount());
    const uint32_t tc = static_cast<uint32_t>(mesh.triangleCount());
    if (vc == 0 || tc == 0) return;

    // 1) Интерливинг: позиция (12 Б) + нормаль (12 Б) = шаг 24 Б
    std::vector<float> inter;
    inter.resize(vc * 6);
    for (uint32_t i = 0; i < vc; i++) {
        inter[i * 6 + 0] = mesh.positions[i * 3 + 0];
        inter[i * 6 + 1] = mesh.positions[i * 3 + 1];
        inter[i * 6 + 2] = mesh.positions[i * 3 + 2];
        inter[i * 6 + 3] = mesh.normals[i * 3 + 0];
        inter[i * 6 + 4] = mesh.normals[i * 3 + 1];
        inter[i * 6 + 5] = mesh.normals[i * 3 + 2];
    }

    // 2) Единый индексный буфер: треугольники + рёбра для каркаса
    std::vector<uint32_t> idx;
    idx.resize(static_cast<size_t>(tc) * 9);
    for (uint32_t t = 0; t < tc; t++) {
        const uint32_t a = mesh.indices[t * 3 + 0];
        const uint32_t b = mesh.indices[t * 3 + 1];
        const uint32_t c = mesh.indices[t * 3 + 2];
        idx[t * 3 + 0] = a;
        idx[t * 3 + 1] = b;
        idx[t * 3 + 2] = c;
        const size_t o = static_cast<size_t>(tc) * 3 + static_cast<size_t>(t) * 6;
        idx[o + 0] = a; idx[o + 1] = b;
        idx[o + 2] = b; idx[o + 3] = c;
        idx[o + 4] = c; idx[o + 5] = a;
    }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(inter.size() * sizeof(float)),
                 inter.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(idx.size() * sizeof(uint32_t)),
                 idx.data(), GL_STATIC_DRAW);

    const GLsizei stride = 6 * static_cast<GLsizei>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    vertexCount_ = vc;
    triangleCount_ = tc;
}

void GlRenderer::render() {
    glViewport(0, 0, viewW_, viewH_);
    glClearColor(bgR, bgG, bgB, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = viewH_ > 0 ? static_cast<float>(viewW_) / static_cast<float>(viewH_) : 1.f;
    float view[16], proj[16], mvp[16];
    camera.getViewMatrix(view);
    camera.getProjMatrix(aspect, proj);
    m3d::multiply(proj, view, mvp);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    // -------- оси (должны быть видны и без модели) --------
    if (showAxes && axisProg_) {
        glDisable(GL_CULL_FACE);
        glUseProgram(axisProg_);
        glUniformMatrix4fv(aMvp_, 1, GL_FALSE, mvp);
        glBindVertexArray(axisVao_);
        glDrawArrays(GL_LINES, 0, 6);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }

    if (!prog_ || triangleCount_ == 0 || viewW_ <= 0 || viewH_ <= 0) {
        glBindVertexArray(0);
        return;
    }

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    glUseProgram(prog_);
    glUniformMatrix4fv(uMvp_, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(uView_, 1, GL_FALSE, view);

    glBindVertexArray(vao_);

    // Заливка
    glUniform3f(uColor_, 0.62f, 0.66f, 0.78f);
    glUniform1f(uAmbient_, ambient);
    glDrawElements(GL_TRIANGLES,
                   static_cast<GLsizei>(triangleCount_) * 3,
                   GL_UNSIGNED_INT, nullptr);

    // Каркас (рёбра из того же EBO, со смещением)
    if (wireframe) {
        glUniform3f(uColor_, 0.02f, 0.02f, 0.03f);
        glUniform1f(uAmbient_, 1.0f);
        glDrawElements(GL_LINES,
                       static_cast<GLsizei>(triangleCount_) * 6,
                       GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(
                           static_cast<uintptr_t>(triangleCount_) * 3 * sizeof(uint32_t)));
    }
    glBindVertexArray(0);
}

void GlRenderer::destroy() {
    if (prog_) { glDeleteProgram(prog_); prog_ = 0; }
    if (axisProg_) { glDeleteProgram(axisProg_); axisProg_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
    if (axisVao_) { glDeleteVertexArrays(1, &axisVao_); axisVao_ = 0; }
    if (axisVbo_) { glDeleteBuffers(1, &axisVbo_); axisVbo_ = 0; }
    vertexCount_ = 0;
    triangleCount_ = 0;
}