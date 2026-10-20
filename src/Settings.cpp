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
    {"done",    0x858585},
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

const uint32_t kCodeBackground = 0x2A2A2A;
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

static std::u16string uncommentSetting(const std::u16string& out);

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

        // A path may hold spaces, so it runs to the end of the line or to a
        // " #" comment. Empty, or auto, is the automatic choice.
        if (key == "python.interpreter") {
            size_t hash = rest.find(" #");
            std::string path = trim(hash == std::string::npos ? rest : rest.substr(0, hash));
            if (!path.empty() && path[0] == '#') path.clear();
            st.pythonInterpreter_ = lower(path) == "auto" ? "" : path;
            continue;
        }
        // A language server, run or formatter command is a whole command
        // line, so it runs to the end of the line or to a " #" comment.
        if ((key.rfind("lsp.", 0) == 0 && key != "lsp.enabled") ||
            (key.rfind("run.", 0) == 0 && key != "run.terminal") ||
            (key.rfind("format.", 0) == 0 && key != "format.on-save")) {
            size_t hash = rest.find(" #");
            std::string command = trim(hash == std::string::npos ? rest
                                                                 : rest.substr(0, hash));
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

    if (key == "markdown.web-images") {
        if (!parseBool(value, webImages_)) {
            error = "'" + value + "' is not true or false";
            return false;
        }
        return true;
    }
    if (key == "markdown.math") {
        if (!parseBool(value, math_)) {
            error = "'" + value + "' is not true or false";
            return false;
        }
        return true;
    }
    if (key == "editor.keep-sorted-on-save") {
        if (!parseBool(value, keepSortedOnSave_)) {
            error = "'" + value + "' is not true or false";
            return false;
        }
        return true;
    }
    if (key == "lsp.enabled") {
        if (!parseBool(value, lspEnabled_)) {
            error = "'" + value + "' is not true or false";
            return false;
        }
        return true;
    }
    if (key == "run.terminal") {
        std::string v = lower(value);
        if (v != "current" && v != "new") {
            error = "'" + value + "' is not current or new";
            return false;
        }
        runInNewTerminal_ = v == "new";
        return true;
    }
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
        for (const std::string& known : lspServers())
            if (server == known) { lspCommands_[server] = value; return true; }
        error = "unknown setting '" + key + "' (language servers are lsp.cpp, "
                "lsp.python, lsp.go, lsp.rust and lsp.typescript)";
        return false;
    }

    if (key == "format.on-save") {
        if (!parseBool(value, formatOnSave_)) {
            error = "'" + value + "' is not true or false";
            return false;
        }
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

const std::vector<std::string>& Settings::lspServers() {
    static const std::vector<std::string> servers = {"cpp", "python", "go", "rust",
                                                     "typescript"};
    return servers;
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
# Sort the lines between "keep-sorted start" and "keep-sorted end" comments
# every time a file is saved, as Google's keep-sorted tool would.
# editor.keep-sorted-on-save = false

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

# Language servers, for completion, go to definition and error underlines
# (macOS only for now). MiniCode only talks to them, so install the ones you
# want; clangd comes with the Xcode command line tools. When a file of that
# language is opened, its server is looked for on PATH and in the Homebrew,
# npm, pipx, Cargo and Go folders. Give a command to use a different server
# or an absolute path, or off to leave a language alone. For Python,
# basedpyright, pylsp and jedi-language-server are tried, in that order, when
# pyright is not installed.
#
# The status bar shows the open file's server: click it, or choose Language
# Servers... in the MiniCode menu, to see each server's state, where it was
# found and how to install a missing one, and to restart or stop it.
# lsp.enabled = true
# lsp.cpp = clangd
# lsp.python = pyright-langserver --stdio
# lsp.go = gopls
# lsp.rust = rust-analyzer
# lsp.typescript = typescript-language-server --stdio

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
)CONF";
}
