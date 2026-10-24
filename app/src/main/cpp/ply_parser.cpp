// ply_parser.cpp — разбор PLY без зависимостей.
// Поддерживаются ascii и binary (little/big endian), любые свойства;
// x/y/z берутся по имени, индексы face — первый list-property элемента face.
#include "ply_parser.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Prop {
    std::string name;
    int type = 0;           // 0 = нет
    bool isList = false;
    int listCountType = 0;
    int listValType = 0;
};

struct Element {
    std::string name;
    size_t count = 0;
    std::vector<Prop> props;
    int xIdx = -1, yIdx = -1, zIdx = -1;  // индексы во props (для vertex)
    int listIdx = -1;                     // первый list-проп (для face)
};

int typeCode(const char* s) {
    if (std::strcmp(s, "char") == 0 || std::strcmp(s, "int8") == 0) return 1;
    if (std::strcmp(s, "uchar") == 0 || std::strcmp(s, "uint8") == 0) return 2;
    if (std::strcmp(s, "short") == 0 || std::strcmp(s, "int16") == 0) return 3;
    if (std::strcmp(s, "ushort") == 0 || std::strcmp(s, "uint16") == 0) return 4;
    if (std::strcmp(s, "int") == 0 || std::strcmp(s, "int32") == 0) return 5;
    if (std::strcmp(s, "uint") == 0 || std::strcmp(s, "uint32") == 0) return 6;
    if (std::strcmp(s, "float") == 0 || std::strcmp(s, "float32") == 0) return 7;
    if (std::strcmp(s, "double") == 0 || std::strcmp(s, "float64") == 0) return 8;
    return 0;
}

inline int typeSize(int t) {
    if (t <= 2) return 1;
    if (t <= 4) return 2;
    if (t <= 6) return 4;
    if (t == 7) return 4;
    if (t == 8) return 8;
    return 0;
}

inline bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

const char* skipWs(const char* p, const char* end) {
    while (p < end && isWs(*p)) p++;
    return p;
}

// Читает строку до '\n' (или конца); возвращает длину строки; p продвигается
size_t readLine(const char*& p, const char* end) {
    const char* s = p;
    while (p < end && *p != '\n') p++;
    const size_t raw = static_cast<size_t>(p - s);
    if (p < end) p++;  // '\n'
    size_t len = raw;
    if (len > 0 && s[len - 1] == '\r') len--;
    return len;
}

inline uint16_t rd16le(const uint8_t* b) { return uint16_t(b[0]) | uint16_t(uint16_t(b[1]) << 8); }
inline uint16_t rd16be(const uint8_t* b) { return uint16_t(uint16_t(b[0]) << 8) | uint16_t(b[1]); }
inline uint32_t rd32le(const uint8_t* b) {
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
inline uint32_t rd32be(const uint8_t* b) {
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | uint32_t(b[3]);
}

struct Reader {  // бинарный ридер с порядком байт
    const uint8_t* d;
    size_t size;
    size_t pos = 0;
    bool be = false;

    bool val(int t, double& out) {
        const int sz = typeSize(t);
        if (t <= 0 || pos + static_cast<size_t>(sz) > size) return false;
        const uint8_t* b = d + pos;
        switch (t) {
            case 1: out = static_cast<double>(static_cast<int8_t>(b[0])); pos += 1; return true;
            case 2: out = static_cast<double>(b[0]); pos += 1; return true;
            case 3: out = be ? static_cast<double>(static_cast<int16_t>(rd16be(b)))
                             : static_cast<double>(static_cast<int16_t>(rd16le(b))); pos += 2; return true;
            case 4: out = be ? static_cast<double>(rd16be(b)) : static_cast<double>(rd16le(b)); pos += 2; return true;
            case 5: out = be ? static_cast<double>(static_cast<int32_t>(rd32be(b)))
                             : static_cast<double>(static_cast<int32_t>(rd32le(b))); pos += 4; return true;
            case 6: out = be ? static_cast<double>(rd32be(b)) : static_cast<double>(rd32le(b)); pos += 4; return true;
            case 7: {
                const uint32_t u = be ? rd32be(b) : rd32le(b);
                float f;
                std::memcpy(&f, &u, 4);
                out = f;
                pos += 4;
                return true;
            }
            case 8: {
                uint64_t u = 0;
                if (be) for (int i = 0; i < 8; i++) u = (u << 8) | b[i];
                else for (int i = 7; i >= 0; i--) u = (u << 8) | b[i];
                double dv;
                std::memcpy(&dv, &u, 8);
                out = dv;
                pos += 8;
                return true;
            }
        }
        return false;
    }
};

} // namespace

bool parsePlyFile(const uint8_t* data, size_t size, MeshData& out, std::string& err) {
    out.clear();
    err.clear();

    bool binary = false;
    bool be = false;
    std::vector<Element> elements;
    Element* curElem = nullptr;

    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + size;

    // ------------------------------------------------------------ заголовок
    bool magicOk = false;
    while (p < end) {
        const size_t ll = readLine(p, end);
        const char* ls = p - ll;  // начало строки
        if (ls < reinterpret_cast<const char*>(data)) break;

        if (!magicOk) {
            if (ll >= 3 && std::memcmp(ls, "ply", 3) == 0) { magicOk = true; continue; }
            err = "PLY: неверная сигнатура";
            return false;
        }

        char tok1[64], tok2[64], tok3[64];
        const char* tp = ls;
        const char* tEnd = ls + ll;
        auto nextTok = [&](char* o, size_t cap) -> bool {
            tp = skipWs(tp, tEnd);
            if (tp >= tEnd) return false;
            size_t n = 0;
            while (tp < tEnd && !isWs(*tp) && n + 1 < cap) o[n++] = *tp++;
            o[n] = '\0';
            return n > 0;
        };

        if (!nextTok(tok1, sizeof tok1)) continue;
        if (std::strcmp(tok1, "format") == 0 && nextTok(tok2, sizeof tok2)) {
            binary = std::strcmp(tok2, "binary_little_endian") == 0;
            be = std::strcmp(tok2, "binary_big_endian") == 0;
            binary = binary || be;
        } else if (std::strcmp(tok1, "element") == 0 &&
                   nextTok(tok2, sizeof tok2) && nextTok(tok3, sizeof tok3)) {
            elements.emplace_back();
            curElem = &elements.back();
            curElem->name = tok2;
            curElem->count = static_cast<size_t>(std::strtoul(tok3, nullptr, 10));
        } else if (std::strcmp(tok1, "property") == 0 && curElem) {
            if (nextTok(tok2, sizeof tok2)) {
                if (std::strcmp(tok2, "list") == 0) {
                    char tc[64], tv[64], tn[64];
                    if (nextTok(tc, sizeof tc) && nextTok(tv, sizeof tv) && nextTok(tn, sizeof tn)) {
                        Prop pr;
                        pr.isList = true;
                        pr.listCountType = typeCode(tc);
                        pr.listValType = typeCode(tv);
                        pr.name = tn;
                        curElem->props.push_back(pr);
                        if (curElem->listIdx < 0) curElem->listIdx = static_cast<int>(curElem->props.size() - 1);
                    }
                } else {
                    char tn[64];
                    if (nextTok(tn, sizeof tn)) {
                        Prop pr;
                        pr.type = typeCode(tok2);
                        pr.name = tn;
                        curElem->props.push_back(pr);
                    }
                }
            }
        } else if (std::strcmp(tok1, "end_header") == 0) {
            break;
        }
    }
    if (!magicOk) { err = "PLY: неверная сигнатура"; return false; }

    const Element* vElem = nullptr;
    const Element* fElem = nullptr;
    for (const auto& e : elements) {
        if (e.name == "vertex") vElem = &e;
        else if (e.name == "face") fElem = &e;
    }
    if (!vElem || vElem->count == 0) { err = "PLY: нет элемента vertex"; return false; }

    Reader rd;
    rd.d = data;
    rd.size = size;
    rd.pos = static_cast<size_t>(p - reinterpret_cast<const char*>(data));
    rd.be = be;

    out.positions.reserve(vElem->count * 3);
    if (fElem) out.indices.reserve(fElem->count * 3);

    auto findCoord = [](const Element& e, const char* name) -> int {
        for (size_t i = 0; i < e.props.size(); i++)
            if (e.props[i].name == name) return static_cast<int>(i);
        return -1;
    };
    const int xIdx = findCoord(*vElem, "x");
    const int yIdx = findCoord(*vElem, "y");
    const int zIdx = findCoord(*vElem, "z");
    if (xIdx < 0 || yIdx < 0 || zIdx < 0) { err = "PLY: нет координат x/y/z"; return false; }

    const long totalV = static_cast<long>(vElem->count);
    auto pushTri = [&](long a, long b, long c, bool& bad) {
        if (a < 0 || a >= totalV || b < 0 || b >= totalV || c < 0 || c >= totalV) { bad = true; return; }
        out.indices.push_back(static_cast<uint32_t>(a));
        out.indices.push_back(static_cast<uint32_t>(b));
        out.indices.push_back(static_cast<uint32_t>(c));
    };

    if (!binary) {
        // ------------------------------------------------------------- ascii
        const char* ap = p;
        for (size_t i = 0; i < vElem->count; i++) {
            double x = 0, y = 0, z = 0;
            bool ok = true;
            for (size_t k = 0; k < vElem->props.size(); k++) {
                ap = skipWs(ap, end);
                if (ap >= end) { ok = false; break; }
                char nb[48];
                size_t nn = 0;
                while (ap < end && !isWs(*ap) && nn + 1 < sizeof nb) nb[nn++] = *ap++;
                if (nn == 0) { ok = false; break; }
                nb[nn] = '\0';
                const double v = std::strtod(nb, nullptr);
                const int ki = static_cast<int>(k);
                if (ki == xIdx) x = v; else if (ki == yIdx) y = v; else if (ki == zIdx) z = v;
            }
            if (!ok) { err = "PLY ascii: обрыв данных вершин"; return false; }
            out.positions.push_back(static_cast<float>(x));
            out.positions.push_back(static_cast<float>(y));
            out.positions.push_back(static_cast<float>(z));
        }
        if (fElem) {
            for (size_t i = 0; i < fElem->count; i++) {
                ap = skipWs(ap, end);
                if (ap >= end) { err = "PLY ascii: обрыв данных граней"; return false; }
                char nb[48];
                size_t nn = 0;
                while (ap < end && !isWs(*ap) && nn + 1 < sizeof nb) nb[nn++] = *ap++;
                nb[nn] = '\0';
                const long cnt = std::strtol(nb, nullptr, 10);
                if (cnt < 3 || cnt > 64) { err = "PLY: некорректная грань"; return false; }
                long fi[64];
                for (long k = 0; k < cnt; k++) {
                    ap = skipWs(ap, end);
                    if (ap >= end) { err = "PLY ascii: обрыв индексов"; return false; }
                    nn = 0;
                    while (ap < end && !isWs(*ap) && nn + 1 < sizeof nb) nb[nn++] = *ap++;
                    nb[nn] = '\0';
                    fi[k] = std::strtol(nb, nullptr, 10);
                }
                bool bad = false;
                if (cnt == 3) pushTri(fi[0], fi[1], fi[2], bad);
                else for (long k = 1; k + 1 < cnt && !bad; k++) pushTri(fi[0], fi[k], fi[k + 1], bad);
                if (bad) { err = "PLY: индекс вне диапазона"; return false; }
            }
        }
    } else {
        // ------------------------------------------------------------ binary
        for (size_t i = 0; i < vElem->count; i++) {
            double x = 0, y = 0, z = 0;
            for (size_t k = 0; k < vElem->props.size(); k++) {
                double v;
                const Prop& pr = vElem->props[k];
                if (pr.isList || !rd.val(pr.type, v)) { err = "PLY binary: обрыв данных вершин"; return false; }
                const int ki = static_cast<int>(k);
                if (ki == xIdx) x = v; else if (ki == yIdx) y = v; else if (ki == zIdx) z = v;
            }
            out.positions.push_back(static_cast<float>(x));
            out.positions.push_back(static_cast<float>(y));
            out.positions.push_back(static_cast<float>(z));
        }
        if (fElem) {
            if (fElem->listIdx < 0) { err = "PLY: нет list-свойства у face"; return false; }
            const Prop& lp = fElem->props[static_cast<size_t>(fElem->listIdx)];
            for (size_t i = 0; i < fElem->count; i++) {
                double c;
                if (!rd.val(lp.listCountType, c)) { err = "PLY binary: обрыв счётчика грани"; return false; }
                const long cnt = static_cast<long>(c);
                if (cnt < 3 || cnt > 64) { err = "PLY: некорректная грань"; return false; }
                long fi[64];
                for (long k = 0; k < cnt; k++) {
                    double vv;
                    if (!rd.val(lp.listValType, vv)) { err = "PLY binary: обрыв индексов"; return false; }
                    fi[k] = static_cast<long>(vv);
                }
                bool bad = false;
                if (cnt == 3) pushTri(fi[0], fi[1], fi[2], bad);
                else for (long k = 1; k + 1 < cnt && !bad; k++) pushTri(fi[0], fi[k], fi[k + 1], bad);
                if (bad) { err = "PLY: индекс вне диапазона"; return false; }
            }
        }
    }

    if (out.indices.empty()) { err = "PLY: нет треугольных граней"; return false; }
    return finalizeMesh(out, &err);
}