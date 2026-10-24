// threemf_parser.cpp — быстрый парсер 3MF:
//   1) собственный ZIP-ридер извлекает 3D/3dmodel.model
//   2) байтовое сканирование XML (без xml-библиотек, без аллокаций на тег)
//   3) объекты из <build> объединяются с учётом transform
// Поддержка "этапа 1": однокомпонентная сетка, компоненты/текстуры/цвета — далее.
#include "threemf_parser.h"

#include "zip_reader.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// ------------------------------------------------------------------ helpers

inline bool isWs(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Ищет атрибут `name=` внутри тега [tag, end). Проверяет, что перед
// именем — пробел или начало тега (чтобы "id" не сматчил "objectid").
const char* findAttrStart(const char* tag, const char* end, const char* name) {
    const size_t nl = std::strlen(name);
    const char* p = tag;
    while (p + nl <= end) {
        bool match = true;
        for (size_t i = 0; i < nl; i++) {
            if (p[i] != name[i]) { match = false; break; }
        }
        if (match) {
            const bool boundary = (p == tag) || isWs(p[-1]);
            if (boundary) {
                const char* after = p + nl;
                while (after < end && isWs(*after)) after++;
                if (after < end && *after == '=') return p;
            }
        }
        p++;
    }
    return nullptr;
}

// Значение атрибута копируется в buf (с завершающим нулём)
bool getAttr(const char* tag, const char* end, const char* name,
             char* buf, size_t bufSize) {
    const char* p = findAttrStart(tag, end, name);
    if (!p) return false;
    p += std::strlen(name);
    while (p < end && isWs(*p)) p++;
    if (p < end) p++;                 // '='
    while (p < end && isWs(*p)) p++;
    if (p >= end) return false;
    const char qc = *p;
    if (qc != '"' && qc != '\'') return false;
    p++;
    size_t i = 0;
    while (p < end && *p != qc) {
        if (i + 1 < bufSize) buf[i++] = *p;
        p++;
    }
    buf[i] = '\0';
    return i > 0;
}

// Имя тега без XML-префикса: "m:vertex" -> "vertex"
void tagBase(const char* s, const char* e, char* out, size_t cap) {
    const char* colon = nullptr;
    for (const char* q = s; q < e; q++) if (*q == ':') colon = q;
    if (colon) s = colon + 1;
    size_t n = 0;
    for (const char* q = s; q < e && n + 1 < cap; q++) {
        if (isWs(*q) || *q == '/' ) break;
        out[n++] = *q;
    }
    out[n] = '\0';
}

// Парсинг 12 чисел трансформа 3MF (3x4, row-major; последняя колонка — перенос)
bool parseTransform(const char* s, float m[12]) {
    for (int i = 0; i < 12; i++) {
        while (isWs(*s)) s++;
        char* next = nullptr;
        const float v = std::strtof(s, &next);
        if (next == s) return false;
        m[i] = v;
        s = next;
    }
    return true;
}

struct ObjMesh {
    uint32_t id = 0;
    std::vector<float> pos;   // xyz
    std::vector<uint32_t> idx;
};

struct BuildItem {
    uint32_t objectId = 0;
    float m[12];
    bool hasTransform = false;
};

void appendObject(MeshData& out, const ObjMesh& o, const float* m) {
    const uint32_t base = static_cast<uint32_t>(out.vertexCount());
    const size_t vc = o.pos.size() / 3;
    out.positions.reserve(out.positions.size() + o.pos.size());
    for (size_t i = 0; i < vc; i++) {
        float x = o.pos[i * 3 + 0];
        float y = o.pos[i * 3 + 1];
        float z = o.pos[i * 3 + 2];
        if (m) {
            const float nx = m[0] * x + m[1] * y + m[2] * z + m[3];
            const float ny = m[4] * x + m[5] * y + m[6] * z + m[7];
            const float nz = m[8] * x + m[9] * y + m[10] * z + m[11];
            x = nx; y = ny; z = nz;
        }
        out.positions.push_back(x);
        out.positions.push_back(y);
        out.positions.push_back(z);
    }
    for (uint32_t idx : o.idx) out.indices.push_back(base + idx);
}

// -------------------------------------------------------------- XML-сканирование

bool parseXml(const uint8_t* xml, size_t size, MeshData& out, std::string& err) {
    const char* p = reinterpret_cast<const char*>(xml);
    const char* end = p + size;

    std::vector<ObjMesh> objects;
    objects.reserve(8);
    std::vector<BuildItem> items;
    ObjMesh* cur = nullptr;

    char tagB[64];
    char attr[256];

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
        if (t >= gt) { p = gt + 1; continue; }

        tagBase(t, gt, tagB, sizeof tagB);
        const char* attrs = t;
        while (attrs < gt && !isWs(*attrs) && *attrs != '/' ) attrs++;

        if (closing) {
            if (std::strcmp(tagB, "object") == 0) cur = nullptr;
        } else {
            if (std::strcmp(tagB, "object") == 0) {
                if (getAttr(attrs, gt, "id", attr, sizeof attr)) {
                    objects.emplace_back();
                    objects.back().id = static_cast<uint32_t>(std::strtoul(attr, nullptr, 10));
                    cur = &objects.back();
                }
            } else if (std::strcmp(tagB, "vertex") == 0 && cur) {
                float x = 0.f, y = 0.f, z = 0.f;
                if (getAttr(attrs, gt, "x", attr, sizeof attr)) x = std::strtof(attr, nullptr);
                if (getAttr(attrs, gt, "y", attr, sizeof attr)) y = std::strtof(attr, nullptr);
                if (getAttr(attrs, gt, "z", attr, sizeof attr)) z = std::strtof(attr, nullptr);
                cur->pos.push_back(x);
                cur->pos.push_back(y);
                cur->pos.push_back(z);
            } else if (std::strcmp(tagB, "triangle") == 0 && cur) {
                uint32_t v1 = 0, v2 = 0, v3 = 0;
                if (getAttr(attrs, gt, "v1", attr, sizeof attr)) v1 = static_cast<uint32_t>(std::strtoul(attr, nullptr, 10));
                if (getAttr(attrs, gt, "v2", attr, sizeof attr)) v2 = static_cast<uint32_t>(std::strtoul(attr, nullptr, 10));
                if (getAttr(attrs, gt, "v3", attr, sizeof attr)) v3 = static_cast<uint32_t>(std::strtoul(attr, nullptr, 10));
                cur->idx.push_back(v1);
                cur->idx.push_back(v2);
                cur->idx.push_back(v3);
            } else if (std::strcmp(tagB, "item") == 0) {
                if (getAttr(attrs, gt, "objectid", attr, sizeof attr)) {
                    BuildItem it;
                    it.objectId = static_cast<uint32_t>(std::strtoul(attr, nullptr, 10));
                    if (getAttr(attrs, gt, "transform", attr, sizeof attr)) {
                        if (parseTransform(attr, it.m)) it.hasTransform = true;
                    }
                    items.push_back(it);
                }
            }
            // <model>, <resources>, <mesh>, <build>, <basematerials> и пр. игнорируем
        }
        p = gt + 1;
    }

    // Сборка: используем объекты из <build> (или все объекты, если build пуст)
    if (!items.empty()) {
        for (const BuildItem& it : items) {
            bool found = false;
            for (const ObjMesh& o : objects) {
                if (o.id == it.objectId) {
                    appendObject(out, o, it.hasTransform ? it.m : nullptr);
                    found = true;
                    break;
                }
            }
            if (!found) { /* объект не найден в ресурсах — пропускаем */ }
        }
    } else {
        for (const ObjMesh& o : objects) appendObject(out, o, nullptr);
    }

    if (out.triangleCount() == 0) {
        err = "3MF: в модели нет треугольников";
        return false;
    }
    return finalizeMesh(out, &err);
}

} // namespace

bool parse3mfFile(const uint8_t* data, size_t size, MeshData& out, std::string& err) {
    out.clear();
    err.clear();

    std::vector<uint8_t> xml;
    if (!zip_extract_file(data, size, "3D/3dmodel.model", xml)) {
        if (!zip_extract_file(data, size, "3D/3dmodel.xml", xml)) {
            err = "3MF: не найден 3D/3dmodel.model (или архив повреждён)";
            return false;
        }
    }
    if (xml.empty()) {
        err = "3MF: пустой файл модели";
        return false;
    }
    return parseXml(xml.data(), xml.size(), out, err);
}