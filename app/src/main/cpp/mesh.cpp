// mesh.cpp — финализация сетки: нормализация в единичный куб + нормали.
#include "mesh.h"

#include <algorithm>
#include <cmath>

bool finalizeMesh(MeshData& m, std::string* err) {
    const size_t vc = m.vertexCount();
    const size_t tc = m.triangleCount();
    if (vc == 0 || tc == 0) {
        if (err) *err = "Сетка пуста (нет вершин/треугольников)";
        return false;
    }

    // 1) Границы
    float mnx = 1e30f, mny = 1e30f, mnz = 1e30f;
    float mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
    for (size_t i = 0; i < vc; i++) {
        float x = m.positions[i * 3 + 0];
        float y = m.positions[i * 3 + 1];
        float z = m.positions[i * 3 + 2];
        mnx = std::min(mnx, x); mxx = std::max(mxx, x);
        mny = std::min(mny, y); mxy = std::max(mxy, y);
        mnz = std::min(mnz, z); mxz = std::max(mxz, z);
    }

    // 2) Центр и масштаб
    m.cx = (mnx + mxx) * 0.5f;
    m.cy = (mny + mxy) * 0.5f;
    m.cz = (mnz + mxz) * 0.5f;
    float span = std::max(mxx - mnx, std::max(mxy - mny, mxz - mnz));
    m.span = span;
    float k = span > 1e-12f ? 2.f / span : 1.f;
    m.scale = k;

    // 3) Перенос + масштаб в позиции
    for (size_t i = 0; i < vc; i++) {
        float* p = &m.positions[i * 3];
        p[0] = (p[0] - m.cx) * k;
        p[1] = (p[1] - m.cy) * k;
        p[2] = (p[2] - m.cz) * k;
    }

    // 4) Площадевзвешенные нормали вершин (сумма cross-произведений)
    m.normals.assign(vc * 3, 0.f);
    for (size_t i = 0; i < tc; i++) {
        uint32_t a = m.indices[i * 3 + 0];
        uint32_t b = m.indices[i * 3 + 1];
        uint32_t c = m.indices[i * 3 + 2];
        if (a >= vc || b >= vc || c >= vc) { m.normals.clear(); if (err) *err = "Индекс вне диапазона"; return false; }
        const float* pa = &m.positions[a * 3];
        const float* pb = &m.positions[b * 3];
        const float* pc = &m.positions[c * 3];
        float ux = pb[0] - pa[0], uy = pb[1] - pa[1], uz = pb[2] - pa[2];
        float vx = pc[0] - pa[0], vy = pc[1] - pa[1], vz = pc[2] - pa[2];
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        m.normals[a * 3 + 0] += nx; m.normals[a * 3 + 1] += ny; m.normals[a * 3 + 2] += nz;
        m.normals[b * 3 + 0] += nx; m.normals[b * 3 + 1] += ny; m.normals[b * 3 + 2] += nz;
        m.normals[c * 3 + 0] += nx; m.normals[c * 3 + 1] += ny; m.normals[c * 3 + 2] += nz;
    }

    // 5) Нормализация
    const float fallback[3] = {0.f, 1.f, 0.f};
    for (size_t i = 0; i < vc; i++) {
        float* n = &m.normals[i * 3];
        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-12f) {
            n[0] /= l; n[1] /= l; n[2] /= l;
        } else {
            n[0] = fallback[0]; n[1] = fallback[1]; n[2] = fallback[2];
        }
    }
    return true;
}