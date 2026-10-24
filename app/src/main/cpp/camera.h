// camera.h — орбитальная камера (yaw/pitch/dist + панорама цели).
#pragma once

struct Camera {
    float yaw = 0.6f;     // радианы, орбита вокруг оси Y
    float pitch = 0.35f;  // радианы, подъём над горизонтом
    float dist = 2.4f;    // расстояние до цели
    float tx = 0.f, ty = 0.f, tz = 0.f;  // целевая точка
    float fovDeg = 45.f;

    void reset();
    void rotate(float dxDeg, float dyDeg);
    void zoom(float factor);
    void pan(float dxPx, float dyPx, float viewH);
    void getViewMatrix(float out[16]) const;    // column-major
    void getProjMatrix(float aspect, float out[16]) const;
};