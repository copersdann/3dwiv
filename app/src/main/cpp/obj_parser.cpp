// obj_parser.cpp — максимально быстрый разбор Wavefront OBJ.
// Поддерживаются: v (позиции), f (face, с формами a, a/b, a/b/c, a//c,
// отрицательные индексы, n-угольники через веер). Остальное игнорируется.
#include "obj_parser.h"

#include <cstdlib>
#include <cstring>

namespace {

inline bool isWs(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

const char* skipWs(const char* p, const char* end) {
    while (p < end && isWs(*p)) p++;
    return p;
}

double readNum(const char*& p, const char* end) {
    char buf[48];
    size_t n = 0;
    const char* s = p;
    while (p < end && n + 1 < sizeof buf &&
           ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' ||
            *p == 'e' || *p == 'E')) {
        buf[n++] = *p;
        p++;
    }
    if (n == 0 || s == p) return 0.0;
    buf[n] = '\0';
    return std::strtod(buf, nullptr);
}

// индекс вершины face: "12", "12/1", "12/1/2", "12//2"
bool readFaceIndex(const char*& p, const char* end, long& out) {
    p = skipWs(p, end);
    bool neg = false;
    if (p < end && *p == '-') { neg = true; p++; }
    long v = 0;
    bool any = false;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = true; }
    if (!any) return false;
    int slashes = 0;
    while (p < end && *p == '/' && slashes < 2) {
        p++;
        while (p < end && *p >= '0' && *p <= '9') p++;
        slashes++;
    }
    out = neg ? -v : v;
    return true;
}

inline void skipToEol(const char*& p, const char* end) {
    while (p < end && *p != '\n') p++;
}

} // namespace

bool parseObjFile(const uint8_t* data, size_t size, MeshData& out, std::string& err) {
    out.clear();
    err.clear();

    std::vector<float> verts;
    std::vector<uint32_t> idx;
    verts.reserve(65536);
    idx.reserve(262144);

    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + size;

    while (p < end) {
        p = skipWs(p, end);
        if (p >= end) break;
        const char c0 = *p;

        if (c0 == '#') { skipToEol(p, end); continue; }

        if (c0 == 'v' && p + 1 < end && isWs(p[1])) {
            p = skipWs(p + 1, end);
            const float x = static_cast<float>(readNum(p, end));
            p = skipWs(p, end);
            const float y = static_cast<float>(readNum(p, end));
            p = skipWs(p, end);
            const float z = static_cast<float>(readNum(p, end));
            verts.push_back(x);
            verts.push_back(y);
            verts.push_back(z);
            skipToEol(p, end);
            continue;
        }

        if (c0 == 'f') {
            const char* q = p + 1;
            long fi[8];
            int n = 0;
            while (n < 8) {
                const char* before = q;
                long v;
                if (!readFaceIndex(q, end, v)) { q = before; break; }
                if (q < end && (*q != '\n')) {
                    fi[n++] = v;
                } else if (q >= end) {
                    fi[n++] = v;
                    break;
                } else {
                    fi[n++] = v;
                    break;
                }
            }
            // Проверка формата числа в face: если строка face пустая — пропускаем
            if (n >= 3) {
                const long total = static_cast<long>(verts.size() / 3);
                bool bad = false;
                auto idxOf = [&](long v, bool& ok) -> uint32_t {
                    const long vi = v > 0 ? v - 1 : total + v;
                    if (vi < 0 || vi >= total) { ok = false; return 0; }
                    return static_cast<uint32_t>(vi);
                };
                if (n == 3) {
                    idx.push_back(idxOf(fi[0], bad));
                    idx.push_back(idxOf(fi[1], bad));
                    idx.push_back(idxOf(fi[2], bad));
                } else {
                    for (int i = 1; i + 1 < n; i++) {
                        idx.push_back(idxOf(fi[0], bad));
                        idx.push_back(idxOf(fi[i], bad));
                        idx.push_back(idxOf(fi[i + 1], bad));
                    }
                }
                if (bad) { err = "OBJ: индекс вершины вне диапазона"; return false; }
            }
            skipToEol(p, end);
            continue;
        }

        skipToEol(p, end);
    }

    if (verts.empty() || idx.size() < 3) { err = "OBJ: нет вершин или граней"; return false; }
    out.positions = std::move(verts);
    out.indices = std::move(idx);
    return finalizeMesh(out, &err);
}