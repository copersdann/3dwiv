// amf_parser.h — разбор AMF (Additive Manufacturing File Format, XML).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mesh.h"

/** Парсит .amf (vertex + volume/triangle), все объекты объединяются. */
bool parseAmfFile(const uint8_t* data, size_t size, MeshData& out, std::string& err);