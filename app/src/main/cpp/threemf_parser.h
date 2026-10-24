// threemf_parser.h — разбор 3MF (ZIP + XML-модель) в MeshData.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mesh.h"

/**
 * Парсит 3MF: извлекает 3D/3dmodel.model из ZIP (собственный inflate),
 * сканирует XML без внешних библиотек, собирает все объекты из блока
 * <build> с учётом атрибута transform, нормализует и считает нормали.
 */
bool parse3mfFile(const uint8_t* data, size_t size, MeshData& out, std::string& err);