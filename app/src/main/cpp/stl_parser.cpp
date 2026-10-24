// stl_parser.cpp — быстрые парсеры STL: binary (основной путь, ~0 аллокаций
// на треугольник) и ASCII-фолбэк. Нормали из файла игнорируются — они
// пересчитываются один раз в finalizeMesh.
#include "stl_parser.h"

#include <cstdlib>
#include <cstring>

namespace {

// ------------------------------------------------------------ little-endian

inline uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline float readF32(const uint8_t* p) {
    // Android-таргеты — little-endian (ARM64/x86_64), копирование без выравнивания
    float f;
    std::memcpy(&f, p, 4);
    return f;
}

// ---------------------------------------------------------------- helpers

inline void emitTriangle(MeshData& m, float ax, float ay, float az,
                         float bx, float by, float bz,
                         float cx, float cy, float cz) {
    const uint32_t base = static_cast<uint32_t>(m.vertexCount());
    m.positions.push_back(ax); m.positions.push_back(ay); m.positions.push_back(az);
    m.positions.push_back(bx); m.positions.push_back(by); m.positions.push_back(bz);
    m.positions.push_back(cx); m.positions.push_back(cy); m.positions.push_back(cz);
    m.indices.push_back(base);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 2);
}

inline bool isWs(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

inline const char* skipWs(const char* p, const char* end) {
    while (p < end && isWs(*p)) p++;
    return p;
}

// Читает токен до пробела (для ключевых слов STL)
inline bool readWord(const char*& p, const char* end, char* out, size_t cap) {
    const char* s = p;
    size_t n = 0;
    while (p < end && !isWs(*p)) {
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return n > 0 && s != p;
}

// Читает число (strtof-совместимый набор символов)
inline bool readNum(const char*& p, const char* end, float& out) {
    const char* s = p;
    char buf[40];
    size_t n = 0;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' ||
                       *p == '.' || *p == 'e' || *p == 'E')) {
        if (n + 1 < sizeof buf) buf[n++] = *p;
        p++;
    }
    if (n == 0 || s == p) return false;
    buf[n] = '\0';
    out = strtof(buf, nullptr);
    return true;
}

// ------------------------------------------------------------- binary STL

bool parseBinary(const uint8_t* d, size_t size, MeshData& out, std::string& err) {
    if (size < 84) { err = "STL: файл слишком мал для binary-формата"; return false; }
    const uint32_t n = readU32(d + 80);
    const size_t expect = 84ull + (size_t)n * 50ull;
    if (n > 500000000u) { err = "STL: подозрительное количество треугольников"; return false; }
    if (expect > size) { err = "STL: файл обрезан"; return false; }

    out.positions.reserve((size_t)n * 9);
    out.indices.reserve((size_t)n * 3);

    const uint8_t* p = d + 84;
    const uint8_t* pEnd = d + expect;
    const uint8_t* fragEnd = d + size;

    while (p + 50 <= pEnd) {
        // нормаль (12 байт) пропускаем — пересчитаем
        p += 12;
        for (int i = 0; i < 3; i++) {
            float x = readF32(p);
            float y = readF32(p + 4);
            float z = readF32(p + 8);
            p += 12;
            out.positions.push_back(x);
            out.positions.push_back(y);
            out.positions.push_back(z);
        }
        p += 2;  // attribute byte count
        const uint32_t base = static_cast<uint32_t>(out.vertexCount()) - 3;
        out.indices.push_back(base);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
    }
    (void)fragEnd;

    if (out.triangleCount() != n) { err = "STL: не все треугольники прочитаны"; return false; }
    return finalizeMesh(out, &err);
}

// -------------------------------------------------------------- ASCII STL

bool parseAscii(const uint8_t* d, size_t size, MeshData& out, std::string& err) {
    const char* p = reinterpret_cast<const char*>(d);
    const char* end = p + size;
    float v[9] = {0};
    int nv = 0;
    char word[64];

    while (p < end) {
        p = skipWs(p, end);
        if (p >= end) break;
        if (!readWord(p, end, word, sizeof word)) break;

        if (std::strcmp(word, "facet") == 0) {
            nv = 0;
        } else if (std::strcmp(word, "vertex") == 0) {
            if (nv < 3) {
                float x, y, z;
                p = skipWs(p, end);
                if (!readNum(p, end, x)) { err = "STL ASCII: ожидалась координата X"; return false; }
                p = skipWs(p, end);
                if (!readNum(p, end, y)) { err = "STL ASCII: ожидалась координата Y"; return false; }
                p = skipWs(p, end);
                if (!readNum(p, end, z)) { err = "STL ASCII: ожидалась координата Z"; return false; }
                v[nv * 3 + 0] = x; v[nv * 3 + 1] = y; v[nv * 3 + 2] = z;
                nv++;
            }
        } else if (std::strcmp(word, "endfacet") == 0 ||
                   std::strcmp(word, "endsolid") == 0) {
            if (nv == 3) {
                emitTriangle(out, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
                nv = 0;
            } else if (nv > 0 && nv < 3) {
                nv = 0; // пропускаем повреждённый facet
            }
            if (word[0] == 'e' && word[4] == 'o') break; // endsolid — стоп
        }
        // прочие токены (solid, normal, outer, loop, ...) игнорируем
    }

    if (nv == 3) emitTriangle(out, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);

    if (out.triangleCount() == 0) { err = "STL ASCII: не найдено треугольников"; return false; }
    return finalizeMesh(out, &err);
}

} // namespace

bool parseStlFile(const uint8_t* data, size_t size, MeshData& out, std::string& err) {
    out.clear();
    err.clear();
    if (!data || size < 15) { err = "STL: файл слишком маленький"; return false; }

    // 1) Попытка binary: имя файла может быть "solid ...", поэтому
    //    решает точная формула размера: 84 + N*50.
    if (size >= 84) {
        const uint32_t n = readU32(data + 80);
        const size_t expect = 84ull + (size_t)n * 50ull;
        if (n < 500000000u && expect <= size && size - expect <= 2048u) {
            if (parseBinary(data, size, out, err)) return true;
        }
    }

    // 2) ASCII
    if (parseAscii(data, size, out, err)) return true;

    // 3) Строгий binary-фолбэк (без допуска на хвост)
    if (size >= 84) {
        const uint32_t n = readU32(data + 80);
        const size_t expect = 84ull + (size_t)n * 50ull;
        if (n < 500000000u && expect == size) {
            if (parseBinary(data, size, out, err)) return true;
        }
    }
    if (err.empty()) err = "STL: не удалось распознать формат";
    return false;
}