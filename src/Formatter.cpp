// Formatter.cpp — see Formatter.h.
#include "Formatter.h"
#include "InstallHints.h"
#include "Json.h"
#include "LspClient.h"   // Lsp::splitCommand, shared with lsp.* commands
#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace Formatter {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string extensionOf(const std::string& path) {
    size_t slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    return lower(name.substr(dot + 1));
}

std::string parentOf(const std::string& path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) return "";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

// A project file that says a tool is configured: the file itself, or a
// table/section in it.
enum class Kind { Any, TomlTable, IniSection, EditorConfigShell, JsonKey, JsonDependency };
struct Marker {
    const char* file;
    Kind kind;
    const char* section;
};

struct Tool {
    const char* program;
    const char* google;     // the command when the project configures nothing
    const char* project;    // the command when it does
    std::vector<Marker> markers;
    bool byDefault;         // tried without project configuration
    // The extensions it formats, when only some of the language's (empty: all).
    std::vector<const char*> only = {};
};

// The JavaScript and TypeScript extensions, which Prettier and Biome format
// in place of clang-format when the project configures them.
#define MC_JS_EXTENSIONS {"js", "mjs", "cjs", "jsx", "ts", "mts", "cts", "tsx"}

// What says a project formats with Prettier: its configuration files, a
// "prettier" key in package.json, or Prettier among the packages it
// installs (Prettier runs with its defaults when there is no file).
const std::vector<Marker>& prettierMarkers() {
    static const std::vector<Marker> m = {
        {".prettierrc", Kind::Any, ""},
        {".prettierrc.json", Kind::Any, ""},
        {".prettierrc.json5", Kind::Any, ""},
        {".prettierrc.yaml", Kind::Any, ""},
        {".prettierrc.yml", Kind::Any, ""},
        {".prettierrc.toml", Kind::Any, ""},
        {".prettierrc.js", Kind::Any, ""},
        {".prettierrc.cjs", Kind::Any, ""},
        {".prettierrc.mjs", Kind::Any, ""},
        {".prettierrc.ts", Kind::Any, ""},
        {".prettierrc.cts", Kind::Any, ""},
        {".prettierrc.mts", Kind::Any, ""},
        {"prettier.config.js", Kind::Any, ""},
        {"prettier.config.cjs", Kind::Any, ""},
        {"prettier.config.mjs", Kind::Any, ""},
        {"prettier.config.ts", Kind::Any, ""},
        {"prettier.config.cts", Kind::Any, ""},
        {"prettier.config.mts", Kind::Any, ""},
        {"package.json", Kind::JsonKey, "prettier"},
        {"package.json", Kind::JsonDependency, "prettier"},
        {"node_modules/.bin/prettier", Kind::Any, ""},
    };
    return m;
}

const std::vector<Marker>& biomeMarkers() {
    static const std::vector<Marker> m = {{"biome.json", Kind::Any, ""},
                                          {"biome.jsonc", Kind::Any, ""}};
    return m;
}

const char* const kPrettierCommand = "prettier --stdin-filepath {file}";
const char* const kBiomeCommand = "biome format --stdin-file-path={file}";
// The user's own Prettier where no project configures it (format.<lang> =
// prettier): Markdown's prose keeps its line breaks unless a configuration
// file Prettier finds says otherwise.
const char* const kPrettierOwnCommand =
    "prettier --stdin-filepath {file} --prose-wrap preserve --config-precedence prefer-file";

struct Language {
    const char* key;
    const char* display;
    std::vector<const char*> extensions;
    std::vector<Tool> tools;
};

// Google's Python style guide asks for 80 columns and one quote character
// used consistently within a file, with 4-space indents. pyink is the
// formatter Google maintains for its Python (a Black fork); at its default
// indent and these options it formats to the published guide. yapf's
// built-in google style is the long-standing alternative and agrees on
// both. Google's internal 2-space indent is --pyink-indentation 2.
const std::vector<Language>& table() {
    static const std::vector<Language> langs = {
        {"python", "Python", {"py", "pyi", "pyw"},
         {
             {"pyink",
              "pyink --line-length 80 --pyink-use-majority-quotes --quiet "
              "--stdin-filename {file} -",
              "pyink --quiet --stdin-filename {file} -",
              {{"pyproject.toml", Kind::TomlTable, "tool.pyink"}},
              true},
             {"yapf", "yapf --style=google", "yapf",
              {{".style.yapf", Kind::Any, ""},
               {"pyproject.toml", Kind::TomlTable, "tool.yapf"},
               {"setup.cfg", Kind::IniSection, "yapf"}},
              true},
             {"black", "", "black --quiet --stdin-filename {file} -",
              {{"pyproject.toml", Kind::TomlTable, "tool.black"}},
              false},
             // Any [tool.ruff] table counts, lint settings alone included: a
             // project that lints with Ruff almost always formats with it, and
             // its line length lives in [tool.ruff]. A pyink, yapf or black
             // table in the same folder still wins, being checked first.
             {"ruff", "", "ruff format --stdin-filename {file} -",
              {{"ruff.toml", Kind::Any, ""},
               {".ruff.toml", Kind::Any, ""},
               {"pyproject.toml", Kind::TomlTable, "tool.ruff"}},
              false},
         }},
        {"cpp", "C-family",
         {"c", "h", "cc", "cpp", "cxx", "c++", "hh", "hpp", "hxx", "h++", "ipp",
          "inl", "tpp", "cu", "cuh", "m", "mm", "java", "js", "mjs", "cjs", "jsx",
          "ts", "mts", "cts", "tsx", "proto", "cs"},
         {
             // A JavaScript or TypeScript project that configures Prettier or
             // Biome formats with it. They come before clang-format, so a
             // folder holding a .prettierrc and a .clang-format (a project
             // with both kinds of code) formats its scripts with Prettier.
             {"prettier", "", kPrettierCommand, prettierMarkers(), false, MC_JS_EXTENSIONS},
             {"biome", "", kBiomeCommand, biomeMarkers(), false, MC_JS_EXTENSIONS},
             // --style=file uses the nearest .clang-format and falls back to
             // Google's style when the project has none.
             {"clang-format",
              "clang-format --style=file --fallback-style=Google "
              "--assume-filename={file}",
              "clang-format --style=file --fallback-style=Google "
              "--assume-filename={file}",
              {{".clang-format", Kind::Any, ""}, {"_clang-format", Kind::Any, ""}},
              true},
         }},
        {"go", "Go", {"go"},
         {{"gofmt", "gofmt", "gofmt", {}, true}}},
        {"rust", "Rust", {"rs"},
         {{"rustfmt", "rustfmt --edition 2021", "rustfmt --edition 2021",
           {{"rustfmt.toml", Kind::Any, ""}, {".rustfmt.toml", Kind::Any, ""}},
           true}}},
        // Google's shell style: 2-space indents, case patterns indented,
        // a pipeline split with the | starting each continued line.
        {"shell", "shell", {"sh", "bash", "ksh", "bats"},
         {{"shfmt", "shfmt -i 2 -ci -bn --filename {file}", "shfmt --filename {file}",
           {{".editorconfig", Kind::EditorConfigShell, ""}},
           true}}},
        // Markdown, JSON, YAML and CSS have no Google formatter: they are
        // formatted only where the project sets up Prettier (or Biome, for
        // the JSON and CSS it formats), with the project's configuration.
        {"markdown", "Markdown", {"md", "markdown"},
         {{"prettier", "", kPrettierCommand, prettierMarkers(), false}}},
        {"json", "JSON", {"json", "jsonc", "json5"},
         {{"prettier", "", kPrettierCommand, prettierMarkers(), false},
          {"biome", "", kBiomeCommand, biomeMarkers(), false, {"json", "jsonc"}}}},
        {"yaml", "YAML", {"yaml", "yml"},
         {{"prettier", "", kPrettierCommand, prettierMarkers(), false}}},
        {"css", "CSS", {"css", "scss", "less"},
         {{"prettier", "", kPrettierCommand, prettierMarkers(), false},
          {"biome", "", kBiomeCommand, biomeMarkers(), false, {"css"}}}},
        // latexindent, which TeX distributions and Homebrew ship. A project's
        // .latexindent.yaml or localSettings.yaml (the names its -l looks
        // for) is its style, given by path, since the text comes on stdin.
        // -g /dev/null: no indent.log in the project's folder.
        {"latex", "LaTeX", {"tex", "ltx", "sty", "cls"},
         {{"latexindent", "latexindent -g /dev/null", "latexindent -g /dev/null -l={config}",
           {{".latexindent.yaml", Kind::Any, ""},
            {"latexindent.yaml", Kind::Any, ""},
            {"localSettings.yaml", Kind::Any, ""},
            {".localSettings.yaml", Kind::Any, ""}},
           true}}},
    };
    return langs;
}

const Language* findLanguage(const std::string& key) {
    for (const Language& l : table())
        if (key == l.key) return &l;
    return nullptr;
}

// The best way to install a formatter, from the one table (InstallHints).
std::string hintFor(const std::string& program) {
    const Install::Tool* t = Install::formatter(program);
    return t && !t->ways.empty() ? t->ways.front().command : "";
}

bool appliesTo(const Tool& t, const std::string& path) {
    if (t.only.empty()) return true;
    std::string ext = extensionOf(path);
    for (const char* e : t.only)
        if (ext == e) return true;
    return false;
}

bool markerMatches(const Marker& m, const std::string& dir, const Fs& fs,
                   std::string& file) {
    file = join(dir, m.file);
    if (!fs.exists || !fs.exists(file)) return false;
    if (m.kind == Kind::Any) return true;
    std::string text = fs.read ? fs.read(file) : "";
    switch (m.kind) {
        case Kind::TomlTable: return tomlHasTable(text, m.section);
        case Kind::IniSection: return iniHasSection(text, m.section);
        case Kind::EditorConfigShell: return editorconfigCoversShell(text);
        case Kind::JsonKey: return jsonHasKey(text, m.section);
        case Kind::JsonDependency: return jsonHasDependency(text, m.section);
        case Kind::Any: break;
    }
    return true;
}

// The folders searched for configuration, nearest first.
std::vector<std::string> searchDirs(const std::string& path, const std::string& root,
                                    const Fs& fs) {
    std::vector<std::string> dirs;
    std::string r = root;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    std::string dir = parentOf(path);
    while (!dir.empty()) {
        dirs.push_back(dir);
        if (dir == r || dir == "/") break;
        if (fs.exists && fs.exists(join(dir, ".git"))) break;
        dir = parentOf(dir);
    }
    return dirs;
}

// Each line of a TOML or INI file that opens a table: its name, trimmed.
std::vector<std::string> headers(const std::string& text, bool toml) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.size() < 3 || line[0] != '[') continue;
        bool array = toml && line[1] == '[';
        size_t open = array ? 2 : 1;
        size_t close = line.find(']', open);
        if (close == std::string::npos) continue;
        std::string name = trim(line.substr(open, close - open));
        if (toml) {   // [ tool . pyink ] and quoted keys: compare without either
            std::string bare;
            for (char c : name)
                if (c != ' ' && c != '\t' && c != '"' && c != '\'') bare += c;
            name = bare;
        }
        out.push_back(name);
    }
    return out;
}

}  // namespace

std::string languageFor(const std::string& path) {
    std::string ext = extensionOf(path);
    if (ext.empty()) return "";
    for (const Language& l : table())
        for (const char* e : l.extensions)
            if (ext == e) return l.key;
    return "";
}

const std::vector<std::string>& languages() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (const Language& l : table()) k.push_back(l.key);
        return k;
    }();
    return keys;
}

std::string displayName(const std::string& lang) {
    const Language* l = findLanguage(lang);
    return l ? l->display : lang;
}

std::string defaultCommand(const std::string& lang) {
    const Language* l = findLanguage(lang);
    if (!l) return "";
    for (const Tool& t : l->tools)
        if (t.byDefault) return t.google;
    return "";
}

Plan plan(const std::string& lang, const std::string& setting,
          const std::string& path, const std::string& root, const Fs& fs) {
    Plan p;
    const Language* l = findLanguage(lang);
    if (!l) return p;

    std::string value = trim(setting);
    if (!value.empty()) {
        std::string v = lower(value);
        if (v == "off" || v == "none" || v == "false") { p.off = true; return p; }
        std::vector<std::string> words = Lsp::splitCommand(value);
        if (words.empty()) return p;
        p.fromSettings = true;
        // format.markdown = prettier: the user's own Prettier for a language
        // Prettier formats, where no project set it up too. A project's
        // configuration still applies where there is one (prefer-file lets
        // a configuration file Prettier finds win over the options given
        // here); elsewhere Markdown's prose keeps its line breaks.
        if (words.size() == 1 && v == "prettier") {
            for (const Tool& t : l->tools) {
                if (std::string(t.program) != "prettier" || !appliesTo(t, path)) continue;
                for (const std::string& dir : searchDirs(path, root, fs)) {
                    for (const Marker& m : t.markers) {
                        std::string file;
                        if (markerMatches(m, dir, fs, file)) {
                            p.configFile = file;
                            break;
                        }
                    }
                    if (!p.configFile.empty()) break;
                }
                p.candidates.push_back({"prettier",
                                        p.configFile.empty() ? kPrettierOwnCommand
                                                             : kPrettierCommand,
                                        !p.configFile.empty()});
                p.installHint = hintFor("prettier");
                return p;
            }
        }
        p.candidates.push_back({words[0], value, false});
        p.installHint = hintFor(words[0]);
        return p;
    }

    // The nearest folder that configures one of the language's tools decides.
    const Tool* chosen = nullptr;
    for (const std::string& dir : searchDirs(path, root, fs)) {
        for (const Tool& t : l->tools) {
            if (!appliesTo(t, path)) continue;
            for (const Marker& m : t.markers) {
                std::string file;
                if (markerMatches(m, dir, fs, file)) {
                    chosen = &t;
                    p.configFile = file;
                    break;
                }
            }
            if (chosen) break;
        }
        if (chosen) break;
    }

    if (chosen) {
        p.candidates.push_back({chosen->program, chosen->project, true});
        p.installHint = hintFor(chosen->program);
    } else {
        for (const Tool& t : l->tools) {
            if (!t.byDefault || !appliesTo(t, path)) continue;
            p.candidates.push_back({t.program, t.google, false});
            if (p.installHint.empty()) p.installHint = hintFor(t.program);
        }
    }

    // rustfmt needs the crate's edition to parse it, which Cargo.toml has.
    if (lang == "rust") {
        for (const std::string& dir : searchDirs(path, root, fs)) {
            std::string cargo = join(dir, "Cargo.toml");
            if (!fs.exists || !fs.exists(cargo)) continue;
            std::string edition = cargoEdition(fs.read ? fs.read(cargo) : "");
            for (Candidate& c : p.candidates) {
                size_t at = c.command.find("--edition 2021");
                if (at != std::string::npos)
                    c.command.replace(at, 14, "--edition " + edition);
            }
            break;
        }
    }
    return p;
}

// The folders whose project tools a file may use, nearest first: up to the
// open folder; a file outside it has only its own folder searched, so a
// stray .venv or node_modules higher up never supplies the tools.
static std::vector<std::string> toolFolders(const std::string& path, const std::string& root) {
    std::string r = root;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    std::string folder = parentOf(path);
    bool inside = !r.empty() && (folder == r || folder.rfind(r == "/" ? r : r + "/", 0) == 0);
    std::vector<std::string> dirs;
    if (inside) {
        Fs none;
        dirs = searchDirs(path, r, none);
    } else if (!folder.empty()) {
        dirs.push_back(folder);
    }
    return dirs;
}

std::string configFile(const std::string& program, const std::string& path,
                       const std::string& root, const Fs& fs) {
    for (const std::string& dir : searchDirs(path, root, fs))
        for (const Language& l : table())
            for (const Tool& t : l.tools) {
                if (program != t.program) continue;
                for (const Marker& m : t.markers) {
                    std::string file;
                    if (markerMatches(m, dir, fs, file)) return file;
                }
            }
    return "";
}

std::vector<std::string> venvBinDirs(const std::string& path, const std::string& root,
                                     const std::string& virtualEnv) {
    std::vector<std::string> out;
    for (const std::string& dir : toolFolders(path, root)) {
        out.push_back(join(dir, ".venv/bin"));
        out.push_back(join(dir, "venv/bin"));
    }
    if (!virtualEnv.empty()) {
        std::string bin = join(virtualEnv, "bin");
        if (std::find(out.begin(), out.end(), bin) == out.end()) out.push_back(bin);
    }
    return out;
}

std::vector<std::string> nodeBinDirs(const std::string& path, const std::string& root) {
    std::vector<std::string> out;
    for (const std::string& dir : toolFolders(path, root))
        out.push_back(join(dir, "node_modules/.bin"));
    return out;
}

bool fromNodeModules(const std::string& program) {
    std::string name = programName(program);
    return name == "prettier" || name == "biome";
}

bool insideFolder(const std::string& path, const std::string& root) {
    if (root.empty() || path.empty() || path[0] != '/' || root[0] != '/') return false;
    std::string r = root;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    if (r == "/") return true;
    return path == r || path.rfind(r + "/", 0) == 0;
}

Source chooseProgram(bool fromSettings, const std::string& local, const std::string& outside,
                     const std::string& consent, bool configIsCode) {
    if (configIsCode && (!local.empty() || !outside.empty()) && consent != "allow")
        return consent == "deny" ? Source::Refused : Source::Ask;
    if (local.empty()) return outside.empty() ? Source::None : Source::Outside;
    if (consent == "allow") return Source::Local;
    if (consent == "deny" || (fromSettings && !outside.empty()))
        return outside.empty() ? Source::None : Source::Outside;
    return Source::Ask;
}

namespace {

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Copies `text` from `i`, a string's opening quote, through its closing one.
size_t copyString(const std::string& text, size_t i, std::string& out) {
    out += text[i++];
    while (i < text.size()) {
        const char c = text[i++];
        out += c;
        if (c == '\\' && i < text.size()) out += text[i++];
        else if (c == '"') break;
    }
    return i;
}

// JSON with // and /* */ comments and trailing commas (biome.jsonc, a
// commented .prettierrc) as plain JSON, so Json::parse reads it.
std::string plainJson(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '"') {
            i = copyString(text, i, out);
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') i++;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 2;
            out += ' ';
        } else {
            out += c;
            i++;
        }
    }
    std::string clean;   // without trailing commas
    clean.reserve(out.size());
    for (size_t i = 0; i < out.size();) {
        if (out[i] == '"') {
            i = copyString(out, i, clean);
            continue;
        }
        if (out[i] == ',') {
            size_t k = i + 1;
            while (k < out.size() && std::isspace((unsigned char)out[k])) k++;
            if (k < out.size() && (out[k] == '}' || out[k] == ']')) {
                i++;
                continue;
            }
        }
        clean += out[i++];
    }
    return clean;
}

// A plugins list with something in it, or (Prettier 2) a parser given by
// path, anywhere in a parsed configuration, its overrides included.
bool jsonNamesCode(const Json& j, bool prettier, int depth = 0) {
    if (depth > 32) return true;   // deeper than any real configuration: do not guess
    if (j.isArray()) {
        for (const Json& e : j.elements())
            if (jsonNamesCode(e, prettier, depth + 1)) return true;
        return false;
    }
    if (!j.isObject()) return false;
    for (const auto& kv : j.items()) {
        const Json& v = kv.second;
        if (kv.first == "plugins" && !v.isNull() && !(v.isArray() && v.size() == 0) &&
            !(v.isString() && v.asString().empty()))
            return true;
        if (prettier && kv.first == "parser" && v.isString() &&
            v.asString().find_first_of("/.") != std::string::npos)
            return true;
        if (jsonNamesCode(v, prettier, depth + 1)) return true;
    }
    return false;
}

// A YAML, TOML or JSON5 Prettier configuration, read as text and on the
// safe side: the word plugins anywhere, a parser whose value looks like a
// path, or no key at all (the whole file a shared configuration's name).
bool textNamesCode(const std::string& text) {
    if (text.find("plugins") != std::string::npos) return true;
    bool anyKey = false, anyText = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == '#' || line == "---" || line == "..." ||
            line.rfind("//", 0) == 0)
            continue;
        anyText = true;
        const size_t sep = line.find_first_of(":=");
        if (sep == std::string::npos) continue;
        anyKey = true;
        const size_t parser = line.find("parser");
        if (parser != std::string::npos && parser < sep &&
            line.find_first_of("/.", sep + 1) != std::string::npos)
            return true;
    }
    return anyText && !anyKey;
}

// The value of package.json's "prettier" key, or a whole configuration.
bool prettierValueRunsCode(const Json& v) {
    if (v.isString()) return !v.asString().empty();   // a shared configuration module
    return jsonNamesCode(v, true);
}

// "/a/b/../c" -> "/a/c", for a configuration named by a relative path.
std::string normalized(const std::string& path) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos) slash = path.size();
        const std::string part = path.substr(pos, slash - pos);
        pos = slash + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(part);
        }
    }
    std::string out;
    for (const std::string& part : parts) out += "/" + part;
    return out.empty() ? "/" : out;
}

const char* const kBiomeFiles[] = {"biome.json", "biome.jsonc", ".biome.json", ".biome.jsonc"};

bool biomeRunsCode(const std::string& file,
                   const std::function<std::string(const std::string&)>& read, int depth) {
    if (depth > 8) return true;
    const std::string text = read ? read(file) : "";
    Json j;
    if (!Json::parse(plainJson(text), j))
        return text.find("plugins") != std::string::npos ||
               text.find("extends") != std::string::npos;
    if (jsonNamesCode(j, false)) return true;
    const Json& ext = j["extends"];
    std::vector<std::string> names;
    if (ext.isString()) names.push_back(ext.asString());
    for (const Json& e : ext.elements())
        if (e.isString()) names.push_back(e.asString());
    const std::string dir = parentOf(file);
    for (const std::string& name : names) {
        if (name.empty()) continue;
        if (name == "//") {
            // A monorepo's root configuration: the nearest one above.
            for (std::string up = parentOf(dir); !up.empty(); up = up == "/" ? "" : parentOf(up)) {
                std::string found;
                for (const char* n : kBiomeFiles)
                    if (found.empty() && read && !read(join(up, n)).empty()) found = join(up, n);
                if (found.empty()) continue;
                if (biomeRunsCode(found, read, depth + 1)) return true;
                break;
            }
            continue;
        }
        // A file beside it is read and checked; a package's is not visible.
        const bool file = name[0] != '/' && name[0] != '@' &&
                          (name.rfind("./", 0) == 0 || name.rfind("../", 0) == 0 ||
                           endsWith(name, ".json") || endsWith(name, ".jsonc"));
        if (!file) return true;
        const std::string target = normalized(join(dir, name));
        if (!read || read(target).empty() || biomeRunsCode(target, read, depth + 1)) return true;
    }
    return false;
}

// The files Prettier 3 reads its configuration from, in one folder;
// package.json and package.yaml count only with a "prettier" key.
const char* const kPrettierFiles[] = {
    "package.json", "package.yaml", ".prettierrc", ".prettierrc.json", ".prettierrc.yaml",
    ".prettierrc.yml", ".prettierrc.json5", ".prettierrc.js", "prettier.config.js",
    ".prettierrc.ts", "prettier.config.ts", ".prettierrc.mjs", "prettier.config.mjs",
    ".prettierrc.mts", "prettier.config.mts", ".prettierrc.cjs", "prettier.config.cjs",
    ".prettierrc.cts", "prettier.config.cts", ".prettierrc.toml",
};

// Where package.yaml's top-level prettier key starts, or npos.
size_t packageYamlPrettier(const std::string& text) {
    if (text.rfind("prettier:", 0) == 0) return 0;
    const size_t at = text.find("\nprettier:");
    return at == std::string::npos ? at : at + 1;
}

}  // namespace

bool configRunsCode(const std::string& program, const std::string& configFile,
                    const std::function<std::string(const std::string&)>& read) {
    if (configFile.empty()) return false;
    const std::string tool = programName(program);
    const std::string name = programName(configFile);
    if (tool == "prettier") {
        for (const char* ext : {".js", ".cjs", ".mjs", ".ts", ".cts", ".mts"})
            if (endsWith(name, ext)) return true;
        const std::string text = read ? read(configFile) : "";
        if (name == "package.json") {
            Json j;
            return Json::parse(text, j) && j.isObject() && j.has("prettier") &&
                   prettierValueRunsCode(j["prettier"]);
        }
        if (name == "package.yaml") {
            const size_t at = packageYamlPrettier(text);
            if (at == std::string::npos) return false;
            // The key's value: the rest of its line, then the indented lines.
            size_t end = text.find('\n', at);
            while (end != std::string::npos && end + 1 < text.size() &&
                   (text[end + 1] == ' ' || text[end + 1] == '\t' || text[end + 1] == '\n'))
                end = text.find('\n', end + 1);
            if (end == std::string::npos) end = text.size();
            const std::string value = text.substr(at + 9, end - at - 9);
            const std::string inlineValue = trim(value.substr(0, value.find('\n')));
            if (!inlineValue.empty() && inlineValue[0] != '#' && inlineValue[0] != '{')
                return true;   // a shared configuration's name
            return textNamesCode(value);
        }
        Json j;
        if (Json::parse(plainJson(text), j)) return prettierValueRunsCode(j);
        return textNamesCode(text);
    }
    if (tool == "biome") return biomeRunsCode(configFile, read, 0);
    if (tool == "latexindent") {
        // A replacement given as a substitution is run with Perl's eval
        // (LatexIndent/Replacement.pm) once replacement mode is on, which
        // the file can turn on itself (switchesViaYaml: rSwitch).
        const std::string text = read ? read(configFile) : "";
        return text.find("substitution") != std::string::npos;
    }
    return false;
}

std::string codeConfigFile(const std::string& program, const std::string& path,
                           const std::string& root, const std::string& planConfig,
                           const Fs& fs) {
    const std::string tool = programName(program);
    if (tool == "latexindent")
        return configRunsCode(tool, planConfig, fs.read) ? planConfig : "";
    const bool prettier = tool == "prettier";
    if (!prettier && tool != "biome") return "";
    (void)root;
    // Both tools search from the file's folder up to the filesystem root,
    // past the open folder and past .git: a configuration above the folder
    // (the repository's, when only its docs folder is open) is read and
    // run just the same.
    std::vector<std::string> dirs;
    for (std::string dir = parentOf(path); !dir.empty(); dir = parentOf(dir)) {
        dirs.push_back(dir);
        if (dir == "/") break;
    }
    for (const std::string& dir : dirs) {
        std::vector<std::string> present;
        auto consider = [&](const std::string& name) {
            const std::string file = join(dir, name);
            if (!fs.exists || !fs.exists(file)) return;
            const std::string text = name.rfind("package.", 0) == 0 && fs.read ? fs.read(file) : "";
            if (name == "package.json" && !jsonHasKey(text, "prettier")) return;
            if (name == "package.yaml" && packageYamlPrettier(text) == std::string::npos) return;
            present.push_back(file);
        };
        if (prettier)
            for (const char* name : kPrettierFiles) consider(name);
        else
            for (const char* name : kBiomeFiles) consider(name);
        if (present.empty()) continue;
        // The tool stops at the nearest folder holding one; whichever of
        // its files a given version prefers, any that runs code counts.
        for (const std::string& file : present)
            if (configRunsCode(tool, file, fs.read)) return file;
        return "";
    }
    return "";
}

namespace {

// "@biomejs/biome@2.1" -> "biome", "prettier@3" -> "prettier", "./x" -> "x".
std::string binOfPackage(std::string spec) {
    const size_t from = !spec.empty() && spec[0] == '@' ? spec.find('/') : 0;
    const size_t at = from == std::string::npos ? std::string::npos : spec.find('@', from + 1);
    if (at != std::string::npos && at > 0) spec.resize(at);
    return programName(spec);
}

}  // namespace

Launch launched(const std::string& command) {
    Launch out;
    const std::vector<std::string> w = Lsp::splitCommand(command);
    if (w.empty()) return out;
    const std::string runner = programName(w[0]);
    size_t i = 1;
    // Skips options before the next word; `valued` ones take the word after
    // them. True when one of them (-c, --call) gave the command itself.
    auto skip = [&](std::initializer_list<const char*> valued) {
        while (i < w.size() && w[i].size() > 1 && w[i][0] == '-') {
            const std::string o = w[i];
            if (o == "--") {
                i++;
                return false;
            }
            const size_t eq = o.find('=');
            const std::string key = eq == std::string::npos ? o : o.substr(0, eq);
            if (key == "-c" || key == "--call") {
                const std::string value =
                    eq != std::string::npos ? o.substr(eq + 1) : i + 1 < w.size() ? w[i + 1] : "";
                const std::vector<std::string> inner = Lsp::splitCommand(value);
                if (!inner.empty()) out.tool = binOfPackage(inner[0]);
                return true;
            }
            bool takes = false;
            if (eq == std::string::npos)
                for (const char* v : valued)
                    if (key == v) takes = true;
            i += takes ? 2 : 1;
        }
        return false;
    };
    auto next = [&](bool folderCode) {
        if (i < w.size()) {
            out.tool = binOfPackage(w[i]);
            out.folderCode = folderCode;
        }
        return out;
    };
    const std::initializer_list<const char*> npxOptions = {
        "-p", "--package", "--cache", "--userconfig", "-w", "--workspace", "--prefix"};
    if (runner == "npx" || runner == "pnpx" || runner == "bunx") {
        if (skip(npxOptions)) return out;
        return next(false);
    }
    if (runner == "npm") {
        if (skip({"--prefix", "-w", "--workspace", "--userconfig", "--cache"})) return out;
        if (i >= w.size()) return out;
        const std::string sub = w[i++];
        if (sub == "exec" || sub == "x") {
            if (skip(npxOptions)) return out;
            return next(false);
        }
        if (sub == "run" || sub == "run-script" || sub == "rum" || sub == "urn") {
            skip({});
            return next(true);
        }
        return out;
    }
    if (runner == "pnpm") {
        if (skip({"-C", "--dir", "--filter", "-F", "--workspace-dir", "--config"})) return out;
        if (i >= w.size()) return out;
        const std::string sub = w[i];
        if (sub == "exec" || sub == "dlx") {
            i++;
            if (skip({"--package", "--filter", "-F"})) return out;
            return next(false);
        }
        if (sub == "run" || sub == "run-script") {
            i++;
            skip({});
            return next(true);
        }
        return next(false);   // pnpm runs a bin by its name
    }
    if (runner == "yarn") {
        if (skip({"--cwd"})) return out;
        if (i >= w.size()) return out;
        const std::string sub = w[i];
        if (sub == "dlx") {
            i++;
            if (skip({"-p", "--package"})) return out;
            return next(false);
        }
        if (sub == "exec" || sub == "run") {
            i++;
            skip({});
        }
        return next(true);   // a script or one of the project's own bins
    }
    if (runner == "bun") {
        if (skip({"--cwd", "--config"})) return out;
        if (i >= w.size()) return out;
        const std::string sub = w[i];
        if (sub == "x") {
            i++;
            if (skip(npxOptions)) return out;
            return next(false);
        }
        if (sub == "run") {
            i++;
            skip({});
        }
        return next(true);   // a script, or a file of the folder's
    }
    return out;
}

Source chooseLaunched(bool fromSettings, bool folderCopy, const std::string& runner,
                      const std::string& consent, bool configIsCode) {
    return folderCopy ? chooseProgram(fromSettings, runner, "", consent, configIsCode)
                      : chooseProgram(fromSettings, "", runner, consent, configIsCode);
}

bool serverMayFormat(const std::string& lang, const Plan& plan) {
    if (lang.empty()) return true;
    if (plan.off || plan.fromSettings) return false;
    return plan.candidates.empty();
}

bool hasDefault(const std::string& lang) {
    return !defaultCommand(lang).empty();
}

std::string unplannedMessage(const std::string& lang, const std::string& extension) {
    const Language* l = findLanguage(lang);
    if (!l) return "minicode has no formatter for ." + extension + " files.";
    bool biome = false, prettier = false;
    for (const Tool& t : l->tools) {
        if (std::string(t.program) == "biome") biome = true;
        if (std::string(t.program) == "prettier") prettier = true;
    }
    return std::string("minicode formats ") + l->display + " files only in projects set up for " +
           (biome ? "Prettier or Biome" : "Prettier") +
           (prettier ? ", or when the settings say format." + lang +
                           " = prettier (your own Prettier) or give a command of your own."
                     : ", or with a command of your own (format." + lang + " in the settings).");
}

bool jsonHasKey(const std::string& text, const std::string& key) {
    Json j;
    return Json::parse(text, j) && j.isObject() && j.has(key);
}

bool jsonHasDependency(const std::string& text, const std::string& name) {
    Json j;
    if (!Json::parse(text, j) || !j.isObject()) return false;
    return (j["dependencies"].isObject() && j["dependencies"].has(name)) ||
           (j["devDependencies"].isObject() && j["devDependencies"].has(name));
}

std::string programName(const std::string& program) {
    size_t slash = program.rfind('/');
    return slash == std::string::npos ? program : program.substr(slash + 1);
}

std::vector<std::string> arguments(const std::string& command, const std::string& path,
                                   int firstLine, int lastLine, bool* ranged,
                                   const std::string& config) {
    if (ranged) *ranged = false;
    std::vector<std::string> words = Lsp::splitCommand(command);
    const std::pair<std::string, const std::string*> marks[] = {{"{file}", &path},
                                                                {"{config}", &config}};
    for (std::string& w : words) {
        for (const auto& m : marks) {
            size_t at = 0;
            while ((at = w.find(m.first, at)) != std::string::npos) {
                w.replace(at, m.first.size(), *m.second);
                at += m.second->size();
            }
        }
    }
    if (words.empty() || firstLine <= 0 || lastLine < firstLine) return words;

    std::string name = programName(words[0]);
    std::string a = std::to_string(firstLine), b = std::to_string(lastLine);
    std::string option;
    if (name == "clang-format") option = "--lines=" + a + ":" + b;
    else if (name == "yapf") option = "--lines=" + a + "-" + b;
    else if (name == "pyink" || name == "black") option = "--line-ranges=" + a + "-" + b;
    if (option.empty()) return words;
    // Before a closing "-" (read stdin), where options conventionally go.
    if (words.size() > 1 && words.back() == "-")
        words.insert(words.end() - 1, option);
    else
        words.push_back(option);
    if (ranged) *ranged = true;
    return words;
}

std::string errorSummary(const std::string& err, const std::string& out,
                         const std::string& path, size_t maxLines) {
    std::string name = programName(path);
    const std::string& src = trim(err).empty() ? out : err;
    std::vector<std::string> lines;
    size_t total = 0, pos = 0;
    while (pos <= src.size()) {
        size_t nl = src.find('\n', pos);
        if (nl == std::string::npos) nl = src.size();
        std::string line = src.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                 line.back() == '\t'))
            line.pop_back();
        if (trim(line).empty()) continue;
        total++;
        if (lines.size() >= maxLines) continue;
        for (const std::string& from : {path, std::string("<standard input>"),
                                        std::string("<stdin>")}) {
            if (from.empty()) continue;
            size_t at = 0;
            while ((at = line.find(from, at)) != std::string::npos) {
                line.replace(at, from.size(), name);
                at += name.size();
            }
        }
        if (line.size() > 300) line = line.substr(0, 300) + "…";
        lines.push_back(line);
    }
    std::string result;
    for (const std::string& line : lines) {
        if (!result.empty()) result += "\n";
        result += line;
    }
    if (total > lines.size()) result += "\n…";
    return result;
}

bool tomlHasTable(const std::string& text, const std::string& name) {
    for (const std::string& h : headers(text, true))
        if (h == name || h.rfind(name + ".", 0) == 0) return true;
    return false;
}

bool iniHasSection(const std::string& text, const std::string& name) {
    for (const std::string& h : headers(text, false))
        if (lower(h) == lower(name)) return true;
    return false;
}

std::string cargoEdition(const std::string& text) {
    bool inPackage = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (!line.empty() && line[0] == '[') {
            inPackage = line == "[package]";
            continue;
        }
        if (!inPackage || line.rfind("edition", 0) != 0) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || trim(line.substr(7, eq - 7)) != "") continue;
        std::string v = trim(line.substr(eq + 1));
        size_t hash = v.find('#');
        if (hash != std::string::npos) v = trim(v.substr(0, hash));
        if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0])
            v = v.substr(1, v.size() - 2);
        if (v.size() == 4 && std::all_of(v.begin(), v.end(), ::isdigit)) return v;
    }
    return "2021";
}

bool editorconfigCoversShell(const std::string& text) {
    for (const std::string& h : headers(text, false)) {
        if (h == "*") return true;
        std::string l = lower(h);
        if (l.find("sh") != std::string::npos || l.find("bash") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace Formatter
