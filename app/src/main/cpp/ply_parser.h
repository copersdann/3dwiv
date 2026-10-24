// ply_parser.h — разбор PLY (Stanford): ascii и binary (little/big endian).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mesh.h"

/** Парсит .ply в финализированную сетку (x,y,z + triangle-индексы). */
bool parsePlyFile(const uint8_t* data, size_t size, MeshData& out, std::string& err);