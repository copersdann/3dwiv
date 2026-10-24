// stl_parser.h — разбор STL (binary и ASCII) в MeshData.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mesh.h"

/**
 * Парсит STL из байтового буфера. Формат определяется автоматически:
 * сначала проверяется binary (по точному совпадению размера),
 * при неудаче — ASCII.
 * Возвращает true и заполняет out (уже финализированную сетку).
 */
bool parseStlFile(const uint8_t* data, size_t size, MeshData& out, std::string& err);