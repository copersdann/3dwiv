// inflate.cpp — декомпрессор DEFLATE (RFC 1951) с нуля, без зависимостей.
//
// Чтение битов — LSB-first, как в спецификации. Код Хаффмана передаётся
// MSB-first, поэтому при построении таблицы код реверсируется (reverse bits).
// Таблица — прямой lookup на 15 бит (32768 x uint32: [len<<16 | sym]).
//
// Реализовано: stored-блоки, fixed-Huffman, dynamic-Huffman (полные и
// неполные деревья), LZ77-копии. Это ядро распаковки 3MF.

#include "inflate.h"

#include <cstring>

namespace {

constexpr int kTBits = 15;                 // размер корневой таблицы
constexpr uint32_t kInvalid = 0xFFFFFFFFu; // маркер "кода нет/ошибка"
constexpr size_t kMaxOut = 1ull << 30;     // защита от zip-бомб (1 GiB)

// ------------------------------------------------------------------ bits

struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    uint32_t buf = 0;
    int cnt = 0;  // сколько значащих бит в buf

    explicit BitReader(const uint8_t* d, size_t s) : data(d), size(s) {}

    inline uint32_t peek(int n) {
        while (cnt < n && cnt < 24) {
            if (pos < size) {
                buf |= uint32_t(data[pos++]) << cnt;
            }
            cnt += 8;  // если поток кончился — добиваем нулями (безопасно)
        }
        return buf;
    }

    inline void drop(int n) { buf >>= n; cnt -= n; }

    inline uint32_t read(int n) {
        uint32_t v = peek(n);
        drop(n);
        return v;
    }
};

// ------------------------------------------------------------------ huffman

inline uint32_t reverseBits(uint32_t v, int bits) {
    uint32_t r = 0;
    for (int i = 0; i < bits; i++) { r = (r << 1) | (v & 1u); v >>= 1; }
    return r;
}

struct Huff {
    std::vector<uint32_t> table;

    bool build(const uint8_t* lens, int numSyms) {
        int count[16] = {0};
        for (int i = 0; i < numSyms; i++) {
            if (lens[i] > 15) return false;
            count[lens[i]]++;
        }

        // Проверка переподписки (over-subscribed)
        int left = 1;
        for (int len = 1; len <= 15; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }

        table.assign(1u << kTBits, kInvalid);

        // Канонические коды: символы сортируются по (длине, значению)
        int code = 0;
        for (int len = 1; len <= 15; len++) {
            for (int sym = 0; sym < numSyms; sym++) {
                if (lens[sym] != len) continue;
                const uint32_t rev = reverseBits(static_cast<uint32_t>(code), len);
                const uint32_t step = 1u << (kTBits - len);
                const uint32_t entry = (static_cast<uint32_t>(len) << 16) | static_cast<uint32_t>(sym);
                for (uint32_t k = 0; k < step; k++) {
                    table[rev | (k << len)] = entry;
                }
                code++;
            }
            code <<= 1;
        }
        return true;
    }

    inline int decode(BitReader& br) const {
        const uint32_t v = br.peek(kTBits) & ((1u << kTBits) - 1u);
        const uint32_t e = table[v];
        if (e == kInvalid) return -1;
        br.drop(static_cast<int>(e >> 16));
        return static_cast<int>(e & 0xFFFFu);
    }
};

// ------------------------------------------------------------------ tables

// RFC 1951, таблица 3: базовые значения и доп. биты для length-кодов 257..285
const int kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                          67,83,99,115,131,163,195,227,258};
const int kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};

// RFC 1951, таблица 4: дистанции 0..29
const int kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,
                           513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const int kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

// Порядок символов для code-length кодов (таблица 3 RFC 1951)
const int kClOrder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

void buildFixed(Huff& lit, Huff& dst) {
    uint8_t lens[288];
    int i = 0;
    for (; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    lit.build(lens, 288);

    uint8_t dlens[32];
    for (i = 0; i < 32; i++) dlens[i] = 5;
    dst.build(dlens, 32);
}

int decodeBlock(BitReader& br, const Huff& lit, const Huff& dst,
                std::vector<uint8_t>& out) {
    for (;;) {
        int sym = lit.decode(br);
        if (sym < 0) return 1;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return 0; // конец блока
        } else if (sym <= 285) {
            const int li = sym - 257;
            const int len = kLenBase[li] + static_cast<int>(br.read(kLenExtra[li]));
            const int dsym = dst.decode(br);
            if (dsym < 0 || dsym > 29) return 2;
            const int dist = kDistBase[dsym] + static_cast<int>(br.read(kDistExtra[dsym]));
            const size_t here = out.size();
            if (dist > static_cast<int>(here)) return 3;
            if (here + static_cast<size_t>(len) > kMaxOut) return 4;
            out.resize(here + static_cast<size_t>(len));
            size_t src = here - static_cast<size_t>(dist);
            for (int i = 0; i < len; i++) out[here + static_cast<size_t>(i)] = out[src + static_cast<size_t>(i)];
        } else {
            return 5; // коды 286/287 использовать нельзя
        }
    }
}

} // namespace

namespace inflate {

int raw(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out) {
    out.clear();
    if (!src || srcLen == 0) return 6;
    out.reserve(srcLen * 2 + 1024);

    BitReader br(src, srcLen);

    for (;;) {
        const int finalFlag = static_cast<int>(br.read(1));
        const int type = static_cast<int>(br.read(2));

        if (type == 0) {
            // stored block: выравнивание на байт = сброс битового буфера
            br.buf = 0;
            br.cnt = 0;
            const uint32_t len = br.read(16);
            const uint32_t nlen = br.read(16);
            if ((len ^ nlen) != 0xFFFFu) return 7;
            if (out.size() + static_cast<size_t>(len) > kMaxOut) return 8;
            const size_t need = out.size() + len;
            out.resize(need);
            uint8_t* dst = out.data() + out.size() - len;
            for (uint32_t i = 0; i < len; i++) dst[i] = static_cast<uint8_t>(br.read(8));
        } else if (type == 3) {
            return 9; // зарезервированный тип
        } else {
            Huff lit, dst;
            if (type == 1) {
                buildFixed(lit, dst);
            } else {
                const int hlit = static_cast<int>(br.read(5)) + 257;
                const int hdist = static_cast<int>(br.read(5)) + 1;
                const int hclen = static_cast<int>(br.read(4)) + 4;
                if (hlit > 288 || hdist > 32) return 10;

                uint8_t clens[19] = {0};
                for (int i = 0; i < hclen; i++) clens[kClOrder[i]] = static_cast<uint8_t>(br.read(3));

                Huff cl;
                if (!cl.build(clens, 19)) return 11;

                uint8_t lens[288 + 32];
                const int total = hlit + hdist;
                int idx = 0;
                while (idx < total) {
                    const int s = cl.decode(br);
                    if (s < 0) return 12;
                    if (s < 16) {
                        lens[idx++] = static_cast<uint8_t>(s);
                    } else if (s == 16) {
                        if (idx == 0) return 13;
                        const uint8_t prev = lens[idx - 1];
                        int rep = 3 + static_cast<int>(br.read(2));
                        while (rep-- > 0 && idx < total) lens[idx++] = prev;
                    } else if (s == 17) {
                        int rep = 3 + static_cast<int>(br.read(3));
                        while (rep-- > 0 && idx < total) lens[idx++] = 0;
                    } else { // 18
                        int rep = 11 + static_cast<int>(br.read(7));
                        while (rep-- > 0 && idx < total) lens[idx++] = 0;
                    }
                }
                if (!lit.build(lens, hlit)) return 14;
                if (!dst.build(lens + hlit, hdist)) return 15;
            }

            const int rc = decodeBlock(br, lit, dst, out);
            if (rc != 0) return 16;
        }

        if (finalFlag) break;
    }
    return 0;
}

} // namespace inflate