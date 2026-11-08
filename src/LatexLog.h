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
// A typeset that works is read too, for its warnings: tectonic prints
// TeX's overfull and underfull boxes as "warning: file:line: ..." lines,
// and the transcript it keeps with --keep-logs (<stem>.log) holds LaTeX's
// undefined references and citations, multiply defined labels, and package
// warnings (parseTranscriptWarnings). Underfull boxes are left out, as
// LaTeX Workshop's default filter leaves them.
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

// Every problem in tectonic's output, in order, each once. An overfull
// box "in paragraph at lines a--b" is placed at line a (tectonic's summary
// names b, where the paragraph ended); underfull boxes and tectonic's own
// "warnings were issued" line are left out.
std::vector<Problem> parse(const std::string &output);

// The warnings in TeX's transcript (the .log file), in order, each once:
// "LaTeX Warning: Reference `x' on page 1 undefined on input line 4." and
// the like for citations, a package's or class's warning that names an
// input line, and "Label `x' multiply defined" (which names none). Each is
// in the file TeX had open when it was written (the innermost "(name" not
// yet closed; empty when it names no line). Lines TeX wrapped at 79
// characters are joined first. LaTeX's closing summaries ("There were
// undefined references"), font substitutions and Info lines are left out;
// boxes come from tectonic's own lines (parse).
std::vector<Problem> parseTranscriptWarnings(const std::string &log);

// What the preview lists for one typeset: tectonic's output and the
// transcript, errors first, then warnings, each once and in order.
std::vector<Problem> collect(const std::string &output, const std::string &transcript);

// The file a problem is about. `texPath` is the user's document and
// `siblingPath` the hidden copy tectonic typeset in its place (same folder,
// same lines), so a problem in the sibling, or one naming no file, is about
// `texPath`. Another name is read relative to the document's folder, with
// ".tex" added when only that exists, as TeX's \input finds it. Returns ""
// when the name leads to no file.
std::string sourcePath(const std::string &file, const std::string &texPath,
                       const std::string &siblingPath,
                       const std::function<bool(const std::string &)> &exists);

// The main document a part names in a magic comment, LaTeX Workshop's (and
// TeXShop's) convention: a line "% !TEX root = ../main.tex" (any case of
// TeX, spaces optional). The value as written; "" when there is none.
std::string rootComment(const std::string &text);
// That value read against the folder of `partPath` (absolute), with "."
// and ".." steps taken.
std::string resolveRoot(const std::string &partPath, const std::string &value);

}  // namespace LatexLog
