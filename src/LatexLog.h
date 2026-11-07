// LatexLog.h — what tectonic said went wrong, as a list of problems with the
// file and line each is about, for the LaTeX preview's failure panel (rows
// that open the source at the line) and the gutter's error dots.
//
// tectonic sums up TeX's first error on a line of its own,
//     error: .paper.minicode.tex:5: Undefined control sequence
// (the file as TeX named it: the preview's hidden sibling, or an \input
// file without its extension), then prints TeX's transcript between rules of
// '=' signs. A complaint without a place (an emergency stop, a runaway
// argument) has no line. When the summary names no line at all, the
// transcript's "! message" blocks are read instead, with their "l.N" lines
// and the file TeX had open (the innermost "(name" not yet closed).
//
// Pure C++, no I/O: the caller says which files exist.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace LatexLog {

enum class Severity { Error = 1, Warning = 2 };

struct Problem {
    Severity severity = Severity::Error;
    std::string file;     // as the log names it; empty when it names none
    int line = 0;         // 1-based; 0 when the log gives no line
    std::string message;  // one line, without TeX's leading "!"
};

// Every problem in tectonic's output, in order, each once.
std::vector<Problem> parse(const std::string &output);

// The file a problem is about. `texPath` is the user's document and
// `siblingPath` the hidden copy tectonic typeset in its place (same folder,
// same lines), so a problem in the sibling, or one naming no file, is about
// `texPath`. Another name is read relative to the document's folder, with
// ".tex" added when only that exists, as TeX's \input finds it. Returns ""
// when the name leads to no file.
std::string sourcePath(const std::string &file, const std::string &texPath,
                       const std::string &siblingPath,
                       const std::function<bool(const std::string &)> &exists);

}  // namespace LatexLog
