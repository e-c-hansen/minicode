// LspFeatures.cpp — see LspClient.h. Results of the requests that read
// the code: hover, folding ranges, semantic tokens, locations (definition,
// references grouped by file), and document and workspace symbols with
// their kinds' names, glyphs and colors.
#include "LspClient.h"
#include "LspInternal.h"
#include "SyntaxHighlighter.h"
#include <algorithm>
#include <set>
#include <tuple>

namespace Lsp {

namespace {

// A Markdown horizontal rule: three or more of one of - * _, spaces allowed.
bool isRuleLine(const std::string& line) {
    std::string t = trim(line);
    if (t.empty() || (t[0] != '-' && t[0] != '*' && t[0] != '_')) return false;
    int n = 0;
    for (char c : t) {
        if (c == t[0]) n++;
        else if (c != ' ' && c != '\t') return false;
    }
    return n >= 3;
}

// Markdown to readable text: keep code fence contents, drop the fence lines
// and rules (pyright puts one between the signature and the docs), and
// unescape the backslash escapes servers put before punctuation.
std::string markdownToText(const std::string& md) {
    std::string out, line;
    bool inFence = false;
    for (size_t i = 0; i <= md.size(); i++) {
        if (i < md.size() && md[i] != '\n') { line += md[i]; continue; }
        bool fence = trim(line).compare(0, 3, "```") == 0;
        if (fence) inFence = !inFence;
        if (!fence && !inFence && isRuleLine(line)) {
            out += '\n';
        } else if (!fence) {
            std::string un;
            for (size_t k = 0; k < line.size(); k++) {
                if (line[k] == '\\' && k + 1 < line.size() &&
                    std::ispunct((unsigned char)line[k + 1])) {
                    un += line[++k];
                } else {
                    un += line[k];
                }
            }
            out += un;
            out += '\n';
        }
        line.clear();
    }
    return out;
}

std::string markedToText(const Json& j) {
    if (j.isString()) return markdownToText(j.asString());
    if (j.isObject()) {
        if (j.has("kind") && j["kind"].asString() == "markdown")
            return markdownToText(j["value"].asString());
        return j["value"].asString();   // plaintext, or {language, value}
    }
    return "";
}

}  // namespace

std::string parseHover(const Json& result) {
    const Json& contents = result["contents"];
    std::string text;
    if (contents.isArray()) {
        for (const Json& part : contents.elements()) {
            std::string t = trim(markedToText(part));
            if (t.empty()) continue;
            if (!text.empty()) text += "\n\n";
            text += t;
        }
    } else {
        text = markedToText(contents);
    }
    // Collapse runs of blank lines, then trim.
    std::string out;
    int newlines = 0;
    for (char c : text) {
        if (c == '\n') { if (++newlines > 2) continue; }
        else newlines = 0;
        out += c;
    }
    return trim(out);
}

HoverDoc parseHoverDoc(const Json& result) {
    HoverDoc doc;
    if (!result.isObject()) return doc;
    auto add = [&](const Json& j) {
        HoverDoc::Part part;
        if (j.isString()) {   // a MarkedString: Markdown
            part.text = j.asString();
            part.markdown = true;
        } else if (j.isObject() && j.has("language")) {
            std::string value = trim(j["value"].asString());
            if (!value.empty())
                part.text = "```" + j["language"].asString() + "\n" + value + "\n```";
            part.markdown = true;
        } else if (!parseMarkup(j, part.text, part.markdown)) {
            return;
        }
        part.text = trim(part.text);
        if (!part.text.empty()) doc.parts.push_back(part);
    };
    const Json& contents = result["contents"];
    if (contents.isArray()) {
        for (const Json& part : contents.elements()) add(part);
    } else {
        add(contents);
    }
    if (result["range"].isObject()) {
        doc.hasRange = true;
        doc.range = parseRange(result["range"]);
    }
    return doc;
}

std::vector<FoldingRange> parseFoldingRanges(const Json& result) {
    std::vector<FoldingRange> out;
    if (!result.isArray()) return out;
    for (size_t i = 0; i < result.size(); i++) {
        const Json& r = result[i];
        if (!r["startLine"].isNumber() || !r["endLine"].isNumber()) continue;
        const double s = r["startLine"].asNumber(), e = r["endLine"].asNumber();
        if (s < 0 || e <= s || s > 1e9 || e > 1e9 || s != (double)(long)s || e != (double)(long)e)
            continue;
        FoldingRange f;
        f.startLine = (int)s;
        f.endLine = (int)e;
        if (r["kind"].isString()) f.kind = r["kind"].asString();
        out.push_back(f);
    }
    return out;
}

// ------------------------------------------------------------ semantic tokens
SemanticStyle semanticStyle(const std::string& type, bool readonly) {
    static const std::map<std::string, SemanticStyle> kStyles = {
        {"namespace", SemanticStyle::Class},     {"type", SemanticStyle::Class},
        {"class", SemanticStyle::Class},         {"enum", SemanticStyle::Class},
        {"interface", SemanticStyle::Class},     {"struct", SemanticStyle::Class},
        {"typeParameter", SemanticStyle::Class}, {"function", SemanticStyle::Function},
        {"method", SemanticStyle::Function},     {"member", SemanticStyle::Function},
        {"decorator", SemanticStyle::Function},  {"variable", SemanticStyle::Variable},
        {"event", SemanticStyle::Variable},      {"parameter", SemanticStyle::Parameter},
        {"property", SemanticStyle::Property},   {"enumMember", SemanticStyle::Constant},
    };
    auto it = kStyles.find(type);
    if (it == kStyles.end()) return SemanticStyle::None;
    // variable.readonly probes variable.other.constant, property.readonly
    // variable.other.constant.property: the themes' constant color.
    if (readonly && (it->second == SemanticStyle::Variable || it->second == SemanticStyle::Property))
        return SemanticStyle::Constant;
    return it->second;
}

SemanticLegend SemanticLegend::from(const Json& caps) {
    SemanticLegend l;
    const Json& legend = caps["semanticTokensProvider"]["legend"];
    for (const Json& t : legend["tokenTypes"].elements()) l.types.push_back(t.asString());
    for (const Json& m : legend["tokenModifiers"].elements()) l.modifiers.push_back(m.asString());
    for (size_t i = 0; i < l.modifiers.size() && i < 32; i++)
        if (l.modifiers[i] == "readonly") l.readonlyBit = 1u << i;
    l.styles.reserve(l.types.size());
    for (const std::string& t : l.types) l.styles.push_back(semanticStyle(t, false));
    return l;
}

bool offersSemanticTokens(const Json& caps) {
    const Json& p = caps["semanticTokensProvider"];
    if (!p.isObject() || !p["legend"]["tokenTypes"].isArray()) return false;
    return p["full"].isObject() || p["full"].asBool();
}

bool offersSemanticTokensDelta(const Json& caps) {
    return offersSemanticTokens(caps) &&
           caps["semanticTokensProvider"]["full"]["delta"].asBool();
}

namespace {
// A JSON array of whole numbers from 0 to 2^32 - 1, or false.
bool readUints(const Json& j, std::vector<uint32_t>& out) {
    if (!j.isArray()) return false;
    std::vector<uint32_t> got;
    got.reserve(j.size());
    for (const Json& n : j.elements()) {
        if (!n.isNumber()) return false;
        const double v = n.asNumber();
        if (v < 0 || v > 4294967295.0 || v != (double)(uint32_t)v) return false;
        got.push_back((uint32_t)v);
    }
    out.swap(got);
    return true;
}
}  // namespace

bool applySemanticEdits(std::vector<uint32_t>& data, const Json& edits) {
    if (!edits.isArray()) return false;
    struct Edit {
        size_t start, deleteCount;
        std::vector<uint32_t> insert;
    };
    std::vector<Edit> list;
    for (const Json& e : edits.elements()) {
        const double start = e["start"].asNumber(-1), count = e["deleteCount"].asNumber(-1);
        if (start < 0 || count < 0 || start > 1e15 || count > 1e15 ||
            start != (double)(size_t)start || count != (double)(size_t)count)
            return false;
        Edit one{(size_t)start, (size_t)count, {}};
        if (e.has("data") && !readUints(e["data"], one.insert)) return false;
        if (one.start > data.size() || one.deleteCount > data.size() - one.start) return false;
        list.push_back(std::move(one));
    }
    std::stable_sort(list.begin(), list.end(),
                     [](const Edit& a, const Edit& b) { return a.start < b.start; });
    for (size_t i = 1; i < list.size(); i++)
        if (list[i].start < list[i - 1].start + list[i - 1].deleteCount) return false;
    // One pass from the front, copying the untouched stretches between.
    std::vector<uint32_t> out;
    out.reserve(data.size());
    size_t at = 0;
    for (const Edit& e : list) {
        out.insert(out.end(), data.begin() + (ptrdiff_t)at, data.begin() + (ptrdiff_t)e.start);
        out.insert(out.end(), e.insert.begin(), e.insert.end());
        at = e.start + e.deleteCount;
    }
    out.insert(out.end(), data.begin() + (ptrdiff_t)at, data.end());
    data.swap(out);
    return true;
}

bool SemanticTokens::apply(const Json& result) {
    bool ok = false;
    if (result.isObject() && result.has("data")) ok = readUints(result["data"], data);
    else if (result.isObject() && result.has("edits")) ok = applySemanticEdits(data, result["edits"]);
    if (!ok || data.size() % 5 != 0) {
        resultId.clear();
        data.clear();
        return false;
    }
    resultId = result["resultId"].asString();
    return true;
}

std::vector<SemanticSpan> decodeSemanticTokens(const std::vector<uint32_t>& data,
                                               const SemanticLegend& legend,
                                               const PositionIndex& index) {
    std::vector<SemanticSpan> out;
    const size_t textLength = index.text().size();
    const int64_t kMax = 0x7FFFFFFF;
    int64_t line = 0, start = 0;
    size_t lastEnd = 0;
    for (size_t i = 0; i + 5 <= data.size(); i += 5) {
        const uint32_t dLine = data[i], dStart = data[i + 1], length = data[i + 2];
        const uint32_t type = data[i + 3], mods = data[i + 4];
        line += dLine;
        start = dLine ? (int64_t)dStart : start + dStart;
        if (line >= (int64_t)index.lineCount() || line > kMax) break;
        if (type >= legend.styles.size() || length == 0 || start > kMax) continue;
        SemanticStyle st = legend.styles[type];
        if (legend.readonlyBit && (mods & legend.readonlyBit))
            st = semanticStyle(legend.types[type], true);
        if (st == SemanticStyle::None) continue;
        const size_t a = index.offset(Position{(int)line, (int)start});
        const size_t b = index.offset(Position{(int)line, (int)std::min<int64_t>(start + length, kMax)});
        if (b <= a || a >= textLength || b > 0xFFFFFFFFu) continue;
        if (!out.empty() && a < lastEnd) continue;
        out.push_back({(uint32_t)a, (uint32_t)(b - a), st});
        lastEnd = b;
    }
    return out;
}

std::vector<SemanticSpan> semanticSpansInUtf8(const std::u16string& text,
                                             const std::vector<SemanticSpan>& spans,
                                             size_t* utf8Length) {
    std::vector<SemanticSpan> out;
    out.reserve(spans.size());
    size_t unit = 0, byte = 0;
    // Forward to UTF-16 offset `to` (never past the text), counting bytes;
    // a surrogate pair is crossed whole or not at all.
    auto advance = [&](size_t to) {
        to = std::min(to, text.size());
        while (unit < to) {
            const char16_t c = text[unit];
            if (c >= 0xD800 && c < 0xDC00 && unit + 1 < text.size() && text[unit + 1] >= 0xDC00 &&
                text[unit + 1] < 0xE000) {
                if (unit + 2 > to) break;
                byte += 4;
                unit += 2;
                continue;
            }
            byte += c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
            unit++;
        }
        return byte;
    };
    for (const SemanticSpan& s : spans) {
        if (s.start < unit) continue;   // not ascending: left out
        const size_t a = advance(s.start);
        const size_t b = advance((size_t)s.start + s.length);
        if (b > a && b <= 0xFFFFFFFFu) out.push_back({(uint32_t)a, (uint32_t)(b - a), s.style});
    }
    if (utf8Length) *utf8Length = advance(text.size());
    return out;
}

std::vector<SemanticSpan> semanticSpansOverCode(const std::vector<SemanticSpan>& spans,
                                               const std::vector<Token>& tokens) {
    std::vector<SemanticSpan> out;
    out.reserve(spans.size());
    size_t first = 0;   // the first token that may reach past where a span starts
    for (const SemanticSpan& s : spans) {
        const size_t end = (size_t)s.start + s.length;
        size_t at = s.start;
        while (first < tokens.size() && tokens[first].start + tokens[first].length <= s.start)
            first++;
        for (size_t k = first; k < tokens.size() && tokens[k].start < end; k++) {
            const Token& t = tokens[k];
            if (!isStringStyle(t.style) && t.style != TokenStyle::Comment) continue;
            const size_t a = std::max(at, t.start), b = std::min(end, t.start + t.length);
            if (b <= a) continue;
            if (a > at) out.push_back({(uint32_t)at, (uint32_t)(a - at), s.style});
            at = b;
        }
        if (end > at) out.push_back({(uint32_t)at, (uint32_t)(end - at), s.style});
    }
    return out;
}

std::vector<Location> parseLocations(const Json& result) {
    std::vector<Location> out;
    auto one = [&](const Json& j) {
        if (!j.isObject()) return;
        Location l;
        if (j.has("targetUri")) {   // LocationLink
            l.uri = j["targetUri"].asString();
            l.range = parseRange(j.has("targetSelectionRange")
                                     ? j["targetSelectionRange"] : j["targetRange"]);
        } else {
            l.uri = j["uri"].asString();
            l.range = parseRange(j["range"]);
        }
        if (!l.uri.empty()) out.push_back(l);
    };
    if (result.isArray()) for (const Json& j : result.elements()) one(j);
    else one(result);
    return out;
}

// ------------------------------------------------------------- symbols
namespace {
bool rangeHolds(const Range& r, Position p) {
    return !positionBefore(p, r.start) && !positionBefore(r.end, p);
}

void addDocumentSymbol(const Json& j, const std::string& container, int depth,
                       std::vector<Symbol>& out) {
    if (!j.isObject() || !j["name"].isString()) return;
    Symbol s;
    s.name = j["name"].asString();
    s.detail = j["detail"].asString();
    s.container = container;
    s.kind = (int)j["kind"].asInt();
    s.depth = depth;
    s.range = parseRange(j["range"]);
    s.selectionRange = j.has("selectionRange") ? parseRange(j["selectionRange"]) : s.range;
    out.push_back(s);
    std::vector<const Json*> kids;
    for (const Json& c : j["children"].elements()) kids.push_back(&c);
    std::stable_sort(kids.begin(), kids.end(), [](const Json* a, const Json* b) {
        return positionBefore(parsePosition((*a)["range"]["start"]),
                              parsePosition((*b)["range"]["start"]));
    });
    const std::string name = s.name;
    for (const Json* c : kids) addDocumentSymbol(*c, name, depth + 1, out);
}

// SymbolInformation, or a WorkspaceSymbol (whose location may have no range).
bool symbolInformation(const Json& j, Symbol& s) {
    if (!j.isObject() || !j["name"].isString()) return false;
    s.name = j["name"].asString();
    s.kind = (int)j["kind"].asInt();
    s.container = j["containerName"].asString();
    s.detail = j["detail"].asString();
    const Json& loc = j["location"];
    s.uri = loc["uri"].asString();
    s.hasRange = loc["range"].isObject();
    if (s.hasRange) s.range = parseRange(loc["range"]);
    s.selectionRange = s.range;
    return true;
}
}  // namespace

std::vector<Symbol> parseDocumentSymbols(const Json& result) {
    std::vector<Symbol> out;
    if (!result.isArray()) return out;
    bool tree = false;
    for (const Json& j : result.elements())
        if (j.isObject() && j.has("range") && !j.has("location")) tree = true;
    if (tree) {
        std::vector<const Json*> top;
        for (const Json& j : result.elements()) top.push_back(&j);
        std::stable_sort(top.begin(), top.end(), [](const Json* a, const Json* b) {
            return positionBefore(parsePosition((*a)["range"]["start"]),
                                  parsePosition((*b)["range"]["start"]));
        });
        for (const Json* j : top) addDocumentSymbol(*j, "", 0, out);
        return out;
    }
    for (const Json& j : result.elements()) {
        Symbol s;
        if (!symbolInformation(j, s)) continue;
        s.uri.clear();   // the file asked about
        out.push_back(s);
    }
    std::stable_sort(out.begin(), out.end(), [](const Symbol& a, const Symbol& b) {
        return positionBefore(a.range.start, b.range.start);
    });
    return out;
}

std::vector<Symbol> parseWorkspaceSymbols(const Json& result) {
    std::vector<Symbol> out;
    for (const Json& j : result.elements()) {
        Symbol s;
        if (symbolInformation(j, s) && !s.uri.empty()) out.push_back(s);
    }
    return out;
}

std::vector<Symbol> mergeSymbols(const std::vector<std::vector<Symbol>>& perServer,
                                 const CanonicalUri& canonical) {
    std::vector<Symbol> out;
    std::set<std::tuple<std::string, std::string, int, int>> seen;
    for (const std::vector<Symbol>& list : perServer)
        for (const Symbol& s : list) {
            const std::string uri = canonical && !s.uri.empty() ? canonical(s.uri) : s.uri;
            auto key = std::make_tuple(s.name, uri, s.hasRange ? s.range.start.line : -1,
                                       s.hasRange ? s.range.start.character : -1);
            if (!seen.insert(key).second) continue;
            out.push_back(s);
        }
    return out;
}

int symbolAt(const std::vector<Symbol>& symbols, Position p) {
    int best = -1;
    for (size_t i = 0; i < symbols.size(); i++) {
        const Symbol& s = symbols[i];
        if (!s.hasRange || !rangeHolds(s.range, p)) continue;
        if (best < 0) { best = (int)i; continue; }
        const Symbol& b = symbols[(size_t)best];
        // The deeper one; at one depth (a flat list) the one inside the other,
        // else the one starting last.
        bool inside = rangeHolds(b.range, s.range.start) && rangeHolds(b.range, s.range.end);
        if (s.depth > b.depth ||
            (s.depth == b.depth && (inside || !positionBefore(s.range.start, b.range.start))))
            best = (int)i;
    }
    return best;
}

namespace {
struct KindInfo {
    const char* name;
    const char* plural;
    const char16_t* glyph;
    unsigned color;
};
// VS Code's symbol icon colors: purple for functions and methods, orange for
// classes, enums, structs and events, blue for variables, fields and
// interfaces, plain for the rest.
const KindInfo kKinds[27] = {
    {"symbol", "symbols", u"•", 0xC5C5C5},
    {"file", "files", u"≣", 0xC5C5C5},
    {"module", "modules", u"{}", 0xC5C5C5},
    {"namespace", "namespaces", u"{}", 0xC5C5C5},
    {"package", "packages", u"{}", 0xC5C5C5},
    {"class", "classes", u"C", 0xEE9D28},
    {"method", "methods", u"ƒ", 0xB180D7},
    {"property", "properties", u"p", 0x75BEFF},
    {"field", "fields", u"f", 0x75BEFF},
    {"constructor", "constructors", u"ƒ", 0xB180D7},
    {"enumeration", "enumerations", u"E", 0xEE9D28},
    {"interface", "interfaces", u"I", 0x75BEFF},
    {"function", "functions", u"ƒ", 0xB180D7},
    {"variable", "variables", u"x", 0x75BEFF},
    {"constant", "constants", u"π", 0xC5C5C5},
    {"string", "strings", u"#", 0xC5C5C5},
    {"number", "numbers", u"#", 0xC5C5C5},
    {"boolean", "booleans", u"◐", 0xC5C5C5},
    {"array", "arrays", u"[]", 0xC5C5C5},
    {"object", "objects", u"{}", 0xC5C5C5},
    {"key", "keys", u"k", 0xC5C5C5},
    {"null", "nulls", u"∅", 0xC5C5C5},
    {"enumeration member", "enumeration members", u"e", 0x75BEFF},
    {"struct", "structs", u"S", 0xEE9D28},
    {"event", "events", u"ϟ", 0xEE9D28},
    {"operator", "operators", u"±", 0xC5C5C5},
    {"type parameter", "type parameters", u"T", 0xC5C5C5},
};
const KindInfo& kindInfo(int kind) { return kKinds[kind >= 1 && kind <= 26 ? kind : 0]; }
}  // namespace

std::string symbolKindName(int kind) { return kindInfo(kind).name; }
std::string symbolKindPlural(int kind) { return kindInfo(kind).plural; }
std::u16string symbolKindGlyph(int kind) { return kindInfo(kind).glyph; }
unsigned symbolKindColor(int kind) { return kindInfo(kind).color; }

std::vector<ReferenceFile> groupReferences(const std::vector<Location>& locs,
                                           const std::string& firstUriIn,
                                           const CanonicalUri& canonical) {
    std::vector<ReferenceFile> files;
    std::map<std::string, size_t> at;
    const std::string firstUri = canonical && !firstUriIn.empty() ? canonical(firstUriIn)
                                                                  : firstUriIn;
    for (const Location& l : locs) {
        std::string uri = canonical ? canonical(l.uri) : l.uri;
        auto it = at.find(uri);
        if (it == at.end()) {
            it = at.emplace(uri, files.size()).first;
            files.push_back(ReferenceFile{uri, {}});
        }
        files[it->second].ranges.push_back(l.range);
    }
    for (ReferenceFile& f : files) {
        std::stable_sort(f.ranges.begin(), f.ranges.end(), [](const Range& a, const Range& b) {
            return positionBefore(a.start, b.start) ||
                   (a.start == b.start && positionBefore(a.end, b.end));
        });
        f.ranges.erase(std::unique(f.ranges.begin(), f.ranges.end(),
                                   [](const Range& a, const Range& b) {
                                       return a.start == b.start && a.end == b.end;
                                   }),
                       f.ranges.end());
    }
    std::stable_sort(files.begin(), files.end(), [&](const ReferenceFile& a,
                                                     const ReferenceFile& b) {
        bool fa = a.uri == firstUri, fb = b.uri == firstUri;
        if (fa != fb) return fa;
        std::string pa = pathFromUri(a.uri), pb = pathFromUri(b.uri);
        return (pa.empty() ? a.uri : pa) < (pb.empty() ? b.uri : pb);
    });
    return files;
}

}  // namespace Lsp
