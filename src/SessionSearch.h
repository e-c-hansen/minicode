// SessionSearch.h — the session host's heavier reads of the workspace
// (SESSION-DESIGN.md, stage 7): Quick Open's list of the folder's files and
// Find in Folder, for a browser's page of the session (SessionWeb.h). A
// renderer asks as for any read, FsRead {req, window, json: {"op", ...}}
// (SessionFiles.h), and the host answers with FsData; these ops run on
// worker threads of the host's own (ReadWorkers), never on the host's
// thread, where every keystroke and terminal byte is handled, and a search
// answers with several FsData, the last saying "done". A host offers them
// in its Welcome (flag 8), so a renderer never sends them to a host from
// before them, which would answer "no read" (the message itself is not
// new, so nothing drops the connection either way).
//
//   {"op": "files", "hidden"}
//     -> {"op": "files", "files": [path, ...], "truncated", "done": true}
//   the folder's files as Quick Open lists them (CommandPalette::listFiles:
//   Find in Folder's skips, what .gitignore and .ignore ignore left out,
//   nearer files first, at most kMaxListedFiles), held to the folder (a
//   symlink out of it neither listed nor entered) and with the page's
//   dotfile rule (none unless "hidden", as the tree).
//
//   {"op": "search", "query", "caseSensitive", "wholeWord", "regex",
//    "include", "exclude", "hidden"}
//     -> {"op": "search", "rows": [row, ...], "done": false}, ... then
//        {"op": "search", "rows": [...], "done": true, "filesSearched",
//         "filesMatched", "totalMatches", "truncated", "timedOut", "tooSlow",
//         "cancelled", "error", "skipped"}
//   the core's FolderSearch::search over the folder with the Mac's Find in
//   Folder options (its row cap, kMaxSearchRows; one line of context above
//   and below, repeats dropped; the regex fallback's own time limit,
//   "tooSlow"), held to the folder and the dotfile rule as above, and a
//   time limit of its own (kSearchSeconds, "timedOut"). Rows come as the
//   walk takes them, in its order, a batch at most every kBatchMs (or
//   kBatchRows rows); each row is {path (from the folder), line, column
//   (1-based, code points), text (the line for display), hl ([[start,
//   length], ...] the matches in `text`, UTF-16 units, as the Mac's row
//   highlights them), before, after (context lines), count}.
//
//   {"op": "cancel", "req": n}
//   stops the renderer's own read `n` (its last answer says "cancelled");
//   it has no answer of its own.
//
//   {"op": "replace", "query", "caseSensitive", "wholeWord", "regex",
//    "replacement", "hidden", "files": [{"path", "rows": [{"line", "text",
//    "count", "on"}, ...]}, ...]}
//     -> {"op": "replace", "done": true, "replaced" (matches), "files"
//        (files written), "edited" (open documents edited), "editedMatches",
//        "skipped": [{"path", "reason", "why"}, ...], "cancelled"}
//   Replace in Folder from a page that may edit (a host's Welcome flag 16;
//   a reader's is refused): only in the files and on the rows the page's
//   search showed, each row (a line, every match on it) replaced unless the
//   page took it out ("on" false). A file is left alone whole, and named,
//   when it changed since the search: a row's line no longer reads as the
//   row showed it, or the file holds another count of matches
//   (replaceRows). The host edits a document it holds itself, on its own
//   thread, as one undoable step per file ("Replace in Folder"), so every
//   view and window showing it sees the edit, and leaves one with unsaved
//   changes alone; it hands the rest to a worker ("files" then holds only
//   those, each with its "abs" path, and "edited" what the host did),
//   which rewrites each with the save rules (its own encoding and mark,
//   atomically; one whose encoding cannot store the replacement, or that
//   changed on disk while it was read, is left alone and named).
//
// At most kMaxReadJobs run at once on a host; one more is answered at once
// with {"op", "error", "done": true}. A renderer that goes has its reads
// cancelled. Pure C++17 and POSIX threads.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "FolderSearch.h"
#include "Json.h"
#include "SessionProtocol.h"

namespace Shared {

constexpr std::size_t kMaxListedFiles = 20000;   // the app's Quick Open
constexpr std::size_t kMaxSearchRows = 20000;    // the Mac's Find in Folder
constexpr double kSearchSeconds = 30;            // a search's time on the host
constexpr double kListSeconds = 15;              // a files list's
constexpr std::size_t kMaxReadJobs = 8;          // at once, on one host
constexpr std::size_t kBatchRows = 500;
constexpr int kBatchMs = 100;

// Whether `op` is one of these reads (a worker's), and "cancel".
bool isWorkerRead(const std::string& op);

// ---- Replace in Folder from a page
// The most rows one replace takes (the search's own cap).
constexpr std::size_t kMaxReplaceRows = kMaxSearchRows;
// The largest file a replace reads whole (as the host follows a file).
constexpr std::uintmax_t kMaxReplaceBytes = std::uintmax_t(64) << 20;
// A row of the page's search as the replace gets it back: its line
// (1-based), the line's text as the row showed it (FolderSearch::
// displayText), its matches, and whether the page left it in.
struct ReplaceRow {
    int line = 0;
    std::string text;
    std::size_t count = 1;
    bool on = true;
};
enum class ReplaceCheck {
    Replaced,   // *out holds the text with the rows' matches replaced
    Nothing,    // every row taken out: nothing to replace
    Changed,    // the text is not as the search read it: left alone
};
// `content` (UTF-8, a file's text or a document's) with the matches on the
// rows left in replaced (FolderSearch::replaceInText, its query from the
// options), after checking it is the text the search read: every row's line
// still reads as the row showed it, and the text holds as many matches as
// the rows counted. *replaced says how many were replaced.
ReplaceCheck replaceRows(const std::string& content, const std::vector<ReplaceRow>& rows, const std::string& query,
                         const std::string& replacement, const FolderSearchOptions& options, std::string* out,
                         std::size_t* replaced);
// Whether a name in `rel` starts with '.': hidden on a page that shows no
// dotfiles, so never replaced in from it.
bool replaceHidesPath(const std::string& rel);
// A replace request's rows for one file ({"rows": [...]}), at most `most`
// (fewer when the request held more), or false for rows that are not rows.
bool replaceRowsFrom(const Json& rows, std::size_t most, std::vector<ReplaceRow>* out);

// A search request's options for FolderSearch (the Mac's, plus the page's
// rules: confine, and skipDotfiles unless "hidden").
FolderSearchOptions searchOptions(const Json& request);

// One row as the answer carries it: `hl` from `query` over the row's
// display text.
Json searchRowJson(const FolderSearchMatch& m, const TextFind::Query& query);

// Runs one read in `root`, on the calling thread: `emit` gets each answer
// in turn (one for "files", several for "search", the last with "done").
// Stops early when `cancel` goes up or `seconds` pass (0 for no limit).
// `emit` may be called from a second thread of the read's own (a batch
// that waited kBatchMs), never two at once. `eachFile`, for checks, runs
// after each file a search reads (it slows a search down). For the
// workers, and for checks.
void runWorkerRead(const std::string& root, const Json& request, std::atomic<bool>& cancel,
                   const std::function<void(Json&&)>& emit, double seconds,
                   const std::function<void()>& eachFile = nullptr);

// The host's workers (Host owns one, made at its first such read): a thread
// per read, answers queued for the host's thread, which polls fd() and
// takes them (Host::takeReads). Every method is the host thread's.
class ReadWorkers {
public:
    ReadWorkers();
    ~ReadWorkers();   // cancels every read, and waits for each to end
    ReadWorkers(const ReadWorkers&) = delete;
    ReadWorkers& operator=(const ReadWorkers&) = delete;

    // Starts a read for renderer `c`'s request `req` (FsRead's window, for
    // its answers) in `root`: false, with why, when kMaxReadJobs run.
    // `seconds` overrides the op's own time limit (checks; < 0 the op's).
    bool start(ClientId c, std::uint64_t req, std::uint64_t window, const std::string& root,
               const Json& request, std::string* why, double seconds = -1);
    // Stops a read (its last answer still comes, saying "cancelled"), or
    // every read of a renderer that went.
    void cancel(ClientId c, std::uint64_t req);
    void cancelAll(ClientId c);

    struct Answer {
        ClientId to = 0;
        std::uint64_t req = 0, window = 0;
        Json json;
    };
    // What the workers answered since the last take, in order per read.
    std::vector<Answer> take();
    // Readable while answers wait (a pipe), for the host's poll(2).
    int fd() const { return wake_[0]; }
    // Reads not finished (their last answer queued or not).
    std::size_t running() const;
    // For checks: waits until an answer waits or no read runs (at most
    // `timeoutMs`); true when one waits.
    bool wait(int timeoutMs);
    // For checks: runs after each file a search reads, on its threads
    // (runWorkerRead's eachFile). Set before a read starts.
    std::function<void()> eachFile;

private:
    struct Job {
        ClientId to = 0;
        std::uint64_t req = 0, window = 0;
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
        std::thread thread;
    };
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Answer> queue_;
    std::vector<std::unique_ptr<Job>> jobs_;
    int wake_[2] = {-1, -1};
    bool wakePending_ = false;   // under mu_
    void push(Answer&& a);
    // Joins the jobs that finished.
    void reap();
};

}  // namespace Shared
