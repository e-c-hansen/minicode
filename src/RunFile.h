// RunFile.h — pure C++. The pieces of "Run File" (Shift+Cmd+Return) that need
// no GUI: quoting for the shell, filling in a run.<language> template from
// the settings, and finding a project's virtualenv.
//
// A template is a command line with placeholders: {file} (the file's absolute
// path), {dir} (its folder), {root} (the window's folder), {name} (the file's
// name) and {python} (the interpreter for the file). Each value is quoted for
// the shell as it goes in; anything else in braces is left alone, so shell
// brace expansion still works.
#pragma once
#include <map>
#include <string>

namespace RunFile {

// A word the shell reads back as `s`: unchanged when it holds only
// characters no shell treats specially, otherwise in single quotes.
std::string shellQuote(const std::string& s);

// `tmpl` with each {key} in `vars` replaced by its value, quoted.
std::string expand(const std::string& tmpl,
                   const std::map<std::string, std::string>& vars);

// The python in the nearest virtualenv: .venv, venv or env holding
// bin/python, in the file's folder or a folder above it, up to and including
// `root` (only the file's own folder when it is outside root). "" when there
// is none.
std::string findVirtualenvPython(const std::string& file, const std::string& root);

}  // namespace RunFile
