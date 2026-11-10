// FormatRun.h — pure C++. Format Document's decisions around the plan
// (Formatter.h): which copy of which formatter runs for a file, or why none
// does (the folder's answer about its own programs, a configuration that
// runs code, nothing installed); the command line and folder it runs in;
// what its exit means for the buffer; the question put to the user; and the
// formatter's output as line runs to put in place of the buffer's text.
// No I/O of its own: the file system and the program search come in `Env`.
//
// The Mac's FormatRunner.mm makes the same decisions in Objective-C++
// (MCFormatText), with the same words; the GTK port's FormatRunner.cpp runs
// these. A rule or a wording changes in both until the Mac uses this file.
#pragma once
#include "Formatter.h"
#include "LineDiff.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace FormatRun {

// What the decisions may look at.
struct Env {
    Formatter::Fs fs;   // the project's files (exists, read)
    // Is there a runnable file (not a folder) at this path?
    std::function<bool(const std::string&)> executable;
    // A program by name on PATH and in the usual folders, or a path as
    // given when it is runnable; "" when none.
    std::function<std::string(const std::string&)> find;
    // A path with its symlinks resolved; the path itself when that fails.
    // Identity when not given.
    std::function<std::string(const std::string&)> realPath;
    // $VIRTUAL_ENV where minicode started ("" for none): activated by the
    // user, so its bin is trusted, as the servers trust it.
    std::string virtualEnv;
    std::string home;   // a file above the folder is named from "~"
    // Where Ask Again About This Folder's Tools is, for the messages.
    std::string askAgainMenu = "minicode";
};

enum class Kind {
    Run,            // `argv` in `cwd`
    Unavailable,    // nothing to run: no formatter for the file, or format.<lang> = off
    NeedsConsent,   // the folder's own program, not asked about yet: ask, then decide again
    Failed,         // `message` says why nothing runs
};

struct Decision {
    Kind kind = Kind::Failed;
    // Unavailable because no formatter is planned for the file at all
    // (Formatter::serverMayFormat): a running language server that formats
    // it may; `message` is the note for when none does.
    bool serverMayFormat = false;
    std::string message;
    // What to install, best first ("pyink", "yapf"), when none was found.
    std::vector<std::string> installPrograms;
    std::string tool;    // the program's name ("pyink"), "" when none was chosen
    // Which style it applies: "Google style", "the project's .clang-format",
    // "format.python in the settings".
    std::string style;
    std::vector<std::string> argv;   // Run: argv[0] is the program's path
    std::string cwd;                 // Run: the file's folder (the caller checks it is one)
    bool ranged = false;             // only the asked lines are formatted
    // NeedsConsent: the folder the program comes from as the window's folder
    // names it (".venv", "web/node_modules", "package.json"), "" when the
    // program itself is installed outside the folder; and the folder's
    // configuration that runs code when the tool reads it
    // ("prettier.config.js", or a whole path from "~" above the folder), "".
    std::string consentFolder, consentConfig;
};

// The decision for formatting `path` in the window's folder `root` (both as
// the window spells them; the plan is made on their resolved spellings).
// `setting` is format.<lang> for the file's language
// (Formatter::languageFor), "" when unset; `consent` the folder's answer,
// "allow", "deny" or "". firstLine..lastLine (1-based) asks for those lines
// only, 0 and 0 for the whole file.
Decision decide(const std::string& path, const std::string& root, const std::string& setting,
                const std::string& consent, int firstLine, int lastLine, const Env& env);

// The question for a NeedsConsent decision: Allow, Don't Allow, Cancel.
struct Question {
    std::string message, detail;
};
Question consentQuestion(const Decision& d);

// What a run's end means for the buffer. `launchError` is set when the
// program could not be started ("could not be started") or left its output
// open after exiting; `inputBytes` is the size of the text it was given.
struct Outcome {
    bool ok = false;
    std::string text;      // the formatted text, when ok
    std::string message;   // why the buffer stays as it is, when not
};
Outcome outcome(const std::string& tool, const std::string& path, bool timedOut,
                double timeoutSeconds, const std::string& launchError, int status,
                const std::string& out, const std::string& err, std::size_t inputBytes);

// The note for a format that changed nothing: "Already formatted (pyink,
// Google style)."
std::string alreadyFormatted(const std::string& tool, const std::string& style);

// The formatter's output put in place of a text as the smallest set of
// changed line runs (LineDiff), so lines it left alone are never touched,
// in what GtkTextIter counts: code points, with UTF-8 text. map() moves a
// position in the old text to the same place in the new one (a caret before
// a word stays before it), as the Mac's replaceSourceWithText does in UTF-16.
class Replacement {
public:
    Replacement(const std::string& before, const std::string& after);
    struct Change {
        std::size_t start = 0;    // code points from the text's start
        std::size_t length = 0;   // code points replaced
        std::string text;         // UTF-8
    };
    const std::vector<Change>& changes() const { return changes_; }
    bool empty() const { return changes_.empty(); }
    std::size_t map(std::size_t point) const;

private:
    std::u16string from_, to_;
    std::vector<LineDiff::Hunk> hunks_;
    std::vector<Change> changes_;
};

// Lines `firstLine`..`lastLine` (1-based, counted at '\n' as the formatters
// count them) that a selection of [start, end) code points of `text` covers:
// a selection ending just after a line break leaves that next line out.
// 0 and 0 for an empty selection.
void selectedLines(const std::string& text, std::size_t start, std::size_t end, int* firstLine,
                   int* lastLine);

}  // namespace FormatRun
