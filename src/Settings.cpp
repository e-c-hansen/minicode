// Settings.cpp — see Settings.h.
#include "Settings.h"
#include <algorithm>
#include <cctype>
#include <iterator>

namespace {

// Indexed by Surface.
const char* const kSurfaceNames[kSurfaceCount] = {
    "titlebar", "sidebar", "editor", "terminal", "browser", "statusbar",
};
// Indexed by TokenStyle; Plain has no key of its own (it is editor.text).
const char* const kSyntaxNames[8] = {
    nullptr, "keyword", "type", "string", "comment", "number", "preprocessor", "function",
};
// Indexed by MarkdownColor.
const char* const kMarkdownNames[kMarkdownColorCount] = {
    "heading", "link", "code", "quote", "done",
};

// VS Code's built-in themes. Each row: id, title, dark; backgrounds and texts
// in Surface order (title bar, sidebar, editor, terminal, browser toolbar,
// status bar); syntax in TokenStyle order (plain unused, keyword, type,
// string, comment, number, preprocessor, function); Markdown (heading, link,
// code, quote, done); code block background, ruler, terminal input text;
// line numbers, the caret line's number, the bracket match box.
// Dark+ is minicode's look from the start, so its values are the defaults
// the settings file documents; the others come from the themes' own files
// (editor, sideBar, titleBar, panel and statusBar colors, the token colors
// for comments, strings, keywords, keyword.control, types, numbers and
// function names, editorLineNumber's foreground and activeForeground,
// editorBracketMatch's border), the nearest where a theme leaves a color to
// its base.
const Theme kThemes[] = {
    {"dark-modern", "Dark Modern", true,
     {0x181818, 0x181818, 0x1F1F1F, 0x181818, 0x1F1F1F, 0x181818},
     {0xCCCCCC, 0xCCCCCC, 0xCCCCCC, 0xCCCCCC, 0xCCCCCC, 0xCCCCCC},
     {0, 0x569CD6, 0x4EC9B0, 0xCE9178, 0x6A9955, 0xB5CEA8, 0xC586C0, 0xDCDCAA},
     {0xFFFFFF, 0x4DAAFC, 0xCE9178, 0x9D9D9D, 0x6E7681},
     0x2B2B2B, 0x5A5A5A, 0xE6E6E6,
     0x6E7681, 0xCCCCCC, 0x888888},
    {"dark-plus", "Dark+", true,
     {0x323233, 0x252526, 0x1E1E1E, 0x181818, 0x2A2A2A, 0x007ACC},
     {0xCCCCCC, 0xCCCCCC, 0xD4D4D4, 0xD4D4D4, 0xD4D4D4, 0xFFFFFF},
     {0, 0x569CD6, 0x4EC9B0, 0xCE9178, 0x6A9955, 0xB5CEA8, 0xC586C0, 0xDCDCAA},
     {0xFFFFFF, 0x4EA1F7, 0xCE9178, 0x9CA3AF, 0x858585},
     0x2A2A2A, 0x5A5A5A, 0xEDEDED,
     0x858585, 0xC6C6C6, 0x888888},
    {"monokai", "Monokai", true,
     {0x1E1F1C, 0x1E1F1C, 0x272822, 0x1E1F1C, 0x1E1F1C, 0x414339},
     {0xCCCCCC, 0xCCCCCC, 0xF8F8F2, 0xF8F8F2, 0xF8F8F2, 0xFFFFFF},
     {0, 0xF92672, 0x66D9EF, 0xE6DB74, 0x88846F, 0xAE81FF, 0xF92672, 0xA6E22E},
     {0xF8F8F2, 0x66D9EF, 0xE6DB74, 0x88846F, 0x75715E},
     0x3E3D32, 0x464741, 0xFFFFFF,
     0x90908A, 0xC2C2BF, 0x888888},
    {"solarized-dark", "Solarized Dark", true,
     {0x002C39, 0x00212B, 0x002B36, 0x00212B, 0x00212B, 0x00212B},
     {0x93A1A1, 0x93A1A1, 0x839496, 0x839496, 0x93A1A1, 0x93A1A1},
     {0, 0x859900, 0xB58900, 0x2AA198, 0x586E75, 0xD33682, 0xCB4B16, 0x268BD2},
     {0x93A1A1, 0x268BD2, 0x2AA198, 0x657B83, 0x586E75},
     0x073642, 0x0C4A59, 0x93A1A1,
     0x858585, 0x949494, 0x888888},
    {"tomorrow-night-blue", "Tomorrow Night Blue", true,
     {0x001126, 0x001C40, 0x002451, 0x001C40, 0x001C40, 0x001126},
     {0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF},
     {0, 0xEBBBFF, 0xFFEEAD, 0xD1F1A9, 0x7285B7, 0xFFC58F, 0xEBBBFF, 0xBBDAFF},
     {0xFFFFFF, 0xBBDAFF, 0xD1F1A9, 0x7285B7, 0x7285B7},
     0x00346E, 0x003F8E, 0xFFFFFF,
     0x7285B7, 0xFFFFFF, 0x888888},
    {"light-modern", "Light Modern", false,
     {0xF8F8F8, 0xF8F8F8, 0xFFFFFF, 0xF8F8F8, 0xF8F8F8, 0xF8F8F8},
     {0x1E1E1E, 0x3B3B3B, 0x3B3B3B, 0x3B3B3B, 0x3B3B3B, 0x3B3B3B},
     {0, 0x0000FF, 0x267F99, 0xA31515, 0x008000, 0x098658, 0xAF00DB, 0x795E26},
     {0x1F1F1F, 0x005FB8, 0xA31515, 0x6E7781, 0x8B949E},
     0xF0F0F0, 0xD3D3D3, 0x1F1F1F,
     0x6E7681, 0x171184, 0xB9B9B9},
    {"light-plus", "Light+", false,
     {0xDDDDDD, 0xF3F3F3, 0xFFFFFF, 0xFFFFFF, 0xF3F3F3, 0x007ACC},
     {0x333333, 0x616161, 0x000000, 0x333333, 0x333333, 0xFFFFFF},
     {0, 0x0000FF, 0x267F99, 0xA31515, 0x008000, 0x098658, 0xAF00DB, 0x795E26},
     {0x000000, 0x006AB1, 0xA31515, 0x6A737D, 0x8E8E8E},
     0xF3F3F3, 0xD3D3D3, 0x000000,
     0x237893, 0x0B216F, 0xB9B9B9},
    {"quiet-light", "Quiet Light", false,
     {0xC4B7D7, 0xF2F2F2, 0xF5F5F5, 0xF5F5F5, 0xF2F2F2, 0x705697},
     {0x333333, 0x333333, 0x333333, 0x333333, 0x333333, 0xFFFFFF},
     {0, 0x4B83CD, 0x7A3E9D, 0x448C27, 0xAAAAAA, 0xAB6526, 0x7A3E9D, 0xAA3731},
     {0x333333, 0x4B83CD, 0x448C27, 0x777777, 0xAAAAAA},
     0xE8E8E8, 0xD3D3D3, 0x000000,
     0x6D705B, 0x9769DC, 0xB9B9B9},
    {"solarized-light", "Solarized Light", false,
     {0xEEE8D5, 0xEEE8D5, 0xFDF6E3, 0xEEE8D5, 0xEEE8D5, 0xEEE8D5},
     {0x586E75, 0x586E75, 0x657B83, 0x586E75, 0x586E75, 0x586E75},
     {0, 0x859900, 0xB58900, 0x2AA198, 0x93A1A1, 0xD33682, 0xCB4B16, 0x268BD2},
     {0x586E75, 0x268BD2, 0x2AA198, 0x93A1A1, 0x93A1A1},
     0xEEE8D5, 0xDDD6C1, 0x073642,
     0x237893, 0x567983, 0xB9B9B9},
};

const char* const kMaterials[] = {
    "titlebar", "sidebar", "menu", "popover", "hud", "sheet", "window",
    "under-window", "header", "content", "fullscreen", "tooltip",
};

// Run File's built-in commands, only where the command is not in doubt.
const struct { const char* language; const char* command; } kRunDefaults[] = {
    {"python", "{python} {file}"},
    {"sh",     "bash {file}"},
    {"go",     "go run {file}"},
    {"js",     "node {file}"},
    {"rb",     "ruby {file}"},
};


std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// A plain decimal: digits, optionally a point and more digits. No sign, no
// exponent, and independent of the C locale's decimal separator.
bool parseDecimal(const std::string& s, double& out) {
    double v = 0, scale = 1;
    bool digits = false, point = false;
    for (char c : s) {
        if (c == '.' && !point) { point = true; continue; }
        if (c < '0' || c > '9') return false;
        digits = true;
        if (point) { scale /= 10; v += (c - '0') * scale; }
        else v = v * 10 + (c - '0');
    }
    if (!digits) return false;
    out = v;
    return true;
}

bool parseBool(const std::string& s, bool& out) {
    std::string v = lower(s);
    if (v == "true" || v == "on" || v == "yes" || v == "1") { out = true; return true; }
    if (v == "false" || v == "off" || v == "no" || v == "0") { out = false; return true; }
    return false;
}

std::string colorError(const std::string& v) {
    return "'" + v + "' is not a color (use #RRGGBB or #RRGGBBAA)";
}

}  // namespace

bool Settings::parseColor(const std::string& s, Rgba& out) {
    if (s.empty() || s[0] != '#') return false;
    std::string h = s.substr(1);
    for (char c : h) if (hexDigit(c) < 0) return false;
    auto byte = [&](size_t i) { return hexDigit(h[i]) * 16 + hexDigit(h[i + 1]); };
    Rgba c;
    if (h.size() == 3) {
        c.r = hexDigit(h[0]) * 17; c.g = hexDigit(h[1]) * 17; c.b = hexDigit(h[2]) * 17;
    } else if (h.size() == 6 || h.size() == 8) {
        c.r = byte(0); c.g = byte(2); c.b = byte(4);
        if (h.size() == 8) c.a = byte(6) / 255.0;
    } else {
        return false;
    }
    out = c;
    return true;
}

bool Settings::parseOpacity(const std::string& s, double& out) {
    double v;
    if (!s.empty() && s.back() == '%') {
        if (!parseDecimal(s.substr(0, s.size() - 1), v) || v > 100) return false;
        out = v / 100;
        return true;
    }
    if (!parseDecimal(s, v) || v > 1) return false;
    out = v;
    return true;
}

const char* Settings::surfaceName(Surface s) { return kSurfaceNames[(int)s]; }

const std::vector<Theme>& Settings::themes() {
    static const std::vector<Theme> all(std::begin(kThemes), std::end(kThemes));
    return all;
}

const Theme* Settings::findTheme(const std::string& id) {
    const std::string want = lower(trim(id));
    for (const Theme& t : themes())
        if (want == t.id) return &t;
    return nullptr;
}

bool Settings::isColorKey(const std::string& key) {
    if (const KeyRule* r = findKeyRule(key)) return r->color;
    size_t dot = key.find('.');
    if (dot == std::string::npos) return false;
    const std::string group = key.substr(0, dot), field = key.substr(dot + 1);
    for (const char* name : kSurfaceNames)
        if (group == name) return field == "background" || field == "text";
    if (group == "syntax")
        for (const char* name : kSyntaxNames)
            if (name && field == name) return true;
    if (group == "markdown")
        for (const char* name : kMarkdownNames)
            if (field == name) return true;
    return false;
}

namespace {
// The text as lines, each without its "\n" (a "\r" stays on its line), and
// back. A text ending in "\n" gives a last empty line, so the round trip is
// exact.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (true) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) { lines.push_back(text.substr(pos)); break; }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}
std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
}
// The key of an uncommented "key = value" line, lowercased; "" otherwise.
std::string activeKey(const std::string& line) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') return "";
    size_t eq = t.find('=');
    if (eq == std::string::npos) return "";
    return lower(trim(t.substr(0, eq)));
}
// The key of a commented-out "# key = value" line, lowercased; "" otherwise.
std::string commentedKey(const std::string& line) {
    std::string t = trim(line);
    if (t.empty() || t[0] != '#') return "";
    t = trim(t.substr(1));
    size_t eq = t.find('=');
    if (eq == std::string::npos) return "";
    std::string key = lower(trim(t.substr(0, eq)));
    for (char c : key)
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) return "";
    return key;
}
}  // namespace

int Settings::colorOverrides(const std::string& text) {
    int n = 0;
    for (const std::string& line : splitLines(text))
        if (isColorKey(activeKey(line))) n++;
    return n;
}

std::string Settings::withValue(const std::string& text, const std::string& key,
                                const std::string& value) {
    std::vector<std::string> lines = splitLines(text);
    int active = -1, commented = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        if (activeKey(lines[i]) == key) active = (int)i;
        else if (commented < 0 && commentedKey(lines[i]) == key) commented = (int)i;
    }
    const int at = active >= 0 ? active : commented;
    if (at < 0) {
        std::string out = text;
        if (!out.empty() && out.back() != '\n') out += "\n";
        return out + key + " = " + value + "\n";
    }
    const std::string& old = lines[at];
    const std::string eol = !old.empty() && old.back() == '\r' ? "\r" : "";
    size_t indent = 0;
    while (indent < old.size() && (old[indent] == ' ' || old[indent] == '\t')) indent++;
    std::string tail;
    if (active >= 0) {
        size_t eq = old.find('=');
        size_t hash = old.find(" #", eq);
        if (hash != std::string::npos) {
            while (hash > eq + 1 && (old[hash - 1] == ' ' || old[hash - 1] == '\t')) hash--;
            tail = old.substr(hash);
            if (!tail.empty() && tail.back() == '\r') tail.pop_back();
        }
    }
    lines[at] = old.substr(0, indent) + key + " = " + value + tail + eol;
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) out += lines[i] + (i + 1 < lines.size() ? "\n" : "");
    return out;
}

std::string Settings::withTheme(const std::string& text, const std::string& id,
                                bool commentColors) {
    if (text.empty()) return "theme = " + id + "\n";
    std::vector<std::string> lines = splitLines(text);
    auto indentOf = [](const std::string& line) {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        return line.substr(0, i);
    };
    auto endOf = [](const std::string& line) {
        return !line.empty() && line.back() == '\r' ? std::string("\r") : std::string();
    };
    int active = -1, commented = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        if (activeKey(lines[i]) == "theme") active = (int)i;
        else if (commented < 0 && commentedKey(lines[i]) == "theme") commented = (int)i;
    }
    const int at = active >= 0 ? active : commented;
    if (at >= 0) {
        // Keep a comment after the value ("theme = x  # mine").
        const std::string& old = lines[at];
        std::string tail;
        if (active >= 0) {
            size_t eq = old.find('=');
            size_t hash = old.find(" #", eq);
            if (hash != std::string::npos) {
                while (hash > eq + 1 && (old[hash - 1] == ' ' || old[hash - 1] == '\t')) hash--;
                tail = old.substr(hash);
            }
            if (!tail.empty() && tail.back() == '\r') tail.pop_back();
        }
        lines[at] = indentOf(old) + "theme = " + id + tail + endOf(old);
    } else {
        // After the file's opening comment and the blank line ending it, or
        // at the top of a file that starts with a setting.
        size_t i = 0;
        while (i < lines.size() && !trim(lines[i]).empty() && trim(lines[i])[0] == '#') i++;
        const std::string eol = lines.empty() ? "" : endOf(lines[0]);
        if (i == 0) {
            lines.insert(lines.begin(), {"theme = " + id + eol, eol});
        } else {
            while (i < lines.size() && trim(lines[i]).empty()) i++;
            lines.insert(lines.begin() + i, {"theme = " + id + eol, eol});
        }
    }
    if (commentColors)
        for (std::string& line : lines)
            if (isColorKey(activeKey(line))) {
                const std::string indent = indentOf(line);
                line = indent + "# " + line.substr(indent.size());
            }
    return joinLines(lines);
}

bool Settings::findColor(const std::u16string& line, ColorSpan& out) {
    size_t eq = line.find(u'=');
    if (eq == std::u16string::npos) return false;
    size_t start = eq + 1;
    while (start < line.size() && (line[start] == u' ' || line[start] == u'\t')) start++;
    size_t end = start;
    std::string token;
    while (end < line.size() && line[end] != u' ' && line[end] != u'\t' &&
           line[end] != u'\r') {
        if (line[end] > 0x7F) return false;
        token += (char)line[end];
        end++;
    }
    Rgba c;
    if (!parseColor(token, c)) return false;
    out = {start, end - start, c};
    return true;
}

std::string Settings::formatColor(const Rgba& c) {
    static const char* digits = "0123456789ABCDEF";
    std::string s = "#";
    auto put = [&](int v) { s += digits[(v >> 4) & 0xF]; s += digits[v & 0xF]; };
    put(c.r); put(c.g); put(c.b);
    int alpha = (int)(c.a * 255 + 0.5);
    if (alpha < 255) put(alpha < 0 ? 0 : alpha);
    return s;
}

static std::u16string uncommentSetting(const std::u16string& out);

// Where a whole command line ends: at a " #" comment outside quotes, else at
// the end. A '#' inside quotes is the command's own (echo "a #b"), so it
// never cuts the line short and leaves an unclosed quote for the shell.
static size_t commandLineEnd(const std::string& rest) {
    char quote = 0;
    for (size_t i = 0; i < rest.size(); i++) {
        const char c = rest[i];
        if (quote == '\'') {
            if (c == '\'') quote = 0;
        } else if (c == '\\') {
            i++;   // the next character is taken as it is, in double quotes too
        } else if (quote == '"') {
            if (c == '"') quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '#' && i > 0 && (rest[i - 1] == ' ' || rest[i - 1] == '\t')) {
            return i - 1;
        }
    }
    return rest.size();
}

std::u16string Settings::setColor(const std::u16string& line, const Rgba& c) {
    ColorSpan span;
    if (!findColor(line, span)) return line;
    std::string hex = formatColor(c);
    return uncommentSetting(line.substr(0, span.start) +
                            std::u16string(hex.begin(), hex.end()) +
                            line.substr(span.start + span.length));
}

// "# key = value" -> "key = value", keeping any indentation. Only when what
// follows the # is a plain key, so prose is never touched.
static std::u16string uncommentSetting(const std::u16string& out) {
    size_t hash = 0;
    while (hash < out.size() && (out[hash] == u' ' || out[hash] == u'\t')) hash++;
    if (hash >= out.size() || out[hash] != u'#') return out;
    size_t key = hash + 1;
    while (key < out.size() && out[key] == u' ') key++;
    size_t eq = out.find(u'=', key);
    size_t keyEnd = eq;
    while (keyEnd > key && out[keyEnd - 1] == u' ') keyEnd--;
    if (keyEnd == key) return out;
    for (size_t i = key; i < keyEnd; i++) {
        char16_t ch = out[i];
        bool ok = (ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z') ||
                  (ch >= u'0' && ch <= u'9') || ch == u'.' || ch == u'_' || ch == u'-';
        if (!ok) return out;
    }
    return out.substr(0, hash) + out.substr(key);
}

// The key of a "key = value" or "# key = value" line, lowercased, or "" when
// the line is prose or blank.
static std::string settingKey(const std::u16string& line, size_t* eqOut = nullptr) {
    size_t i = 0;
    while (i < line.size() && (line[i] == u' ' || line[i] == u'\t')) i++;
    if (i < line.size() && line[i] == u'#') {
        i++;
        while (i < line.size() && line[i] == u' ') i++;
    }
    size_t eq = line.find(u'=', i);
    if (eq == std::u16string::npos) return "";
    size_t end = eq;
    while (end > i && (line[end - 1] == u' ' || line[end - 1] == u'\t')) end--;
    std::string key;
    for (size_t k = i; k < end; k++) {
        char16_t ch = line[k];
        bool ok = (ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z') ||
                  (ch >= u'0' && ch <= u'9') || ch == u'.' || ch == u'_' || ch == u'-';
        if (!ok) return "";
        key += (char)std::tolower((int)ch);
    }
    if (eqOut) *eqOut = eq;
    return key;
}

bool Settings::findValue(const std::u16string& line, ValueSpan& out) {
    size_t eq;
    if (settingKey(line, &eq).empty()) return false;
    size_t start = eq + 1;
    while (start < line.size() && (line[start] == u' ' || line[start] == u'\t')) start++;
    size_t end = start;
    while (end < line.size() && line[end] != u' ' && line[end] != u'\t' &&
           line[end] != u'\r' && line[end] != u'\n')
        end++;
    if (end == start) return false;
    out.start = start;
    out.length = end - start;
    return true;
}

bool Settings::findOpacity(const std::u16string& line, ValueSpan& out, double& value) {
    std::string key = settingKey(line);
    const std::string suffix = ".opacity";
    if (key.size() <= suffix.size() ||
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    ValueSpan span;
    if (!findValue(line, span)) return false;
    std::string token;
    for (size_t i = span.start; i < span.start + span.length; i++) {
        if (line[i] > 0x7F) return false;
        token += (char)line[i];
    }
    if (!parseOpacity(token, value)) return false;
    out = span;
    return true;
}

std::u16string Settings::setOpacity(const std::u16string& line, double value) {
    ValueSpan span;
    double old;
    if (!findOpacity(line, span, old)) return line;
    if (value < 0) value = 0;
    if (value > 1) value = 1;
    bool percent = line[span.start + span.length - 1] == u'%';
    std::string text;
    if (percent) {
        text = std::to_string((int)(value * 100 + 0.5)) + "%";
    } else {
        int hundredths = (int)(value * 100 + 0.5);
        if (hundredths == 100) text = "1";
        else if (hundredths == 0) text = "0";
        else {
            text = "0." + std::string(1, char('0' + hundredths / 10));
            if (hundredths % 10) text += char('0' + hundredths % 10);
        }
    }
    return uncommentSetting(line.substr(0, span.start) +
                            std::u16string(text.begin(), text.end()) +
                            line.substr(span.start + span.length));
}

const std::vector<AppearanceSurface>& Settings::appearanceSurfaces() {
    // window.opacity and window.text are what every panel falls back to.
    static const std::vector<AppearanceSurface> surfaces = {
        {"tree", "File Tree",
         {"sidebar.background", "sidebar.opacity", "sidebar.text",
          "window.opacity", "window.text"}, false},
        {"git", "Source Control",
         {"sidebar.background", "sidebar.opacity", "sidebar.text",
          "window.opacity", "window.text"}, false},
        {"editor", "Editor",
         {"editor.background", "editor.opacity", "editor.text",
          "editor.gutter-background", "editor.line-number-color",
          "editor.line-number-active-color", "editor.bracket-match",
          "syntax.keyword", "syntax.type", "syntax.string", "syntax.comment",
          "syntax.number", "syntax.preprocessor", "syntax.function",
          "window.opacity", "window.text"}, false},
        {"syntax", "Syntax Colors",
         {"syntax.keyword", "syntax.type", "syntax.string", "syntax.comment",
          "syntax.number", "syntax.preprocessor", "syntax.function",
          "editor.text"}, false},
        {"markdown", "Markdown Preview",
         {"markdown.heading", "markdown.link", "markdown.code", "markdown.quote",
          "markdown.done", "editor.background", "editor.opacity", "editor.text",
          "window.opacity", "window.text"}, false},
        {"terminal", "Terminal",
         {"terminal.background", "terminal.opacity", "terminal.text",
          "window.opacity", "window.text"}, false},
        {"browser", "Browser",
         {"browser.background", "browser.opacity", "browser.text",
          "window.opacity", "window.text"}, false},
        {"statusbar", "Status Bar",
         {"statusbar.background", "statusbar.opacity", "statusbar.text",
          "window.opacity", "window.text"}, false},
        {"titlebar", "Title Bar",
         {"titlebar.background", "titlebar.opacity", "titlebar.text",
          "window.opacity", "window.text"}, false},
        {"window", "Window",
         {"window.opacity", "window.text", "window.blur", "window.material"}, false},
        // The editor's and the terminal's tab strips: the active tab is drawn
        // in its pane's colors, the strip behind the tabs in the sidebar's.
        {"tabs", "Tabs",
         {"editor.background", "editor.opacity", "editor.text",
          "sidebar.background", "sidebar.opacity", "sidebar.text",
          "window.opacity", "window.text"}, false},
        {"terminal-tabs", "Terminal Tabs",
         {"terminal.background", "terminal.opacity", "terminal.text",
          "sidebar.background", "sidebar.opacity", "sidebar.text",
          "window.opacity", "window.text"}, false},
    };
    return surfaces;
}

const AppearanceSurface* Settings::appearanceSurface(const std::string& id) {
    for (const AppearanceSurface& s : appearanceSurfaces())
        if (id == s.id) return &s;
    return nullptr;
}

int Settings::findKeyLine(const std::string& text, const std::string& key) {
    std::string want = lower(key);
    int active = -1, commented = -1, lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        std::u16string wide(line.begin(), line.end());   // keys are ASCII
        if (settingKey(wide) == want) {
            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
            if (i < line.size() && line[i] == '#') {
                if (commented < 0) commented = lineNo;
            } else {
                active = lineNo;
            }
        }
        pos = nl + 1;
        lineNo++;
    }
    return active >= 0 ? active : commented;
}

std::string Settings::missingSurfaceLines(const std::string& text, const std::string& surface) {
    const AppearanceSurface* s = appearanceSurface(surface);
    if (!s) return "";
    std::vector<std::string> lines;
    const std::string defaults = defaultFileText();
    for (size_t pos = 0; pos < defaults.size();) {
        size_t nl = defaults.find('\n', pos);
        if (nl == std::string::npos) nl = defaults.size();
        lines.push_back(defaults.substr(pos, nl - pos));
        pos = nl + 1;
    }
    auto keyOf = [](const std::string& line) {
        return settingKey(std::u16string(line.begin(), line.end()));   // ASCII
    };
    std::vector<bool> wanted(lines.size(), false);
    for (size_t k = 0; k < lines.size(); k++) {
        const std::string key = keyOf(lines[k]);
        if (key.empty()) continue;
        bool ours = false;
        for (const char* own : s->keys) ours = ours || key == own;
        if (!ours || findKeyLine(text, key) >= 0) continue;
        wanted[k] = true;
        // The comment just above it: its section's heading, or its own note.
        for (size_t up = k; up > 0; up--) {
            const std::string& above = lines[up - 1];
            if (above.empty() || !keyOf(above).empty()) break;
            wanted[up - 1] = true;
        }
    }
    std::string out;
    bool gap = false;   // a blank line passed since the last line taken
    for (size_t k = 0; k < lines.size(); k++) {
        if (!wanted[k]) {
            if (lines[k].empty()) gap = true;
            continue;
        }
        if (gap && !out.empty()) out += "\n";
        gap = false;
        out += lines[k] + "\n";
    }
    return out;
}

Settings Settings::parse(const std::string& text,
                         std::vector<SettingsError>* errors) {
    Settings st;
    std::map<std::string, int> lspLines;   // lsp.* keys of a family, by line
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        lineNo++;
        if (line.empty() || line[0] == '#') continue;

        auto fail = [&](const std::string& msg) {
            if (errors) errors->push_back({lineNo, msg});
        };
        size_t eq = line.find('=');
        if (eq == std::string::npos) { fail("expected key = value"); continue; }
        std::string key = lower(trim(line.substr(0, eq)));
        std::string rest = trim(line.substr(eq + 1));
        if (key.empty()) { fail("missing key before '='"); continue; }

        // How far the value runs: the key's row says, else its family (a
        // language server, run or formatter command is a whole command line).
        const KeyRule* rule = findKeyRule(key);
        if (!rule && key.rfind("lsp.", 0) == 0) lspLines[key] = lineNo;
        const ValueKind kind =
            rule ? rule->value
                 : (key.rfind("lsp.", 0) == 0 || key.rfind("run.", 0) == 0 ||
                    key.rfind("format.", 0) == 0)
                       ? ValueKind::CommandLine
                       : ValueKind::Word;
        if (kind == ValueKind::Path) {
            // A path may hold spaces, so it runs to the end of the line or to
            // a " #" comment. Empty, or auto, is the automatic choice.
            size_t hash = rest.find(" #");
            std::string path = trim(hash == std::string::npos ? rest : rest.substr(0, hash));
            if (!path.empty() && path[0] == '#') path.clear();
            std::string msg;
            if (!st.apply(key, path, msg)) fail(msg);
            continue;
        }
        if (kind == ValueKind::CommandLine) {
            std::string command = trim(rest.substr(0, commandLineEnd(rest)));
            if (command.empty() || command[0] == '#') {
                fail("missing value for " + key);
                continue;
            }
            std::string msg;
            if (!st.apply(key, command, msg)) fail(msg);
            continue;
        }

        // The value is one word; anything after it must be a comment.
        size_t end = 0;
        while (end < rest.size() && !std::isspace((unsigned char)rest[end])) end++;
        std::string value = rest.substr(0, end);
        std::string tail = trim(rest.substr(end));
        if (value.empty()) { fail("missing value for " + key); continue; }
        if (!tail.empty() && tail[0] != '#') {
            fail("unexpected text after the value of " + key);
            continue;
        }
        std::string msg;
        if (!st.apply(key, value, msg)) fail(msg);
    }
    // A server of the user's own needs its command and its files; the line
    // that has one without the other says so.
    if (errors) {
        for (const auto& [key, line] : lspLines) {
            const bool exts = key.size() > 11 &&
                              key.compare(key.size() - 11, 11, ".extensions") == 0;
            std::string name = key.substr(4, key.size() - 4 - (exts ? 11 : 0));
            // A line already refused (or an .also = auto) is not looked at again.
            if (exts ? !st.lspExtensions_.count(name) : !st.lspCommands_.count(name)) continue;
            if (name.size() > 5 && name.compare(name.size() - 5, 5, ".also") == 0)
                name = name.substr(0, name.size() - 5);
            bool builtIn = false;
            for (const std::string& k : lspServers()) builtIn = builtIn || k == name;
            if (builtIn) continue;
            if (!exts && !st.lspExtensions_.count(name))
                errors->push_back({line, "lsp." + name + " is not a built-in server (" +
                                             "cpp, python, go, rust, typescript, latex, "
                                             "markdown, toml, shell): a server of your own "
                                             "also needs lsp." + name +
                                             ".extensions, the files it is for"});
            else if (exts && !st.lspCommands_.count(name))
                errors->push_back({line, "lsp." + name + ".extensions has no server: add lsp." +
                                             name + " = <the command that runs it>"});
        }
        std::stable_sort(errors->begin(), errors->end(),
                         [](const SettingsError& a, const SettingsError& b) {
                             return a.line < b.line;
                         });
    }
    return st;
}

namespace {

bool boolValue(const std::string& value, bool& out, std::string& error) {
    if (parseBool(value, out)) return true;
    error = "'" + value + "' is not true or false";
    return false;
}
bool colorValue(const std::string& value, std::optional<Rgba>& out, std::string& error) {
    Rgba color;
    if (!Settings::parseColor(value, color)) { error = colorError(value); return false; }
    out = color;
    return true;
}
bool opacityValue(const std::string& value, std::optional<double>& out, std::string& error) {
    double number = 1;
    if (!Settings::parseOpacity(value, number)) {
        error = "'" + value + "' is not an opacity (use 0 to 1, or a percentage like 80%)";
        return false;
    }
    out = number;
    return true;
}
bool fontSizeValue(const std::string& value, double& out, std::string& error) {
    double size;
    if (!parseDecimal(value, size) || size < Settings::kMinFontSize ||
        size > Settings::kMaxFontSize) {
        error = "'" + value + "' is not a font size in points from 6 to 72";
        return false;
    }
    out = size;
    return true;
}
// A whole number of at most `digits` digits from lo to hi.
bool countValue(const std::string& value, size_t digits, long lo, long hi, long& out) {
    bool ok = !value.empty() && value.size() <= digits;
    for (char c : value) ok = ok && c >= '0' && c <= '9';
    out = ok ? std::atol(value.c_str()) : -1;
    return ok && out >= lo && out <= hi;
}

}  // namespace

// One row per key with a value of its own. The lambdas are in a member
// function, so they reach the private fields.
#define MC_KEY(name, kind, color, ...)                                          \
    KeyRule {                                                                  \
        name, ValueKind::kind, color,                                          \
            [](Settings& s, const std::string& v, std::string& e) -> bool {    \
                (void)s; (void)v; (void)e;                                     \
                __VA_ARGS__                                                    \
            }                                                                  \
    }
const std::vector<Settings::KeyRule>& Settings::keyTable() {
    static const std::vector<KeyRule> table = {
        MC_KEY("window.opacity", Word, false, return opacityValue(v, s.windowOpacity_, e);),
        MC_KEY("window.text", Word, true, return colorValue(v, s.windowText_, e);),
        MC_KEY("window.blur", Word, false, return boolValue(v, s.blur_, e);),
        MC_KEY("window.material", Word, false,
               const std::string m = lower(v);
               for (const char* name : kMaterials)
                   if (m == name) { s.material_ = m; return true; }
               e = "unknown material '" + v + "'";
               return false;),
        MC_KEY("theme", Word, false,
               if (const Theme* t = findTheme(v)) { s.theme_ = t; return true; }
               e = "unknown theme '" + v + "' (use ";
               for (size_t i = 0; i < themes().size(); i++)
                   e += std::string(i ? (i + 1 == themes().size() ? " or " : ", ") : "") +
                        themes()[i].id;
               e += ")";
               return false;),
        MC_KEY("markdown.web-images", Word, false, return boolValue(v, s.webImages_, e);),
        MC_KEY("markdown.math", Word, false, return boolValue(v, s.math_, e);),
        MC_KEY("markdown.mermaid", Word, false, return boolValue(v, s.mermaid_, e);),
        MC_KEY("editor.keep-sorted-on-save", Word, false,
               return boolValue(v, s.keepSortedOnSave_, e);),
        MC_KEY("editor.organize-imports-on-save", Word, false,
               return boolValue(v, s.organizeImportsOnSave_, e);),
        MC_KEY("editor.auto-save", Word, false,
               const std::string w = lower(v);
               if (w == "off" || w == "false" || w == "none") s.autoSave_ = AutoSave::Off;
               else if (w == "after-delay") s.autoSave_ = AutoSave::AfterDelay;
               else if (w == "focus-change") s.autoSave_ = AutoSave::FocusChange;
               else {
                   e = "'" + v + "' is not off, after-delay or focus-change";
                   return false;
               }
               return true;),
        MC_KEY("editor.auto-save-delay", Word, false,
               long ms;
               if (!countValue(v, 7, 100, 600000, ms)) {
                   e = "'" + v + "' is not a delay in milliseconds from 100 to 600000";
                   return false;
               }
               s.autoSaveDelay_ = (int)ms;
               return true;),
        MC_KEY("editor.ruler", Word, false,
               // A column, a list of them without spaces (80,120), or off.
               const std::string w = lower(v);
               std::vector<int> cols;
               if (w != "off" && w != "none" && w != "false") {
                   size_t i = 0;
                   while (i <= w.size()) {
                       size_t comma = w.find(',', i);
                       if (comma == std::string::npos) comma = w.size();
                       long col;
                       if (!countValue(w.substr(i, comma - i), 4, 1, 1000, col)) {
                           e = "'" + v + "' is not a column (use a number such as 100, "
                               "a list such as 80,120, or off)";
                           return false;
                       }
                       cols.push_back((int)col);
                       i = comma + 1;
                   }
                   std::sort(cols.begin(), cols.end());
                   cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
               }
               s.rulers_ = cols;
               return true;),
        MC_KEY("editor.ruler-color", Word, true, return colorValue(v, s.rulerColor_, e);),
        MC_KEY("editor.font-size", Word, false, return fontSizeValue(v, s.editorFontSize_, e);),
        MC_KEY("editor.line-numbers", Word, false,
               const std::string w = lower(v);
               if (w == "on" || w == "true") s.lineNumbers_ = LineNumbers::On;
               else if (w == "off" || w == "false" || w == "none") s.lineNumbers_ = LineNumbers::Off;
               else if (w == "relative") s.lineNumbers_ = LineNumbers::Relative;
               else {
                   e = "'" + v + "' is not on, off or relative";
                   return false;
               }
               return true;),
        MC_KEY("editor.gutter-background", Word, true,
               return colorValue(v, s.gutterBackground_, e);),
        MC_KEY("editor.line-number-color", Word, true,
               return colorValue(v, s.lineNumberColor_, e);),
        MC_KEY("editor.line-number-active-color", Word, true,
               return colorValue(v, s.lineNumberActiveColor_, e);),
        MC_KEY("editor.bracket-match", Word, true,
               return colorValue(v, s.bracketMatchColor_, e);),
        MC_KEY("terminal.mode", Word, false,
               const std::string w = lower(v);
               if (w != "grid" && w != "line") {
                   e = "'" + v + "' is not grid or line";
                   return false;
               }
               s.terminalLineMode_ = w == "line";
               return true;),
        MC_KEY("terminal.scrollback", Word, false,
               long lines;
               if (!countValue(v, 9, 0, 100000, lines)) {
                   e = "'" + v + "' is not a number of lines from 0 to 100000";
                   return false;
               }
               s.terminalScrollback_ = (int)lines;
               return true;),
        MC_KEY("terminal.font-size", Word, false,
               return fontSizeValue(v, s.terminalFontSize_, e);),
        MC_KEY("terminal.option-as-meta", Word, false,
               return boolValue(v, s.terminalOptionAsMeta_, e);),
        MC_KEY("terminal.activate-venv", Word, false,
               return boolValue(v, s.terminalActivateVenv_, e);),
        MC_KEY("search.use-ignore-files", Word, false,
               return boolValue(v, s.searchUseIgnoreFiles_, e);),
        MC_KEY("lsp.enabled", Word, false, return boolValue(v, s.lspEnabled_, e);),
        MC_KEY("python.interpreter", Path, false,
               s.pythonInterpreter_ = lower(v) == "auto" ? "" : v;
               return true;),
        MC_KEY("run.terminal", Word, false,
               const std::string w = lower(v);
               if (w != "current" && w != "new") {
                   e = "'" + v + "' is not current or new";
                   return false;
               }
               s.runInNewTerminal_ = w == "new";
               return true;),
        MC_KEY("format.on-save", Word, false, return boolValue(v, s.formatOnSave_, e);),
        MC_KEY("develop.command", CommandLine, false, s.developCommand_ = v; return true;),
        MC_KEY("develop.source", Path, false,
               s.developSource_ = lower(v) == "auto" ? "" : v;
               return true;),
        MC_KEY("agent.command", CommandLine, false, s.agentCommand_ = v; return true;),
        MC_KEY("git.autofetch", Word, false, return boolValue(v, s.gitAutoFetch_, e);),
        MC_KEY("git.autofetch-period", Word, false,
               long seconds;
               if (!countValue(v, 6, 180, 86400, seconds)) {
                   e = "'" + v + "' is not a number of seconds from 180 to 86400";
                   return false;
               }
               s.gitAutoFetchPeriod_ = (int)seconds;
               return true;),
        MC_KEY("control.page-any-origin", Word, false, return boolValue(v, s.pageAnyOrigin_, e);),
        MC_KEY("agent.ide-link", Word, false, return boolValue(v, s.ideLink_, e);),
    };
    return table;
}
#undef MC_KEY

const Settings::KeyRule* Settings::findKeyRule(const std::string& key) {
    for (const KeyRule& r : keyTable())
        if (key == r.key) return &r;
    return nullptr;
}

std::vector<std::string> Settings::tableKeys() {
    std::vector<std::string> out;
    for (const KeyRule& r : keyTable()) out.push_back(r.key);
    return out;
}

bool Settings::apply(const std::string& key, const std::string& value,
                     std::string& error) {
    if (const KeyRule* r = findKeyRule(key)) return r->apply(*this, value, error);
    Rgba color;
    double number = 1;
    auto opacityValue = [&]() {
        if (parseOpacity(value, number)) return true;
        error = "'" + value + "' is not an opacity (use 0 to 1, or a "
                "percentage like 80%)";
        return false;
    };

    // The families, by prefix.
    if (key.rfind("run.", 0) == 0) {
        std::string language = key.substr(4);
        bool ok = !language.empty();
        for (char c : language)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '-' || c == '+'))
                ok = false;
        if (!ok) {
            error = "'" + key + "' is not run.<language or extension>";
            return false;
        }
        runCommands_[language] = value;
        return true;
    }
    if (key.rfind("lsp.", 0) == 0) {
        std::string server = key.substr(4);
        // lsp.<server>.extensions: the files a server is for.
        const bool exts = server.size() > 11 &&
                          server.compare(server.size() - 11, 11, ".extensions") == 0;
        if (exts) server = server.substr(0, server.size() - 11);
        // lsp.<server>.also: a second server beside the language's main one.
        // auto is the built-in rule, as if the line were not there.
        const bool also = !exts && server.size() > 5 &&
                          server.compare(server.size() - 5, 5, ".also") == 0;
        const std::string language = also ? server.substr(0, server.size() - 5) : server;
        bool ok = !language.empty();
        for (char c : language)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                  c == '+'))
                ok = false;
        if (!ok) {
            error = "'" + key + "' is not lsp.<server>, lsp.<server>.also or "
                    "lsp.<server>.extensions (a server's name is lowercase letters, digits, "
                    "- and _)";
            return false;
        }
        if (exts) {
            std::vector<std::string> list;
            std::string word;
            for (size_t i = 0; i <= value.size(); i++) {
                const char c = i < value.size() ? value[i] : ' ';
                if (c == ' ' || c == '\t' || c == ',') {
                    while (!word.empty() && word[0] == '.') word.erase(0, 1);
                    if (!word.empty()) list.push_back(lower(word));
                    word.clear();
                } else {
                    word += c;
                }
            }
            if (list.empty()) {
                error = "missing extensions for " + key + " (such as yaml yml)";
                return false;
            }
            lspExtensions_[language] = list;
            return true;
        }
        if (also && lower(value) == "auto") lspCommands_.erase(server);
        else lspCommands_[server] = value;
        return true;
    }

    if (key.rfind("format.", 0) == 0) {
        std::string lang = key.substr(7);
        for (const std::string& known : formatLanguages())
            if (lang == known) { formatCommands_[lang] = value; return true; }
        error = "unknown setting '" + key + "' (formatters are format.python, "
                "format.cpp, format.go, format.rust and format.shell)";
        return false;
    }

    size_t dot = key.find('.');
    if (dot != std::string::npos) {
        std::string group = key.substr(0, dot), field = key.substr(dot + 1);
        for (int i = 0; i < kSurfaceCount; i++) {
            if (group != kSurfaceNames[i]) continue;
            SurfaceSettings& s = surfaces_[i];
            if (field == "opacity") {
                if (!opacityValue()) return false;
                s.opacity = number;
            } else if (field == "background" || field == "text") {
                if (!parseColor(value, color)) { error = colorError(value); return false; }
                (field == "text" ? s.text : s.background) = color;
            } else {
                break;
            }
            if (i == (int)Surface::Titlebar) titlebarKeySet_ = true;
            return true;
        }
        if (group == "syntax" || group == "markdown") {
            bool syntax = group == "syntax";
            const char* const* table = syntax ? kSyntaxNames : kMarkdownNames;
            int count = syntax ? 8 : kMarkdownColorCount;
            for (int i = 0; i < count; i++) {
                if (!table[i] || field != table[i]) continue;
                if (!parseColor(value, color)) { error = colorError(value); return false; }
                (syntax ? syntax_[i] : markdown_[i]) = color;
                return true;
            }
        }
    }
    error = "unknown setting '" + key + "'";
    return false;
}

Rgba Settings::contrastText(const Rgba& c) const {
    Rgba bg = background(Surface::Editor);
    auto mix = [&](uint8_t v, uint8_t under) { return c.a * v + (1 - c.a) * under; };
    double lum = (0.299 * mix(c.r, bg.r) + 0.587 * mix(c.g, bg.g) +
                  0.114 * mix(c.b, bg.b)) / 255;
    return Rgba::hex(lum > 0.55 ? 0x000000 : 0xFFFFFF);
}

double Settings::opacity(Surface s) const {
    const SurfaceSettings& ss = surfaces_[(int)s];
    if (ss.opacity) return *ss.opacity;
    return windowOpacity_ ? *windowOpacity_ : 1.0;
}

Rgba Settings::background(Surface s) const {
    const SurfaceSettings& ss = surfaces_[(int)s];
    Rgba c = ss.background ? *ss.background : Rgba::hex(theme_->background[(int)s]);
    c.a *= opacity(s);
    return c;
}

Rgba Settings::text(Surface s) const {
    const SurfaceSettings& ss = surfaces_[(int)s];
    if (ss.text) return *ss.text;
    if (windowText_) return *windowText_;
    return Rgba::hex(theme_->text[(int)s]);
}

bool Settings::textIsSet(Surface s) const {
    return surfaces_[(int)s].text.has_value() || windowText_.has_value();
}

Rgba Settings::syntax(TokenStyle s) const {
    int i = (int)s;
    if (i <= 0 || i >= 8) return text(Surface::Editor);
    return syntax_[i] ? *syntax_[i] : Rgba::hex(theme_->syntax[i]);
}

Rgba Settings::markdown(MarkdownColor c) const {
    int i = (int)c;
    return markdown_[i] ? *markdown_[i] : Rgba::hex(theme_->markdown[i]);
}

Rgba Settings::markdownCodeBackground() const {
    return Rgba::hex(theme_->codeBackground, opacity(Surface::Editor));
}

Rgba Settings::rulerColor() const {
    return rulerColor_ ? *rulerColor_ : Rgba::hex(theme_->ruler);
}

Rgba Settings::gutterBackground() const {
    if (!gutterBackground_) return background(Surface::Editor);
    Rgba c = *gutterBackground_;
    c.a *= opacity(Surface::Editor);
    return c;
}

Rgba Settings::lineNumberColor() const {
    return lineNumberColor_ ? *lineNumberColor_ : Rgba::hex(theme_->lineNumber);
}

Rgba Settings::lineNumberActiveColor() const {
    return lineNumberActiveColor_ ? *lineNumberActiveColor_
                                  : Rgba::hex(theme_->lineNumberActive);
}

Rgba Settings::bracketMatchColor() const {
    return bracketMatchColor_ ? *bracketMatchColor_ : Rgba::hex(theme_->bracketMatch);
}

Rgba Settings::terminalInputText() const {
    return textIsSet(Surface::Terminal) ? text(Surface::Terminal)
                                        : Rgba::hex(theme_->terminalInput);
}

bool Settings::isDark() const {
    Rgba bg = background(Surface::Editor);
    bg.a = 1;
    const double lum = (0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b) / 255;
    return lum < 0.5;
}

bool Settings::windowIsOpaque() const {
    if (blur_) return false;
    for (int i = 0; i < kSurfaceCount; i++) {
        Surface s = (Surface)i;
        if (s == Surface::Titlebar && !titlebarKeySet_) continue;
        if (background(s).a < 1.0) return false;
    }
    return true;
}

bool Settings::customTitlebar() const {
    return titlebarKeySet_ || !windowIsOpaque();
}

const std::vector<std::string>& Settings::lspServers() {
    static const std::vector<std::string> servers = {"cpp",  "python",   "go",   "rust", "typescript",
                                                     "latex", "markdown", "toml", "shell"};
    return servers;
}

std::vector<std::string> Settings::lspCustomServers() const {
    std::vector<std::string> out;
    auto add = [&](std::string k) {
        if (k.size() > 5 && k.compare(k.size() - 5, 5, ".also") == 0) k = k.substr(0, k.size() - 5);
        for (const std::string& b : lspServers())
            if (b == k) return;
        if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
    };
    for (const auto& c : lspCommands_) add(c.first);
    for (const auto& e : lspExtensions_) add(e.first);
    std::sort(out.begin(), out.end());
    return out;
}

std::string Settings::lspCommand(const std::string& server) const {
    auto it = lspCommands_.find(server);
    return it == lspCommands_.end() ? "" : it->second;
}

bool Settings::lspOff(const std::string& server) const {
    std::string v = lower(lspCommand(server));
    return v == "off" || v == "none" || v == "false";
}

std::string Settings::runLanguage(const std::string& extension) {
    std::string e = lower(extension);
    if (e == "py" || e == "pyw") return "python";
    if (e == "bash") return "sh";
    if (e == "mjs" || e == "cjs") return "js";
    return e;
}

std::string Settings::runTemplate(const std::string& extension) const {
    std::string e = lower(extension), language = runLanguage(e);
    if (e.empty()) return "";
    std::string command;
    auto set = runCommands_.find(e);
    if (set == runCommands_.end()) set = runCommands_.find(language);
    if (set != runCommands_.end()) {
        command = set->second;
    } else {
        for (const auto& d : kRunDefaults)
            if (language == d.language) command = d.command;
    }
    std::string v = lower(command);
    if (v == "off" || v == "none" || v == "false") return "";
    return command;
}

const std::vector<std::string>& Settings::formatLanguages() {
    // The same keys as Formatter::languages(), which a test checks; kept
    // here so the settings parser needs nothing else from the core.
    static const std::vector<std::string> langs = {"python", "cpp", "go", "rust",
                                                   "shell"};
    return langs;
}

std::string Settings::formatCommand(const std::string& lang) const {
    auto it = formatCommands_.find(lang);
    return it == formatCommands_.end() ? "" : it->second;
}

const char* Settings::defaultFileText() {
    return R"CONF(# MiniCode settings
#
# Changes apply as soon as this file is saved. Each line is key = value, and
# everything here is commented out, so MiniCode uses its built-in defaults
# until you uncomment a line. Remove the leading "# " to use one.
#
# Colors are #RRGGBB, or #RRGGBBAA with an alpha channel. Opacity is a number
# from 0 to 1, or a percentage such as 80%. Opacity applies to a panel's
# background only, so text stays solid while the desktop shows through.

# The color theme, one of VS Code's built-in themes: dark-modern (VS Code's
# own default), dark-plus, monokai, solarized-dark, tomorrow-night-blue,
# light-modern, light-plus, quiet-light, or solarized-light. View > Theme
# sets this line for you. Every color below that you uncomment wins over the
# theme's, so a theme can be adjusted color by color; the colors shown below
# are Dark+'s, the theme used when none is chosen.
# theme = dark-plus

# The whole window. window.opacity is the default for every panel below that
# doesn't set its own, and window.text is the default text color.
# window.opacity = 1
# window.text = #D4D4D4
# Blur what is behind the window, like frosted glass. Pair it with panel
# opacity below 1, or the panels cover it up. macOS only; Linux desktops
# don't offer apps a way to blur behind a window, so it is ignored there.
# window.blur = false
# The blur's look (macOS only): under-window, window, content, titlebar,
# header, sidebar, menu, popover, hud, sheet, fullscreen, or tooltip.
# window.material = under-window

# The title bar across the top of the window, and on Linux the menu bar.
# titlebar.background = #323233
# titlebar.opacity = 1
# titlebar.text = #CCCCCC

# The file tree.
# sidebar.background = #252526
# sidebar.opacity = 1
# sidebar.text = #CCCCCC

# The file editor and Markdown preview.
# editor.background = #1E1E1E
# editor.opacity = 1
# editor.text = #D4D4D4
# A thin line down the editor at this column, to show where lines get long:
# a column, several without spaces (80,120), or off.
# editor.ruler = 100
# editor.ruler-color = #5A5A5A
# The editor's font size in points. Cmd+= and Cmd+- zoom in and out from
# it, and Option+Cmd+0 comes back to it (macOS).
# editor.font-size = 13
# Line numbers beside the editor: on, off, or relative (counted up and down
# from the caret's line, which shows its own number).
# editor.line-numbers = on
# The gutter behind the numbers, which is the editor's background unless set
# here. The caret's line has its number in the active color.
# editor.gutter-background = #1E1E1E
# editor.line-number-color = #858585
# editor.line-number-active-color = #C6C6C6
# The box around the bracket beside the caret and its partner.
# editor.bracket-match = #888888
# Sort the lines between "keep-sorted start" and "keep-sorted end" comments
# every time a file is saved, as Google's keep-sorted tool would.
# editor.keep-sorted-on-save = false
# Organize the imports each time Cmd+S saves a file (macOS), with the file's
# language servers' Organize Imports (Shift+Option+O): Ruff's own for Python
# where it runs. It happens before format.on-save, and never on an auto-save.
# editor.organize-imports-on-save = false
# Save files by themselves (macOS): after-delay writes a file once typing has
# paused for editor.auto-save-delay milliseconds, focus-change when the
# editor loses the keyboard (another pane, tab, window or app). A file
# changed on disk since it was read is never written over by an auto-save;
# saving it by hand asks what to do.
# editor.auto-save = off
# editor.auto-save-delay = 1000

# The terminal panel.
# terminal.background = #181818
# terminal.opacity = 1
# terminal.text = #D4D4D4
# grid: a real terminal, with zsh's own prompt, Tab completion, Ctrl+R and
# the rest of its line editor. line: the older log with an input line below
# it (a fallback, to be removed). Applies to terminals opened after a change.
# terminal.mode = grid
# Lines a terminal keeps above its screen, for scrolling back (0 for none).
# terminal.scrollback = 10000
# The terminal's font size in points. The editor's zoom (Cmd+= and Cmd+-)
# scales it too (macOS).
# terminal.font-size = 12
# Option sends ESC before the key, for the shell's Alt bindings (Option+B and
# Option+F move by words). false lets Option type what your keyboard layout
# puts on it, such as @ and { on many European layouts.
# terminal.option-as-meta = true
# A new terminal starts with the folder's Python environment active, as
# after source .venv/bin/activate: the interpreter Run File uses (the
# folder's choice, python.interpreter, or the folder's own .venv once you
# allow it for the language servers), when it is a virtual environment.
# terminal.activate-venv = true

# Find in Folder, Replace in Folder, TODOs and Go to File leave out what the
# folder's .gitignore and .ignore files ignore. Dotfiles are always included.
# search.use-ignore-files = true

# The browser's toolbar. Web pages draw their own background.
# browser.background = #2A2A2A
# browser.opacity = 1
# browser.text = #D4D4D4

# The status bar along the bottom.
# statusbar.background = #007ACC
# statusbar.opacity = 1
# statusbar.text = #FFFFFF

# Syntax highlighting. Plain text uses editor.text.
# syntax.keyword = #569CD6
# syntax.type = #4EC9B0
# syntax.string = #CE9178
# syntax.comment = #6A9955
# syntax.number = #B5CEA8
# syntax.preprocessor = #C586C0
# syntax.function = #DCDCAA

# Rendered Markdown. Body text uses editor.text.
# markdown.heading = #FFFFFF
# markdown.link = #4EA1F7
# markdown.code = #CE9178
# markdown.quote = #9CA3AF
# A checked task's text ("- [x] ..."), which is also struck through.
# markdown.done = #858585
# Pictures in Markdown with an https address are downloaded and shown. Turn
# this off to show their alt text instead, so opening a file never contacts
# the sites it links to.
# markdown.web-images = true
# Math ($x^2$, $$...$$ and ```math blocks) is typeset with tectonic, the
# engine the LaTeX preview uses (macOS only for now). Turn this off to show
# formulas as their TeX instead.
# markdown.math = true
# ```mermaid blocks are drawn as diagrams with mermaid-cli's mmdc when it is
# installed (npm install -g @mermaid-js/mermaid-cli; macOS only for now),
# else shown as their source. Turn this off to always show the source.
# markdown.mermaid = true

# Language servers, for completion, go to definition and error underlines
# (macOS only for now). MiniCode only talks to them, so install the ones you
# want; clangd comes with the Xcode command line tools. When a file of that
# language is opened, its server is looked for on PATH and in the Homebrew,
# npm, pipx, Cargo and Go folders. Give a command to use a different server
# or an absolute path, or off to leave a language alone. For Python,
# basedpyright is tried first, then pyright, pylsp and jedi-language-server.
#
# The status bar shows the open file's server: click it, or choose Language
# Servers... in the MiniCode menu, to see each server's state, where it was
# found and how to install a missing one, and to restart or stop it. A file
# whose server is missing says so in a line over the editor, with the
# command to install it. The README's "Installing servers and formatters"
# section lists every command.
# lsp.enabled = true
# lsp.cpp = clangd
# lsp.python = basedpyright-langserver --stdio
# lsp.go = gopls
# lsp.rust = rust-analyzer
# lsp.typescript = typescript-language-server --stdio
# These run only when installed, and are never offered for installing
# unasked (the README says how): texlab for LaTeX and BibTeX, marksman for
# Markdown, taplo for TOML, and bash-language-server for shell scripts.
# lsp.latex = texlab
# lsp.markdown = marksman server
# lsp.toml = taplo lsp stdio
# lsp.shell = bash-language-server start
# Any other server: give it a name of your own, its command, and the
# extensions of the files it is for, as in
#   lsp.yaml = yaml-language-server --stdio
#   lsp.yaml.extensions = yaml yml
# (the name is also the languageId it is told). lsp.<server>.extensions
# also gives a built-in server more files (lsp.python.extensions = pyw).
# A second server can run beside a language's main one, for its problems
# and quick fixes: lsp.<language>.also, a command like the ones above, or
# off. The main server still answers completion, hover, definitions,
# references and rename. For Python it is Ruff's own server (ruff server)
# when ruff is installed and the project configures Ruff, in a ruff.toml, a
# .ruff.toml or a [tool.ruff] table in pyproject.toml; auto keeps that rule.
# Organize Imports (Shift+Option+O) asks every server of the file.
# lsp.python.also = auto

# Run File (Shift+Cmd+Return) types a command for the open file into the
# terminal, as if you had typed it (macOS only for now). A terminal busy with
# a command is never typed into: the file runs in a new terminal instead, and
# setting run.terminal to new opens a new terminal for every run.
# run.<language> is the command for a language, or for a file extension
# (run.py beats run.python),
# with {file}, {dir}, {root}, {name} and {python} filled in and quoted.
# {python} is the Python the Python server uses: the folder's choice from
# Select Python Interpreter, else python.interpreter, else a .venv, venv or
# env folder beside the file or above it in the project, else python3. Add
# your own (for Lua, a line reading run.lua = lua {file}), or set one to off.
# run.terminal = current
# run.python = {python} {file}
# run.sh = bash {file}
# run.go = go run {file}
# run.js = node {file}
# run.rb = ruby {file}
# Format Document (Shift+Option+F) runs an external formatter on the file
# (macOS only for now). Install the ones you want. When a project configures
# its formatter (.clang-format, [tool.pyink] or [tool.black] in
# pyproject.toml, .style.yapf, ruff.toml, rustfmt.toml, an .editorconfig for
# shell), that formatter runs with the project's style. Otherwise Google's
# style is used: clang-format for C, C++, Objective-C, Java, JavaScript,
# TypeScript, Protocol Buffers and C#; pyink, or yapf --style=google when
# pyink is not installed; gofmt; rustfmt with the edition from Cargo.toml;
# shfmt. Python tools are looked for in the project's .venv or venv first.
# Giving a command here runs exactly that, whatever the project says; {file}
# stands for the file's path. off leaves a language alone. Google's internal
# 2-space Python is pyink --pyink-indentation 2 with the options below.
# A missing formatter's note says how to install it, and so does the
# README's "Installing servers and formatters" section.
# format.python = pyink --line-length 80 --pyink-use-majority-quotes --quiet --stdin-filename {file} -
# format.cpp = clang-format --style=file --fallback-style=Google --assume-filename={file}
# format.go = gofmt
# format.rust = rustfmt --edition 2021
# format.shell = shfmt -i 2 -ci -bn --filename {file}
# Format the file each time Cmd+S saves it.
# format.on-save = false
# The Python the Python server analyses your code with, so it finds the
# packages installed there. auto picks the folder's .venv, venv or env, then
# $VIRTUAL_ENV; a path (such as ~/envs/work/bin/python) is used for every
# folder. Select Python Interpreter... in the MiniCode menu chooses one for
# the open folder alone, which wins over this.
# python.interpreter = auto

# Source Control (Ctrl+Shift+G, macOS only for now). With autofetch on, the
# folder's repository is fetched in the background every
# git.autofetch-period seconds (180 at least) while its window is in front,
# as VS Code's git.autofetch does, so the panel's "to pull" count is current.
# A repository with no remote is never fetched, and one that cannot be (no
# network, a missing password) is tried less and less often, up to once an
# hour. Fetch, Pull, Push and Sync are in the panel's menu and the command
# palette; none of them ever asks for a password.
# git.autofetch = false
# git.autofetch-period = 180

# Agents can check pages in the browser pane (minicode browse and minicode
# page, macOS only for now): they load into a page of their own beside
# yours, and read its text, its console and pictures of it. Those reads work
# only for local pages (localhost, 127.0.0.1, ::1, *.localhost and file://),
# so an agent never reads a site you are logged in to. Set this to true to
# let them read any page.
# control.page-any-origin = false

# Request a Change to minicode (Shift+Cmd+K, macOS only for now) hands what
# you describe to Claude Code, in a terminal tab opened in minicode's own
# source folder, and offers to restart into the new build once Claude has
# rebuilt it with make. develop.command is the command it runs, with your
# request added at the end; add flags here (such as --model). develop.source
# is the source folder, a clone of github.com/e-c-hansen/minicode; auto uses
# the folder this minicode was built in, else clones the release you are
# running into ~/Library/Application Support/MiniCode/source on first use.
# develop.command = claude
# develop.source = auto

# The inline agent (Cmd+I, macOS only for now) asks Claude Code about
# whatever has the keyboard. In a file it changes the code at the cursor or
# the selection, as an edit you keep with Return or throw away with Esc; in
# a terminal it writes a command and types it at the prompt for you to run;
# anywhere else, and with Cmd+Return from any of them, it hands the request
# to Claude Code in a new terminal tab of the open folder. agent.command is
# the command it runs (add flags such as --model here); the answers in the
# bar run it in print mode, allowed to read the project but not to change it.
# agent.command = claude

# The Claude Code IDE link: Claude Code's /ide command, which VS Code and
# JetBrains answer, answered by each minicode window (macOS only for now).
# On, Claude Code started in a window's folder, or in one of its terminals
# (which it then joins by itself), sees the file and the selection you have
# in the editor and the language servers' problems, opens files in a tab
# beside yours, and can offer an edit as a diff you accept or reject. Each
# window listens on 127.0.0.1 only, with a secret it writes for Claude Code
# to read in ~/.claude/ide, and removes when it closes. View > Claude Code
# IDE Link turns it on and off too.
# agent.ide-link = off
)CONF";
}
