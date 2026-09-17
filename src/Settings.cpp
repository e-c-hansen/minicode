// Settings.cpp — see Settings.h.
#include "Settings.h"
#include <cctype>

namespace {

struct SurfaceDefaults {
    const char* name;
    uint32_t background, text;
};
// Indexed by Surface. VS Code Dark+ values, matching the built-in look.
const SurfaceDefaults kSurfaces[kSurfaceCount] = {
    {"titlebar",  0x323233, 0xCCCCCC},
    {"sidebar",   0x252526, 0xCCCCCC},
    {"editor",    0x1E1E1E, 0xD4D4D4},
    {"terminal",  0x181818, 0xD4D4D4},
    {"browser",   0x2A2A2A, 0xD4D4D4},
    {"statusbar", 0x007ACC, 0xFFFFFF},
};

struct NamedColor {
    const char* name;
    uint32_t color;
};
// Indexed by TokenStyle; Plain has no key of its own (it is editor.text).
const NamedColor kSyntax[8] = {
    {nullptr,        0xD4D4D4},
    {"keyword",      0x569CD6},
    {"type",         0x4EC9B0},
    {"string",       0xCE9178},
    {"comment",      0x6A9955},
    {"number",       0xB5CEA8},
    {"preprocessor", 0xC586C0},
    {"function",     0xDCDCAA},
};
// Indexed by MarkdownColor.
const NamedColor kMarkdown[kMarkdownColorCount] = {
    {"heading", 0xFFFFFF},
    {"link",    0x4EA1F7},
    {"code",    0xCE9178},
    {"quote",   0x9CA3AF},
};

const char* const kMaterials[] = {
    "titlebar", "sidebar", "menu", "popover", "hud", "sheet", "window",
    "under-window", "header", "content", "fullscreen", "tooltip",
};

const uint32_t kCodeBackground = 0x2A2A2A;
const uint32_t kTerminalInputBackground = 0x232323;
const uint32_t kTerminalInputText = 0xEDEDED;

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

const char* Settings::surfaceName(Surface s) { return kSurfaces[(int)s].name; }

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

std::u16string Settings::setColor(const std::u16string& line, const Rgba& c) {
    ColorSpan span;
    if (!findColor(line, span)) return line;
    std::string hex = formatColor(c);
    std::u16string out = line.substr(0, span.start) +
                         std::u16string(hex.begin(), hex.end()) +
                         line.substr(span.start + span.length);

    // "# key = value" -> "key = value", keeping any indentation. Only when
    // what follows the # is a plain key, so prose is never touched.
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

Settings Settings::parse(const std::string& text,
                         std::vector<SettingsError>* errors) {
    Settings st;
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
    return st;
}

bool Settings::apply(const std::string& key, const std::string& value,
                     std::string& error) {
    Rgba color;
    double number = 1;
    auto opacityValue = [&]() {
        if (parseOpacity(value, number)) return true;
        error = "'" + value + "' is not an opacity (use 0 to 1, or a "
                "percentage like 80%)";
        return false;
    };

    if (key == "window.opacity") {
        if (!opacityValue()) return false;
        windowOpacity_ = number;
        return true;
    }
    if (key == "window.text") {
        if (!parseColor(value, color)) { error = colorError(value); return false; }
        windowText_ = color;
        return true;
    }
    if (key == "window.blur") {
        if (!parseBool(value, blur_)) {
            error = "'" + value + "' is not true or false";
            return false;
        }
        return true;
    }
    if (key == "window.material") {
        std::string m = lower(value);
        for (const char* name : kMaterials)
            if (m == name) { material_ = m; return true; }
        error = "unknown material '" + value + "'";
        return false;
    }

    size_t dot = key.find('.');
    if (dot != std::string::npos) {
        std::string group = key.substr(0, dot), field = key.substr(dot + 1);
        for (int i = 0; i < kSurfaceCount; i++) {
            if (group != kSurfaces[i].name) continue;
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
            const NamedColor* table = syntax ? kSyntax : kMarkdown;
            int count = syntax ? 8 : kMarkdownColorCount;
            for (int i = 0; i < count; i++) {
                if (!table[i].name || field != table[i].name) continue;
                if (!parseColor(value, color)) { error = colorError(value); return false; }
                (syntax ? syntax_[i] : markdown_[i]) = color;
                return true;
            }
        }
    }
    error = "unknown setting '" + key + "'";
    return false;
}

double Settings::opacity(Surface s) const {
    const SurfaceSettings& ss = surfaces_[(int)s];
    if (ss.opacity) return *ss.opacity;
    return windowOpacity_ ? *windowOpacity_ : 1.0;
}

Rgba Settings::background(Surface s) const {
    const SurfaceSettings& ss = surfaces_[(int)s];
    Rgba c = ss.background ? *ss.background : Rgba::hex(kSurfaces[(int)s].background);
    c.a *= opacity(s);
    return c;
}

Rgba Settings::text(Surface s) const {
    const SurfaceSettings& ss = surfaces_[(int)s];
    if (ss.text) return *ss.text;
    if (windowText_) return *windowText_;
    return Rgba::hex(kSurfaces[(int)s].text);
}

bool Settings::textIsSet(Surface s) const {
    return surfaces_[(int)s].text.has_value() || windowText_.has_value();
}

Rgba Settings::syntax(TokenStyle s) const {
    int i = (int)s;
    if (i <= 0 || i >= 8) return text(Surface::Editor);
    return syntax_[i] ? *syntax_[i] : Rgba::hex(kSyntax[i].color);
}

Rgba Settings::markdown(MarkdownColor c) const {
    int i = (int)c;
    return markdown_[i] ? *markdown_[i] : Rgba::hex(kMarkdown[i].color);
}

Rgba Settings::markdownCodeBackground() const {
    return Rgba::hex(kCodeBackground, opacity(Surface::Editor));
}

Rgba Settings::terminalInputBackground() const {
    return Rgba::hex(kTerminalInputBackground, opacity(Surface::Terminal));
}

Rgba Settings::terminalInputText() const {
    return textIsSet(Surface::Terminal) ? text(Surface::Terminal)
                                        : Rgba::hex(kTerminalInputText);
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

# The whole window. window.opacity is the default for every panel below that
# doesn't set its own, and window.text is the default text color.
# window.opacity = 1
# window.text = #D4D4D4
# Blur what is behind the window, like frosted glass. Pair it with panel
# opacity below 1, or the panels cover it up.
# window.blur = false
# The blur's look: under-window, window, content, titlebar, header, sidebar,
# menu, popover, hud, sheet, fullscreen, or tooltip.
# window.material = under-window

# The title bar across the top of the window.
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

# The terminal panel.
# terminal.background = #181818
# terminal.opacity = 1
# terminal.text = #D4D4D4

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
)CONF";
}
