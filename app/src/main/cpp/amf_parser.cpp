// amf_parser.cpp — разбор AMF через байтовое сканирование XML
// (без сторонних XML-библиотек). Формат:
//   <amf><object id="1"><mesh>
//     <vertices><vertex><coordinates><x/><y/><z/></coordinates></vertex>…
//     <volume><triangle><v1/><v2/><v3/></triangle>…</volume>
//   </mesh></object>…</amf>
#include "amf_parser.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

inline bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// имя тега без префикса: "<m:vertex>" -> "vertex"
void tagBase(const char* s, const char* e, char* out, size_t cap) {
    const char* colon = nullptr;
    for (const char* q = s; q < e; q++) if (*q == ':') colon = q;
    if (colon) s = colon + 1;
    size_t n = 0;
    for (const char* q = s; q < e && n + 1 < cap; q++) {
        if (isWs(*q) || *q == '/' || *q == '<' || *q == '>') break;
        out[n++] = *q;
    }
    out[n] = '\0';
}

// число из текста элемента: skip ws, копия до '<' или ws
bool elementNumber(const char*, const char* end, coу
    while (t < end && *t != '<' && !isWs(*t)) t++;
    if (t == s) return false;
    char buf[48];
    size_t n = static_cast<size_t>(t - s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    std::memcpy(buf, s, n);
    buf[n] = '\0';
    out = std::strtod(buf, nullptr);
    return true;
}

} // namespace

bool parseAmfFile(const uint8_t* data, size_t size, MeshData& out, std::string& err) {
    out.clear();
    err.clear();

    struct Obj { std::vector<float> pos; std::vector<uint32_t> idx; };
    std::vector<Obj> objects;

    Obj* cur = nullptr;
    bool inVertex = false;
    bool inCoord = false;
    float vx = 0.f, vy = 0.f, vz = 0.f;
    uint32_t tri[3] = {0, 0, 0};
    int triN = 0;
    bool vertexPending = false;

    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + size;

    while (p < end) {
        const char* lt = p;
        while (lt < end && *lt != '<') lt++;
        if (lt >= end) break;
        p = lt + 1;
        const char* gt = p;
        while (gt < end && *gt != '>') gt++;
        if (gt >= end) break;

        const char* t = p;
        bool closing = false;
        if (t < gt && *t == '/') { closing = true; t++; }
        char tag[32];
        if (t < gt) tagBase(t, gt, tag, sizeof tag);
        else { p = gt + 1; continue; }
        const char* afterGt = gt + 1;

        if (closing) {
            if (std::strcmp(tag, "object") == 0) {
                cur = nullptr;
                inVertex = inCoord = false;
            } else if (std::strcmp(tag, "coordinates") == 0) {
                if (cur && inVertex) {
                    cur->pos.push_back(vx);
                    cur->pos.push_back(vy);
                    cur->pos.push_back(vz);
                    vertexPending = false;
                }
                inCoord = false;
            } else if (std::strcmp(tag, "vertex") == 0) {
                inVertex = false;
            } else if (std::strcmp(tag, "triangle") == 0) {
                if (cur && triN == 3) {
                    cur->idx.push_back(tri[0]);
                    cur->idx.push_back(tri[1]);
                    cur->idx.push_back(tri[2]);
                }
                triN = 0;
            }
            p = gt + 1;
            continue;
        }

        if (std::strcmp(tag, "object") == 0) {
            objects.emplace_back();
            cur = &objects.back();
        } else if (cur && std::strcmp(tag, "vertex") == 0) {
            inVertex = true;
            inCoord = false;
            vertexPending = true;
        } else if (cur && inVertex && std::strcmp(tag, "coordinates") == 0) {
            inCoord = true;
            vx = vy = vz = 0.f;
        } else if (cur && inVertex && inCoord && std::strcmp(tag, "x") == 0) {
            double d;
            if (elementNumber(p, end, afterGt, d)) vx = static_cast<float>(d);
        } else if (cur && inVertex && inCoord && std::strcmp(tag, "y") == 0) {
            double d;
            if (elementNumber(p, end, afterGt, d)) vy = static_cast<float>(d);
        } else if (cur && inVertex && inCoord && std::strcmp(tag, "z") == 0) {
            double d;
            if (elementNumber(p, end, afterGt, d)) vz = static_cast<float>(d);
        } else if (cur && std::strcmp(tag, "triangle") == 0) {
            triN = 0;
        } else if (cur && triN < 3 && (std::strcmp(tag, "v1") == 0 ||
                                       std::strcmp(tag, "v2") == 0 ||
                                       std::strcmp(tag, "v3") == 0)) {
            double d;
            if (elementNumber(p, end, afterGt, d)) tri[triN++] = static_cast<uint32_t>(d);
        }
        // <amf>, <mesh>, <vertices>, <volume>, <material>, <metadata> и пр. — игнор

        p = gt + 1;
    }
    (void)vertexPending;

    for (Obj& o : objects) {
        if (!o.pos.empty() && !o.idx.empty()) {
            const uint32_t base = static_cast<uint32_t>(out.vertexCount());
            out.positions.insert(out.positions.end(), o.pos.begin(), o.pos.end());
            for (uint32_t i : o.idx) out.indices.push_back(base + i);
        }
    }
    if (out.triangleCount() == 0) { err = "AMF: нет треугольников в модели"; return false; }
    return finalizeMesh(out, &err);
}