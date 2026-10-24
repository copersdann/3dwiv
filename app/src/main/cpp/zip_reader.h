// zip_reader.h — минималистичный ZIP-ридер (без zip64) для извлечения
// внутренних файлов 3MF. Поддерживает STORE (0) и DEFLATE (8).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Ищет запись с именем `wanted` (без учёта регистра) и распаковывает
 * её содержимое в `out`. Возвращает false, если запись не найдена
 * или архив повреждён / требует zip64.
 */
bool zip_extract_file(const uint8_t* zip, size_t zipSize,
                      const char* wanted, std::vector<uint8_t>& out);