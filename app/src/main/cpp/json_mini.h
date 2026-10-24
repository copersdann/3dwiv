// json_mini.h — компактный DOM-парсер JSON (без зависимостей).
// Нужен для glTF (.gltf/.glb). Строки хранятся в UTF-8; поддерживаются
// escape-последовательности и \uXXXX.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jmini {

enum class Type { Null, Bool, Num, Str, Arr, Obj };

struct Node {
    Type t = Type::Null;
    double num = 0.0;
    bool boolean = false;
    std::string str;
    std::vector<std::pair<std::string, Node>> obj;  // упорядоченные пары
    std::vector<Node> arr;

    const Node* find(const char* key) const;
    const Node* at(size_t i) const;
    size_t size() const;

    bool isArr() const { return t == Type::Arr; }
    bool isObj() const { return t == Type::Obj; }
    bool isNum() const { return t == Type::Num; }
    double asNum(double def = 0.0) const { return t == Type::Num ? num : def; }
    int asInt(int def = 0) const { return t == Type::Num ? static_cast<int>(num) : def; }
    bool asBool(bool def = false) const { return t == Type::Bool ? boolean : def; }
    const std::string& asStr(const std::string& def = {}) const {
        return t == Type::Str ? str : def;
    }
};

// Парсит JSON; возвращает true при успехе.
bool parse(const char* text, size_t len, Node& out);

} // namespace jmini