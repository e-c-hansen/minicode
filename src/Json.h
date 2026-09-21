// Json.h — pure C++. A small JSON value with a parser and a serializer, just
// enough for the Language Server Protocol: objects keep their keys in insertion
// order, numbers are doubles (LSP only sends integers that fit), and strings
// are UTF-8 with \u escapes (surrogate pairs included) decoded on parse.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;                               // null
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int n) : type_(Type::Number), num_(n) {}
    Json(int64_t n) : type_(Type::Number), num_((double)n) {}
    Json(size_t n) : type_(Type::Number), num_((double)n) {}
    Json(double n) : type_(Type::Number), num_(n) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }
    // object({{"a", 1}, {"b", "x"}})
    static Json object(std::initializer_list<std::pair<std::string, Json>> kv);

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Typed reads that never throw: the fallback when the type is wrong.
    bool asBool(bool fallback = false) const { return isBool() ? bool_ : fallback; }
    double asNumber(double fallback = 0) const { return isNumber() ? num_ : fallback; }
    int64_t asInt(int64_t fallback = 0) const {
        return isNumber() ? (int64_t)num_ : fallback;
    }
    const std::string& asString() const;            // "" unless a string
    std::string asString(const std::string& fallback) const {
        return isString() ? str_ : fallback;
    }

    // Object access. A missing key, or a non-object, reads as null.
    const Json& operator[](const std::string& key) const;
    const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
    bool has(const std::string& key) const;
    Json& set(const std::string& key, Json value);   // makes this an object
    const std::vector<std::pair<std::string, Json>>& items() const { return obj_; }

    // Array access. Out of range, or a non-array, reads as null.
    const Json& operator[](size_t i) const;
    const Json& operator[](int i) const { return (*this)[(size_t)i]; }
    size_t size() const;                             // elements or members
    Json& push(Json value);                          // makes this an array
    const std::vector<Json>& elements() const { return arr_; }

    bool operator==(const Json& o) const;
    bool operator!=(const Json& o) const { return !(*this == o); }

    // Compact serialization, no whitespace. Non-ASCII is written as UTF-8.
    std::string dump() const;
    // Strict parse of one value (surrounding whitespace allowed).
    static bool parse(const std::string& text, Json& out, std::string* error = nullptr);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;

    void dumpTo(std::string& out) const;
};
