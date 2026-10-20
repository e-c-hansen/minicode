// Formatter.cpp — see Formatter.h.
#include "Formatter.h"
#include "InstallHints.h"
#include "LspClient.h"   // Lsp::splitCommand, shared with lsp.* commands
#include <algorithm>
#include <cctype>

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
enum class Kind { Any, TomlTable, IniSection, EditorConfigShell };
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
};

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
             {"ruff", "", "ruff format --stdin-filename {file} -",
              {{"ruff.toml", Kind::Any, ""},
               {".ruff.toml", Kind::Any, ""},
               {"pyproject.toml", Kind::TomlTable, "tool.ruff.format"}},
              false},
         }},
        {"cpp", "C-family",
         {"c", "h", "cc", "cpp", "cxx", "c++", "hh", "hpp", "hxx", "h++", "ipp",
          "inl", "tpp", "cu", "cuh", "m", "mm", "java", "js", "mjs", "cjs", "jsx",
          "ts", "mts", "cts", "tsx", "proto", "cs"},
         {
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
    return l ? l->tools.front().google : "";
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
        p.candidates.push_back({words[0], value, false});
        p.installHint = hintFor(words[0]);
        return p;
    }

    // The nearest folder that configures one of the language's tools decides.
    const Tool* chosen = nullptr;
    for (const std::string& dir : searchDirs(path, root, fs)) {
        for (const Tool& t : l->tools) {
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
            if (!t.byDefault) continue;
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

std::vector<std::string> venvBinDirs(const std::string& path, const std::string& root,
                                     const std::string& virtualEnv) {
    std::vector<std::string> out;
    // Up to the open folder; a file outside it has only its own folder
    // searched, so a stray .venv higher up never supplies the tools.
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
    for (const std::string& dir : dirs) {
        out.push_back(join(dir, ".venv/bin"));
        out.push_back(join(dir, "venv/bin"));
    }
    if (!virtualEnv.empty()) {
        std::string bin = join(virtualEnv, "bin");
        if (std::find(out.begin(), out.end(), bin) == out.end()) out.push_back(bin);
    }
    return out;
}

std::string programName(const std::string& program) {
    size_t slash = program.rfind('/');
    return slash == std::string::npos ? program : program.substr(slash + 1);
}

std::vector<std::string> arguments(const std::string& command, const std::string& path,
                                   int firstLine, int lastLine, bool* ranged) {
    if (ranged) *ranged = false;
    std::vector<std::string> words = Lsp::splitCommand(command);
    for (std::string& w : words) {
        size_t at = 0;
        while ((at = w.find("{file}", at)) != std::string::npos) {
            w.replace(at, 6, path);
            at += path.size();
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
