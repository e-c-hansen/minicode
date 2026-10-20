// KeepSorted.cpp — see KeepSorted.h. The sorting follows keep-sorted's own
// code (keepsorted/block.go, line_group.go and options.go, v0.10) step by
// step, so the order, the grouping and the edge cases come out the same.
#include "KeepSorted.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <regex>
#include <set>

namespace KeepSorted {

namespace {

// A line and where it came from in the input.
struct Line {
    std::string text;
    int origin = -1;
};

// ------------------------------------------------------------ string helpers

bool isAsciiSpace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// Length of the Unicode space at s[i], or 0 (Go's unicode.IsSpace, for the
// spaces that turn up in source: ASCII, U+0085 and U+00A0).
size_t spaceAt(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    unsigned char c = (unsigned char)s[i];
    if (isAsciiSpace(c)) return 1;
    if (c == 0xC2 && i + 1 < s.size() &&
        ((unsigned char)s[i + 1] == 0x85 || (unsigned char)s[i + 1] == 0xA0))
        return 2;
    return 0;
}

std::string trimLeft(const std::string& s) {
    size_t i = 0;
    while (size_t n = spaceAt(s, i)) i += n;
    return s.substr(i);
}

std::string trimSpace(const std::string& s) {
    std::string t = trimLeft(s);
    size_t e = t.size();
    while (e > 0 && isAsciiSpace((unsigned char)t[e - 1])) --e;
    return t.substr(0, e);
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool contains(const std::string& s, const std::string& p) {
    return s.find(p) != std::string::npos;
}

bool isWordChar(unsigned char c) {
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// strings.ToLower for ASCII and the common European alphabets.
std::string lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out += (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            ++i;
            continue;
        }
        size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (len != 2 || i + 1 >= s.size()) {
            out.append(s, i, len);
            i += len;
            continue;
        }
        uint32_t cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
        uint32_t l = cp;
        if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) || (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2) ||
            (cp >= 0x410 && cp <= 0x42F))
            l = cp + 0x20;
        else if (cp >= 0x400 && cp <= 0x40F)
            l = cp + 0x50;
        else if (cp >= 0x100 && cp <= 0x17F && cp != 0x130 && cp != 0x131 && cp != 0x138 &&
                 cp != 0x149 && cp != 0x17F) {
            bool oddUpper = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
            if (cp == 0x178) l = 0xFF;
            else if (oddUpper ? (cp % 2 == 1) : (cp % 2 == 0)) l = cp + 1;
        }
        if (l < 0x80) {
            out += (char)l;
        } else if (l < 0x800) {
            out += (char)(0xC0 | (l >> 6));
            out += (char)(0x80 | (l & 0x3F));
        }
        i += 2;
    }
    return out;
}

// ------------------------------------------------------------------ options

enum class Dups { No, Yes, KeepFirstComment, MergeComments };

struct Regex {
    std::regex re;
    std::vector<std::string> names;   // each capture group's name, or ""
    bool hasTemplate = false;
    std::string tmpl;                  // by_regex's "pattern: template"
};

struct Options {
    bool allowYamlLists = true;
    std::vector<int> skipLines;
    bool group = true;
    std::vector<std::string> groupPrefixes;
    bool groupPrefixesSet = false;
    bool block = false;
    bool stickyComments = true;
    std::vector<std::string> stickyPrefixes;
    bool desc = false;
    bool caseSensitive = true;
    bool numeric = false;
    std::vector<std::string> prefixOrder;
    std::vector<std::string> ignorePrefixes;
    int newlineSeparated = 0;
    Dups dups = Dups::Yes;
    std::string commentMarker;
    // by_regex, group_start_regex and group_end_regex, compiled.
    std::vector<std::shared_ptr<Regex>> byRegex, groupStartRegex, groupEndRegex;
    std::string unsupported;   // an option this port cannot honour

    int startOffset() const {
        if (skipLines.size() == 2) return std::max(skipLines[0], skipLines[1]);
        if (skipLines.size() == 1 && skipLines[0] > 0) return skipLines[0];
        return 0;
    }
    int endOffset() const {
        if (skipLines.size() == 2) return std::min(skipLines[0], skipLines[1]);
        if (skipLines.size() == 1 && skipLines[0] < 0) return skipLines[0];
        return 0;
    }
};

// keep-sorted's option parser: "key=value" pairs, where a key is a run of
// lowercase letters and underscores at the start or after a space.
struct OptionParser {
    std::string line;
    bool allowYaml = true;

    bool popKey(std::string& key) {
        for (size_t p = 0; p < line.size(); ++p) {
            if (p > 0 && line[p - 1] != ' ') continue;
            size_t q = p;
            while (q < line.size() && ((line[q] >= 'a' && line[q] <= 'z') || line[q] == '_')) ++q;
            if (q > p && q < line.size() && line[q] == '=') {
                key = line.substr(p, q - p);
                line = line.substr(q + 1);
                return true;
            }
        }
        return false;
    }

    // The text up to the next space (strings.Cut(line, " ")).
    std::string popWord() {
        size_t sp = line.find(' ');
        std::string v = line.substr(0, sp);
        line = sp == std::string::npos ? "" : line.substr(sp + 1);
        return v;
    }

    // A YAML flow sequence at the start of `line`: its text and the rest.
    // Returns 0 when there is none, -1 when it never ends.
    int findYamlList(std::string& list, std::string& rest) const {
        if (line.empty() || line[0] != '[') return 0;
        char quote = 0;
        int depth = 0;
        size_t i = 0;
        bool done = false;
        while (i < line.size() && !done) {
            char ch = line[i++];
            switch (ch) {
                case '[':
                    if (!quote) ++depth;
                    break;
                case ']':
                    if (!quote) {
                        if (depth > 1) {
                            --depth;
                            break;
                        }
                        if (i >= line.size() || line[i] == ' ') {
                            if (i < line.size()) ++i;
                            --depth;
                        }
                        done = true;
                    }
                    break;
                case '"':
                case '\'':
                    if (quote == ch) quote = 0;
                    else if (!quote) quote = ch;
                    break;
                case '\\':
                    if (quote == '"' && i < line.size() && (line[i] == '"' || line[i] == '\\')) ++i;
                    break;
            }
        }
        if (depth != 0) return -1;
        list = line.substr(0, i);
        rest = line.substr(i);
        return 1;
    }

    // The items of a YAML flow sequence of strings.
    // The items of a YAML flow sequence of strings. An item may also be a
    // one-pair map, "pattern: template" (by_regex); `templates`, when given,
    // gets each item's template, and `isMap` whether it had one.
    static bool parseYamlList(const std::string& list, std::vector<std::string>& out,
                              std::string& err, std::vector<std::string>* templates = nullptr,
                              std::vector<bool>* isMap = nullptr) {
        std::string body = trimSpace(list);
        body = body.substr(1, body.size() - 2);   // without [ and ]
        size_t i = 0;
        auto skipSpace = [&] {
            while (i < body.size() && isAsciiSpace((unsigned char)body[i])) ++i;
        };
        auto scalar = [&](std::string& item) {
            item.clear();
            if (i < body.size() && body[i] == '"') {
                ++i;
                while (i < body.size()) {
                    char c = body[i++];
                    if (c == '"') return true;
                    if (c == '\\' && i < body.size()) {
                        char e = body[i++];
                        item += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    } else {
                        item += c;
                    }
                }
                err = "unterminated string in a YAML list";
                return false;
            }
            if (i < body.size() && body[i] == '\'') {
                ++i;
                while (i < body.size()) {
                    char c = body[i++];
                    if (c == '\'') {
                        if (i < body.size() && body[i] == '\'') {
                            item += '\'';
                            ++i;
                            continue;
                        }
                        return true;
                    }
                    item += c;
                }
                err = "unterminated string in a YAML list";
                return false;
            }
            size_t st = i;
            while (i < body.size() && body[i] != ',' &&
                   !(body[i] == ':' && (i + 1 >= body.size() || body[i + 1] == ' '))) {
                if (body[i] == '[' || body[i] == '{') {
                    err = "nested values in a YAML list are not supported";
                    return false;
                }
                ++i;
            }
            item = trimSpace(body.substr(st, i - st));
            return true;
        };
        skipSpace();
        if (i >= body.size()) return true;
        while (i <= body.size()) {
            skipSpace();
            std::string item, tmpl;
            if (!scalar(item)) return false;
            out.push_back(item);
            skipSpace();
            bool map = false;
            if (i < body.size() && body[i] == ':') {
                if (!templates) {
                    err = "a map in a YAML list is not supported here";
                    return false;
                }
                ++i;
                skipSpace();
                if (!scalar(tmpl)) return false;
                map = true;
                skipSpace();
            }
            if (templates) templates->push_back(tmpl);
            if (isMap) isMap->push_back(map);
            if (i >= body.size()) break;
            if (body[i] != ',') {
                err = "expected a comma in a YAML list";
                return false;
            }
            ++i;
        }
        return true;
    }

    bool popList(std::vector<std::string>& out, std::string& err,
                 std::vector<std::string>* templates = nullptr, std::vector<bool>* isMap = nullptr) {
        out.clear();
        if (allowYaml) {
            std::string list, rest;
            int r = findYamlList(list, rest);
            if (r < 0) {
                err = "content appears to be an unterminated YAML list: \"" + line + "\"";
                return false;
            }
            if (r > 0) {
                line = trimSpace(rest);
                return parseYamlList(list, out, err, templates, isMap);
            }
        }
        size_t sp = line.find(' ');
        std::string v = line.substr(0, sp);
        line = sp == std::string::npos ? "" : trimSpace(line.substr(sp + 1));
        if (v.empty()) return true;
        size_t s = 0;
        while (true) {
            size_t c = v.find(',', s);
            out.push_back(v.substr(s, c == std::string::npos ? std::string::npos : c - s));
            if (c == std::string::npos) break;
            s = c + 1;
        }
        return true;
    }
};

bool parseBool(const std::string& v, bool& out) {
    if (v == "yes" || v == "true") { out = true; return true; }
    if (v == "no" || v == "false") { out = false; return true; }
    return false;
}

bool parseInt(const std::string& v, int& out) {
    if (v.empty()) return false;
    size_t i = (v[0] == '-' || v[0] == '+') ? 1 : 0;
    if (i >= v.size()) return false;
    long n = 0;
    for (; i < v.size(); ++i) {
        if (v[i] < '0' || v[i] > '9') return false;
        n = n * 10 + (v[i] - '0');
        if (n > 1000000000L) return false;
    }
    out = (int)(v[0] == '-' ? -n : n);
    return true;
}

void addUnique(std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

std::string guessCommentMarker(const std::string& before) {
    std::string t = trimSpace(before);
    for (const char* m : {"//", "#", "/*", "--", ";", "<!--"})
        if (startsWith(t, m)) return m;
    return "";
}

std::string joinInts(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s;
}

// An RE2 pattern as an ECMAScript one for std::regex: a leading (?i), (?s)
// or (?-s) becomes a flag, '.' matches newlines when dotall is on, named
// groups lose their names, \A and \z become ^ and $, and braces that are
// not a repetition become literal. What std::regex cannot read is an error.
bool compileRegex(const std::string& pattern, bool dotall, std::shared_ptr<Regex>& out,
                  std::string& err) {
    std::vector<std::string> names;
    std::string p = pattern;
    bool icase = false;
    if (startsWith(p, "(?")) {
        size_t close = p.find(')');
        std::string flags = close == std::string::npos ? "" : p.substr(2, close - 2);
        bool ok = !flags.empty() && flags.find_first_not_of("is-") == std::string::npos;
        if (ok) {
            bool neg = false;
            for (char f : flags) {
                if (f == '-') neg = true;
                else if (f == 'i') icase = !neg;
                else if (f == 's') dotall = !neg;
            }
            p = p.substr(close + 1);
        }
    }
    auto repetition = [&](size_t i) {   // {n}, {n,} or {n,m} at p[i]
        size_t j = i + 1, d = 0;
        while (j < p.size() && p[j] >= '0' && p[j] <= '9') { ++j; ++d; }
        if (d == 0) return false;
        if (j < p.size() && p[j] == ',') {
            ++j;
            while (j < p.size() && p[j] >= '0' && p[j] <= '9') ++j;
        }
        return j < p.size() && p[j] == '}';
    };
    std::string e;
    bool inClass = false;
    int openBraces = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        if (c == '\\' && i + 1 < p.size()) {
            char n = p[++i];
            if (!inClass && n == 'A') e += '^';
            else if (!inClass && n == 'z') e += '$';
            else if (n == 'Q' || n == 'p' || n == 'P' || n == 'C') {
                err = std::string("\\") + n;   // RE2 syntax std::regex lacks
                out.reset();
                return false;
            } else {
                e += c;
                e += n;
            }
            continue;
        }
        if (inClass) {
            e += c;
            if (c == ']') inClass = false;
            continue;
        }
        if (c == '[') {
            inClass = true;
            e += c;
            if (i + 1 < p.size() && p[i + 1] == '^') e += p[++i];
            if (i + 1 < p.size() && p[i + 1] == ']') {
                e += "\\]";
                ++i;
            }
            continue;
        }
        if (c == '.' && dotall) {
            e += "[\\s\\S]";
            continue;
        }
        if (c == '(' && i + 2 < p.size() && p[i + 1] == '?' &&
            (p[i + 2] == 'P' || (p[i + 2] == '<' && i + 3 < p.size() && p[i + 3] != '=' &&
                                 p[i + 3] != '!'))) {
            size_t lt = p.find('<', i), gt = p.find('>', i);
            if (gt == std::string::npos || lt == std::string::npos || lt > gt) {
                err = "unterminated group name";
                return false;
            }
            names.push_back(p.substr(lt + 1, gt - lt - 1));
            e += '(';
            i = gt;
            continue;
        }
        if (c == '(' && !(i + 1 < p.size() && p[i + 1] == '?')) names.push_back("");
        if (c == '{') {
            if (repetition(i)) {
                ++openBraces;
                e += c;
            } else {
                e += "\\{";
            }
            continue;
        }
        if (c == '}') {
            if (openBraces > 0) {
                --openBraces;
                e += c;
            } else {
                e += "\\}";
            }
            continue;
        }
        e += c;
    }
    try {
        auto flags = std::regex::ECMAScript;
        if (icase) flags |= std::regex::icase;
        out = std::make_shared<Regex>();
        out->re = std::regex(e, flags);
        out->names = names;
    } catch (const std::regex_error&) {
        err = "error parsing regexp: `" + pattern + "`";
        return false;
    }
    return true;
}

Options parseOptions(const std::string& commentPart, const std::string& optionText,
                     std::vector<std::string>& warnings) {
    Options o;
    OptionParser p;
    p.line = optionText;
    std::string key;
    while (true) {
        p.allowYaml = o.allowYamlLists;
        if (!p.popKey(key)) break;
        std::string err;
        auto boolOpt = [&](bool& field) {
            std::string v = p.popWord();
            bool b;
            if (parseBool(v, b)) field = b;
            else err = "unrecognized bool value \"" + v + "\"";
        };
        std::vector<std::string> list;
        if (key == "allow_yaml_lists") boolOpt(o.allowYamlLists);
        else if (key == "group") boolOpt(o.group);
        else if (key == "block") boolOpt(o.block);
        else if (key == "sticky_comments") boolOpt(o.stickyComments);
        else if (key == "case") boolOpt(o.caseSensitive);
        else if (key == "numeric") boolOpt(o.numeric);
        else if (key == "skip_lines") {
            if (p.popList(list, err)) {
                std::vector<int> ints;
                for (const std::string& s : list) {
                    int n;
                    if (parseInt(s, n)) ints.push_back(n);
                    else err = "invalid number \"" + s + "\"";
                }
                if (err.empty()) o.skipLines = ints;
            }
        } else if (key == "group_prefixes" || key == "sticky_prefixes") {
            if (p.popList(list, err)) {
                std::vector<std::string>& dst =
                    key == "group_prefixes" ? o.groupPrefixes : o.stickyPrefixes;
                dst.clear();
                for (const std::string& s : list) addUnique(dst, s);
                if (key == "group_prefixes") o.groupPrefixesSet = true;
            }
        } else if (key == "prefix_order" || key == "ignore_prefixes") {
            if (p.popList(list, err)) (key == "prefix_order" ? o.prefixOrder : o.ignorePrefixes) = list;
        } else if (key == "by_regex" || key == "group_start_regex" || key == "group_end_regex") {
            std::vector<std::string> templates;
            std::vector<bool> isMap;
            bool byRegex = key == "by_regex";
            if (p.popList(list, err, byRegex ? &templates : nullptr, byRegex ? &isMap : nullptr)) {
                std::vector<std::shared_ptr<Regex>> compiled;
                for (size_t k = 0; k < list.size(); ++k) {
                    std::shared_ptr<Regex> re;
                    if (!compileRegex(list[k], byRegex, re, err)) {
                        if (!re && err.size() == 2 && err[0] == '\\') {
                            // Valid for keep-sorted, but not something this port
                            // reads: sorting without it would disagree.
                            if (o.unsupported.empty()) o.unsupported = key + " with " + err;
                            err.clear();
                            list.clear();
                        }
                        break;
                    }
                    if (byRegex && k < isMap.size() && isMap[k]) {
                        re->hasTemplate = true;
                        re->tmpl = templates[k];
                    }
                    compiled.push_back(re);
                }
                if (err.empty()) {
                    (key == "by_regex" ? o.byRegex
                     : key == "group_start_regex" ? o.groupStartRegex
                                                  : o.groupEndRegex) = compiled;
                }
            }
        } else if (key == "order") {
            std::string v = p.popWord();
            if (v == "asc") o.desc = false;
            else if (v == "desc") o.desc = true;
            else err = "unrecognized order value \"" + v + "\", expected 'asc' or 'desc'";
        } else if (key == "newline_separated") {
            std::string v = p.popWord();
            int n;
            bool b;
            if (parseInt(v, n)) o.newlineSeparated = n;
            else if (parseBool(v, b)) o.newlineSeparated = b ? 1 : 0;
            else err = "invalid newline_separated value \"" + v + "\"";
        } else if (key == "remove_duplicates") {
            std::string v = p.popWord();
            if (v == "yes" || v == "true") o.dups = Dups::Yes;
            else if (v == "no" || v == "false") o.dups = Dups::No;
            else if (v == "keep_first_comment") o.dups = Dups::KeepFirstComment;
            else if (v == "merge_comments") o.dups = Dups::MergeComments;
            else err = "unrecognized remove_duplicates value \"" + v + "\"";
        } else {
            warnings.push_back("unrecognized option \"" + key + "\"");
            continue;
        }
        if (!err.empty()) warnings.push_back("while parsing option \"" + key + "\": " + err);
    }

    std::string cm = guessCommentMarker(commentPart);
    if (!cm.empty()) {
        o.commentMarker = cm;
        if (o.stickyComments) addUnique(o.stickyPrefixes, cm);
    }
    std::stable_sort(o.ignorePrefixes.begin(), o.ignorePrefixes.end(),
                     [](const std::string& a, const std::string& b) { return a.size() > b.size(); });

    // validate()
    if (o.skipLines.size() > 2) {
        warnings.push_back("skip_lines accepts at most two values: " + joinInts(o.skipLines));
        o.skipLines.clear();
    } else if (o.skipLines.size() == 2) {
        int a = o.skipLines[0], b = o.skipLines[1];
        if ((a > 0) == (b > 0) && (a < 0) == (b < 0)) {
            if (a < 0) {
                warnings.push_back("skip_lines has conflicting values (should one of these be "
                                   "positive, to skip lines at the start of the block instead?): " +
                                   joinInts(o.skipLines));
                o.skipLines.clear();
            } else if (a > 0) {
                warnings.push_back("skip_lines has conflicting values (should one of these be "
                                   "negative, to skip lines at the end of the block instead?): " +
                                   joinInts(o.skipLines));
                o.skipLines.clear();
            }
        }
    }
    if (o.newlineSeparated < 0) {
        warnings.push_back("newline_separated has invalid value: " +
                           std::to_string(o.newlineSeparated));
        o.newlineSeparated = 0;
    }
    if (o.groupPrefixesSet && !o.group) {
        warnings.push_back("group_prefixes may not be used with group=no");
        o.groupPrefixes.clear();
        o.groupPrefixesSet = false;
    }
    if (!o.byRegex.empty() && !o.ignorePrefixes.empty()) {
        std::string pre;
        for (const std::string& x : o.ignorePrefixes) pre += (pre.empty() ? "" : "|") + x;
        warnings.push_back("by_regex cannot be used with ignore_prefixes (consider adding a "
                           "non-capturing group to the start of your regex instead of "
                           "ignore_prefixes: \"(?:" + pre + ")\")");
        o.ignorePrefixes.clear();
    }
    if (!o.groupStartRegex.empty() && !o.groupEndRegex.empty()) {
        warnings.push_back("group_start_regex should not be used together with group_end_regex; "
                           "ignoring group_end_regex");
        o.groupEndRegex.clear();
    }
    return o;
}

// cutFirstPrefix: the first of `prefixes` that `s` starts with, after its
// leading spaces; `after` is the rest with its leading spaces trimmed too.
bool cutPrefix(const std::string& s, const std::vector<std::string>& prefixes, bool caseSensitive,
               std::string* pre, std::string* after) {
    std::string t = trimLeft(s);
    std::string tl = caseSensitive ? t : lower(t);
    for (const std::string& p : prefixes) {
        const std::string q = caseSensitive ? p : lower(p);
        if (startsWith(tl, q)) {
            if (pre) *pre = p;
            if (after) *after = trimLeft(t.size() >= p.size() ? t.substr(p.size()) : std::string());
            return true;
        }
    }
    if (after) *after = s;
    return false;
}

// ------------------------------------------------------------------ grouping

struct Group {
    std::vector<Line> comment, lines;

    std::string str() const {
        std::string s;
        bool first = true;
        for (const auto* part : {&comment, &lines})
            for (const Line& l : *part) {
                if (!first) s += '\n';
                s += l.text;
                first = false;
            }
        return s;
    }
    bool allEmpty() const {
        if (!comment.empty()) return false;
        for (const Line& l : lines)
            if (!trimSpace(l.text).empty()) return false;
        return true;
    }
};

// codeBlock: are the braces and quotes seen so far balanced?
struct CodeBlock {
    int counts[6] = {0, 0, 0, 0, 0, 0};   // { } [ ] ( )
    std::string expectedQuote;

    bool expectsContinuation() const {
        return counts[0] != counts[1] || counts[2] != counts[3] || counts[4] != counts[5] ||
               !expectedQuote.empty();
    }
    static std::string findQuote(const std::string& s, size_t i) {
        for (const char* q : {"\"\"\"", "'''", "```", "\"", "'", "`"}) {
            size_t n = std::char_traits<char>::length(q);
            if (s.size() - i < n) continue;
            if (n == 1 && i > 0 && s[i - 1] == '\\') continue;
            if (s.compare(i, n, q) == 0) return q;
        }
        return "";
    }
    void append(const std::string& s, const std::string& commentMarker) {
        static const char kBraces[] = "{}[]()";
        for (size_t i = 0; i < s.size();) {
            if (expectedQuote.empty()) {
                for (int k = 0; k < 6; ++k)
                    if (s[i] == kBraces[k]) counts[k]++;
                if (!commentMarker.empty() && s.compare(i, commentMarker.size(), commentMarker) == 0)
                    break;
            }
            std::string q = findQuote(s, i);
            if (expectedQuote.empty() && !q.empty()) {
                expectedQuote = q;
                i += q.size();
                continue;
            }
            if (!expectedQuote.empty() && q == expectedQuote) {
                expectedQuote.clear();
                i += q.size();
                continue;
            }
            ++i;
        }
    }
};

int countIndent(const std::string& s) {
    int c = 0;
    size_t i = 0;
    while (size_t n = spaceAt(s, i)) {
        i += n;
        ++c;
    }
    return i == s.size() ? -1 : c;
}

bool matchesAny(const std::string& s, const std::vector<std::shared_ptr<Regex>>& res) {
    for (const auto& re : res)
        if (std::regex_search(s, re->re)) return true;
    return false;
}

// Go's Regexp.Expand: $1, ${1}, $name and ${name} replaced by what that
// group matched, $$ by $.
std::string expand(const Regex& r, const std::smatch& m) {
    const std::string& t = r.tmpl;
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] != '$') {
            out += t[i];
            continue;
        }
        if (i + 1 < t.size() && t[i + 1] == '$') {
            out += '$';
            ++i;
            continue;
        }
        size_t s = i + 1, e;
        bool brace = s < t.size() && t[s] == '{';
        if (brace) ++s;
        e = s;
        while (e < t.size() && isWordChar((unsigned char)t[e])) ++e;
        if (e == s || (brace && (e >= t.size() || t[e] != '}'))) {
            out += '$';   // malformed: the $ is text
            continue;
        }
        std::string name = t.substr(s, e - s);
        bool number = name.find_first_not_of("0123456789") == std::string::npos;
        size_t group = 0;
        if (number) {
            group = name.size() < 6 ? (size_t)std::stoi(name) : m.size();
        } else {
            group = m.size();
            for (size_t k = 0; k < r.names.size(); ++k)
                if (r.names[k] == name) {
                    group = k + 1;
                    break;
                }
        }
        if (group < m.size() && m[group].matched) out += m.str(group);
        i = brace ? e : e - 1;
    }
    return out;
}

std::vector<Group> groupLines(const std::vector<Line>& lines, const Options& o,
                              const std::string& startDir, const std::string& endDir) {
    std::vector<Group> groups;
    struct Range {
        size_t start = 0, end = 0;
        bool init = false;
        bool empty() const { return !init || start == end; }
        size_t size() const { return init ? end - start : 0; }
        void append(size_t i) {
            if (!init) {
                start = i;
                end = i + 1;
                init = true;
            } else {
                end = i + 1;
            }
        }
    };
    Range commentRange, lineRange;
    std::vector<int> indents;
    bool haveInitial = false;
    int initialIndent = 0;
    int unmatchedStarts = 0;
    CodeBlock block;

    if (o.group) {
        indents.resize(lines.size());
        for (size_t i = 0; i < lines.size(); ++i) indents[i] = countIndent(lines[i].text);
        int indent = -1;
        for (size_t i = lines.size(); i-- > 0;) {
            if (indents[i] == -1) indents[i] = indent;
            else indent = indents[i];
        }
    }
    auto hasStickyPrefix = [&](const std::string& l) {
        return cutPrefix(l, o.stickyPrefixes, o.caseSensitive, nullptr, nullptr);
    };
    auto hasGroupPrefix = [&](const std::string& l) {
        return cutPrefix(l, o.groupPrefixes, o.caseSensitive, nullptr, nullptr);
    };
    auto shouldAddToBlock = [&] { return o.block && !lineRange.empty() && block.expectsContinuation(); };
    auto shouldAddToGroup = [&](size_t i, const std::string& l) {
        if (!o.group) return false;
        bool increased = !lineRange.empty() && haveInitial && indents[i] > initialIndent;
        return increased || unmatchedStarts > 0 || hasGroupPrefix(l);
    };
    auto countDirectives = [&](const std::string& l) {
        if (contains(l, startDir)) unmatchedStarts++;
        else if (contains(l, endDir)) unmatchedStarts--;
    };
    auto appendLine = [&](size_t i, const std::string& l) {
        lineRange.append(i);
        if (o.block) block.append(l, o.commentMarker);
        if (o.group) countDirectives(l);
        if (o.group && !haveInitial) {
            haveInitial = true;
            initialIndent = indents[i];
        }
    };
    auto slice = [&](const Range& r) {
        std::vector<Line> v;
        if (!r.empty()) v.assign(lines.begin() + (long)r.start, lines.begin() + (long)r.end);
        return v;
    };
    auto finishGroup = [&] {
        // Empty lines ending the group become groups of their own.
        std::vector<Line> trailing;
        while (lineRange.size() > 1 && lines[lineRange.end - 1].text.empty()) {
            trailing.push_back(lines[lineRange.end - 1]);
            lineRange.end--;
        }
        groups.push_back({slice(commentRange), slice(lineRange)});
        commentRange = Range();
        lineRange = Range();
        block = CodeBlock();
        for (size_t k = trailing.size(); k-- > 0;) groups.push_back({{}, {trailing[k]}});
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i].text;
        if (shouldAddToBlock() || shouldAddToGroup(i, l)) {
            appendLine(i, l);
        } else if (hasStickyPrefix(l)) {
            if (!lineRange.empty()) finishGroup();
            commentRange.append(i);
            if (o.group) countDirectives(l);
        } else if (!o.groupStartRegex.empty() && !matchesAny(l, o.groupStartRegex)) {
            appendLine(i, l);   // lines up to the next match belong to the group
        } else if (!o.groupEndRegex.empty()) {
            appendLine(i, l);
            if (matchesAny(l, o.groupEndRegex)) finishGroup();
        } else {
            if (!lineRange.empty()) finishGroup();
            appendLine(i, l);
        }
    }
    if (!commentRange.empty() || !lineRange.empty()) finishGroup();
    return groups;
}

// ---------------------------------------------------------------- comparing

std::string joinLines(const std::vector<Line>& lines) {
    std::string s, last;
    for (const Line& line : lines) {
        std::string l = trimLeft(line.text);
        if (!last.empty() && !l.empty() && isWordChar((unsigned char)last.back()) &&
            isWordChar((unsigned char)l[0]))
            s += ' ';
        s += l;
        last = l;
    }
    return s;
}

// A string split into runs of digits and of everything else, starting with
// the latter (maybe empty), so two can be compared run by run.
struct NumericTokens {
    std::vector<std::string> s;
    std::vector<std::string> n;   // digits, without leading zeros
    size_t len() const { return s.size() + n.size(); }
};

NumericTokens numericTokens(const std::string& str, bool numeric) {
    NumericTokens t;
    if (!numeric) {
        t.s.push_back(str);
        return t;
    }
    for (size_t i = 0; i < str.size();) {
        size_t j = i;
        bool digits = str[i] >= '0' && str[i] <= '9';
        while (j < str.size() && ((str[j] >= '0' && str[j] <= '9') == digits)) ++j;
        if (digits) {
            if (t.len() == 0) t.s.push_back("");
            size_t z = i;
            while (z + 1 < j && str[z] == '0') ++z;
            t.n.push_back(str.substr(z, j - z));
        } else {
            t.s.push_back(str.substr(i, j - i));
        }
        i = j;
    }
    return t;
}

int compareStrings(const std::string& a, const std::string& b) {
    int c = a.compare(b);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

int compareTokens(const NumericTokens& a, const NumericTokens& b) {
    size_t n = std::min(a.len(), b.len());
    for (size_t i = 0; i < n; ++i) {
        if (i % 2 == 0) {
            if (int c = compareStrings(a.s[i / 2], b.s[i / 2])) return c;
        } else {
            const std::string &x = a.n[i / 2], &y = b.n[i / 2];
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
            if (int c = compareStrings(x, y)) return c;
        }
    }
    return a.len() < b.len() ? -1 : a.len() > b.len() ? 1 : 0;
}

// One capture group's sort key: its prefix_order weight (only the first
// group of each regular expression has one) and its text as tokens.
struct CaptureKey {
    int weight = 0;
    NumericTokens tokens;
};

// What a group sorts by. `regex` holds one entry per by_regex pattern (just
// one, the joined lines, without by_regex); a pattern that did not match is
// empty and sorts after one that did.
struct Keyed {
    Group g;
    bool commentOnly = false;
    std::vector<std::pair<bool, std::vector<CaptureKey>>> regex;   // matched?, groups
    std::string joined, joinedComment;
};

Keyed keyed(Group g, const Options& o, const std::vector<std::string>& prefixesLongestFirst,
            const std::vector<int>& weights) {
    Keyed k;
    k.commentOnly = g.lines.empty();
    k.joined = joinLines(g.lines);
    for (size_t i = 0; i < g.comment.size(); ++i)
        k.joinedComment += (i ? "\n" : "") + g.comment[i].text;

    auto capture = [&](const std::string& raw, bool first) {
        CaptureKey c;
        if (first && !o.prefixOrder.empty()) {
            std::string pre;
            cutPrefix(raw, prefixesLongestFirst, o.caseSensitive, &pre, nullptr);
            for (size_t i = 0; i < o.prefixOrder.size(); ++i)
                if (o.prefixOrder[i] == pre) c.weight = weights[i];
        }
        std::string s;
        cutPrefix(raw, o.ignorePrefixes, o.caseSensitive, nullptr, &s);
        if (!o.caseSensitive) s = lower(s);
        c.tokens = numericTokens(s, o.numeric);
        return c;
    };

    if (o.byRegex.empty()) {
        k.regex.push_back({true, {capture(k.joined, true)}});
    } else {
        // The lines without their leading spaces, joined with real newlines.
        std::string text;
        for (size_t i = 0; i < g.lines.size(); ++i)
            text += (i ? "\n" : "") + trimLeft(g.lines[i].text);
        for (const auto& re : o.byRegex) {
            std::smatch m;
            if (!std::regex_search(text, m, re->re)) {
                k.regex.push_back({false, {}});
                continue;
            }
            std::vector<std::string> parts;
            if (re->hasTemplate) {
                // Every match, each expanded, end to end.
                std::string all;
                for (std::sregex_iterator it(text.begin(), text.end(), re->re), stop; it != stop; ++it)
                    all += expand(*re, *it);
                parts.push_back(all);
            } else if (m.size() == 1) parts.push_back(m.str(0));
            else
                for (size_t i = 1; i < m.size(); ++i) parts.push_back(m.str(i));
            std::vector<CaptureKey> keys;
            for (size_t j = 0; j < parts.size(); ++j) {
                // A match over several lines is joined the way lines are.
                std::vector<Line> pieces;
                size_t from = 0;
                while (true) {
                    size_t nl = parts[j].find('\n', from);
                    pieces.push_back({parts[j].substr(from, nl == std::string::npos ? std::string::npos
                                                                                   : nl - from), -1});
                    if (nl == std::string::npos) break;
                    from = nl + 1;
                }
                keys.push_back(capture(joinLines(pieces), j == 0));
            }
            k.regex.push_back({true, std::move(keys)});
        }
    }
    k.g = std::move(g);
    return k;
}

int compareCaptures(const std::vector<CaptureKey>& a, const std::vector<CaptureKey>& b) {
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (a[i].weight != b[i].weight) return a[i].weight < b[i].weight ? -1 : 1;
        if (int c = compareTokens(a[i].tokens, b[i].tokens)) return c;
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

int compareKeyed(const Keyed& a, const Keyed& b) {
    if (a.commentOnly != b.commentOnly) return a.commentOnly ? 1 : -1;
    for (size_t i = 0; i < std::min(a.regex.size(), b.regex.size()); ++i) {
        const auto &x = a.regex[i], &y = b.regex[i];
        if (x.first != y.first) return x.first ? -1 : 1;
        if (int c = compareCaptures(x.second, y.second)) return c;
    }
    if (a.regex.size() != b.regex.size()) return a.regex.size() < b.regex.size() ? -1 : 1;
    if (int c = compareStrings(a.joined, b.joined)) return c;
    return compareStrings(a.joinedComment, b.joinedComment);
}

bool lastLineEndsWith(const Group& g, const std::string& s) {
    return !g.lines.empty() && endsWith(g.lines.back().text, s);
}

// ----------------------------------------------------------------- blocks

struct Block {
    int startDir = 0, endDir = 0;   // the marker lines
    int start = 0, end = 0;         // the sorted lines are (start, end)
    Options opts;
    std::vector<Block> nested;
};

struct Found {
    std::vector<Block> blocks;           // top level, nested inside
    std::vector<Message> errors, warnings;
    int count = 0;
};

Found findBlocks(const std::vector<Line>& lines, const std::string& id) {
    const std::string startDir = id + " start", endDir = id + " end";
    Found f;
    std::vector<int> starts;
    std::vector<std::vector<Block>> nestedByDepth;
    std::vector<Message> incomplete;
    for (int i = 0; i < (int)lines.size(); ++i) {
        const std::string& l = lines[(size_t)i].text;
        if (contains(l, startDir)) {
            starts.push_back(i);
            continue;
        }
        if (!contains(l, endDir)) continue;
        if (starts.empty()) {
            incomplete.push_back({i, "this \"" + endDir + "\" has no \"" + startDir + "\" before it"});
            continue;
        }
        int start = starts.back();
        starts.pop_back();
        int endIndex = i;
        while (endIndex > start && trimSpace(lines[(size_t)endIndex - 1].text).empty()) --endIndex;

        const std::string& sl = lines[(size_t)start].text;
        size_t at = sl.find(startDir);
        std::vector<std::string> warns;
        Options opts = parseOptions(sl.substr(0, at), sl.substr(at + startDir.size()), warns);
        for (const std::string& w : warns) f.warnings.push_back({start, w});

        Block b;
        b.startDir = start;
        b.endDir = i;
        b.opts = opts;
        b.start = start + opts.startOffset();
        b.end = endIndex + opts.endOffset();
        f.count++;
        if (b.start >= b.end) {
            f.warnings.push_back({start, "block start is at or after end, possibly due to skip_lines"});
            continue;
        }
        size_t depth = starts.size();
        if (nestedByDepth.size() == depth + 1) {
            b.nested = std::move(nestedByDepth[depth]);
            nestedByDepth.resize(depth);
        }
        if (depth == 0) {
            f.blocks.push_back(std::move(b));
        } else {
            while (nestedByDepth.size() < depth) nestedByDepth.emplace_back();
            nestedByDepth[depth - 1].push_back(std::move(b));
        }
    }
    for (int s : starts)
        incomplete.push_back({s, "this \"" + startDir + "\" has no \"" + endDir + "\" after it"});
    std::sort(incomplete.begin(), incomplete.end(),
              [](const Message& a, const Message& b) { return a.line < b.line; });
    for (Message& m : incomplete) {
        m.text += "; nothing in the file is sorted until it has one";
        f.errors.push_back(m);
    }
    return f;
}

struct Sorted {
    std::vector<Line> lines;
    bool already = true;
};

// Would sorting these groups take a nested block apart?
bool splitsNested(const std::vector<Group>& groups, const std::string& startDir,
                  const std::string& endDir) {
    for (const Group& g : groups) {
        int depth = 0;
        for (const auto* part : {&g.comment, &g.lines})
            for (const Line& l : *part) {
                if (contains(l.text, startDir)) ++depth;
                else if (contains(l.text, endDir) && --depth < 0) return true;
            }
        if (depth != 0) return true;
    }
    return false;
}

Sorted sortBlock(const Block& b, const std::vector<Line>& file, const std::string& id,
                 std::vector<Message>& errors) {
    const std::string startDir = id + " start", endDir = id + " end";
    Sorted out;
    std::vector<Sorted> nestedResults;
    for (const Block& n : b.nested) {
        nestedResults.push_back(sortBlock(n, file, id, errors));
        if (!nestedResults.back().already) out.already = false;
    }
    std::vector<Line> lines(file.begin() + b.start + 1, file.begin() + b.end);
    if (!out.already) {
        std::vector<Line> joined;
        size_t cursor = 0;
        for (size_t i = 0; i < b.nested.size(); ++i) {
            if (nestedResults[i].already) continue;
            const Block& n = b.nested[i];
            long offset = n.start - b.start;
            size_t len = (size_t)(n.end - n.start - 1);
            if (offset < (long)cursor || (size_t)offset + len > lines.size()) {
                errors.push_back({n.startDir, "this block overlaps the skipped lines of the block "
                                              "around it; left as it is"});
                continue;
            }
            joined.insert(joined.end(), lines.begin() + (long)cursor, lines.begin() + offset);
            joined.insert(joined.end(), nestedResults[i].lines.begin(), nestedResults[i].lines.end());
            cursor = (size_t)offset + len;
        }
        joined.insert(joined.end(), lines.begin() + (long)cursor, lines.end());
        lines = std::move(joined);
    }
    out.lines = lines;
    const Options& o = b.opts;
    if (!o.unsupported.empty()) {
        errors.push_back({b.startDir, o.unsupported + " is not supported here, so this block is "
                                      "left as it is"});
        return out;
    }

    std::vector<Group> groups = groupLines(lines, o, startDir, endDir);
    if (!b.nested.empty() && splitsNested(groups, startDir, endDir)) {
        errors.push_back({b.startDir, "sorting this block would split a keep-sorted block nested "
                                      "in it, so it is left as it is (group=yes or block=yes "
                                      "keeps it together)"});
        return out;
    }

    // All but the last item end in a comma: give the last one too while
    // sorting, and take it from whichever comes last afterwards.
    bool trimComma = false;
    {
        std::vector<Group*> data;
        for (Group& g : groups)
            if (!g.lines.empty()) data.push_back(&g);
        size_t n = data.size();
        if (n > 1 && !lastLineEndsWith(*data[n - 1], ",")) {
            bool all = true;
            for (size_t i = 0; i + 1 < n; ++i)
                if (!lastLineEndsWith(*data[i], ",")) all = false;
            if (all) {
                data[n - 1]->lines.back().text += ",";
                trimComma = true;
            }
        }
    }

    const int newlines = o.newlineSeparated;
    bool wasSeparated = true;
    if (newlines > 0) {
        // isNewlineSeparated
        if (!groups.empty()) {
            if (groups.back().allEmpty()) {
                wasSeparated = false;
            } else {
                size_t i = 0;
                while (i < groups.size()) {
                    if (groups[i].allEmpty()) { wasSeparated = false; break; }
                    ++i;
                    if (i == groups.size()) break;
                    int empties = 0;
                    while (i < groups.size() && groups[i].allEmpty()) {
                        empties += (int)groups[i].lines.size();
                        ++i;
                    }
                    if (empties != newlines) { wasSeparated = false; break; }
                }
            }
        }
        std::vector<Group> kept;
        for (Group& g : groups)
            if (!g.allEmpty()) kept.push_back(std::move(g));
        groups = std::move(kept);
    }

    bool removed = false;
    if (o.dups != Dups::No) {
        std::vector<Group> deduped;
        std::set<std::string> seen;
        std::vector<std::string> codeKeys;   // parallel to deduped
        std::vector<std::set<std::string>> commentsSeen;
        for (Group& g : groups) {
            if (o.dups == Dups::Yes) {
                if (seen.insert(g.str()).second) deduped.push_back(std::move(g));
                else removed = true;
                continue;
            }
            std::string code;
            for (size_t i = 0; i < g.lines.size(); ++i) code += (i ? "\n" : "") + g.lines[i].text;
            auto it = std::find(codeKeys.begin(), codeKeys.end(), code);
            std::string commentKey;
            for (size_t i = 0; i < g.comment.size(); ++i)
                commentKey += (i ? "\n" : "") + g.comment[i].text;
            if (it == codeKeys.end()) {
                codeKeys.push_back(code);
                commentsSeen.emplace_back();
                if (!g.comment.empty()) commentsSeen.back().insert(commentKey);
                deduped.push_back(std::move(g));
                continue;
            }
            removed = true;
            size_t k = (size_t)(it - codeKeys.begin());
            Group& firstG = deduped[k];
            if (o.dups == Dups::MergeComments) {
                if (!g.comment.empty() && commentsSeen[k].insert(commentKey).second)
                    firstG.comment.insert(firstG.comment.end(), g.comment.begin(), g.comment.end());
            } else if (firstG.comment.empty() && !g.comment.empty()) {
                firstG.comment = g.comment;
            }
        }
        groups = std::move(deduped);
    }

    // Sort keys.
    std::vector<std::string> prefixes = o.prefixOrder;
    std::stable_sort(prefixes.begin(), prefixes.end(),
                     [](const std::string& x, const std::string& y) { return x.size() > y.size(); });
    std::vector<int> weights(o.prefixOrder.size());
    for (size_t i = 0; i < o.prefixOrder.size(); ++i) {
        // A prefix given twice takes its last position, as in a Go map.
        weights[i] = (int)i - (int)o.prefixOrder.size();
        for (size_t j = i + 1; j < o.prefixOrder.size(); ++j)
            if (o.prefixOrder[j] == o.prefixOrder[i]) weights[i] = (int)j - (int)o.prefixOrder.size();
    }
    std::vector<Keyed> items;
    items.reserve(groups.size());
    for (Group& g : groups) items.push_back(keyed(std::move(g), o, prefixes, weights));
    auto cmp = [&](const Keyed& x, const Keyed& y) {
        return o.desc ? compareKeyed(y, x) : compareKeyed(x, y);
    };

    bool isSorted = true;
    for (size_t i = 1; i < items.size(); ++i)
        if (cmp(items[i], items[i - 1]) < 0) {
            isSorted = false;
            break;
        }
    if (out.already && wasSeparated && !removed && isSorted) return out;

    std::stable_sort(items.begin(), items.end(),
                     [&](const Keyed& x, const Keyed& y) { return cmp(x, y) < 0; });
    if (trimComma) {
        for (size_t i = items.size(); i-- > 0;) {
            if (items[i].g.lines.empty()) continue;
            std::string& t = items[i].g.lines.back().text;
            if (endsWith(t, ",")) t.pop_back();
            break;
        }
    }
    std::vector<Line> result;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0 && newlines > 0)
            for (int k = 0; k < newlines; ++k) result.push_back({"", -1});
        for (const auto* part : {&items[i].g.comment, &items[i].g.lines})
            result.insert(result.end(), part->begin(), part->end());
    }
    out.lines = std::move(result);
    out.already = false;
    return out;
}

// ------------------------------------------------------------- whole files

struct File {
    std::vector<Line> lines;
    std::string ending;
};

File split(const std::string& text) {
    File f;
    f.ending = "\n";
    if (contains(text, "\r\n")) f.ending = "\r\n";
    else if (contains(text, "\r")) f.ending = "\r";
    size_t s = 0;
    int n = 0;
    while (true) {
        size_t e = text.find(f.ending, s);
        f.lines.push_back({text.substr(s, e == std::string::npos ? std::string::npos : e - s), n++});
        if (e == std::string::npos) break;
        s = e + f.ending.size();
    }
    return f;
}

Result finish(const std::string& input, const File& f, std::vector<Line> lines) {
    Result r;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) r.text += f.ending;
        r.text += lines[i].text;
        r.origin.push_back(lines[i].origin);
    }
    r.changed = r.text != input;
    return r;
}

Result unchanged(const std::string& input, const File& f) {
    return finish(input, f, f.lines);
}

// keep-sorted writes a block's new lines each with its newline after it, so
// an outermost block sorted down to nothing (all its lines blank, under
// newline_separated) still leaves one empty line. A nested one leaves none.
void keepOneLine(Sorted& s) {
    if (s.lines.empty()) s.lines.push_back({"", -1});
}

const Block* innermost(const std::vector<Block>& blocks, int line) {
    for (const Block& b : blocks) {
        if (line < b.startDir || line > b.endDir) continue;
        const Block* inner = innermost(b.nested, line);
        return inner ? inner : &b;
    }
    return nullptr;
}

}  // namespace

Result fixAll(const std::string& text, const std::string& id) {
    File f = split(text);
    Found found = findBlocks(f.lines, id);
    if (!found.errors.empty()) {
        Result r = unchanged(text, f);
        r.errors = found.errors;
        r.warnings = found.warnings;
        r.blocks = found.count;
        return r;
    }
    std::vector<Message> errors;
    std::vector<Line> lines = f.lines;
    std::vector<const Block*> order;
    for (const Block& b : found.blocks) order.push_back(&b);
    std::sort(order.begin(), order.end(), [](const Block* a, const Block* b) { return a->start > b->start; });
    for (const Block* b : order) {
        Sorted s = sortBlock(*b, f.lines, id, errors);
        if (s.already) continue;
        keepOneLine(s);
        lines.erase(lines.begin() + b->start + 1, lines.begin() + b->end);
        lines.insert(lines.begin() + b->start + 1, s.lines.begin(), s.lines.end());
    }
    Result r = finish(text, f, std::move(lines));
    std::sort(errors.begin(), errors.end(),
              [](const Message& a, const Message& b) { return a.line < b.line; });
    r.errors = errors;
    r.warnings = found.warnings;
    r.blocks = found.count;
    return r;
}

Result fixAt(const std::string& text, int line, const std::string& id) {
    File f = split(text);
    Found found = findBlocks(f.lines, id);
    Result r;
    if (!found.errors.empty()) {
        r = unchanged(text, f);
        r.errors = found.errors;
        r.warnings = found.warnings;
        r.blocks = found.count;
        return r;
    }
    const Block* b = innermost(found.blocks, line);
    if (!b) {
        r = unchanged(text, f);
        r.blocks = found.count;
        r.warnings = found.warnings;
        r.errors.push_back({line, "not in a keep-sorted block (select the lines to sort them)"});
        return r;
    }
    std::vector<Message> errors;
    Sorted s = sortBlock(*b, f.lines, id, errors);
    std::vector<Line> lines = f.lines;
    if (!s.already) {
        keepOneLine(s);
        lines.erase(lines.begin() + b->start + 1, lines.begin() + b->end);
        lines.insert(lines.begin() + b->start + 1, s.lines.begin(), s.lines.end());
    }
    r = finish(text, f, std::move(lines));
    r.errors = errors;
    for (const Message& w : found.warnings)
        if (w.line >= b->startDir && w.line <= b->endDir) r.warnings.push_back(w);
    r.blocks = found.count;
    return r;
}

Result sortLines(const std::string& text, int first, int last) {
    File f = split(text);
    std::vector<Line> lines = f.lines;
    first = std::max(0, first);
    last = std::min(last, (int)lines.size() - 1);
    if (first < last) {
        std::stable_sort(lines.begin() + first, lines.begin() + last + 1,
                         [](const Line& a, const Line& b) {
                             int c = compareStrings(trimLeft(a.text), trimLeft(b.text));
                             if (c) return c < 0;
                             return a.text < b.text;
                         });
    }
    return finish(text, f, std::move(lines));
}

// ------------------------------------------------------------ editor side

namespace {

std::string toUtf8(const std::u16string& u) {
    std::string s;
    s.reserve(u.size());
    for (size_t i = 0; i < u.size(); ++i) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            s += (char)cp;
        } else if (cp < 0x800) {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xF0 | (cp >> 18));
            s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        }
    }
    return s;
}

std::u16string toUtf16(const std::string& s) {
    std::u16string u;
    u.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; len = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (size_t k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u += (char16_t)(0xD800 + (cp >> 10));
            u += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            u += (char16_t)cp;
        }
    }
    return u;
}

// Where each line starts, in UTF-16 units, the way split() reads lines.
std::vector<size_t> lineStarts(const std::u16string& t) {
    std::u16string ending = u"\n";
    if (t.find(u"\r\n") != std::u16string::npos) ending = u"\r\n";
    else if (t.find(u'\r') != std::u16string::npos) ending = u"\r";
    std::vector<size_t> starts{0};
    size_t s = 0;
    while (true) {
        size_t e = t.find(ending, s);
        if (e == std::u16string::npos) break;
        s = e + ending.size();
        starts.push_back(s);
    }
    return starts;
}

size_t lineOf(const std::vector<size_t>& starts, size_t pos) {
    return (size_t)(std::upper_bound(starts.begin(), starts.end(), pos) - starts.begin()) - 1;
}

size_t lineContentEnd(const std::u16string& t, const std::vector<size_t>& starts, size_t line) {
    if (line + 1 < starts.size()) {
        size_t e = starts[line + 1];
        while (e > starts[line] && (t[e - 1] == u'\n' || t[e - 1] == u'\r')) --e;
        return e;
    }
    return t.size();
}

std::string describe(const std::vector<Message>& errors) {
    std::string s;
    size_t shown = std::min<size_t>(errors.size(), 3);
    for (size_t i = 0; i < shown; ++i) {
        if (i) s += "; ";
        s += "line " + std::to_string(errors[i].line + 1) + ": " + errors[i].text;
    }
    if (errors.size() > shown) s += " (and " + std::to_string(errors.size() - shown) + " more)";
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

// The result as one replacement, with each end of the selection kept on the
// line it was on.
Edit toEdit(const std::u16string& old, size_t a, size_t b, const Result& r, bool selectLines,
            size_t firstLine, size_t lastLine, bool endAtLineStart) {
    Edit e;
    e.error = !r.errors.empty();
    e.message = describe(r.errors);
    if (!r.changed) {
        e.selStart = a;
        e.selEnd = b;
        return e;
    }
    std::u16string now = toUtf16(r.text);
    size_t p = 0, maxP = std::min(old.size(), now.size());
    while (p < maxP && old[p] == now[p]) ++p;
    size_t s = 0;
    while (s < maxP - p && old[old.size() - 1 - s] == now[now.size() - 1 - s]) ++s;
    e.changed = true;
    e.replaceStart = p;
    e.replaceLength = old.size() - p - s;
    e.replacement = now.substr(p, now.size() - p - s);

    std::vector<size_t> oldStarts = lineStarts(old), newStarts = lineStarts(now);
    if (selectLines) {
        e.selStart = newStarts[std::min(firstLine, newStarts.size() - 1)];
        e.selEnd = endAtLineStart && lastLine + 1 < newStarts.size()
                       ? newStarts[lastLine + 1]
                       : lineContentEnd(now, newStarts, std::min(lastLine, newStarts.size() - 1));
        return e;
    }
    auto follow = [&](size_t pos) {
        size_t line = lineOf(oldStarts, pos);
        size_t col = pos - oldStarts[line];
        size_t to = std::min(line, newStarts.size() - 1);
        for (size_t i = 0; i < r.origin.size(); ++i)
            if (r.origin[i] == (int)line) {
                to = i;
                break;
            }
        size_t end = lineContentEnd(now, newStarts, to);
        return std::min(newStarts[to] + col, end);
    };
    e.selStart = follow(a);
    e.selEnd = a == b ? e.selStart : follow(b);
    if (e.selStart > e.selEnd) std::swap(e.selStart, e.selEnd);   // the lines swapped places
    return e;
}

}  // namespace

Edit sortCommand(const std::u16string& text, size_t a, size_t b) {
    if (a > b) std::swap(a, b);
    a = std::min(a, text.size());
    b = std::min(b, text.size());
    std::vector<size_t> starts = lineStarts(text);
    size_t first = lineOf(starts, a);
    size_t last = lineOf(starts, b);
    bool endAtLineStart = b > a && last > first && starts[last] == b;
    if (endAtLineStart) --last;
    const std::string utf8 = toUtf8(text);
    if (last > first) {
        Result r = sortLines(utf8, (int)first, (int)last);
        return toEdit(text, a, b, r, true, first, last, endAtLineStart);
    }
    Result r = fixAt(utf8, (int)first);
    return toEdit(text, a, b, r, false, 0, 0, false);
}

Edit fixCommand(const std::u16string& text, size_t a, size_t b) {
    if (a > b) std::swap(a, b);
    a = std::min(a, text.size());
    b = std::min(b, text.size());
    Result r = fixAll(toUtf8(text));
    Edit e = toEdit(text, a, b, r, false, 0, 0, false);
    if (r.blocks == 0 && e.message.empty()) e.message = "No keep-sorted blocks in this file";
    return e;
}

bool hasBlocks(const std::u16string& text) {
    return text.find(u"keep-sorted start") != std::u16string::npos;
}

}  // namespace KeepSorted
