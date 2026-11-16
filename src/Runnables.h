// Runnables.h — pure C++. The tests in a file, found without a language
// server, and the command that runs one of them, or all of a file's, in the
// terminal (the editor's ▶ beside each test; Zed's runnables and VS Code's
// Testing gutter icons are the models).
//
// Found, in a file each framework collects by default:
//   - pytest (test_*.py, *_test.py, tests.py): a `def test_*` (async too) at
//     the top level, or a method `test_*` of a class named Test* or one
//     whose bases name TestCase, classes nested as deep as pytest goes.
//   - go test (*_test.go): `func TestX(t *testing.T)` at the top level,
//     where X does not start with a lowercase letter, as go test requires.
//   - cargo test (.rs): a function after #[test] or a #[...::test]
//     attribute (#[tokio::test], #[async_std::test(...)]), with the `mod`
//     blocks around it.
//   - Jest and Vitest (*.test.*, *.spec.*, or under __tests__, in .js .jsx
//     .ts .tsx .mjs .cjs .mts .cts): test( and it( (.only, .skip and
//     .concurrent too) with a quoted name, inside the describe( blocks
//     around them.
// Code in comments and strings is skipped: the file is read through the
// highlighter's tokens (SyntaxHighlighter.h).
//
// The command is a template, test.<language> in the settings (python, go,
// rust, js; js covers TypeScript too), or the built-in one, with Run File's
// placeholders (RunFile.h: {file} {dir} {root} {name} {python}) and these:
//   {project}  the folder the tool runs in: the nearest one up from the file
//              (to the window's folder) holding go.mod, Cargo.toml,
//              package.json, or for Python pyproject.toml, pytest.ini,
//              setup.cfg, tox.ini or setup.py; else the window's folder
//   {path}     the file, relative to {project}
//   {tests}    pytest node ids (path::Class::test_x; the file alone for the
//              whole file) or cargo test paths (tests::it_works), each a word
//   {pattern}  go test's -run: ^TestX$, or ^(TestA|TestB)$ for a file
//   {package}  go test's package: ./sub/pkg, or . for {project} itself
//   {target}   cargo's target for a file under tests/ or src/bin/ (--test x,
//              --bin x), else nothing
//   {runner}   the project's own vitest or jest (node_modules/.bin), else
//              npx vitest or npx jest, Vitest when the project or the file
//              names it
//   {filter}   -t and the test's full name as an anchored pattern, for Jest
//              and Vitest; nothing for the whole file
// Each value is quoted for the shell; a placeholder that stands for nothing
// goes, with a space before it.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace Runnables {

enum class Kind { None, Pytest, Go, Rust, Js };

// The kind of test file a path is, by its name alone: None for a file no
// framework collects by default (app.py, main.go, a.ts).
Kind kindFor(const std::string& path);

// The settings' language for a kind: "python", "go", "rust", "js".
std::string languageOf(Kind kind);

struct Test {
    size_t line = 0;                  // 0-based: the line the name is on
    std::string name;                 // test_parse, TestParse, it_works, adds two
    std::vector<std::string> scope;   // the classes, mods or describes around it
    // As the person reads it: "TestParser::test_parse", "tests::it_works",
    // "math > adds two".
    std::string label(Kind kind) const;
};

// The tests in `text` (UTF-8), in order. Lines are counted as the editor
// counts them: a line ends after \n, after a \r not before \n, and after
// U+2029.
std::vector<Test> find(const std::string& text, Kind kind);

// The built-in template for a kind (what test.<language> replaces).
std::string defaultTemplate(Kind kind);

struct Request {
    std::string file;          // absolute
    std::string root;          // the window's folder ("" for none)
    std::string text;          // the file's text (Vitest or Jest, from its imports)
    std::string python;        // what {python} stands for
    std::string tmpl;          // test.<language> from the settings, "" for the default
    // Whether a path exists (a file or a folder); stat when unset.
    std::function<bool(const std::string&)> exists;
};

struct Command {
    std::string line;   // to type, run from `dir`
    std::string dir;    // {project}
};

// The command running `tests` of the file; with wholeFile, `tests` are all
// of the file's and the command runs the file (pytest and Jest or Vitest
// name the file, go test and cargo test every test in it). False, with a
// sentence in *problem, when the template is off, the file is no test
// file, or there are no tests to run.
bool command(const Request& r, Kind kind, const std::vector<Test>& tests, bool wholeFile,
             Command* out, std::string* problem);

// `tmpl` with each {key} filled: a scalar as RunFile::expand fills it (one
// quoted word, or escaped inside the quotes the template puts around it),
// a list as its words, quoted, or nothing at all (with the space before
// it) when it is empty.
std::string expand(const std::string& tmpl,
                   const std::vector<std::pair<std::string, std::string>>& scalars,
                   const std::vector<std::pair<std::string, std::vector<std::string>>>& lists);

// `s` with every character a regular expression gives a meaning to
// escaped by a backslash, so it matches itself (Jest's and Vitest's -t,
// go test's -run).
std::string regexEscape(const std::string& s);

// The test a caret on `line` (0-based) is in, for Run Test at Cursor: the
// last one starting at or above it, or -1 when the line is above every
// test.
int testAt(const std::vector<Test>& tests, size_t line);

}  // namespace Runnables
