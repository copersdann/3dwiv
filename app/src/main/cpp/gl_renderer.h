// gl_renderer.h — рендерер OpenGL ES 3.0: интерливированный VBO,
// единый EBO (треугольники + рёбра), оси координат, настройки фона/света.
#pragma once

#include <GLES3/gl3.h>

#include <cstdint>

#include "camera.h"
#include "mesh.h"

class GlRenderer {
public:
    Camera camera;
    bool wireframe = false;

    // Пользовательские настройки (задаются из UI через native_bridge)
    float ambient = 0.28f;      // доля рассеянного света
    bool showAxes = false;      // рисовать оси XYZ
    float bgR = 0.10f, bgG = 0.11f, bgB = 0.13f;

    bool init();
    void setMesh(const MeshData& mesh);
    void render();
    void resize(int w, int h) { viewW_ = w; viewH_ = h; }
    void destroy();

    bool hasMesh() const { return triangleCount_ > 0; }
    uint32_t vertexCount() const { return vertexCount_; }
    uint32_t triangleCount() const { return triangleCount_; }
    int viewH() const { return viewH_; }

private:
    GLuint prog_ = 0;                 // модель (диффуз)
    GLuint axisProg_ = 0;             // оси (плоский цвет)
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    GLuint axisVao_ = 0, axisVbo_ = 0;
    GLint uMvp_ = -1, uView_ = -1, uColor_ = -1, uAmbient_ = -1;
    GLint aMvp_ = -1;
    uint32_t vertexCount_ = 0;
    uint32_t triangleCount_ = 0;
    int viewW_ = 0, viewH_ = 0;

    GLuint compileShader(GLenum type, const char* src);
    GLuint linkProgram(const char* vs, const char* fs);
    bool initAxes();
};