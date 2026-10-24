// camera.cpp — реализация орбитальной камеры.
#include "camera.h"
#include "math3d.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.f;
}

void Camera::reset() {
    yaw = 0.6f;
    pitch = 0.35f;
    dist = 2.4f;
    tx = ty = tz = 0.f;
}

void Camera::rotate(float dxDeg, float dyDeg) {
    yaw -= dxDeg * kDeg;
    pitch += dyDeg * kDeg;
    const float lim = 89.f * kDeg;
    pitch = std::max(-lim, std::min(lim, pitch));
}

void Camera::zoom(float factor) {
    if (factor < 0.01f) return;
    dist = std::max(0.25f, std::min(30.f, dist * factor));
}

void Camera::pan(float dxPx, float dyPx, float viewH) {
    if (viewH <= 0.f) return;
    const float halfFov = fovDeg * 0.5f * kDeg;
    const float worldPerPx = (2.f * dist * std::tan(halfFov)) / viewH;

    // Базис камеры из (yaw, pitch): forward = target -> eye
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    float fx = cy * cp, fy = std::sin(pitch), fz = sy * cp;

    // right = normalize(f x up), up = (0,1,0)
    float rx = -fz, ry = 0.f, rz = fx;
    const float rl = std::sqrt(rx * rx + rz * rz);
    if (rl > 1e-6f) { rx /= rl; rz /= rl; }

    // up = right x forward
    const float ux = ry * fz - rz * fy;
    const float uy = rz * fx - rx * fz;
    const float uz = rx * fy - ry * fx;

    // Палец вправо/вниз => модель движется за пальцем
    tx += worldPerPx * (dxPx * rx - dyPx * ux);
    ty += worldPerPx * (dxPx * ry - dyPx * uy);
    tz += worldPerPx * (dxPx * rz - dyPx * uz);
}

void Camera::getViewMatrix(float out[16]) const {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float ex = tx + dist * cy * cp;
    const float ey = ty + dist * sp;
    const float ez = tz + dist * sy * cp;
    m3d::lookAt(ex, ey, ez, tx, ty, tz, 0.f, 1.f, 0.f, out);
}

void Camera::getProjMatrix(float aspect, float out[16]) const {
    m3d::perspective(fovDeg, aspect, 0.1f, 100.f, out);
}