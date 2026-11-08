// Formatter.h — pure C++. Format Document (Shift+Option+F): which external
// formatter formats a file, with what command line, and how its complaints
// are shown. No I/O of its own: the file system is reached through the
// callbacks in `Fs`, so every decision here is unit-tested. FormatRunner.mm
// finds the programs and runs them.
//
// Formatters are programs the user installs, like language servers. Each
// language has a settings key, format.<lang>. When it is not set, the
// project decides first and Google's style guides second:
//   - A project that configures a formatter (a .clang-format, a pyproject.toml
//     with [tool.pyink], a .style.yapf, a .prettierrc or biome.json for
//     JavaScript and TypeScript, ...) gets that formatter with its own
//     configuration, and nothing else: falling back to another tool would
//     reformat the project in a style it never chose.
//   - Otherwise the language's Google-style command runs (clang-format
//     --style=Google, pyink at 80 columns, then yapf --style=google, gofmt,
//     rustfmt, shfmt -i 2 -ci -bn), the first one that is installed.
// A command given in the settings runs exactly as written.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace Formatter {

// The settings key for a file's language ("python", "cpp", "go", "rust",
// "shell", "markdown", "json", "yaml", "css", "latex"), or "" when no
// formatter applies. "cpp" covers everything clang-format formats: C, C++,
// Objective-C, Java, JavaScript, TypeScript, Protocol Buffers and C#.
// Markdown, JSON, YAML and CSS have no default: only a project set up for
// Prettier (or Biome, for JSON and CSS) or a format.<lang> command formats
// them, so their plan is often empty.
std::string languageFor(const std::string& path);
const std::vector<std::string>& languages();
// For messages: "Python", "C-family", "Go", "Rust", "shell", "Markdown", ...
std::string displayName(const std::string& lang);
// The command the settings file documents for a language: its first tool's
// Google-style command.
std::string defaultCommand(const std::string& lang);
// Does the language have a formatter that runs without the project
// configuring one (defaultCommand is not "")?
bool hasDefault(const std::string& lang);

// What the planner may look at. `exists` answers for a file path, `read`
// gives a file's text ("" when unreadable). Paths are absolute, '/'-separated.
struct Fs {
    std::function<bool(const std::string&)> exists;
    std::function<std::string(const std::string&)> read;
};

struct Candidate {
    std::string program;   // the command's first word, as written
    std::string command;   // the whole command line, with {file} unexpanded
    bool projectStyle = false;   // the project's own configuration applies
};

struct Plan {
    bool off = false;          // format.<lang> = off
    bool fromSettings = false; // the command came from the settings file
    // Tried in order; the first whose program is installed runs.
    std::vector<Candidate> candidates;
    // The project file that chose the formatter or its style, or "".
    std::string configFile;
    // What to install when none of the candidates is found, as a command to
    // type ("uv tool install pyink"), from InstallHints. "" for a program
    // MiniCode does not know.
    std::string installHint;
};

// The plan for formatting `path` (absolute). `setting` is the format.<lang>
// value ("" when unset). Configuration files are looked for from the file's
// folder upwards, stopping after `root` (the open folder) or after a folder
// holding .git, whichever comes first.
Plan plan(const std::string& lang, const std::string& setting,
          const std::string& path, const std::string& root, const Fs& fs);

// The nearest project file that configures a tool (by its program name,
// "ruff"), looked for as plan looks: from the file's folder upwards, up to
// `root` or a folder holding .git. Unlike plan, any folder's counts, whatever
// a nearer one configures for other tools: Ruff's linter runs beside
// whichever formatter the project chose. "" when none does.
std::string configFile(const std::string& program, const std::string& path,
                       const std::string& root, const Fs& fs);

// May Format Document ask a language server (textDocument/formatting)?
// Only when no formatter is planned for the file at all: its kind of file
// has none (`lang` ""), or nothing configures one and the language has no
// default (a README in a project without Prettier). Never when the project
// configures a formatter, the settings name one or turn the language off,
// or a default tool is merely not installed: the server would format in a
// style the project never chose.
bool serverMayFormat(const std::string& lang, const Plan& plan);
// The note when nothing is planned and no server formats the file:
// "minicode formats Markdown files only in projects set up for Prettier,
// or with a command of your own (format.markdown in the settings)."; for
// `lang` "" it names the file's extension.
std::string unplannedMessage(const std::string& lang, const std::string& extension);

// Folders that may hold a project's own Python tools, nearest first:
// .venv/bin and venv/bin in the file's folder and each one above it up to
// `root` (only the file's own folder when it is outside `root`), then
// $VIRTUAL_ENV/bin when given.
std::vector<std::string> venvBinDirs(const std::string& path, const std::string& root,
                                     const std::string& virtualEnv);

// Folders that may hold a project's own Node tools (Prettier, Biome), nearest
// first: node_modules/.bin in the file's folder and each one above it up to
// `root`, as venvBinDirs.
std::vector<std::string> nodeBinDirs(const std::string& path, const std::string& root);
// Is this a formatter a project installs in node_modules ("prettier", "biome")?
bool fromNodeModules(const std::string& program);

// Is `path` inside the folder `root` (or the folder itself)? Both spelled
// the same way (resolved by the caller); "/" holds everything, "" nothing.
bool insideFolder(const std::string& path, const std::string& root);

// Where a formatter's program comes from. A copy in the project's own
// folders (venvBinDirs, nodeBinDirs) is a program the folder supplies, which
// runs with the user's rights, so it runs only once the user allows the
// folder's own tools (the per-folder answer the Python language servers ask
// for). `local`: that copy ("" for none; never $VIRTUAL_ENV's, which the
// user activated); `outside`: the one found on PATH and the usual folders
// ("" for none); `consent`: "allow", "deny", or "" when not asked yet.
// A command from the settings file is the user's own choice of tool, so it
// asks only when the folder holds the only copy; otherwise it runs the copy
// outside the folder until the folder is allowed.
enum class Source { Local, Outside, Ask, None };
Source chooseProgram(bool fromSettings, const std::string& local, const std::string& outside,
                     const std::string& consent);

// A command line -> argv. Words are split like lsp.* commands (quotes group,
// backslash escapes), and {file} is replaced inside each word afterwards, so
// a path with spaces or quotes stays one argument and never needs quoting.
// firstLine/lastLine (1-based, inclusive; 0 for the whole file) add the
// program's line-range option when it has one (clang-format, yapf, pyink,
// black); `ranged` says whether one was added.
// {config} is replaced the same way by `config`, the project file that
// chose the formatter's style (Plan::configFile), for a program that is told
// it by path (latexindent -l).
std::vector<std::string> arguments(const std::string& command, const std::string& path,
                                   int firstLine = 0, int lastLine = 0,
                                   bool* ranged = nullptr, const std::string& config = "");

// The program's name without its folder ("/opt/x/clang-format" -> "clang-format").
std::string programName(const std::string& program);

// A failed run, for a popover: the first `maxLines` non-blank lines of
// stderr (stdout when stderr is empty), with "<standard input>", "<stdin>"
// and the file's full path shortened to its name.
std::string errorSummary(const std::string& err, const std::string& out,
                         const std::string& path, size_t maxLines = 6);

// Helpers for reading project files, exposed for tests.
// Does a TOML file have the table [name] or a subtable [name.x]?
bool tomlHasTable(const std::string& text, const std::string& name);
// Does an INI file (setup.cfg) have the section [name]?
bool iniHasSection(const std::string& text, const std::string& name);
// The edition a Cargo.toml's [package] names, or "2021".
std::string cargoEdition(const std::string& text);
// Is the text a JSON object with this key at its top (package.json's
// "prettier")?
bool jsonHasKey(const std::string& text, const std::string& key);
// Does a package.json name this package in dependencies or devDependencies?
// Prettier needs no configuration file, so a project that installs it uses
// it with its defaults.
bool jsonHasDependency(const std::string& text, const std::string& name);
// Does an .editorconfig have a section that applies to shell scripts
// ([*], or a glob naming sh or bash)?
bool editorconfigCoversShell(const std::string& text);

}  // namespace Formatter
