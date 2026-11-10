// FormatRunner.h — Format Document's process side (PARITY.md 2.4), the
// counterpart of the Mac's FormatRunner.mm: FormatRun's decision (which
// formatter, which copy of it, or why none), then the program run on the
// buffer's text on a thread of its own: the text on stdin, the formatted
// text from stdout, in the file's folder (so it finds the project's
// configuration), with PATH widened to the folders programs are looked for
// in (Lsp.h, LspSearchDirs) and PYTHONUTF8=1. A run past the limit gets
// SIGTERM, and SIGKILL a second later; a program that exits but leaves its
// output open (a child of its own holding the pipe) is let go after two
// seconds. Formatting.cpp applies the result.
#pragma once

#include "FormatRun.h"

#include <functional>
#include <string>

namespace FormatRunner {

struct Result {
    FormatRun::Decision decision;   // what was decided (NeedsConsent: ask, then again)
    bool ok = false;                // `text` is the formatted text
    std::string text;
    std::string message;            // why the buffer stays as it is ("" when ok)
};

// Decide and run, off the main thread; `done` comes on the main thread.
// `setting` is format.<lang> for the file's language, `consent` the
// folder's answer (FolderConsent::answer), both read by the caller on the
// main thread. firstLine..lastLine (1-based) formats those lines only where
// the formatter can; 0 and 0 for the whole file.
void format(const std::string& text, const std::string& path, const std::string& root,
            const std::string& setting, const std::string& consent, int firstLine,
            int lastLine, double timeoutSeconds, std::function<void(Result)> done);

// Runs started and not yet ended, and the process id of the last one
// started (checks: a formatter killed at the limit is gone).
int running();
int lastPid();

}  // namespace FormatRunner
