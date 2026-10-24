// json_mini.cpp — реализация мини-JSON DOM.
#include "json_mini.h"

#include <cstdlib>
#include <cstring>

namespace jmini {

const Node* Node::find(const char* key) const {
    if (t != Type::Obj) return nullptr;
    for (const auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
}

const Node* Node::at(size_t i) const {
    return (t == Type::Arr && i < arr.size()) ? &arr[i] : nullptr;
}

size_t Node::size() const {
    if (t == Type::Arr) return arr.size();
    if (t == Type::Obj) return obj.size();
    return 0;
}

namespace {

struct Parser {
    const char* s;
    size_t len;
    size_t pos = 0;

    void skipWs() {
        while (pos < len && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n'))
            pos++;
    }

    static unsigned hexVal(char c) {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        return 0xFFFFFFFFu;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseString(std::string& out) {
        if (pos >= len || s[pos] != '"') return false;
        pos++;  // открывающая кавычка
        out.clear();
        while (pos < len) {
            char c = s[pos];
            if (c == '"') { pos++; return true; }
            if (c == '\\') {
                pos++;
                if (pos >= len) return false;
                switch (s[pos]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'u': {
                        if (pos + 4 >= len) return false;
                        unsigned cp = 0;
                        for (int i = 0; i < 4; i++) {
                            const unsigned h = hexVal(s[pos + 1 + static_cast<size_t>(i)]);
                            if (h == 0xFFFFFFFFu) return false;
                            cp = (cp << 4) | h;
                        }
                        pos += 4;
                        appendUtf8(out, cp);
                    } break;
                    default: return false;
                }
                pos++;
            } else {
                out += c;
                pos++;
            }
        }
        return false;
    }

    bool parseNumber(Node& n) {
        const size_t start = pos;
        if (pos < len && s[pos] == '-') pos++;
        bool digits = false;
        while (pos < len && s[pos] >= '0' && s[pos] <= '9') { pos++; digits = true; }
        if (pos < len && s[pos] == '.') {
            pos++;
            while (pos < len && s[pos] >= '0' && s[pos] <= '9') { pos++; digits = true; }
        }
        if (pos < len && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < len && (s[pos] == '+' || s[pos] == '-')) pos++;
            bool edigits = false;
            while (pos < len && s[pos] >= '0' && s[pos] <= '9') { pos++; edigits = true; }
            if (!edigits) return false;
        }
        if (!digits) return false;
        std::string tmp(s + start, pos - start);
        n.t = Type::Num;
        n.num = std::strtod(tmp.c_str(), nullptr);
        return true;
    }

    bool literal(const char* lit, Node& n) {
        const size_t l = std::strlen(lit);
        if (pos + l <= len && std::memcmp(s + pos, lit, l) == 0) {
            pos += l;
            n.t = Type::Bool;
            n.boolean = (lit[0] == 't');
            return true;
        }
        return false;
    }

    bool parseNode(Node& n) {
        skipWs();
        if (pos >= len) return false;
        const char c = s[pos];
        if (c == '{') return parseObj(n);
        if (c == '[') return parseArr(n);
        if (c == '"') { if (!parseString(n.str)) return false; n.t = Type::Str; return true; }
        if (c == 't') return literal("true", n);
        if (c == 'f') return literal("false", n);
        if (c == 'n') { if (!literal("null", n)) return false; n.t = Type::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(n);
        return false;
    }

    bool parseObj(Node& n) {
        pos++;  // '{'
        n.t = Type::Obj;
        skipWs();
        if (pos < len && s[pos] == '}') { pos++; return true; }
        for (;;) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos >= len || s[pos] != ':') return false;
            pos++;
            Node val;
            if (!parseNode(val)) return false;
            n.obj.emplace_back(std::move(key), std::move(val));
            skipWs();
            if (pos >= len) return false;
            if (s[pos] == ',') { pos++; continue; }
            if (s[pos] == '}') { pos++; return true; }
            return false;
        }
    }

    bool parseArr(Node& n) {
        pos++;  // '['
        n.t = Type::Arr;
        skipWs();
        if (pos < len && s[pos] == ']') { pos++; return true; }
        for (;;) {
            skipWs();
            Node val;
            if (!parseNode(val)) return false;
            n.arr.push_back(std::move(val));
            skipWs();
            if (pos >= len) return false;
            if (s[pos] == ',') { pos++; continue; }
            if (s[pos] == ']') { pos++; return true; }
            return false;
        }
    }
};

} // namespace

bool parse(const char* text, size_t len, Node& out) {
    Parser p{text, len, 0};
    if (!p.parseNode(out)) return false;
    p.skipWs();
    return p.pos == len;
}

} // namespace jmini