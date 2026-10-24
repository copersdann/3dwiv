// gltf_parser.cpp — разбор glTF 2.0 (GLB + JSON, data URI).
// Читаются: buffers/bufferViews/accessors, meshes/primitives
// (POSITION + indices; режимы TRIANGLES/STRIP/FAN), node-иерархия
// (matrix или TRS). Нормали пересчитываются в finalizeMesh.
#include "gltf_parser.h"

#include "json_mini.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kGlbMagic = 0x46546C67;  // "glTF"
constexpr uint32_t kChunkJson = 0x4E4F534A; // "JSON"
constexpr uint32_t kChunkBin = 0x004E4942;  // "BIN\0"

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }

size_t compSize(int ct) {
    switch (ct) {
        case 5120: case 5121: return 1;   // BYTE / UBYTE
        case 5122: case 5123: return 2;   // SHORT / USHORT
        case 5125: case 5126: return 4;   // UINT / FLOAT
        default: return 0;
    }
}

int compCount(const char* type) {
    if (std::strcmp(type, "SCALAR") == 0) return 1;
    if (std::strcmp(type, "VEC2") == 0) return 2;
    if (std::strcmp(type, "VEC3") == 0) return 3;
    if (std::strcmp(type, "VEC4") == 0) return 4;
    return 0;
}

inline int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

void base64Decode(const char* s, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(n / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        const int v = b64val(s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
}

struct Buf {
    const uint8_t* p = nullptr;
    size_t len = 0;
    std::vector<uint8_t> owned;
};

struct View {
    const uint8_t* p = nullptr;
    size_t len = 0;
    size_t stride = 0;
};

struct Accessor {
    const uint8_t* p = nullptr;
    size_t count = 0;
    int ct = 0;       // componentType
    int cc = 0;       // число компонент
    size_t stride = 0;
    bool ok = false;
};

void matMult(const float* a, const float* b, float* out) {  // out = a * b
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

void applyMat(const float* m, float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = m[0] * x + m[4] * y + m[8] * z + m[12];
    oy = m[1] * x + m[5] * y + m[9] * z + m[13];
    oz = m[2] * x + m[6] * y + m[10] * z + m[14];
}

void identityMat(float m[16]) {
    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.f : 0.f;
}

// Локальная матрица узла: matrix (column-major) или TRS
void nodeLocalMatrix(const jmini::Node& n, float m[16]) {
    identityMat(m);
    if (const jmini::Node* jm = n.find("matrix")) {
        if (jm->isArr() && jm->size() == 16) {
            for (int i = 0; i < 16; i++) m[i] = static_cast<float>(jm->at(i)->asNum());
            return;
        }
    }
    float t[3] = {0, 0, 0};
    float s[3] = {1, 1, 1};
    float q[4] = {0, 0, 0, 1};
    if (const jmini::Node* tn = n.find("translation"))
        if (tn->isArr() && tn->size() >= 3)
            for (int i = 0; i < 3; i++) t[i] = static_cast<float>(tn->at(i)->asNum());
    if (const jmini::Node* sn = n.find("scale"))
        if (sn->isArr() && sn->size() >= 3)
            for (int i = 0; i < 3; i++) s[i] = static_cast<float>(sn->at(i)->asNum());
    if (const jmini::Node* rn = n.find("rotation"))
        if (rn->isArr() && rn->size() >= 4)
            for (int i = 0; i < 4; i++) q[i] = static_cast<float>(rn->at(i)->asNum());

    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    const float r00 = 1 - 2 * (yy + zz), r01 = 2 * (xy - wz), r02 = 2 * (xz + wy);
    const float r10 = 2 * (xy + wz),  r11 = 1 - 2 * (xx + zz), r12 = 2 * (yz - wx);
    const float r20 = 2 * (xz - wy),  r21 = 2 * (yz + wx),  r22 = 1 - 2 * (xx + yy);
    // Столбцы R*S:
    m[0] = r00 * s[0]; m[1] = r10 * s[0]; m[2] = r20 * s[0]; m[3] = 0;
    m[4] = r01 * s[1]; m[5] = r11 * s[1]; m[6] = r21 * s[1]; m[7] = 0;
    m[8] = r02 * s[2]; m[9] = r12 * s[2]; m[10] = r22 * s[2]; m[11] = 0;
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
}

double readComponent(const uint8_t* p, int ct) {
    switch (ct) {
        case 5120: return static_cast<double>(static_cast<int8_t>(p[0]));
        case 5121: return static_cast<double>(p[0]);
        case 5122: return static_cast<double>(static_cast<int16_t>(rd16(p)));
        case 5123: return static_cast<double>(rd16(p));
        case 5125: return static_cast<double>(rd32(p));
        case 5126: {
            const uint32_t u = rd32(p);
            float f;
            std::memcpy(&f, &u, 4);
            return static_cast<double>(f);
        }
        default: return 0;
    }
}

int64_t readIndexComponent(const uint8_t* p, int ct) {
    switch (ct) {
        case 5120: return static_cast<int8_t>(p[0]);
        case 5121: return static_cast<int64_t>(p[0]);
        case 5122: return static_cast<int16_t>(rd16(p));
        case 5123: return static_cast<int64_t>(rd16(p));
        case 5125: return static_cast<int64_t>(rd32(p));
        default: return 0;
    }
}

struct StackFrame {
    int node;
    float world[16];
};

} // namespace

bool parseGltfFile(const uint8_t* data, size_t size, MeshData& out, std::string& err) {
    out.clear();
    err.clear();

    // ------------------------------------------------------------- GLB/JSON
    std::vector<uint8_t> binChunk;
    std::string jsonText;
    if (size >= 12 && rd32(data) == kGlbMagic) {
        const uint32_t version = rd32(data + 4);
        if (version != 2) { err = "glTF: поддерживается только версия 2"; return false; }
        size_t off = 12;
        while (off + 8 <= size) {
            const uint32_t clen = rd32(data + off);
            const uint32_t ctype = rd32(data + off + 4);
            off += 8;
            if (off + clen > size) { err = "glTF: повреждён GLB-чанк"; return false; }
            if (ctype == kChunkJson) {
                jsonText.assign(reinterpret_cast<const char*>(data + off), clen);
            } else if (ctype == kChunkBin) {
                binChunk.assign(data + off, data + off + clen);
            }
            off += clen;
        }
    } else {
        jsonText.assign(reinterpret_cast<const char*>(data), size);
    }
    if (jsonText.empty()) { err = "glTF: нет JSON-секции"; return false; }

    jmini::Node root;
    if (!jmini::parse(jsonText.data(), jsonText.size(), root)) {
        err = "glTF: не удалось разобрать JSON";
        return false;
    }

    // ------------------------------------------------------------- buffers
    std::vector<Buf> bufs;
    if (const jmini::Node* bn = root.find("buffers")) {
        for (size_t i = 0; i < bn->size(); i++) {
            const jmini::Node* b = bn->at(i);
            Buf buf;
            const std::string& uri = b->asStr("");
            if (!uri.empty()) {
                const std::string prefix = "data:";
                if (uri.compare(0, prefix.size(), prefix) != 0) {
                    err = "glTF: внешние .bin не поддерживаются (используйте GLB или data URI)";
                    return false;
                }
                const size_t comma = uri.find(',');
                if (comma == std::string::npos) { err = "glTF: повреждён data URI"; return false; }
                base64Decode(uri.c_str() + comma + 1, uri.size() - comma - 1, buf.owned);
                buf.p = buf.owned.data();
                buf.len = buf.owned.size();
            } else if (i == 0 && !binChunk.empty()) {
                buf.p = binChunk.data();
                buf.len = binChunk.size();
            } else {
                err = "glTF: буфер без данных";
                return false;
            }
            bufs.push_back(std::move(buf));
        }
    }

    // ---------------------------------------------------------- bufferViews
    std::vector<View> views;
    if (const jmini::Node* vn = root.find("bufferViews")) {
        for (size_t i = 0; i < vn->size(); i++) {
            const jmini::Node* v = vn->at(i);
            View view;
            const int bi = v->find("buffer") ? v->find("buffer")->asInt(-1) : -1;
            const size_t bo = static_cast<size_t>(v->find("byteOffset") ? v->find("byteOffset")->asInt(0) : 0);
            const size_t bl = static_cast<size_t>(v->find("byteLength") ? v->find("byteLength")->asInt(0) : 0);
            if (bi < 0 || bi >= static_cast<int>(bufs.size())) continue;
            if (bo + bl > bufs[static_cast<size_t>(bi)].len) continue;
            view.p = bufs[static_cast<size_t>(bi)].p + bo;
            view.len = bl;
            view.stride = static_cast<size_t>(v->find("byteStride") ? v->find("byteStride")->asInt(0) : 0);
            views.push_back(view);
        }
    }

    // ------------------------------------------------------------ accessors
    std::vector<Accessor> accs;
    if (const jmini::Node* an = root.find("accessors")) {
        for (size_t i = 0; i < an->size(); i++) {
            const jmini::Node* a = an->at(i);
            Accessor acc;
            acc.ct = a->find("componentType") ? a->find("componentType")->asInt(0) : 0;
            acc.count = static_cast<size_t>(a->find("count") ? a->find("count")->asInt(0) : 0);
            if (const jmini::Node* ty = a->find("type")) acc.cc = compCount(ty->asStr("").c_str());
            const int vi = a->find("bufferView") ? a->find("bufferView")->asInt(-1) : -1;
            const size_t ao = static_cast<size_t>(a->find("byteOffset") ? a->find("byteOffset")->asInt(0) : 0);
            const size_t cs = compSize(acc.ct);
            if (acc.ct != 0 && acc.cc > 0 && vi >= 0 && vi < static_cast<int>(views.size())) {
                const View& v = views[static_cast<size_t>(vi)];
                const size_t stride = v.stride != 0 ? v.stride : cs * static_cast<size_t>(acc.cc);
                if (v.p && ao + acc.count * stride <= v.len) {
                    acc.p = v.p + ao;
                    acc.stride = stride;
                    acc.ok = true;
                }
            }
            accs.push_back(acc);
        }
    }

    // ---------------------------------------------------------------- nodes
    std::vector<float> nodeMat;  // 16 * nodeCount
    std::vector<int> nodeMesh;
    std::vector<std::vector<int>> nodeChildren;
    if (const jmini::Node* nn = root.find("nodes")) {
        nodeMesh.assign(nn->size(), -1);
        nodeChildren.resize(nn->size());
        for (size_t i = 0; i < nn->size(); i++) {
            const jmini::Node* n = nn->at(i);
            nodeMat.resize(nodeMat.size() + 16);
            nodeLocalMatrix(*n, &nodeMat[i * 16]);
            if (n->find("mesh")) nodeMesh[i] = n->find("mesh")->asInt(-1);
            if (const jmini::Node* cn = n->find("children")) {
                for (size_t k = 0; k < cn->size(); k++)
                    nodeChildren[i].push_back(cn->at(k)->asInt(-1));
            }
        }
    }
    const size_t nNodes = nodeMesh.size();

    // ------------------------------------------------------------- корни
    std::vector<int> roots;
    if (const jmini::Node* sn = root.find("scenes")) {
        const int scene = root.find("scene") ? root.find("scene")->asInt(0) : 0;
        if (scene >= 0 && static_cast<size_t>(scene) < sn->size()) {
            const jmini::Node* sc = sn->at(static_cast<size_t>(scene));
            if (sc && sc->find("nodes")) {
                const jmini::Node* rn = sc->find("nodes");
                for (size_t i = 0; i < rn->size(); i++) roots.push_back(rn->at(i)->asInt(-1));
            }
        }
    }
    if (roots.empty()) {
        std::vector<bool> hasParent(nNodes, false);
        for (const auto& ch : nodeChildren)
            for (int c : ch)
                if (c >= 0 && c < static_cast<int>(nNodes)) hasParent[static_cast<size_t>(c)] = true;
        for (size_t i = 0; i < nNodes; i++)
            if (!hasParent[i]) roots.push_back(static_cast<int>(i));
    }

    // ------------------------------------------------------------- меши
    const jmini::Node* meshes = root.find("meshes");

    // Обход дерева и сборка треугольников
    std::vector<StackFrame> stk;
    for (int r : roots) {
        if (r < 0 || r >= static_cast<int>(nNodes)) continue;
        StackFrame sf;
        sf.node = r;
        identityMat(sf.world);
        stk.push_back(sf);
    }

    while (!stk.empty()) {
        const StackFrame sf = stk.back();
        stk.pop_back();
        const size_t ni = static_cast<size_t>(sf.node);

        float world[16];
        matMult(sf.world, &nodeMat[ni * 16], world);

        if (nodeMesh[ni] >= 0 && meshes) {
            const jmini::Node* mesh = meshes->at(static_cast<size_t>(nodeMesh[ni]));
            if (!mesh || !mesh->find("primitives")) continue;
            const jmini::Node* prs = mesh->find("primitives");
            for (size_t pi = 0; pi < prs->size(); pi++) {
                const jmini::Node* prim = prs->at(pi);
                if (!prim) continue;
                const int mode = prim->find("mode") ? prim->find("mode")->asInt(4) : 4;
                const jmini::Node* attrs = prim->find("attributes");
                const int posIdx = attrs && attrs->find("POSITION") ? attrs->find("POSITION")->asInt(-1) : -1;
                if (posIdx < 0 || posIdx >= static_cast<int>(accs.size())) continue;
                const Accessor& pa = accs[static_cast<size_t>(posIdx)];
                if (!pa.ok || pa.ct != 5126 || pa.cc != 3) continue;
                const size_t vc = pa.count;

                const size_t base = out.vertexCount();
                for (size_t i = 0; i < vc; i++) {
                    const uint8_t* p = pa.p + i * pa.stride;
                    float x = static_cast<float>(readComponent(p, 5126));
                    float y = static_cast<float>(readComponent(p + 4, 5126));
                    float z = static_cast<float>(readComponent(p + 8, 5126));
                    applyMat(world, x, y, z, x, y, z);
                    out.positions.push_back(x);
                    out.positions.push_back(y);
                    out.positions.push_back(z);
                }

                auto emit = [&](int64_t a, int64_t b, int64_t c) {
                    if (a < 0 || b < 0 || c < 0 ||
                        static_cast<uint64_t>(a) >= vc ||
                        static_cast<uint64_t>(b) >= vc ||
                        static_cast<uint64_t>(c) >= vc) return;
                    out.indices.push_back(static_cast<uint32_t>(base + a));
                    out.indices.push_back(static_cast<uint32_t>(base + b));
                    out.indices.push_back(static_cast<uint32_t>(base + c));
                };

                const int iaIdx = prim->find("indices") ? prim->find("indices")->asInt(-1) : -1;
                const bool indexed = iaIdx >= 0 && iaIdx < static_cast<int>(accs.size()) &&
                                     accs[static_cast<size_t>(iaIdx)].ok;
                const Accessor* ia = indexed ? &accs[static_cast<size_t>(iaIdx)] : nullptr;

                if (mode == 4) {
                    if (ia) {
                        const size_t nt = ia->count / 3;
                        for (size_t t = 0; t < nt; t++)
                            emit(readIndexComponent(ia->p + (t * 3 + 0) * ia->stride, ia->ct),
                                 readIndexComponent(ia->p + (t * 3 + 1) * ia->stride, ia->ct),
                                 readIndexComponent(ia->p + (t * 3 + 2) * ia->stride, ia->ct));
                    } else {
                        for (size_t t = 0; t + 2 < vc; t++) emit(t, t + 1, t + 2);
                    }
                } else if (mode == 5) {
                    const size_t cnt = ia ? ia->count : vc;
                    for (size_t t = 0; t + 2 < cnt; t++) {
                        const int64_t a = ia ? readIndexComponent(ia->p + (t + 0) * ia->stride, ia->ct) : static_cast<int64_t>(t);
                        const int64_t b = ia ? readIndexComponent(ia->p + (t + 1) * ia->stride, ia->ct) : static_cast<int64_t>(t + 1);
                        const int64_t c = ia ? readIndexComponent(ia->p + (t + 2) * ia->stride, ia->ct) : static_cast<int64_t>(t + 2);
                        if (t & 1u) emit(c, b, a); else emit(a, b, c);
                    }
                } else if (mode == 6) {
                    const size_t cnt = ia ? ia->count : vc;
                    const int64_t a0 = ia ? readIndexComponent(ia->p, ia->ct) : 0;
                    for (size_t t = 1; t + 1 < cnt; t++) {
                        const int64_t a = ia ? readIndexComponent(ia->p + t * ia->stride, ia->ct) : static_cast<int64_t>(t);
                        const int64_t b = ia ? readIndexComponent(ia->p + (t + 1) * ia->stride, ia->ct) : static_cast<int64_t>(t + 1);
                        emit(a0, a, b);
                    }
                }
            }
        }

        for (int c : nodeChildren[ni]) {
            if (c < 0 || c >= static_cast<int>(nNodes)) continue;
            StackFrame child;
            child.node = c;
            std::memcpy(child.world, world, sizeof world);
            stk.push_back(child);
        }
    }

    if (out.triangleCount() == 0) { err = "glTF: нет треугольной геометрии"; return false; }
    return finalizeMesh(out, &err);
}