// PythonEnvs.h — pure C++ (POSIX). Finding the Python interpreters on this
// machine, for the Python language server: the project's own virtual
// environment, the active one, conda, pyenv, poetry, pipenv, uv and the
// Pythons on PATH.
//
// Nothing is ever run. A version comes from what is on disk: a venv's
// pyvenv.cfg, conda's package records, a pyenv folder's name, or the file
// name a symlink resolves to (python3.12). That keeps listing fast, and it
// matters on a Mac, where /usr/bin/python3 is a shim that offers to install
// the developer tools when run; the caller lists such shims in `skip`.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace PythonEnvs {

struct Interpreter {
    std::string path;      // the python executable
    std::string kind;      // "project", "active", "conda", "pyenv", "poetry",
                           // "pipenv", "virtualenv", "uv", "path"
    std::string name;      // short: ".venv", "base", "3.12.4", "python3.13"
    std::string version;   // "3.12.4", "3.12", or "" when the disk does not say
    std::string prefix;    // the environment's folder ("" for a Python on PATH)
};

struct Context {
    std::string fileDir;   // the open file's folder (may be "")
    std::string root;      // the open folder
    std::string home;
    // VIRTUAL_ENV, CONDA_PREFIX, PYENV_ROOT, WORKON_HOME, CONDA_ENVS_PATH,
    // POETRY_VIRTUALENVS_PATH, XDG_DATA_HOME, XDG_CACHE_HOME; others ignored.
    std::map<std::string, std::string> env;
    std::vector<std::string> pathDirs;   // looked in for python3, python3.N, python
    std::vector<std::string> skip;       // executables never offered
    bool mac = true;                     // ~/Library/Caches vs ~/.cache for poetry
};

// The project's own environment, looking in each folder from fileDir up to
// root (inclusive) for .venv, venv, env and .env holding bin/python. "" if
// none. Folders above root are never looked at.
std::string projectInterpreter(const std::string& fileDir, const std::string& root);

// The python executable inside an environment folder, or "".
std::string interpreterIn(const std::string& prefix);

// What the disk says the version is (see above), or "".
std::string versionOf(const std::string& pythonPath);

// Every interpreter found, the project's first, then the active
// environment, then the managers' environments, then PATH. No path twice,
// and a Python on PATH that is the same file as an earlier one on PATH is
// left out (python3 and python3.12 in one Homebrew folder).
std::vector<Interpreter> find(const Context& ctx);

// The automatic choice: the project's environment, else $VIRTUAL_ENV's,
// else $CONDA_PREFIX's, else "" (the server's own default).
std::string automatic(const Context& ctx);

}  // namespace PythonEnvs
