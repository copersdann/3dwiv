// obj_parser.h — разбор Wavefront OBJ (+ игнорирование MTL/нормалей/UV).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mesh.h"

/** Парсит .obj (треугольники и n-угольники через веер), возвращает финализированную сетку. */
bool parseObjFile(const uint8_t* data, size_t size, MeshData& out, std::string& err);