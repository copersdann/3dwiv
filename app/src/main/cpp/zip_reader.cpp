// zip_reader.cpp — чтение ZIP напрямую по спецификации PKWARE.
// Не использует java.util.zip — всё в нативном коде для скорости.
#include "zip_reader.h"
#include "inflate.h"

#include <cstring>

namespace {

inline uint16_t rd16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline bool nameEq(const uint8_t* name, uint16_t len, const char* target) {
    const size_t tlen = std::strlen(target);
    if (len != tlen) return false;
    for (uint16_t i = 0; i < len; i++) {
        char c = static_cast<char>(name[i]);
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        char t = target[i];
        if (t >= 'A' && t <= 'Z') t = static_cast<char>(t - 'A' + 'a');
        if (c != t) return false;
    }
    return true;
}

} // namespace

bool zip_extract_file(const uint8_t* zip, size_t zipSize,
                      const char* wanted, std::vector<uint8_t>& out) {
    if (!zip || zipSize < 22) return false;

    // ---- 1) Находим End Of Central Directory (последние 64 КБ)
    const uint32_t kEOCD = 0x06054b50;
    int64_t eocd = -1;
    const size_t scanStart = zipSize > 65557 ? zipSize - 65557 : 0;
    for (int64_t i = static_cast<int64_t>(zipSize) - 22;
         i >= static_cast<int64_t>(scanStart) && i >= 0; i--) {
        if (rd32(zip + i) == kEOCD) { eocd = i; break; }
    }
    if (eocd < 0) return false;

    const uint8_t* e = zip + eocd;
    const uint16_t nEntries = rd16(e + 10);
    const uint32_t cdSize = rd32(e + 12);
    const uint32_t cdOffset = rd32(e + 16);
    if (nEntries == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu)
        return false; // zip64 не поддерживаем (3MF его не требует)

    if (static_cast<uint64_t>(cdOffset) + cdSize > zipSize) return false;

    // ---- 2) Обходим центральный каталог
    const uint8_t* cd = zip + cdOffset;
    const uint8_t* cdEnd = cd + cdSize;
    const uint32_t kCDSIG = 0x02014b50;

    while (cd + 46 <= cdEnd) {
        if (rd32(cd) != kCDSIG) { cd++; continue; }

        const uint16_t method = rd16(cd + 10);
        const uint32_t compSize = rd32(cd + 20);
        const uint32_t uncompSize = rd32(cd + 24);
        const uint16_t nameLen = rd16(cd + 28);
        const uint16_t extraLen = rd16(cd + 30);
        const uint16_t commentLen = rd16(cd + 32);
        const uint32_t localOff = rd32(cd + 42);

        if (static_cast<uint64_t>(cd + 46 - zip) + nameLen > zipSize) return false;

        if (nameEq(cd + 46, nameLen, wanted)) {
            // ---- 3) Local File Header
            if (static_cast<uint64_t>(localOff) + 30 > zipSize) return false;
            const uint8_t* lh = zip + localOff;
            if (rd32(lh) != 0x04034b50) return false;

            const uint16_t lNameLen = rd16(lh + 26);
            const uint16_t lExtraLen = rd16(lh + 28);
            const uint32_t dataOff = localOff + 30 + lNameLen + lExtraLen;
            if (static_cast<uint64_t>(dataOff) + compSize > zipSize) return false;

            const uint8_t* cdata = zip + dataOff;
            if (method == 0) {  // STORE
                out.assign(cdata, cdata + compSize);
                return true;
            }
            if (method == 8) {  // DEFLATE
                out.reserve(uncompSize);
                return inflate::raw(cdata, compSize, out) == 0;
            }
            return false; // другой метод (bzip2/lzma/...) не поддерживается
        }

        cd += 46 + nameLen + extraLen + commentLen;
    }

    return false; // запись не найдена
}