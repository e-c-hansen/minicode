// AgentIndex.h — pure C++17, no dependencies. The Agents panel's model: which
// agent sessions worked in a folder, what each one wrote, and where the work
// lives (its worktree, its scratchpad). Read-only toward everything it reads.
//
// Sources (Claude Code's, read through AgentTranscript):
//   <projects>/<encoded-cwd>/<sessionId>.jsonl                 a main session
//   <projects>/<encoded-cwd>/<sessionId>/subagents/agent-<id>.jsonl
//   <projects>/<encoded-cwd>/<sessionId>/subagents/agent-<id>.meta.json
//   <tmpRoot>/<encoded-cwd>/<sessionId>/scratchpad             its scratch folder
// The encoded folder names are lossy ('/' and '.' both become '-'), so they
// are never decoded: a session belongs to the folder when a `cwd` inside its
// transcript is the folder or below it, and a scratchpad is found by listing
// `<tmpRoot>/*/<sessionId>/scratchpad`.
//
// Using it from a GUI (the design this was built for):
//
//   AgentIndex::Index index(AgentTranscript::posixFs(),
//                           {"~/.claude/projects (expanded)", "/private/tmp/claude-<uid>"});
//   index.setFolder(windowFolder);
//   // On a background serial queue, at first and on every FSEvents callback
//   // for <projects> (latency about 0.5 s; the spec wants the panel current
//   // within 2 s of a write):
//   if (index.refresh()) { auto rows = index.rows(); ... hand copies to main }
//
// refresh() lists the project folders and stats every transcript; a file
// whose size, date and inode are unchanged is not read, a grown one is read
// from where the last read stopped, and one whose first `cwd` is outside the
// folder is never read past that line. The Index is not thread-safe: keep it
// on one queue, and copy what the main thread shows (Row points into it and
// is valid only until the next refresh or setFolder).
#pragma once
#include "AgentTranscript.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace AgentIndex {

using AgentTranscript::Fs;

// Where a written file is, for the panel's three groups (R2.6).
enum class Place {
    Main,       // in the window's folder (not in a worktree under it)
    Worktree,   // in the session's worktree, or another one under the folder
    Outside,    // anywhere else: scratchpads, /tmp, the home folder
};

struct FileTouch {
    std::string path;           // absolute, as the tool call gave it
    Place place = Place::Outside;
    int writes = 0;             // Write calls not refused
    int edits = 0;              // Edit, MultiEdit and NotebookEdit calls not refused
    int failed = 0;             // calls whose result was an error
    double last = 0;            // time of the last call
    bool touched() const { return writes + edits > 0; }
};

struct Session {
    std::string transcript;     // the .jsonl; also the session's key
    std::string metaPath;       // a subagent's .meta.json ("" for a main session)
    std::string sessionId;      // from the lines (a subagent has its parent's)
    std::string agentId;        // a subagent's id ("" for a main session)
    std::string parent;         // transcript of the session that started it, or ""
    std::string parentToolUseId;   // meta toolUseId: the Agent call that started it

    // What the panel shows as its name: meta `description`, else the first
    // 80 characters of the first prompt, else the agent's own title.
    std::string description;
    std::string firstPrompt;    // first prompt, whitespace collapsed, 80 characters
    std::string title;          // the last ai-title line's title
    std::string agentType;      // meta agentType

    std::string cwd;            // the first cwd seen
    std::string gitBranch;      // the last gitBranch seen
    std::string worktreePath;   // meta worktreePath, else a .claude/worktrees/<x> cwd
    std::string worktreeBranch; // meta worktreeBranch, else gitBranch there
    std::vector<std::string> versions;   // distinct agent versions seen, in order

    double started = 0;         // first timestamp
    double lastActivity = 0;    // last timestamp (else the file's date)
    double written = 0;         // the transcript's modification time
    bool hasMeta = false;
    bool finished = false;      // meta finished == true

    int lines = 0;              // records read
    int badLines = 0;           // lines skipped as unreadable (R2.2)
    int prompts = 0;
    int toolUses = 0;
    int bashCommands = 0;
    int filesTouched() const;   // files with a write or edit not refused

    std::vector<FileTouch> files;   // in the order first touched
    std::string scratchpad;     // "" until found
    std::vector<std::string> agentCalls;   // ids of its Agent tool calls
    std::vector<std::string> spawned;      // subagent ids its Agent calls reported

    // Running: written within `window` seconds and not marked finished.
    bool running(double now, double window = 120) const;
};

struct Options {
    std::string projectsDir;    // ~/.claude/projects, expanded
    std::string tmpRoot;        // /private/tmp/claude-<uid> ("" to skip scratchpads)
};

struct RefreshStats {
    int transcripts = 0;        // .jsonl files seen
    int read = 0;               // of them, read this time
    uint64_t bytesRead = 0;
    int badLines = 0;
};

class Index {
public:
    Index(Fs fs, Options options,
          const AgentTranscript::Format &format = AgentTranscript::claudeCode());
    ~Index();
    Index(const Index &) = delete;
    Index &operator=(const Index &) = delete;

    // The window's folder. A different folder starts the index afresh.
    void setFolder(const std::string &folder);
    const std::string &folder() const { return folder_; }

    // Reads what changed. True when anything a row shows changed (a session
    // came, went or grew, or a meta file changed). A missing projects folder
    // is an empty index, not an error.
    bool refresh(RefreshStats *stats = nullptr);

    struct Row {
        const Session *session;
        int depth;              // 0 for a top-level session, 1 for its subagents, ...
    };
    // Sessions newest activity first, each followed by the subagents it
    // started (newest first), nested to any depth (R2.5, R2.7).
    std::vector<Row> rows() const;
    std::vector<const Session *> sessions() const;   // unordered
    const Session *find(const std::string &transcript) const;
    // Every known scratchpad, newest session first, no duplicates.
    std::vector<std::string> scratchpads() const;
    // Worktrees the sessions worked in (meta worktreePath, or cwd under
    // .claude/worktrees), for path resolution before `git worktree list`
    // has answered.
    std::vector<std::string> worktrees() const;

    // Approximate bytes the index holds (R2.3: at most 10 MB for 50 sessions).
    size_t memoryEstimate() const;

private:
    struct File;
    Fs fs_;
    Options opt_;
    const AgentTranscript::Format &format_;
    std::string folder_, folderReal_;
    std::map<std::string, std::unique_ptr<File>> files_;   // by transcript path
    std::map<std::string, std::string> realCache_;

    bool inFolder(const std::string &cwd);
    bool readFile(File &f, const AgentTranscript::Stat &st, RefreshStats &rs);
    bool readMeta(File &f);
    void classify(Session &s) const;
    void link();
    void findScratchpads(bool all);
};

// The groups' order and names, for the panel.
const char *placeName(Place p);    // "In the folder", "In its worktree", "Outside the folder"
Place classify(const std::string &path, const std::string &folder,
               const std::string &worktree);

// A prompt as a one-line description: tags the harness wraps around slash
// commands dropped, whitespace collapsed, cut at `max` characters (UTF-8
// characters, an ellipsis added when cut).
std::string describePrompt(const std::string &text, size_t max = 80);

// ------------------------------------------------------- worktrees (R2.8)
struct Worktree {
    std::string path;
    std::string head;           // commit hash
    std::string branch;         // "worktree-agent-x" (refs/heads/ dropped), "" when detached
    bool main = false;          // the first entry: the repository's own folder
    bool bare = false, detached = false, locked = false, prunable = false;
    // Joined from the index: the subagent whose worktree it is.
    std::string agentId;        // meta's agent id, or the folder name's "agent-<id>"
    std::string description;    // its meta description, "" when unknown
    std::string transcript;     // the subagent's transcript, "" when unknown
    bool running = false;
};

// `git worktree list --porcelain` (newline- or, with -z, NUL-separated).
std::vector<Worktree> parseWorktreeList(const std::string &out);
// Those inside `folder` other than the main one, labelled from the index:
// a worktree is a subagent's when its meta worktreePath is the worktree's
// path, or else when its agent id is the folder name (agent-<id>).
std::vector<Worktree> worktreesUnder(const std::vector<Worktree> &all,
                                     const std::string &folder, const Index *index,
                                     double now);

// ------------------------------------------- paths in terminal output (R2.10)
struct Roots {
    std::string cwd;                       // the terminal's folder
    std::string folder;                    // the window's folder
    std::vector<std::string> worktrees;    // worktrees under it
    std::vector<std::string> scratchpads;  // the indexed sessions' scratchpads
    std::string home;                      // for ~/
};

struct Match {
    std::string path;           // existing, normalized
    std::string root;           // the root it was found under ("" for absolute)
    double mtime = 0;
    bool isDir = false;
};

struct Resolution {
    std::string target;         // the path as written, without :line:col
    int line = 0, column = 0;   // 1-based, 0 when not given
    std::vector<Match> matches; // newest first; one opens directly, several pick
    std::vector<std::string> tried;   // what was looked in, in order (for "not found in …")
};

// `text` is what was clicked: a path, optionally with :line[:col] (or any
// form TermLinks reads, such as `File "x", line 3`). An absolute path is
// itself; ~/ is the home folder; a relative path is tried under each root in
// order (cwd, folder, worktrees, scratchpads; empty and repeated roots
// skipped) and every one that exists is a match.
Resolution resolvePath(const std::string &text, const Roots &roots, const Fs &fs);
Resolution resolveTarget(const std::string &target, int line, int column, const Roots &roots,
                         const Fs &fs);

// Lexical: "a/./b//c/../d" -> "a/b/d", keeping a leading '/'.
std::string normalizePath(const std::string &path);
bool isInside(const std::string &path, const std::string &folder);   // or equal

}  // namespace AgentIndex
