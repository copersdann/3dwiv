// math3d.h — минимальная матрично-векторная математика для OpenGL ES.
// Все матрицы 4x4 — колоночно-главные (как принято в OpenGL).
#pragma once

#include <cmath>

namespace m3d {

// out = a * b
inline void multiply(const float* a, const float* b, float* out) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float s = 0.f;
            for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
    }
}

// Перспективная проекция (fov по вертикали, в градусах)
inline void perspective(float fovDeg, float aspect, float zn, float zf, float* out) {
    float f = 1.f / std::tan(fovDeg * 0.5f * (3.14159265f / 180.f));
    for (int i = 0; i < 16; i++) out[i] = 0.f;
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (zf + zn) / (zn - zf);
    out[11] = -1.f;
    out[14] = (2.f * zf * zn) / (zn - zf);
}

// Видовая матрица: eye -> center, up (column-major результат)
inline void lookAt(float ex, float ey, float ez,
                   float cx, float cy, float cz,
                   float ux, float uy, float uz, float* out) {
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl > 1e-12f) { fx /= fl; fy /= fl; fz /= fl; }

    // side = f x up
    float sx = fy * uz - fz * uy;
    float sy = fz * ux - fx * uz;
    float sz = fx * uy - fy * ux;
    float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (sl > 1e-12f) { sx /= sl; sy /= sl; sz /= sl; }

    // реальный up = side x f
    float rux = sy * fz - sz * fy;
    float ruy = sz * fx - sx * fz;
    float ruz = sx * fy - sy * fx;

    out[0] = sx;  out[1] = rux; out[2] = -fx; out[3] = 0.f;
    out[4] = sy;  out[5] = ruy; out[6] = -fy; out[7] = 0.f;
    out[8] = sz;  out[9] = ruz; out[10] = -fz; out[11] = 0.f;
    out[12] = -(sx * ex + sy * ey + sz * ez);
    out[13] = -(rux * ex + ruy * ey + ruz * ez);
    out[14] = (fx * ex + fy * ey + fz * ez);
    out[15] = 1.f;
}

} // namespace m3d