// mesh.h — структура данных сетки и финализация (нормализация + нормали).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct MeshData {
    std::vector<float> positions;   // 3 * vertexCount
    std::vector<float> normals;     // 3 * vertexCount
    std::vector<uint32_t> indices;  // 3 * triangleCount

    // Информация для статистики
    float cx = 0.f, cy = 0.f, cz = 0.f, scale = 1.f;
    float span = 0.f;   // макс. размер до нормализации (для отображения в единицах)

    size_t vertexCount() const { return positions.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }

    void clear() {
        positions.clear();
        normals.clear();
        indices.clear();
        cx = cy = cz = 0.f;
        scale = 1.f;
    }
};

/**
 * Приводит сетку к единичному кубу (полуразмер 1) вокруг начала координат
 * и вычисляет сглаженные (площадевзвешенные) нормали вершин.
 * Возвращает false при пустой сетке/ошибке.
 */
bool finalizeMesh(MeshData& m, std::string* err);