// AgentEdits.h — pure C++17. What one agent session changed in one file: the
// Agents panel's page for a file under a session (A1b, "what did that
// session do to this file?").
//
// A session's transcript (AgentTranscript.h) records every Edit, MultiEdit,
// Write and NotebookEdit call with its input: the text it replaced and the
// text it put there, or a whole file. `collect` reads those calls for one
// file, in order, with whether each was refused (an error result, which
// Claude Code did not apply) and whether a Write made the file. `analyze`
// then holds them against the file as it is now, last first:
//
// - an edit stands when the text it put in is still there; it is gone when
//   something changed that text since (the user, another session);
// - while each one is found exactly once, undoing them in turn gives the
//   file as it was before the session, which the page shows against now.
//   Anything that cannot be undone exactly (a text found twice, an edit
//   that only removed text, a replace_all of several places, a rewrite of a
//   file whose old text the transcript never held, a notebook cell) stops
//   that, with the reason, and the edits are still listed one by one.
//
// Read only, as every reader of transcripts: inputs are read from the file
// at their Span, on demand, never kept for the whole transcript. Paths are
// compared after resolving symlinks on both sides, and /private/tmp is
// /tmp. Text is UTF-8.
#pragma once
#include "AgentTranscript.h"

#include <string>
#include <vector>

namespace AgentEdits {

using AgentTranscript::Fs;

enum class Tool { Edit, MultiEdit, Write, NotebookEdit };
// Edit, MultiEdit, Write, NotebookEdit: the calls that change a file.
bool isEditTool(const std::string &name);
const char *toolName(Tool t);

enum class Standing { Unknown, Stands, Gone };

struct Change {
    Tool tool = Tool::Edit;
    std::string toolUseId;
    double time = 0;                 // when it was asked for (Unix seconds)
    // Edit and each part of a MultiEdit: what was there and what went in.
    // Write: newText is the whole file. NotebookEdit: newText is the cell's
    // new source, oldText empty.
    std::string oldText, newText;
    bool replaceAll = false;
    int part = 0, parts = 1;         // a MultiEdit's parts, in order
    bool answered = false;           // its result is in the transcript
    bool refused = false;            // the result was an error: not applied
    bool created = false;            // a Write that made the file
    bool cut = false;                // its input was too large to read whole
    Standing standing = Standing::Unknown;   // analyze's
};

// The changes one call's input makes (the input object as JSON text). Empty
// when the input does not read or is not an edit of a file.
std::vector<Change> changesFromInput(Tool tool, const std::string &inputJson);

struct FileHistory {
    std::string transcript, file;
    std::vector<Change> changes;     // in the order they were made
    int unreadable = 0;              // calls whose input could not be read
};

// Every change one session made to `file`, from the transcript's start.
// An input larger than `maxInput` bytes is left out and counted as
// unreadable.
FileHistory collect(const Fs &fs, const std::string &transcript, const std::string &file,
                    size_t maxInput = 16u << 20);

// Do two spellings name the same file? Resolved paths (Fs::realpath when
// it answers), and /private/tmp and /tmp (or /private/var and /var) alike.
bool samePath(const Fs &fs, const std::string &a, const std::string &b);

struct Result {
    std::vector<Change> changes;     // standing filled in; refused ones as they were
    bool rebuilt = false;            // `before` is the file before the session
    bool existedBefore = true;       // false: the session's Write made it
    std::string before;
    std::string why;                 // when not rebuilt: the first reason, as a sentence
    int standing = 0, gone = 0;      // counts of changes that stand and that are gone
};

// The changes held against the file as it is (`exists` false: there is no
// file now).
Result analyze(std::vector<Change> changes, const std::string &current, bool exists);

// How many times `needle` occurs in `hay`, not overlapping (0 for an empty
// needle), stopping at `limit`.
size_t occurrences(const std::string &hay, const std::string &needle, size_t limit = 2);

}  // namespace AgentEdits
