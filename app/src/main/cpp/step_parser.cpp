// step_parser.cpp — быстрый парсер STEP (ISO 10303-21) для низкоуровневого
// рендера. Лёгкий BREP: аналитические поверхности + trim-кривые.
//
// Алгоритм грани:
//   1) считываем ADVANCED_FACE, её surface и loops (outer + holes);
//   2) дискретизируем граничные кривые в 3D, проецируем в UV поверхности;
//   3) объединяем петли (мосты) и триангулируем UV-полигон — алгоритм ушей;
//   4) отображаем каждый треугольник обратно в 3D, выравниваем нормаль.
#include "step_parser.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.28318530f;

// =================================================================== helpers
inline bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

inline void sub3(const float* a, const float* b, float* o) {
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}

inline void cross3(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

inline void norm3(float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-14f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

std::string lowerOf(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return r;
}

// ============================================================== STEP-модель

struct Ent {
    std::string cls;       // lowercase class name
    const char* argsStart; // содержимое между скобками
    size_t argsLen;
};

struct StepModel {
    std::vector<uint64_t> ids;
    std::vector<Ent> ents;
    std::unordered_map<uint64_t, size_t> idx;
    std::unordered_map<uint64_t, std::array<float, 3>> pointCache;

    const Ent* find(uint64_t id) const {
        auto it = idx.find(id);
        return it == idx.end() ? nullptr : &ents[it->second];
    }
};

struct Arg {
    uint8_t kind = 0;   // 0 = ref(#id), 1 = token(число/.T.), 2 = строка, 3 = список (...)
    uint64_t ref = 0;
    std::string text;
};

// разбивает строку аргументов на элементы верхнего уровня
bool parseItems(const char* s, size_t n, std::vector<Arg>& out) {
    const char* p = s;
    const char* end = s + n;
    while (p < end) {
        while (p < end && isWs(*p)) p++;
        if (p >= end) break;
        if (*p == '#') {
            p++;
            uint64_t id = 0;
            bool any = false;
            while (p < end && *p >= '0' && *p <= '9') { id = id * 10 + (*p - '0'); p++; any = true; }
            if (!any) return false;
            out.push_back(Arg{0, id, {}});
            while (p < end && isWs(*p)) p++;
            if (p < end && *p == ',') p++;
            continue;
        }
        if (*p == '\'') {
            p++;
            std::string str;
            while (p < end) {
                if (*p == '\'') {
                    if (p + 1 < end && *(p + 1) == '\'') { str += '\''; p += 2; continue; }
                    p++;
                    break;
                }
                str += *p;
                p++;
            }
            out.push_back(Arg{2, 0, std::move(str)});
            while (p < end && isWs(*p)) p++;
            if (p < end && *p == ',') p++;
            continue;
        }
        if (*p == '(') {
            p++;
            int depth = 1;
            const char* start = p;
            while (p < end && depth > 0) {
                char c = *p;
                if (c == '\'') {
                    p++;
                    while (p < end) {
                        if (*p == '\'') {
                            if (p + 1 < end && *(p + 1) == '\'') { p += 2; continue; }
                            p++;
                            break;
                        }
                        p++;
                    }
                    continue;
                }
                if (c == '(') depth++;
                else if (c == ')') depth--;
                p++;
            }
            if (depth != 0) return false;
            out.push_back(Arg{3, 0, std::string(start, static_cast<size_t>((p - 1) - start))});
            while (p < end && isWs(*p)) p++;
            if (p < end && *p == ',') p++;
            continue;
        }
        // токен до ',' или конца
        const char* start = p;
        while (p < end && *p != ',' &&
               !(*p == ')' && p + 1 >= end)) p++;
        std::string tok(start, static_cast<size_t>(p - start));
        // убрать пробелы
        size_t b = 0, e = tok.size();
        while (b < e && isWs(tok[b])) b++;
        while (e > b && isWs(tok[e - 1])) e--;
        if (e > b) out.push_back(Arg{1, 0, tok.substr(b, e - b)});
        while (p < end && isWs(*p)) p++;
        if (p < end && *p == ',') p++;
    }
    return true;
}

// список ссылок вида ( #1,#2,... ) — text = содержимое списка
bool parseRefList(const std::string& text, std::vector<uint64_t>& refs) {
    std::vector<Arg> items;
    if (!parseItems(text.c_str(), text.size(), items)) return false;
    for (const Arg& a : items) if (a.kind == 0) refs.push_back(a.ref);
    return true;
}

bool parseFloatList(const std::string& text, std::vector<float>& out) {
    std::vector<Arg> items;
    if (!parseItems(text.c_str(), text.size(), items)) return false;
    for (const Arg& a : items) {
        if (a.kind != 1) return false;
        out.push_back(std::strtof(a.text.c_str(), nullptr));
    }
    return true;
}

uint64_t itemRef(const Arg& a, bool& ok) {
    if (a.kind == 0) { ok = true; return a.ref; }
    ok = false;
    return 0;
}

// прямой вариант: return bool + заполнение ref
bool itemRef2(const Arg& a, uint64_t& out) {
    if (a.kind != 0) return false;
    out = a.ref;
    return true;
}

// ============================================================== геометрия

struct Placement {
    float o[3];
    float x[3], y[3], z[3];
};

bool resolvePoint(const StepModel& st, uint64_t id, float out[3]) {
    auto it = st.pointCache.find(id);
    if (it != st.pointCache.end()) { std::memcpy(out, it->second.data(), 12); return true; }
    const Ent* e = st.find(id);
    if (!e || e->cls != "cartesian_point") return false;
    std::vector<Arg> items;
    if (!parseItems(e->argsStart, e->argsLen, items) || items.size() < 2 || items[1].kind != 3) return false;
    std::vector<float> fl;
    if (!parseFloatList(items[1].text, fl) || fl.size() < 3) return false;
    out[0] = fl[0]; out[1] = fl[1]; out[2] = fl[2];
    std::array<float, 3> arr = {out[0], out[1], out[2]};
    const_cast<StepModel&>(st).pointCache[id] = arr;
    return true;
}

bool resolveDirection(const StepModel& st, uint64_t id, float out[3]) {
    const Ent* e = st.find(id);
    if (!e || e->cls != "direction") return false;
    std::vector<Arg> items;
    if (!parseItems(e->argsStart, e->argsLen, items) || items.size() < 2 || items[1].kind != 3) return false;
    std::vector<float> fl;
    if (!parseFloatList(items[1].text, fl) || fl.empty()) return false;
    out[0] = fl.size() > 0 ? fl[0] : 0.f;
    out[1] = fl.size() > 1 ? fl[1] : 0.f;
    out[2] = fl.size() > 2 ? fl[2] : 0.f;
    norm3(out);
    return true;
}

bool resolvePlacement(const StepModel& st, uint64_t id, Placement& pl) {
    const Ent* e = st.find(id);
    if (!e || e->cls != "axis2_placement_3d") return false;
    std::vector<Arg> items;
    if (!parseItems(e->argsStart, e->argsLen, items) || items.size() < 2) return false;
    bool ok = false;
    const uint64_t loc = itemRef(items[1], ok);
    if (!ok || !resolvePoint(st, loc, pl.o)) return false;

    float axis[3] = {0, 0, 1};
    if (items.size() >= 3 && items[2].kind == 0) {
        uint64_t ref = 0;
        bool ok2 = false;
        ref = itemRef(items[2], ok2);
        if (ok2) resolveDirection(st, ref, axis);
    }
    std::memcpy(pl.z, axis, 12);
    norm3(pl.z);
    if (pl.z[0] == 0 && pl.z[1] == 0 && pl.z[2] == 0) { pl.z[2] = 1; }

    // x: ref_direction с удалением z-компоненты
    float refd[3] = {1, 0, 0};
    if (items.size() >= 4 && items[3].kind == 0) {
        uint64_t ref = 0;
        bool ok2 = false;
        ref = itemRef(items[3], ok2);
        if (ok2) resolveDirection(st, ref, refd);
    }
    for (int i = 0; i < 3; i++) pl.x[i] = refd[i] - pl.z[i] * dot3(refd, pl.z);
    norm3(pl.x);
    if (pl.x[0] == 0 && pl.x[1] == 0 && pl.x[2] == 0) {
        // перпендикуляр к z
        float t[3] = {0, 0, 1};
        if (std::fabs(pl.z[2]) > 0.9f) { t[0] = 1; t[1] = 0; t[2] = 0; }
        for (int i = 0; i < 3; i++) pl.x[i] = t[i] - pl.z[i] * dot3(t, pl.z);
        norm3(pl.x);
    }
    cross3(pl.z, pl.x, pl.y);
    return true;
}

struct Curve {
    int kind = -1;       // 0 line, 1 circle, 2 ellipse
    float o[3];          // точка/центр
    float v[3];          // направление линии (с величиной)
    float x[3], y[3], z[3];
    float r1 = 0, r2 = 0;
};

bool resolveCurve(const StepModel& st, uint64_t id, Curve& c) {
    const Ent* e = st.find(id);
    if (!e) return false;
    std::vector<Arg> items;
    if (!parseItems(e->argsStart, e->argsLen, items)) return false;

    if (e->cls == "line") {
        if (items.size() < 3) return false;
        uint64_t pt = 0, vec = 0;
        if (!itemRef2(items[1], pt) || !resolvePoint(st, pt, c.o)) return false;
        if (!itemRef2(items[2], vec)) return false;
        const Ent* ve = st.find(vec);
        if (!ve || ve->cls != "vector") return false;
        std::vector<Arg> vitems;
        if (!parseItems(ve->argsStart, ve->argsLen, vitems) || vitems.size() < 3) return false;
        uint64_t dr = 0;
        if (!itemRef2(vitems[1], dr) || !resolveDirection(st, dr, c.v)) return false;
        const float mag = std::strtof(vitems[2].text.c_str(), nullptr);
        c.v[0] *= mag; c.v[1] *= mag; c.v[2] *= mag;
        c.kind = 0;
        return true;
    }
    if (e->cls == "circle") {
        if (items.size() < 3) return false;
        bool ok = false;
        const uint64_t pl = itemRef(items[1], ok);
        Placement p;
        if (!ok || !resolvePlacement(st, pl, p)) return false;
        c.kind = 1;
        c.r1 = std::strtof(items[2].text.c_str(), nullptr);
        std::memcpy(c.o, p.o, 12);
        std::memcpy(c.x, p.x, 12);
        std::memcpy(c.y, p.y, 12);
        std::memcpy(c.z, p.z, 12);
        return true;
    }
    if (e->cls == "ellipse") {
        if (items.size() < 4) return false;
        bool ok = false;
        const uint64_t pl = itemRef(items[1], ok);
        Placement p;
        if (!ok || !resolvePlacement(st, pl, p)) return false;
        c.kind = 2;
        c.r1 = std::strtof(items[2].text.c_str(), nullptr);
        c.r2 = std::strtof(items[3].text.c_str(), nullptr);
        std::memcpy(c.o, p.o, 12);
        std::memcpy(c.x, p.x, 12);
        std::memcpy(c.y, p.y, 12);
        std::memcpy(c.z, p.z, 12);
        return true;
    }
    return false; // b_spline_* и прочие пока не поддерживаются
}

struct Surf {
    int kind = -1;  // 0 plane, 1 cyl, 2 cone, 3 sphere, 4 torus
    Placement pl;
    float r = 0, r2 = 0, angle = 0;
};

bool resolveSurface(const StepModel& st, uint64_t id, Surf& s) {
    const Ent* e = st.find(id);
    if (!e) return false;
    std::vector<Arg> items;
    if (!parseItems(e->argsStart, e->argsLen, items) || items.size() < 2) return false;
    bool ok = false;
    const uint64_t plRef = itemRef(items[1], ok);
    Placement p;
    if (!ok || !resolvePlacement(st, plRef, p)) return false;
    s.pl = p;

    if (e->cls == "plane") { s.kind = 0; return true; }
    if (e->cls == "cylindrical_surface") {
        if (items.size() < 3) return false;
        s.kind = 1;
        s.r = std::strtof(items[2].text.c_str(), nullptr);
        return s.r > 0;
    }
    if (e->cls == "conical_surface") {
        if (items.size() < 3) return false;
        s.kind = 2;
        s.angle = std::strtof(items[2].text.c_str(), nullptr);
        return true;
    }
    if (e->cls == "spherical_surface") {
        if (items.size() < 3) return false;
        s.kind = 3;
        s.r = std::strtof(items[2].text.c_str(), nullptr);
        return s.r > 0;
    }
    if (e->cls == "toroidal_surface") {
        if (items.size() < 4) return false;
        s.kind = 4;
        s.r = std::strtof(items[2].text.c_str(), nullptr);
        s.r2 = std::strtof(items[3].text.c_str(), nullptr);
        return s.r > 0 && s.r2 > 0;
    }
    return false; // b_spline_surface_* и пр.
}

// S(u,v): обе координаты в радианах/длинах соответствующих параметров
inline void evalSurf(const Surf& s, float u, float v, float* o) {
    const float* P = s.pl.o;
    const float* X = s.pl.x;
    const float* Y = s.pl.y;
    const float* Z = s.pl.z;
    float q[3];
    switch (s.kind) {
        case 0:  // plane
            o[0] = P[0] + X[0] * u + Y[0] * v;
            o[1] = P[1] + X[1] * u + Y[1] * v;
            o[2] = P[2] + X[2] * u + Y[2] * v;
            break;
        case 1: { // cylinder
            const float cu = std::cos(u), su = std::sin(u);
            q[0] = X[0] * (s.r * cu) + Y[0] * (s.r * su);
            q[1] = X[1] * (s.r * cu) + Y[1] * (s.r * su);
            q[2] = X[2] * (s.r * cu) + Y[2] * (s.r * su);
            o[0] = P[0] + q[0] + Z[0] * v;
            o[1] = P[1] + q[1] + Z[1] * v;
            o[2] = P[2] + q[2] + Z[2] * v;
            break;
        }
        case 2: { // cone: radius = v*tan(angle)
            const float rad = v * std::tan(s.angle);
            const float cu = std::cos(u), su = std::sin(u);
            q[0] = X[0] * (rad * cu) + Y[0] * (rad * su);
            q[1] = X[1] * (rad * cu) + Y[1] * (rad * su);
            q[2] = X[2] * (rad * cu) + Y[2] * (rad * su);
            o[0] = P[0] + q[0] + Z[0] * v;
            o[1] = P[1] + q[1] + Z[1] * v;
            o[2] = P[2] + q[2] + Z[2] * v;
            break;
        }
        case 3: { // sphere
            const float cv = std::cos(v), sv = std::sin(v);
            const float cu = std::cos(u), su = std::sin(u);
            q[0] = X[0] * (cv * cu) + Y[0] * (cv * su) + Z[0] * sv;
            q[1] = X[1] * (cv * cu) + Y[1] * (cv * su) + Z[1] * sv;
            q[2] = X[2] * (cv * cu) + Y[2] * (cv * su) + Z[2] * sv;
            o[0] = P[0] + q[0] * s.r;
            o[1] = P[1] + q[1] * s.r;
            o[2] = P[2] + q[2] * s.r;
            break;
        }
        case 4: { // torus: u,v углы
            const float cv = std::cos(v), sv = std::sin(v);
            const float cu = std::cos(u), su = std::sin(u);
            const float Rr = s.r + s.r2 * cv;
            q[0] = X[0] * (Rr * cu) + Y[0] * (Rr * su) + Z[0] * (s.r2 * sv);
            q[1] = X[1] * (Rr * cu) + Y[1] * (Rr * su) + Z[1] * (s.r2 * sv);
            q[2] = X[2] * (Rr * cu) + Y[2] * (Rr * su) + Z[2] * (s.r2 * sv);
            o[0] = P[0] + q[0];
            o[1] = P[1] + q[1];
            o[2] = P[2] + q[2];
            break;
        }
        default:
            o[0] = P[0]; o[1] = P[1]; o[2] = P[2];
    }
}

// обратное отображение: 3D точка -> (u,v)
void uvinv(const Surf& s, const float* pt, float& u, float& v) {
    float d[3];
    sub3(pt, s.pl.o, d);
    const float dx = dot3(d, s.pl.x);
    const float dy = dot3(d, s.pl.y);
    const float dz = dot3(d, s.pl.z);
    switch (s.kind) {
        case 0: u = dx; v = dy; break;
        case 1:
            u = std::atan2(dy, dx);
            v = dz;
            break;
        case 2: {
            u = std::atan2(dy, dx);
            const float rec = std::sqrt(dx * dx + dy * dy);
            const float tanA = std::tan(s.angle);
            v = std::fabs(dz) > 1e-6f ? dz : (std::fabs(tanA) > 1e-6f ? rec / tanA : 0.f);
            break;
        }
        case 3: {
            u = std::atan2(dy, dx);
            float t = dz / s.r;
            if (t > 1.f) t = 1.f;
            if (t < -1.f) t = -1.f;
            v = std::asin(t);
            break;
        }
        case 4: {
            float t = dz / s.r2;
            if (t > 1.f) t = 1.f;
            if (t < -1.f) t = -1.f;
            v = std::asin(t);
            u = std::atan2(dy, dx);
            break;
        }
        default: u = v = 0;
    }
}

// численная нормаль поверхности в (u,v)
void surfNormal(const Surf& s, float u, float v, float* n) {
    float p0[3], pu[3], pv[3], du[3], dv[3];
    const float h = 1e-4f;
    evalSurf(s, u, v, p0);
    evalSurf(s, u + h, v, pu);
    evalSurf(s, u, v + h, pv);
    sub3(pu, p0, du);
    sub3(pv, p0, dv);
    cross3(du, dv, n);
    norm3(n);
}

// дискретизация loop в UV (может вернуть false для b-spline кривых)
bool sampleLoopUv(const StepModel& st, const Surf& s, uint64_t loopId,
                  std::vector<float>& uv) {
    const Ent* loop = st.find(loopId);
    if (!loop) return false;
    std::vector<Arg> items;
    if (!parseItems(loop->argsStart, loop->argsLen, items)) return false;

    if (loop->cls == "poly_loop") {
        if (items.size() < 2 || items[1].kind != 3) return false;
        std::vector<uint64_t> refs;
        if (!parseRefList(items[1].text, refs)) return false;
        for (uint64_t r : refs) {
            float pt[3];
            if (!resolvePoint(st, r, pt)) return false;
            float u, v;
            uvinv(s, pt, u, v);
            uv.push_back(u);
            uv.push_back(v);
        }
        return true;
    }
    if (loop->cls != "edge_loop") return false;
    if (items.size() < 2 || items[1].kind != 3) return false;
    std::vector<uint64_t> edgeRefs;
    if (!parseRefList(items[1].text, edgeRefs) || edgeRefs.empty()) return false;

    constexpr int kSeg = 48;   // сегментов на полный оборот
    for (uint64_t eid : edgeRefs) {
        const Ent* oe = st.find(eid);
        if (!oe || oe->cls != "oriented_edge") return false;
        std::vector<Arg> oi;
        if (!parseItems(oe->argsStart, oe->argsLen, oi) || oi.size() < 4) return false;
        uint64_t ecRef = 0, startR = 0, endR = 0;
        if (!itemRef2(oi[3], ecRef)) return false;
        if (!itemRef2(oi[1], startR)) return false;
        if (!itemRef2(oi[2], endR)) return false;
        const bool orient = oi.size() >= 5 && oi[4].kind == 1 && oi[4].text == ".T.";

        const Ent* ec = st.find(ecRef);
        if (!ec || ec->cls != "edge_curve") return false;
        std::vector<Arg> ei;
        if (!parseItems(ec->argsStart, ec->argsLen, ei) || ei.size() < 4) return false;
        uint64_t v1 = 0, v2 = 0, curveRef = 0;
        if (!itemRef2(ei[1], v1) || !itemRef2(ei[2], v2) || !itemRef2(ei[3], curveRef)) return false;

        float pa[3], pb[3];
        if (!resolvePoint(st, v1, pa) || !resolvePoint(st, v2, pb)) return false;
        Curve c;
        if (!resolveCurve(st, curveRef, c)) return false;

        // направление обхода
        float a[3], b[3];
        if (orient) { std::memcpy(a, pa, 12); std::memcpy(b, pb, 12); }
        else { std::memcpy(a, pb, 12); std::memcpy(b, pa, 12); }

        int n = 2;
        float t0 = 0, t1 = 0;
        if (c.kind == 0) {  // line
            const float v2n = dot3(c.v, c.v);
            if (v2n < 1e-12f) return false;
            t0 = dot3(a, c.v) / v2n;
            t1 = dot3(b, c.v) / v2n;
            n = 2;
        } else if (c.kind == 1 || c.kind == 2) {  // circle/ellipse
            float da[3], db[3];
            sub3(a, c.o, da);
            sub3(b, c.o, db);
            float A0, A1;
            if (c.kind == 1) {
                A0 = std::atan2(dot3(da, c.y), dot3(da, c.x));
                A1 = std::atan2(dot3(db, c.y), dot3(db, c.x));
            } else {
                const float ax = dot3(da, c.x) / c.r1, ay = dot3(da, c.y) / c.r2;
                const float bx = dot3(db, c.x) / c.r1, by = dot3(db, c.y) / c.r2;
                A0 = std::atan2(ay, ax);
                A1 = std::atan2(by, bx);
            }
            const float dist = std::sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
            float delta = std::fmod(A1 - A0 + kTwoPi, kTwoPi);
            if (delta < 1e-4f || dist < 1e-6f) delta = kTwoPi;
            t0 = A0;
            t1 = A0 + delta;
            n = static_cast<int>(std::ceil(delta / (kTwoPi / kSeg)));
            if (n < 4) n = 4;
            if (n > 128) n = 128;
        } else {
            return false;  // b-spline/неизвестная кривая
        }

        const int steps = n;
        for (int i = 0; i <= steps; i++) {
            const float t = t0 + (t1 - t0) * static_cast<float>(i) / steps;
            float p[3];
            if (c.kind == 0) {
                p[0] = c.o[0] + c.v[0] * t;
                p[1] = c.o[1] + c.v[1] * t;
                p[2] = c.o[2] + c.v[2] * t;
            } else if (c.kind == 1) {
                const float ct = std::cos(t), stt = std::sin(t);
                p[0] = c.o[0] + c.x[0] * (c.r1 * ct) + c.y[0] * (c.r1 * stt);
                p[1] = c.o[1] + c.x[1] * (c.r1 * ct) + c.y[1] * (c.r1 * stt);
                p[2] = c.o[2] + c.x[2] * (c.r1 * ct) + c.y[2] * (c.r1 * stt);
            } else {
                const float ct = std::cos(t), stt = std::sin(t);
                p[0] = c.o[0] + c.x[0] * (c.r1 * ct) + c.y[0] * (c.r2 * stt);
                p[1] = c.o[1] + c.x[1] * (c.r1 * ct) + c.y[1] * (c.r2 * stt);
                p[2] = c.o[2] + c.x[2] * (c.r1 * ct) + c.y[2] * (c.r2 * stt);
            }
            float u, v;
            uvinv(s, p, u, v);
            uv.push_back(u);
            uv.push_back(v);
        }
    }
    return true;
}

// "развёртка" периодических координат: убрать скачки > π
void unwrap(std::vector<float>& uv, bool periodicU) {
    if (uv.size() < 4) return;
    for (size_t i = 2; i < uv.size(); i += 2) {
        if (periodicU) {
            float cu = uv[i], pu = uv[i - 2];
            while (cu - pu > kPi) cu -= kTwoPi;
            while (cu - pu < -kPi) cu += kTwoPi;
            uv[i] = cu;
        }
    }
}

// устранение подряд идущих одинаковых точек
void collapseDups(std::vector<float>& uv) {
    if (uv.size() < 6) return;
    std::vector<float> out;
    out.reserve(uv.size());
    const float tol = 1e-5f;
    for (size_t i = 0; i + 1 < uv.size(); i += 2) {
        const size_t m = out.size();
        if (m >= 2) {
            const float du = uv[i] - out[m - 2];
            const float dv = uv[i + 1] - out[m - 1];
            if (du * du + dv * dv < tol * tol) continue;
        }
        out.push_back(uv[i]);
        out.push_back(uv[i + 1]);
    }
    // замкнутость: первая == последняя → выкинуть последнюю
    if (out.size() >= 6) {
        const size_t n = out.size();
        const float du = out[n - 2] - out[0];
        const float dv = out[n - 1] - out[1];
        if (du * du + dv * dv < tol * tol) out.resize(n - 2);
    }
    uv = std::move(out);
}

double signedArea2(const std::vector<float>& uv) {
    double a = 0;
    const size_t n = uv.size() / 2;
    for (size_t i = 0; i < n; i++) {
        const size_t j = (i + 1) % n;
        a += static_cast<double>(uv[i * 2]) * uv[j * 2 + 1] - static_cast<double>(uv[j * 2]) * uv[i * 2 + 1];
    }
    return a * 0.5;
}

// ================================================= 2D-полигоны и триангуляция

bool segIntersect(const float* a, const float* b, const float* c, const float* d, float* ipt) {
    // a,b и c,d — точки (u,v), ipt — точка пересечения (не обязательна)
    const float ax = a[0], ay = a[1], bx = b[0], by = b[1];
    const float cx = c[0], cy = c[1], dx = d[0], dy = d[1];
    const float r1x = bx - ax, r1y = by - ay;
    const float r2x = dx - cx, r2y = dy - cy;
    const float denom = r1x * r2y - r1y * r2x;
    const float eps = 1e-9f;
    if (std::fabs(denom) < eps) {
        // параллельны: пересечение, если коллинеарны и перекрываются
        const float qpx = cx - ax, qpy = cy - ay;
        if (std::fabs(qpx * r1y - qpy * r1x) > eps) return false;
        // перекрытие по проекции на r1
        const float t0 = (qpx * r1x + qpy * r1y) / std::max(r1x*r1x + r1y*r1y, eps);
        const float t1 = t0 + (r2x * r1x + r2y * r1y) / std::max(r1x*r1x + r1y*r1y, eps);
        return (t0 <= 1 && t1 >= 0) || (t1 <= 1 && t0 >= 0);
    }
    const float t = ((cx - ax) * r2y - (cy - ay) * r2x) / denom;
    const float t2 = ((cx - ax) * r1y - (cy - ay) * r1x) / denom;
    if (t >= -1e-4f && t <= 1 + 1e-4f && t2 >= -1e-4f && t2 <= 1 + 1e-4f) {
        if (ipt) { ipt[0] = ax + t * r1x; ipt[1] = ay + t * r1y; }
        return true;
    }
    return false;
}

bool pointInPoly(const std::vector<float>& uv, const std::vector<uint32_t>& ring, float px, float py) {
    bool inside = false;
    const size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float yi = uv[ring[j] * 2 + 1];
        const float yj = uv[ring[i] * 2 + 1];
        if ((yi > py) != (yj > py)) {
            const float xi = uv[ring[j] * 2];
            const float xj = uv[ring[i] * 2];
            const float xint = xi + (py - yi) / (yj - yi) * (xj - xi);
            if (px < xint) inside = !inside;
        }
    }
    return inside;
}

// onEdge: точка лежит на границе
bool pointOnEdge(const std::vector<float>& uv, const std::vector<uint32_t>& ring,
                 float px, float py, float tol) {
    const size_t n = ring.size();
    for (size_t i = 0; i < n; i++) {
        const size_t j = (i + 1) % n;
        const float ax = uv[ring[i] * 2], ay = uv[ring[i] * 2 + 1];
        const float bx = uv[ring[j] * 2], by = uv[ring[j] * 2 + 1];
        // расстояние от точки до отрезка
        const float ex = bx - ax, ey = by - ay;
        const float L2 = ex * ex + ey * ey;
        float tt = L2 > 0 ? ((px - ax) * ex + (py - ay) * ey) / L2 : 0;
        if (tt < 0) tt = 0;
        if (tt > 1) tt = 1;
        const float pxx = ax + tt * ex, pyy = ay + tt * ey;
        const float ddx = px - pxx, ddy = py - pyy;
        if (ddx * ddx + ddy * ddy < tol * tol) return true;
    }
    return false;
}

// объединение отверстий с внешней петлёй (мосты) и заплаточная триангуляция.
// polys: [0] = внешняя (CCW), остальные — отверстия (CW).
bool bridgeAndEarClip(std::vector<float>& cuv,
                      std::vector<std::vector<uint32_t>>& polys,
                      std::vector<uint32_t>& outTris) {
    std::vector<uint32_t> chain = polys[0];
    const size_t nHoles = polys.size();
    for (size_t h = 1; h < nHoles; h++) {
        // верхняя точка отверстия
        const std::vector<uint32_t>& hole = polys[h];
        size_t hj = 0;
        float bestY = -1e30f, bestX = 0;
        for (size_t i = 0; i < hole.size(); i++) {
            const float y = cuv[hole[i] * 2 + 1];
            const float x = cuv[hole[i] * 2];
            if (y > bestY || (y == bestY && x < bestX)) { bestY = y; bestX = x; hj = i; }
        }
        // ищем видимую вершину цепочки
        int bestK = -1;
        double bestD = 1e30;
        for (size_t k = 0; k < chain.size(); k++) {
            const float px = cuv[hole[hj] * 2], py = cuv[hole[hj] * 2 + 1];
            const float qx = cuv[chain[k] * 2], qy = cuv[chain[k] * 2 + 1];
            // середина должна быть строго внутри полигона
            const float mx = (px + qx) * 0.5f, my = (py + qy) * 0.5f;
            if (pointOnEdge(cuv, chain, mx, my, 1e-6f)) continue;
            if (!pointInPoly(cuv, chain, mx, my)) continue;
            // отрезок не должен пересекать рёбра цепочки (кроме рёбер при k)
            bool bad = false;
            const size_t cn = chain.size();
            for (size_t e = 0; e < cn && !bad; e++) {
                const size_t e0 = chain[e], e1 = chain[(e + 1) % cn];
                if (e == k || (e + 1) % cn == k) continue;  // рёбра, инцидентные k
                const float a[2] = {px, py};
                const float b[2] = {qx, qy};
                const float c[2] = {cuv[e0 * 2], cuv[e0 * 2 + 1]};
                const float d[2] = {cuv[e1 * 2], cuv[e1 * 2 + 1]};
                if (segIntersect(a, b, c, d, nullptr)) { bad = true; break; }
            }
            if (bad) continue;
            const float dx = qx - px, dy = qy - py;
            const double dist = static_cast<double>(dx) * dx + dy * dy;
            if (dist < bestD) { bestD = dist; bestK = static_cast<int>(k); }
        }
        if (bestK < 0) continue;  // мост не найден — отверстие пропустим
        // splice: chain[0..k] + hole вокруг + chain[k+1..]
        std::vector<uint32_t> nc;
        nc.reserve(chain.size() + hole.size() + 1);
        for (int i = 0; i <= bestK; i++) nc.push_back(chain[static_cast<size_t>(i)]);
        // обходим отверстие от hj полностью и возвращаемся к hj
        const size_t m = hole.size();
        for (size_t step = 0; step <= m; step++) {
            nc.push_back(hole[(hj + step) % m]);
        }
        for (size_t i = static_cast<size_t>(bestK) + 1; i < chain.size(); i++) nc.push_back(chain[i]);
        chain = std::move(nc);
    }

    // ---- заплаточная триангуляция
    if (chain.size() < 3) return false;
    std::vector<uint32_t> idx = chain;
    const size_t cap = idx.size() > 2 ? (idx.size() - 2) * 3 : 0;
    outTris.reserve(outTris.size() + cap);

    while (idx.size() > 3) {
        bool clipped = false;
        int bestI = -1;
        double bestCross = -1e30;
        const size_t n = idx.size();
        for (size_t i = 0; i < n; i++) {
            const size_t ip = (i + n - 1) % n, in = (i + 1) % n;
            const float ax = cuv[idx[ip] * 2], ay = cuv[idx[ip] * 2 + 1];
            const float bx = cuv[idx[i] * 2], by = cuv[idx[i] * 2 + 1];
            const float cx = cuv[idx[in] * 2], cy = cuv[idx[in] * 2 + 1];
            const float cross = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (cross > bestCross) { bestCross = cross; bestI = static_cast<int>(i); }
            if (cross <= 1e-6) continue;  // не выпуклое ухо
            // никакая точка не внутри треугольника
            bool inside = false;
            for (size_t k = 0; k < n; k++) {
                if (k == ip || k == i || k == in) continue;
                const float px = cuv[idx[k] * 2], py = cuv[idx[k] * 2 + 1];
                // барицентрические знаки (одинаковый знак → внутри)
                const float d1 = (px - bx) * (ay - by) - (py - by) * (ax - bx);
                const float d2 = (px - cx) * (by - cy) - (py - cy) * (bx - cx);
                const float d3 = (px - ax) * (cy - ay) - (py - ay) * (cx - ax);
                const bool hasNeg = d1 < -1e-7 || d2 < -1e-7 || d3 < -1e-7;
                const bool hasPos = d1 > 1e-7 || d2 > 1e-7 || d3 > 1e-7;
                if (!hasNeg && !hasPos) { inside = true; break; }  // на ребре
                if (hasNeg && hasPos) continue;                    // точно снаружи
                inside = true;
                break;
            }
            if (inside) continue;
            outTris.push_back(idx[ip]);
            outTris.push_back(idx[i]);
            outTris.push_back(idx[in]);
            idx.erase(idx.begin() + static_cast<ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            // форс-клип самого выпуклого
            if (bestI < 0) return false;
            const size_t n = idx.size();
            const size_t i = static_cast<size_t>(bestI);
            const size_t ip = (i + n - 1) % n, in = (i + 1) % n;
            outTris.push_back(idx[ip]);
            outTris.push_back(idx[i]);
            outTris.push_back(idx[in]);
            idx.erase(idx.begin() + static_cast<ptrdiff_t>(i));
        }
    }
    if (idx.size() == 3) {
        outTris.push_back(idx[0]);
        outTris.push_back(idx[1]);
        outTris.push_back(idx[2]);
    }
    return !outTris.empty();
}

} // namespace

// ================================================================== main

bool parseStepFile(const uint8_t* data, size_t size, MeshData& out, std::string& err) {
    out.clear();
    err.clear();

    StepModel st;
    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + size;

    // ---------------------------------------------- сбор сущностей (#id=CLS(...))
    while (p < end) {
        while (p < end && *p != '#') p++;
        if (p >= end) break;
        p++;
        uint64_t id = 0;
        bool any = false;
        while (p < end && *p >= '0' && *p <= '9') { id = id * 10 + (*p - '0'); p++; any = true; }
        if (!any) continue;
        while (p < end && isWs(*p)) p++;
        if (p >= end || *p != '=') continue;
        p++;
        while (p < end && isWs(*p)) p++;
        const char* cs = p;
        while (p < end && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                           (*p >= '0' && *p <= '9') || *p == '_')) p++;
        if (p == cs) continue;
        std::string cls(cs, static_cast<size_t>(p - cs));
        while (p < end && isWs(*p)) p++;
        if (p >= end || *p != '(') continue;
        p++;
        int depth = 1;
        const char* as = p;
        size_t guard = 0;
        while (p < end && depth > 0 && guard++ < 100000000ull) {
            const char c = *p;
            if (c == '\'') {
                p++;
                while (p < end) {
                    if (*p == '\'') {
                        if (p + 1 < end && *(p + 1) == '\'') { p += 2; continue; }
                        p++;
                        break;
                    }
                    p++;
                }
                continue;
            }
            if (c == '(') depth++;
            else if (c == ')') depth--;
            p++;
        }
        if (depth != 0) break;
        const size_t argsLen = static_cast<size_t>((p - 1) - as);
        const size_t entIdx = st.ids.size();
        st.ids.push_back(id);
        st.ents.push_back(Ent{lowerOf(cls), as, argsLen});
        st.idx[id] = entIdx;
    }

    // ----------------------------------------------- сборка всех ADVANCED_FACE
    std::vector<uint64_t> faces;
    faces.reserve(st.ents.size() / 4);
    for (size_t i = 0; i < st.ents.size(); i++)
        if (st.ents[i].cls == "advanced_face") faces.push_back(st.ids[i]);
    if (faces.empty()) { err = "STEP: не найдены грани (поддерживается BREP)"; return false; }

    constexpr size_t kFaceLimit = 400000;
    constexpr size_t kVertLimit = 6000000;
    if (faces.size() > kFaceLimit) faces.resize(kFaceLimit);

    size_t unsupportedSurf = 0, unsupportedCurve = 0, okFaces = 0;

    for (uint64_t fid : faces) {
        const Ent* fa = st.find(fid);
        if (!fa) continue;
        std::vector<Arg> items;
        if (!parseItems(fa->argsStart, fa->argsLen, items) || items.size() < 3) continue;

        Surf s;
        {
            uint64_t surfRef = 0;
            if (!itemRef2(items[2], surfRef) || !resolveSurface(st, surfRef, s)) {
                unsupportedSurf++;
                continue;
            }
        }

        // границы: items[1] — список ссылок на face_bound
        std::vector<uint64_t> boundRefs;
        if (items[1].kind != 3 || !parseRefList(items[1].text, boundRefs) || boundRefs.empty()) continue;

        // петли в UV
        std::vector<std::vector<float>> loopsUv;
        bool allOk = true;
        for (uint64_t bref : boundRefs) {
            const Ent* fb = st.find(bref);
            if (!fb || (fb->cls != "face_bound" && fb->cls != "face_outer_bound")) { allOk = false; break; }
            std::vector<Arg> bi;
            if (!parseItems(fb->argsStart, fb->argsLen, bi) || bi.size() < 2) { allOk = false; break; }
            uint64_t loopRef = 0;
            if (!itemRef2(bi[1], loopRef)) { allOk = false; break; }
            std::vector<float> uv;
            if (!sampleLoopUv(st, s, loopRef, uv)) { unsupportedCurve++; allOk = false; break; }
            if (uv.size() < 6) { allOk = false; break; }
            const bool periodic = s.kind == 1 || s.kind == 2 || s.kind == 3 || s.kind == 4;
            unwrap(uv, periodic);
            collapseDups(uv);
            if (uv.size() < 6) { allOk = false; break; }
            loopsUv.push_back(std::move(uv));
        }
        if (!allOk || loopsUv.empty()) continue;

        // классификация: внешняя = максимальная |площадь|
        {
            size_t outerIdx = 0;
            double maxA = -1;
            for (size_t i = 0; i < loopsUv.size(); i++) {
                const double a = std::fabs(signedArea2(loopsUv[i]));
                if (a > maxA) { maxA = a; outerIdx = i; }
            }
            std::swap(loopsUv[0], loopsUv[outerIdx]);
            if (loopsUv[0].size() < 6) continue;
        }

        // общий (u,v) пул + кольца
        std::vector<float> cuv;
        std::vector<std::vector<uint32_t>> polys;
        for (size_t i = 0; i < loopsUv.size(); i++) {
            const std::vector<float>& lp = loopsUv[i];
            const double a = signedArea2(lp);
            bool ccw = a > 0;
            const bool shouldCCW = (i == 0);
            std::vector<uint32_t> ring;
            ring.reserve(lp.size() / 2);
            // копируем в пул (в нужном порядке обхода)
            if (ccw == shouldCCW) {
                for (size_t k = 0; k + 1 < lp.size(); k += 2) {
                    ring.push_back(static_cast<uint32_t>(cuv.size() / 2));
                    cuv.push_back(lp[k]);
                    cuv.push_back(lp[k + 1]);
                }
            } else {
                for (size_t k = lp.size(); k >= 2; k -= 2) {
                    ring.push_back(static_cast<uint32_t>(cuv.size() / 2));
                    cuv.push_back(lp[k - 2]);
                    cuv.push_back(lp[k - 1]);
                }
            }
            polys.push_back(std::move(ring));
        }

        std::vector<uint32_t> tris;
        if (!bridgeAndEarClip(cuv, polys, tris)) continue;
        if (tris.empty()) continue;

        // отображение в 3D: каждая UV-тройка -> треугольник с нормалью по поверхности
        const size_t base = out.vertexCount();
        const size_t tCount = tris.size() / 3;
        for (size_t t = 0; t < tCount; t++) {
            const uint32_t a = tris[t * 3 + 0];
            const uint32_t b = tris[t * 3 + 1];
            const uint32_t c = tris[t * 3 + 2];
            float pa[3], pb[3], pc[3];
            evalSurf(s, cuv[a * 2], cuv[a * 2 + 1], pa);
            evalSurf(s, cuv[b * 2], cuv[b * 2 + 1], pb);
            evalSurf(s, cuv[c * 2], cuv[c * 2 + 1], pc);
            float e1[3], e2[3], n[3];
            sub3(pb, pa, e1);
            sub3(pc, pa, e2);
            cross3(e1, e2, n);
            const float nu = (cuv[a * 2] + cuv[b * 2] + cuv[c * 2]) / 3.f;
            const float nv = (cuv[a * 2 + 1] + cuv[b * 2 + 1] + cuv[c * 2 + 1]) / 3.f;
            float sn[3];
            surfNormal(s, nu, nv, sn);
            if (dot3(n, sn) < 0) {
                // разворот для согласованности с нормалью поверхности
                float tmp[3];
                std::memcpy(tmp, pb, 12);
                std::memcpy(pb, pc, 12);
                std::memcpy(pc, tmp, 12);
            }
            out.positions.push_back(pa[0]); out.positions.push_back(pa[1]); out.positions.push_back(pa[2]);
            out.positions.push_back(pb[0]); out.positions.push_back(pb[1]); out.positions.push_back(pb[2]);
            out.positions.push_back(pc[0]); out.positions.push_back(pc[1]); out.positions.push_back(pc[2]);
            const uint32_t v0 = static_cast<uint32_t>(base + t * 3);
            out.indices.push_back(v0);
            out.indices.push_back(v0 + 1);
            out.indices.push_back(v0 + 2);
        }
        okFaces++;
        if (out.vertexCount() > kVertLimit) break;
    }

    if (out.triangleCount() == 0) {
        err = "STEP: не удалось построить геометрию (";
        err += std::to_string(unsupportedSurf) + " пов. без поддержки, ";
        err += std::to_string(unsupportedCurve) + " кривых b-spline)";
        if (unsupportedSurf == 0 && unsupportedCurve == 0) err = "STEP: ошибка тесселяции граней";
        return false;
    }
    (void)okFaces;
    return finalizeMesh(out, &err);
}