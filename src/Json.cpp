// Json.cpp — see Json.h.
#include "Json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

const Json kNull;
const std::string kEmpty;

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

void dumpString(std::string& out, const std::string& s) {
    static const char* hex = "0123456789abcdef";
    out += '"';
    for (char ch : s) {
        unsigned char c = (unsigned char)ch;
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0xF];
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string error;
    int depth = 0;

    explicit Parser(const std::string& text) : s(text) {}

    bool fail(const std::string& msg) {
        if (error.empty()) error = msg + " at offset " + std::to_string(i);
        return false;
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
    }
    bool literal(const char* word) {
        size_t n = std::char_traits<char>::length(word);
        if (s.compare(i, n, word) != 0) return fail("unexpected character");
        i += n;
        return true;
    }
    bool hex4(uint32_t& v) {
        if (i + 4 > s.size()) return fail("short \\u escape");
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        i++;   // opening quote
        while (true) {
            if (i >= s.size()) return fail("unterminated string");
            char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return fail("control character in string");
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return fail("unterminated escape");
            char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // A high surrogate must be followed by \u and a low one.
                    uint32_t lo;
                    if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else { appendUtf8(out, 0xFFFD); cp = lo; }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;   // a lone low surrogate
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
    }
    bool number(Json& out) {
        size_t start = i;
        if (s[i] == '-') i++;
        if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) return fail("bad number");
        if (s[i] == '0') i++;
        else while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        if (i < s.size() && s[i] == '.') {
            i++;
            if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) return fail("bad number");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            i++;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
            if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) return fail("bad number");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        }
        // strtod follows the C locale, which is "C" unless someone changed it;
        // JSON numbers only ever use '.', so parse a copy to be safe.
        std::string num = s.substr(start, i - start);
        out = Json(std::strtod(num.c_str(), nullptr));
        return true;
    }
    bool value(Json& out) {
        ws();
        if (i >= s.size()) return fail("unexpected end");
        if (++depth > 256) return fail("nested too deeply");
        bool ok = true;
        char c = s[i];
        if (c == '{') {
            out = Json::object();
            i++;
            ws();
            if (i < s.size() && s[i] == '}') { i++; }
            else {
                while (true) {
                    ws();
                    if (i >= s.size() || s[i] != '"') { ok = fail("expected key"); break; }
                    std::string key;
                    if (!string(key)) { ok = false; break; }
                    ws();
                    if (i >= s.size() || s[i] != ':') { ok = fail("expected ':'"); break; }
                    i++;
                    Json v;
                    if (!value(v)) { ok = false; break; }
                    out.set(key, std::move(v));
                    ws();
                    if (i < s.size() && s[i] == ',') { i++; continue; }
                    if (i < s.size() && s[i] == '}') { i++; break; }
                    ok = fail("expected ',' or '}'");
                    break;
                }
            }
        } else if (c == '[') {
            out = Json::array();
            i++;
            ws();
            if (i < s.size() && s[i] == ']') { i++; }
            else {
                while (true) {
                    Json v;
                    if (!value(v)) { ok = false; break; }
                    out.push(std::move(v));
                    ws();
                    if (i < s.size() && s[i] == ',') { i++; continue; }
                    if (i < s.size() && s[i] == ']') { i++; break; }
                    ok = fail("expected ',' or ']'");
                    break;
                }
            }
        } else if (c == '"') {
            std::string str;
            ok = string(str);
            if (ok) out = Json(std::move(str));
        } else if (c == 't') {
            ok = literal("true");
            if (ok) out = Json(true);
        } else if (c == 'f') {
            ok = literal("false");
            if (ok) out = Json(false);
        } else if (c == 'n') {
            ok = literal("null");
            if (ok) out = Json();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            ok = number(out);
        } else {
            ok = fail("unexpected character");
        }
        depth--;
        return ok;
    }
};

}  // namespace

Json Json::object(std::initializer_list<std::pair<std::string, Json>> kv) {
    Json j = object();
    for (const auto& p : kv) j.set(p.first, p.second);
    return j;
}

const std::string& Json::asString() const { return isString() ? str_ : kEmpty; }

const Json& Json::operator[](const std::string& key) const {
    if (!isObject()) return kNull;
    for (const auto& p : obj_)
        if (p.first == key) return p.second;
    return kNull;
}

bool Json::has(const std::string& key) const {
    if (!isObject()) return false;
    for (const auto& p : obj_)
        if (p.first == key) return true;
    return false;
}

Json& Json::set(const std::string& key, Json value) {
    if (!isObject()) { *this = object(); }
    for (auto& p : obj_)
        if (p.first == key) { p.second = std::move(value); return *this; }
    obj_.emplace_back(key, std::move(value));
    return *this;
}

const Json& Json::operator[](size_t i) const {
    if (!isArray() || i >= arr_.size()) return kNull;
    return arr_[i];
}

size_t Json::size() const {
    if (isArray()) return arr_.size();
    if (isObject()) return obj_.size();
    return 0;
}

Json& Json::push(Json value) {
    if (!isArray()) { *this = array(); }
    arr_.push_back(std::move(value));
    return *this;
}

bool Json::operator==(const Json& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
    case Type::Null: return true;
    case Type::Bool: return bool_ == o.bool_;
    case Type::Number: return num_ == o.num_;
    case Type::String: return str_ == o.str_;
    case Type::Array: return arr_ == o.arr_;
    case Type::Object:
        if (obj_.size() != o.obj_.size()) return false;
        for (const auto& p : obj_)
            if (!o.has(p.first) || o[p.first] != p.second) return false;
        return true;
    }
    return false;
}

void Json::dumpTo(std::string& out) const {
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
        char buf[32];
        if (!std::isfinite(num_)) {
            out += "null";   // JSON has no NaN or infinity
        } else if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
            std::snprintf(buf, sizeof buf, "%lld", (long long)num_);
            out += buf;
        } else {
            std::snprintf(buf, sizeof buf, "%.17g", num_);
            out += buf;
        }
        break;
    }
    case Type::String: dumpString(out, str_); break;
    case Type::Array:
        out += '[';
        for (size_t i = 0; i < arr_.size(); i++) {
            if (i) out += ',';
            arr_[i].dumpTo(out);
        }
        out += ']';
        break;
    case Type::Object:
        out += '{';
        for (size_t i = 0; i < obj_.size(); i++) {
            if (i) out += ',';
            dumpString(out, obj_[i].first);
            out += ':';
            obj_[i].second.dumpTo(out);
        }
        out += '}';
        break;
    }
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

bool Json::parse(const std::string& text, Json& out, std::string* error) {
    Parser p(text);
    Json v;
    bool ok = p.value(v);
    if (ok) {
        p.ws();
        if (p.i != text.size()) ok = p.fail("trailing characters");
    }
    if (!ok) {
        if (error) *error = p.error;
        return false;
    }
    out = std::move(v);
    return true;
}
