// inflate.h — самодостаточный декомпрессор DEFLATE (RFC 1951),
// сырой поток (без zlib/gzip-обёртки), как в ZIP-контейнерах.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace inflate {

// 0 — успех, иначе код ошибки (для отладки)
int raw(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out);

} // namespace inflate