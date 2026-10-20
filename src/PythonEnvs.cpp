// PythonEnvs.cpp — see PythonEnvs.h.
#include "PythonEnvs.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace PythonEnvs {

namespace {

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}

std::string dirName(const std::string& p) {
    size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return "";
    return slash == 0 ? "/" : p.substr(0, slash);
}

std::string baseName(const std::string& p) {
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string stripSlash(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

bool isDir(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// A regular file (after symlinks) that may be executed.
bool isExecutable(const std::string& p) {
    struct stat st;
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           access(p.c_str(), X_OK) == 0;
}

std::string realPath(const std::string& p) {
    char buf[PATH_MAX];
    if (realpath(p.c_str(), buf)) return buf;
    return p;
}

std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> names;
    DIR* d = opendir(dir.c_str());
    if (!d) return names;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n != "." && n != "..") names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

std::string readSmallFile(const std::string& p, size_t limit = 64 * 1024) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::string s(limit, '\0');
    in.read(&s[0], (std::streamsize)limit);
    s.resize((size_t)in.gcount());
    return s;
}

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

// The leading "3.12.4" of a string such as "3.12.4.final.0" or
// "3.12.4-h99e_0", at most three numbers; "" unless it starts with one.
std::string leadingVersion(const std::string& s) {
    std::string out;
    int parts = 0;
    size_t i = 0;
    while (i < s.size() && parts < 3) {
        size_t start = i;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
        if (i == start) break;
        if (!out.empty()) out += '.';
        out += s.substr(start, i - start);
        parts++;
        if (i < s.size() && s[i] == '.' && i + 1 < s.size() &&
            std::isdigit((unsigned char)s[i + 1]))
            i++;
        else
            break;
    }
    return parts >= 2 ? out : "";
}

std::string envValue(const Context& ctx, const char* key) {
    auto it = ctx.env.find(key);
    return it == ctx.env.end() ? "" : it->second;
}

// Is `dir` the same as `root` or inside it?
bool within(const std::string& dir, const std::string& root) {
    if (root.empty()) return false;
    if (dir == root) return true;
    std::string r = root.back() == '/' ? root : root + "/";
    return dir.compare(0, r.size(), r) == 0;
}

// The project's name for poetry and pipenv folders: pyproject's `name`,
// else the folder's own name. Lowercase.
std::string projectName(const std::string& root) {
    std::string text = readSmallFile(join(root, "pyproject.toml"));
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = trim(text.substr(pos, nl == std::string::npos ? std::string::npos
                                                                          : nl - pos));
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (line.compare(0, 4, "name") != 0) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || trim(line.substr(4, eq - 4)) != "") continue;
        std::string v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'')) {
            size_t end = v.find(v[0], 1);
            if (end != std::string::npos) return lower(v.substr(1, end - 1));
        }
    }
    return lower(baseName(stripSlash(root)));
}

// poetry names its environments <name>-<hash>-py3.12, pipenv <name>-<hash>;
// both clean up the name a little, so compare loosely.
bool namedFor(const std::string& envName, const std::string& project) {
    if (project.empty()) return false;
    auto norm = [](std::string s) {
        std::string out;
        for (char c : lower(s)) out += (std::isalnum((unsigned char)c) ? c : '-');
        return out;
    };
    std::string e = norm(envName), p = norm(project);
    return e.size() > p.size() + 1 && e.compare(0, p.size(), p) == 0 && e[p.size()] == '-';
}

struct Collector {
    const Context& ctx;
    std::vector<Interpreter> out;
    std::set<std::string> seenEnvs, seenPathReal, seenPaths;
    std::set<std::string> skip;

    explicit Collector(const Context& c) : ctx(c), skip(c.skip.begin(), c.skip.end()) {}

    void env(const std::string& prefixIn, const std::string& kind, const std::string& name,
             std::string version = "") {
        std::string prefix = stripSlash(prefixIn);
        std::string exe = interpreterIn(prefix);
        if (exe.empty() || skip.count(exe)) return;
        std::string key = join(realPath(prefix), baseName(exe));
        if (!seenEnvs.insert(key).second || !seenPaths.insert(exe).second) return;
        if (version.empty()) version = versionOf(exe);
        out.push_back({exe, kind, name, version, prefix});
    }

    void onPath(const std::string& exe) {
        if (skip.count(exe) || !isExecutable(exe)) return;
        if (!seenPaths.insert(exe).second) return;
        if (!seenPathReal.insert(realPath(exe)).second) return;
        out.push_back({exe, "path", baseName(exe), versionOf(exe), ""});
    }

    // Every environment folder directly inside `dir`.
    void envsIn(const std::string& dir, const std::string& kind,
                const std::string& onlyFor = "") {
        for (const std::string& n : listDir(dir)) {
            if (!onlyFor.empty() && !namedFor(n, onlyFor)) continue;
            env(join(dir, n), kind, n);
        }
    }
};

}  // namespace

std::string interpreterIn(const std::string& prefix) {
    if (prefix.empty()) return "";
    for (const char* name : {"bin/python3", "bin/python"}) {
        std::string p = join(stripSlash(prefix), name);
        if (isExecutable(p)) return p;
    }
    return "";
}

std::string projectInterpreter(const std::string& fileDirIn, const std::string& rootIn) {
    std::string root = stripSlash(rootIn);
    std::string dir = stripSlash(fileDirIn.empty() ? root : fileDirIn);
    if (!within(dir, root)) dir = root;
    while (!dir.empty()) {
        for (const char* name : {".venv", "venv", "env", ".env"}) {
            std::string exe = interpreterIn(join(dir, name));
            if (!exe.empty()) return exe;
        }
        if (dir == root || dir == "/") break;
        dir = dirName(dir);
        if (!within(dir, root)) break;
    }
    return "";
}

std::string versionOf(const std::string& pythonPath) {
    std::string bin = dirName(pythonPath);
    std::string prefix = dirName(bin);
    // A venv (python -m venv, virtualenv, uv) records the version it was
    // made from.
    std::string cfg = readSmallFile(join(prefix, "pyvenv.cfg"));
    if (!cfg.empty()) {
        size_t pos = 0;
        while (pos < cfg.size()) {
            size_t nl = cfg.find('\n', pos);
            std::string line = cfg.substr(pos, nl == std::string::npos ? std::string::npos
                                                                         : nl - pos);
            pos = nl == std::string::npos ? cfg.size() : nl + 1;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = lower(trim(line.substr(0, eq)));
            if (key == "version" || key == "version_info") {
                std::string v = leadingVersion(trim(line.substr(eq + 1)));
                if (!v.empty()) return v;
            }
        }
    }
    // conda keeps a record per installed package: python-3.12.4-<build>.json.
    for (const std::string& n : listDir(join(prefix, "conda-meta"))) {
        if (n.compare(0, 7, "python-") != 0) continue;
        std::string v = leadingVersion(n.substr(7));
        if (!v.empty()) return v;
    }
    // python3 is usually a symlink to python3.12.
    std::string real = baseName(realPath(pythonPath));
    if (real.compare(0, 6, "python") == 0) return leadingVersion(real.substr(6));
    return "";
}

std::vector<Interpreter> find(const Context& ctx) {
    Collector c(ctx);
    std::string root = stripSlash(ctx.root);
    std::string home = stripSlash(ctx.home);

    // The project's own, nearest first.
    if (!root.empty()) {
        std::string dir = stripSlash(ctx.fileDir.empty() ? root : ctx.fileDir);
        if (!within(dir, root)) dir = root;
        while (!dir.empty()) {
            for (const char* name : {".venv", "venv", "env", ".env"}) {
                std::string prefix = join(dir, name);
                std::string label = prefix.size() > root.size() + 1
                                        ? prefix.substr(root.size() + 1) : name;
                c.env(prefix, "project", label);
            }
            if (dir == root || dir == "/") break;
            dir = dirName(dir);
            if (!within(dir, root)) break;
        }
    }

    // The ones active where MiniCode was started.
    std::string venv = envValue(ctx, "VIRTUAL_ENV");
    if (!venv.empty()) c.env(venv, "active", baseName(stripSlash(venv)));
    std::string condaPrefix = envValue(ctx, "CONDA_PREFIX");
    if (!condaPrefix.empty()) c.env(condaPrefix, "conda", baseName(stripSlash(condaPrefix)));

    // conda: the usual installs, each base then its envs, and the
    // environments conda itself has recorded.
    if (!home.empty()) {
        std::vector<std::string> bases;
        for (const char* d : {"miniconda3", "miniforge3", "mambaforge", "anaconda3",
                              "micromamba"})
            bases.push_back(join(home, d));
        for (const char* d : {"/opt/homebrew/Caskroom/miniconda/base",
                              "/opt/homebrew/Caskroom/miniforge/base",
                              "/usr/local/Caskroom/miniconda/base",
                              "/usr/local/Caskroom/miniforge/base", "/opt/miniconda3",
                              "/opt/anaconda3", "/opt/conda"})
            bases.push_back(d);
        for (const std::string& b : bases) {
            if (!isDir(b)) continue;
            c.env(b, "conda", "base");
            c.envsIn(join(b, "envs"), "conda");
        }
        c.envsIn(join(home, ".conda/envs"), "conda");
        std::string recorded = readSmallFile(join(home, ".conda/environments.txt"));
        size_t pos = 0;
        while (pos < recorded.size()) {
            size_t nl = recorded.find('\n', pos);
            std::string line = trim(recorded.substr(pos, nl == std::string::npos
                                                             ? std::string::npos : nl - pos));
            pos = nl == std::string::npos ? recorded.size() : nl + 1;
            if (!line.empty() && line[0] == '/')
                c.env(line, "conda", baseName(stripSlash(line)));
        }
    }
    std::string condaEnvs = envValue(ctx, "CONDA_ENVS_PATH");
    for (size_t a = 0; a <= condaEnvs.size() && !condaEnvs.empty();) {
        size_t colon = condaEnvs.find(':', a);
        std::string d = condaEnvs.substr(a, colon == std::string::npos ? std::string::npos
                                                                        : colon - a);
        if (!d.empty()) c.envsIn(d, "conda");
        if (colon == std::string::npos) break;
        a = colon + 1;
    }

    // pyenv: every installed version (and pyenv-virtualenv's environments).
    std::string pyenv = envValue(ctx, "PYENV_ROOT");
    if (pyenv.empty() && !home.empty()) pyenv = join(home, ".pyenv");
    for (const std::string& n : listDir(join(pyenv, "versions")))
        c.env(join(join(pyenv, "versions"), n), "pyenv", n, leadingVersion(n));

    std::string project = root.empty() ? "" : projectName(root);
    std::string folderName = lower(baseName(root));
    if (!home.empty()) {
        // poetry: only this project's environments.
        std::string poetry = envValue(ctx, "POETRY_VIRTUALENVS_PATH");
        if (poetry.empty()) {
            std::string cache = envValue(ctx, "XDG_CACHE_HOME");
            if (ctx.mac) cache = join(home, "Library/Caches");
            else if (cache.empty()) cache = join(home, ".cache");
            poetry = join(cache, "pypoetry/virtualenvs");
        }
        for (const std::string& n : listDir(poetry))
            if (namedFor(n, project) || namedFor(n, folderName))
                c.env(join(poetry, n), "poetry", n);

        // pipenv (named after the folder) and virtualenvwrapper (all of them).
        std::string data = envValue(ctx, "XDG_DATA_HOME");
        if (data.empty()) data = join(home, ".local/share");
        c.envsIn(join(data, "virtualenvs"), "pipenv", folderName);
        std::string workon = envValue(ctx, "WORKON_HOME");
        if (workon.empty()) workon = join(home, ".virtualenvs");
        c.envsIn(workon, "virtualenv");

        // Pythons uv installed (uv python install).
        std::string uvDir = join(data, "uv/python");
        for (const std::string& n : listDir(uvDir)) {
            // cpython-3.12.4-macos-aarch64-none
            size_t dash = n.find('-');
            std::string v = dash == std::string::npos ? "" : leadingVersion(n.substr(dash + 1));
            c.env(join(uvDir, n), "uv", v.empty() ? n : "uv " + v, v);
        }
    }

    // On PATH and in the package managers' folders: python3, then the
    // versioned ones, newest first, then plain python.
    for (const std::string& dir : ctx.pathDirs) {
        c.onPath(join(dir, "python3"));
        std::vector<std::pair<int, std::string>> versioned;
        for (const std::string& n : listDir(dir)) {
            if (n.compare(0, 8, "python3.") != 0 || n.size() == 8) continue;
            bool digits = std::all_of(n.begin() + 8, n.end(),
                                      [](char ch) { return std::isdigit((unsigned char)ch); });
            if (digits) versioned.push_back({std::atoi(n.c_str() + 8), n});
        }
        std::sort(versioned.begin(), versioned.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& v : versioned) c.onPath(join(dir, v.second));
        c.onPath(join(dir, "python"));
    }
    return c.out;
}

std::string automatic(const Context& ctx) {
    std::string p = projectInterpreter(ctx.fileDir, ctx.root);
    if (p.empty()) p = interpreterIn(envValue(ctx, "VIRTUAL_ENV"));
    if (p.empty()) p = interpreterIn(envValue(ctx, "CONDA_PREFIX"));
    return p;
}

}  // namespace PythonEnvs
