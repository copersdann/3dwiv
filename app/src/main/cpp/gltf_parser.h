// gltf_parser.h — разбор glTF 2.0: бинарный .glb и текстовый .gltf
// (встроенные data URI). Без сторонних библиотек.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mesh.h"

/**
 * Парсит glTF 2.0 в MeshData. Геометрия всех выбранных узлов сцены
 * объединяется с учётом node-трансформов (matrix/TRS, иерархия).
 */
bool parseGltfFile(const uint8_t* data, size_t size, MeshData& out, std::string& err);