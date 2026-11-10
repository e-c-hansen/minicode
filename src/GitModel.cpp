// GitModel.cpp — see GitModel.h.
#include "GitModel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>

namespace GitUi {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' ||
                     s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

std::vector<std::string> nonBlankLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) nl = s.size();
        std::string t = trim(s.substr(start, nl - start));
        if (!t.empty()) out.push_back(t);
        start = nl + 1;
    }
    return out;
}

}  // namespace

// ------------------------------------------------------------------ rows
std::string Row::key() const {
    return std::string(staged ? "1:" : "0:") + (header ? title : path);
}

void applyStatus(Snapshot& s, const Git::Status& st) {
    s.initial = st.initial;
    s.headOid = st.oid;
    s.branch = st.branch;
    s.detached = st.detached;
    s.upstream = st.upstream;
    s.hasAheadBehind = st.hasAheadBehind;
    s.ahead = st.ahead;
    s.behind = st.behind;
    s.branchText = Git::branchLabel(st);
    if (st.initial) s.branchText += "  (no commits yet)";

    std::vector<Row> staged, changes;
    for (const Git::Entry& e : st.entries) {
        if (Git::isStaged(e)) {
            Row r;
            r.staged = true;
            r.letter = Git::stagedLetter(e);
            r.path = e.path;
            r.origPath = e.origPath;
            r.submodule = e.submodule;
            staged.push_back(r);
        }
        if (Git::hasUnstaged(e)) {
            Row r;
            r.letter = Git::unstagedLetter(e);
            r.path = e.path;
            r.untracked = e.untracked;
            r.unmerged = e.unmerged;
            r.submodule = e.submodule;
            changes.push_back(r);
        }
    }
    s.rows.clear();
    auto section = [&](const char* title, const std::vector<Row>& items, bool isStaged) {
        if (items.empty()) return;
        Row h;
        h.header = true;
        h.staged = isStaged;
        h.title = std::string(title) + "  " + std::to_string(items.size());
        s.rows.push_back(h);
        s.rows.insert(s.rows.end(), items.begin(), items.end());
    };
    section("Staged changes", staged, true);
    section("Changes", changes, false);
    if (s.rows.empty()) s.notice = "No changes.";
}

bool parseTopLevel(const std::string& out, std::string& top, std::string& gitDir) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < out.size()) {
        size_t nl = out.find('\n', start);
        if (nl == std::string::npos) nl = out.size();
        std::string l = out.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        start = nl + 1;
    }
    if (lines.empty() || lines[0].empty()) return false;
    top = lines[0];
    gitDir = lines.size() > 1 ? lines[1] : "";
    return true;
}

bool graphWanted(const Snapshot& s) {
    return s.inRepository && !s.initial && !s.headOid.empty() && s.errorText.empty();
}

std::string summaryText(const Snapshot& s) {
    if (s.detached) return "HEAD is detached, so there is no upstream to compare with.";
    if (s.upstream.empty())
        return (s.branch.empty() ? std::string("This branch") : s.branch) +
               " has no upstream, so nothing here is marked as pushed or not.";
    if (!s.hasAheadBehind) return "The upstream " + s.upstream + " is gone.";
    if (!s.ahead && !s.behind) return "Up to date with " + s.upstream + ".";
    std::string parts;
    if (s.ahead) parts = "↑ " + std::to_string(s.ahead) + " to push";
    if (s.behind) {
        if (!parts.empty()) parts += ", ";
        parts += "↓ " + std::to_string(s.behind) + " to pull";
    }
    return parts + ", against " + s.upstream;
}

std::string failureText(int status, const std::string& out, const std::string& err) {
    std::vector<std::string> e = nonBlankLines(err);
    if (!e.empty()) {
        std::string r;
        for (const std::string& l : e) r += (r.empty() ? "" : "\n") + l;
        return validUtf8(r);
    }
    std::vector<std::string> o = nonBlankLines(out);
    if (!o.empty()) return validUtf8(o.back());
    return "git exited with status " + std::to_string(status) + ".";
}

std::string firstLine(const std::string& out) {
    size_t nl = out.find('\n');
    return validUtf8(nl == std::string::npos ? out : out.substr(0, nl));
}

int nextFileRow(const std::vector<Row>& rows, int from, int step) {
    const int n = (int)rows.size();
    int r = from;
    if (r < 0) r = step > 0 ? -1 : n;
    for (r += step; r >= 0 && r < n; r += step)
        if (!rows[r].header) return r;
    return -1;
}

int lastFileRow(const std::vector<Row>& rows) {
    return nextFileRow(rows, -1, -1);
}

int pickAfterRefresh(const std::vector<Row>& oldRows, int sel, const std::vector<Row>& rows) {
    if (sel < 0 || sel >= (int)oldRows.size() || oldRows[sel].header) return -1;
    const Row& was = oldRows[sel];
    int wasIndex = 0;   // its place in its own list
    for (int i = sel; i >= 0 && !oldRows[i].header; --i) wasIndex = sel - i;
    const int n = (int)rows.size();
    for (int i = 0; i < n; ++i)
        if (!rows[i].header && rows[i].key() == was.key()) return i;
    int head = -1;
    for (int i = 0; i < n; ++i)
        if (rows[i].header && rows[i].staged == was.staged) { head = i; break; }
    if (head >= 0) {
        int count = 0;
        while (head + 1 + count < n && !rows[head + 1 + count].header) ++count;
        if (count) return head + 1 + std::min(wasIndex, count - 1);
    }
    for (int i = 0; i < n; ++i)
        if (!rows[i].header) return i;
    return -1;
}

std::vector<int> keepChosen(const std::vector<Row>& oldRows, const std::vector<int>& chosen,
                            const std::vector<Row>& rows) {
    std::vector<std::string> keys, paths;
    for (int i : chosen)
        if (i >= 0 && i < (int)oldRows.size() && !oldRows[(size_t)i].header) {
            keys.push_back(oldRows[(size_t)i].key());
            paths.push_back(oldRows[(size_t)i].path);
        }
    std::vector<int> still;
    for (int byPath = 0; byPath < 2 && still.size() < 2 && !keys.empty(); byPath++) {
        still.clear();
        const std::vector<std::string>& want = byPath ? paths : keys;
        for (size_t i = 0; i < rows.size(); i++)
            if (!rows[i].header &&
                std::find(want.begin(), want.end(), byPath ? rows[i].path : rows[i].key()) !=
                    want.end())
                still.push_back((int)i);
    }
    if (still.size() < 2) still.clear();
    return still;
}

std::string rowToolTip(const Row& r) {
    if (r.header) return "";
    const char* what = r.unmerged ? "conflict" : r.untracked ? "untracked"
                     : r.staged ? "staged" : "changed";
    if (!r.origPath.empty())
        return r.path + " (renamed from " + r.origPath + "), " + what;
    return r.path + ", " + what;
}

unsigned letterColor(char c, bool unmerged) {
    if (unmerged) return 0xE4676B;
    switch (c) {
    case 'M': case 'T': return 0xE2C08D;
    case 'A': return 0x81B88B;
    case 'D': return 0xC74E39;
    case 'R': case 'C': case 'U': return 0x73C991;
    default: return 0x9CA3AF;
    }
}

// ----------------------------------------------------------- arguments
std::vector<std::string> topLevelArgs() {
    return {"rev-parse", "--show-toplevel", "--absolute-git-dir"};
}

std::vector<std::string> statusArgs() {
    return {"status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all"};
}

std::vector<std::string> refArgs() {
    return {"for-each-ref", std::string("--format=") + Git::refFormat(), "refs/heads",
            "refs/remotes", "refs/tags"};
}

std::vector<std::string> logArgs(int limit, bool all, bool compare) {
    std::vector<std::string> a = {"-c", "log.showSignature=false", "log", "-z",
                                  "--topo-order", "--no-color",
                                  std::string("--format=") + Git::logFormat(),
                                  "--max-count=" + std::to_string(limit)};
    if (all) { a.push_back("--branches"); a.push_back("--remotes"); }
    a.push_back("HEAD");
    if (compare) a.push_back("@{upstream}");
    a.push_back("--");
    return a;
}

std::vector<std::string> leftRightArgs() {
    return {"rev-list", "--left-right", "HEAD...@{upstream}", "--"};
}

std::vector<std::string> stageArgs(const Row& r, bool initial) {
    if (r.staged) {
        if (initial)
            return {"--literal-pathspecs", "rm", "--cached", "--force", "-q", "--", r.path};
        std::vector<std::string> a = {"--literal-pathspecs", "restore", "--staged", "--",
                                      r.path};
        if (!r.origPath.empty()) a.push_back(r.origPath);
        return a;
    }
    return {"--literal-pathspecs", "add", "-A", "--", r.path};
}

std::vector<std::string> diffArgs(const Row& r) {
    // --submodule=short: a submodule's diff stays one "Subproject commit"
    // line a side under its 160000 header whatever the user's
    // diff.submodule says (log and diff write no such header).
    std::vector<std::string> a = {"-c", "core.quotePath=false", "--literal-pathspecs",
                                  "diff", "--no-color", "--no-ext-diff", "--submodule=short",
                                  "--src-prefix=a/", "--dst-prefix=b/"};
    if (r.untracked) {
        a.insert(a.end(), {"--no-index", "--", "/dev/null", r.path});
    } else if (r.staged) {
        a.insert(a.end(), {"--cached", "-M", "--", r.path});
        if (!r.origPath.empty()) a.push_back(r.origPath);
    } else {
        a.insert(a.end(), {"--", r.path});
    }
    return a;
}

std::vector<std::string> showArgs(const std::string& hash) {
    return {"-c", "core.quotePath=false", "show", "--no-color", "--no-ext-diff",
            "--src-prefix=a/", "--dst-prefix=b/", "-M", "--diff-merges=first-parent",
            "--stat", "--patch", std::string("--format=") + Git::showFormat(), hash, "--"};
}

std::vector<std::string> commitArgs(const std::string& message) {
    return {"commit", "-m", message};
}

// ----------------------------------------- Stage All, Unstage All, Discard
namespace {
std::string fileName(const std::string& path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string plural(size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}
}  // namespace

ActionPlan stageAllPlan(const Snapshot& s) {
    ActionPlan p;
    if (!s.inRepository) { p.refusal = "This folder is not in a git repository."; return p; }
    size_t changes = 0, conflicts = 0;
    for (const Row& r : s.rows) {
        if (r.header || r.staged) continue;
        changes++;
        if (r.unmerged) conflicts++;
    }
    if (!changes) { p.refusal = "There are no changes to stage."; return p; }
    p.args = {"--literal-pathspecs", "add", "-A", "--", "."};
    if (conflicts) {
        p.question = "Stage " + plural(conflicts, "file that still has", "files that still have") +
                     " conflicts?";
        p.detail = "Staging a file marks its conflict resolved. Check that no conflict "
                   "markers are left in " + std::string(conflicts == 1 ? "it" : "them") + " first.";
        p.button = "Stage All";
    }
    p.doneText = "Staged " + plural(changes, "file.", "files.");
    return p;
}

ActionPlan unstageAllPlan(const Snapshot& s) {
    ActionPlan p;
    if (!s.inRepository) { p.refusal = "This folder is not in a git repository."; return p; }
    size_t staged = 0;
    for (const Row& r : s.rows)
        if (!r.header && r.staged) staged++;
    if (!staged) { p.refusal = "Nothing is staged."; return p; }
    if (s.initial)
        p.args = {"--literal-pathspecs", "rm", "--cached", "-r", "--force", "-q", "--", "."};
    else
        p.args = {"--literal-pathspecs", "restore", "--staged", "--", "."};
    p.doneText = "Unstaged " + plural(staged, "file.", "files.");
    return p;
}

DiscardPlan discardPlan(const Row& r) {
    DiscardPlan p;
    if (r.header || r.path.empty()) { p.refusal = "Choose a file to discard."; return p; }
    const std::string name = fileName(r.path);
    if (r.staged) {
        p.refusal = name + " is staged: unstage it first, then discard its changes.";
        return p;
    }
    if (r.unmerged) {
        p.refusal = name + " has a conflict: resolve it, or take one side in the terminal "
                    "(git checkout --ours or --theirs), rather than discard it.";
        return p;
    }
    if (r.submodule) {
        p.refusal = name + " is a submodule: discard inside it (open it as a folder) or run "
                    "git submodule update.";
        return p;
    }
    const std::string undo = "Undo Discard in this panel's menu, or Put Back in the Trash, "
                             "brings it back.";
    if (r.untracked) {
        p.trashFirst = true;
        p.question = "Move " + name + " to the Trash?";
        p.detail = name + " is not tracked by git, so discarding it removes the file. " + undo;
        p.button = "Move to Trash";
        p.doneText = "Moved " + name + " to the Trash.";
        return p;
    }
    if (r.letter == 'A') {   // intent to add: the index holds an empty file
        p.refusal = name + " was added with git add -N: unstage it first.";
        return p;
    }
    p.args = {"--literal-pathspecs", "restore", "--worktree", "--", r.path};
    if (r.letter == 'D') {
        p.question = "Restore " + name + "?";
        p.detail = name + " was deleted. Discarding that brings it back as git has it "
                   "(staged, or else in the last commit). Undo Discard deletes it again.";
        p.button = "Restore";
        p.doneText = "Restored " + name + ".";
        return p;
    }
    p.trashFirst = true;
    p.question = "Discard the changes to " + name + "?";
    p.detail = name + " goes back to the version git has for it (staged, or else in the "
               "last commit). The changed copy goes to the Trash: " + undo;
    p.button = "Discard Changes";
    p.doneText = "Discarded the changes to " + name + "; the changed copy is in the Trash.";
    return p;
}

std::string undoDiscardTitle(const std::string& path) {
    return "Undo Discard of " + fileName(path);
}

std::string undoneDiscardText(const std::string& path) {
    return "Put " + fileName(path) + " back as it was before the discard.";
}

int changesRowFor(const std::vector<Row>& rows, const std::string& path) {
    for (size_t i = 0; i < rows.size(); ++i)
        if (!rows[i].header && !rows[i].staged && rows[i].path == path) return (int)i;
    return -1;
}

// ------------------------------------------- several rows chosen at once
std::vector<std::string> stageRowsArgs(const std::vector<Row>& rows, bool initial, bool stage) {
    std::vector<std::string> paths;
    for (const Row& r : rows) {
        if (r.header || r.path.empty() || r.staged == stage) continue;
        paths.push_back(r.path);
        if (!stage && !initial && !r.origPath.empty()) paths.push_back(r.origPath);
    }
    if (paths.empty()) return {};
    std::vector<std::string> a;
    if (stage) a = {"--literal-pathspecs", "add", "-A", "--"};
    else if (initial) a = {"--literal-pathspecs", "rm", "--cached", "--force", "-q", "--"};
    else a = {"--literal-pathspecs", "restore", "--staged", "--"};
    a.insert(a.end(), paths.begin(), paths.end());
    return a;
}

DiscardBatch discardBatchPlan(const std::vector<Row>& rows) {
    DiscardBatch b;
    for (const Row& r : rows) {
        if (r.header) continue;
        DiscardPlan p = discardPlan(r);
        if (!p.refusal.empty()) {
            b.plans.clear();
            b.paths.clear();
            b.refusal = rows.size() > 1 ? "Nothing was discarded: " + p.refusal : p.refusal;
            return b;
        }
        b.plans.push_back(p);
        b.paths.push_back(r.path);
    }
    if (b.plans.empty()) { b.refusal = "Choose a file to discard."; return b; }
    if (b.plans.size() == 1) {
        static_cast<ActionPlan&>(b) = b.plans[0];
        return b;
    }
    size_t untracked = 0;
    for (const Row& r : rows)
        if (!r.header && r.untracked) untracked++;
    const std::string n = std::to_string(b.plans.size());
    b.question = "Discard the changes to " + n + " files?";
    b.detail = "Each goes back to the version git has for it (staged, or else in the last "
               "commit)";
    if (untracked) b.detail += ", and " + plural(untracked, "file git does not track",
                                                 "files git does not track") + " go" +
                               (untracked == 1 ? "es" : "") + " to the Trash";
    b.detail += ". The changed copies go to the Trash first: Undo Discard in this panel's "
                "menu brings them back, one file at a time.";
    b.button = "Discard " + n + " Files";
    b.doneText = "Discarded the changes to " + n + " files; the changed copies are in the Trash.";
    return b;
}

// ------------------------------------- amend, undo the last commit, stash
bool headIsPushed(const Snapshot& s) {
    return !s.detached && !s.upstream.empty() && s.hasAheadBehind && s.ahead == 0;
}

namespace {
bool anyChange(const Snapshot& s) {
    for (const Row& r : s.rows)
        if (!r.header) return true;
    return false;
}
const char* kNoRepo = "This folder is not in a git repository.";
}  // namespace

ActionPlan amendPlan(const Snapshot& s, const std::string& message) {
    ActionPlan p;
    if (!s.inRepository) { p.refusal = kNoRepo; return p; }
    if (s.initial || s.headOid.empty()) {
        p.refusal = "There is no commit to amend yet: commit first.";
        return p;
    }
    const std::string m = trim(message);
    if (m.empty()) p.args = {"commit", "--amend", "--no-edit"};
    else p.args = {"commit", "--amend", "-m", message};
    if (headIsPushed(s)) {
        p.question = "Amend a commit that is already pushed?";
        p.detail = "The last commit is on " + s.upstream + ". After amending it, pushing "
                   "needs a force, and anyone who has the old commit has to rebase onto the "
                   "new one.";
        p.button = "Amend";
    }
    // doneText stays empty: the panel says what commit printed, as for a
    // commit ("[main 1a2b3c4] Subject").
    return p;
}

ActionPlan undoCommitPlan(const Snapshot& s, int parents) {
    ActionPlan p;
    if (!s.inRepository) { p.refusal = kNoRepo; return p; }
    if (s.initial || s.headOid.empty()) {
        p.refusal = "There is no commit to undo yet.";
        return p;
    }
    if (s.detached) {
        p.refusal = "HEAD is detached: switch to a branch to undo its last commit.";
        return p;
    }
    if (parents < 0) {
        p.refusal = "The last commit could not be read, so nothing was undone.";
        return p;
    }
    if (parents == 0) {
        // The repository's first commit: HEAD~1 does not exist. Deleting
        // the branch's ref leaves the index, so its files stay staged.
        p.args = {"update-ref", "-d", "HEAD"};
    } else {
        p.args = {"reset", "--soft", "HEAD~1"};
    }
    const std::string onUpstream = headIsPushed(s)
        ? " It is already on " + s.upstream + ", so the next push needs a force." : "";
    if (parents >= 2) {
        p.question = "Undo the merge commit?";
        p.detail = "The last commit is a merge. Undoing it moves " +
                   (s.branch.empty() ? std::string("the branch") : s.branch) +
                   " back to its first parent and leaves the merge's changes staged; the "
                   "merged branch keeps its own commits." + onUpstream;
        p.button = "Undo Merge Commit";
    } else if (!onUpstream.empty()) {
        p.question = "Undo a commit that is already pushed?";
        p.detail = "Its changes stay staged and its message goes back in the box." + onUpstream;
        p.button = "Undo Last Commit";
    }
    p.doneText = parents == 0
        ? "Undid the first commit; its files are staged, as before it."
        : "Undid the last commit; its changes are staged and its message is in the box.";
    return p;
}

std::vector<std::string> headParentsArgs() {
    return {"rev-list", "--parents", "-n", "1", "HEAD", "--"};
}

int parentCount(const std::string& out) {
    const std::string line = trim(out);
    if (line.empty() || line.find('\n') != std::string::npos) return -1;
    int words = 0;
    bool in = false;
    for (char c : line) {
        if (c == ' ') { in = false; continue; }
        if (!std::isxdigit((unsigned char)c)) return -1;
        if (!in) { words++; in = true; }
    }
    return words - 1;
}

std::vector<std::string> headMessageArgs() {
    return {"log", "-1", "--no-color", "--format=%B", "HEAD", "--"};
}

ActionPlan stashPlan(const Snapshot& s, const std::string& message) {
    ActionPlan p;
    if (!s.inRepository) { p.refusal = kNoRepo; return p; }
    if (s.initial || s.headOid.empty()) {
        p.refusal = "git cannot stash before the first commit: commit first.";
        return p;
    }
    if (!anyChange(s)) { p.refusal = "There are no changes to stash."; return p; }
    p.args = {"stash", "push", "--include-untracked"};
    const std::string m = trim(message);
    if (!m.empty()) { p.args.push_back("-m"); p.args.push_back(m); }
    p.doneText = "Stashed the changes" + (m.empty() ? std::string() : " as “" + m + "”") + ".";
    return p;
}

ActionPlan popStashPlan(const Snapshot& s, int stashes) {
    ActionPlan p;
    if (!s.inRepository) { p.refusal = kNoRepo; return p; }
    if (stashes <= 0) { p.refusal = "There is no stash to pop."; return p; }
    p.args = {"stash", "pop"};
    p.doneText = stashes == 1 ? "Popped the stash." : "Popped the newest stash; " +
                 plural((size_t)stashes - 1, "stash is", "stashes are") + " left.";
    return p;
}

std::vector<std::string> stashListArgs() { return {"stash", "list", "--format=%gd"}; }

size_t countLines(const std::string& out) { return nonBlankLines(out).size(); }

// --------------------------------------------- the editor's change bars
std::vector<std::string> indexEntryArgs(const std::string& rel) {
    return {"--literal-pathspecs", "ls-files", "--stage", "-z", "--", rel};
}

IndexEntry parseIndexEntry(const std::string& out) {
    // "<mode> <object> <stage>\t<path>\0", one record a stage.
    IndexEntry e;
    const size_t end = out.find('\0');
    if (end == std::string::npos || end + 1 != out.size()) return e;   // none, or a conflict
    const std::string rec = out.substr(0, end);
    const size_t s1 = rec.find(' '), s2 = rec.find(' ', s1 + 1), tab = rec.find('\t');
    if (s1 == std::string::npos || s2 == std::string::npos || tab == std::string::npos ||
        !(s1 < s2 && s2 < tab))
        return e;
    const std::string mode = rec.substr(0, s1), object = rec.substr(s1 + 1, s2 - s1 - 1),
                      stage = rec.substr(s2 + 1, tab - s2 - 1);
    if (stage != "0" || (mode != "100644" && mode != "100755")) return e;
    if (object.size() < 40) return e;
    for (char c : object)
        if (!std::isxdigit((unsigned char)c)) return e;
    e.ok = true;
    e.mode = mode;
    e.object = object;
    return e;
}

std::vector<std::string> blobArgs(const std::string& object) {
    return {"cat-file", "blob", object};
}

std::vector<std::string> hashObjectArgs(const std::string& rel) {
    return {"hash-object", "-w", "--stdin", "--path", rel};
}

std::vector<std::string> updateIndexArgs(const IndexEntry& e, const std::string& object,
                                         const std::string& rel) {
    return {"update-index", "--cacheinfo", e.mode + "," + trim(object) + "," + rel};
}

// ------------------------------------------------- hunks and lines of a diff
DiffSide diffSideOf(const Row& r) {
    if (r.untracked) return DiffSide::Untracked;
    return r.staged ? DiffSide::Staged : DiffSide::Changes;
}

std::vector<HunkOp> hunkOps(DiffSide side) {
    switch (side) {
    case DiffSide::Changes: return {HunkOp::Stage, HunkOp::Discard};
    case DiffSide::Staged: return {HunkOp::Unstage};
    case DiffSide::Untracked: return {};
    }
    return {};
}

std::string hunkOpTitle(HunkOp op, bool lines) {
    const char* verb = op == HunkOp::Stage ? "Stage" : op == HunkOp::Unstage ? "Unstage" : "Discard";
    return std::string(verb) + (lines ? " Selected Lines" : " Hunk");
}

std::vector<std::string> applyArgs(HunkOp op) {
    std::vector<std::string> a = {"apply"};
    if (op != HunkOp::Discard) a.push_back("--cached");
    if (op != HunkOp::Stage) a.push_back("--reverse");
    a.push_back("--whitespace=nowarn");
    a.push_back("-");
    return a;
}

bool DiffHunk::changes(size_t line) const {
    if (line <= firstLine || line >= firstLine + lineCount || line - firstLine >= kinds.size())
        return false;
    const char c = kinds[line - firstLine];
    return c == '+' || c == '-';
}

namespace {
bool startsWith(const std::string& s, const char* p) {
    return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}

// A hunk's lines by kind, one character each: '@' the header, ' ' context,
// '+', '-', '\\' a "\ No newline" line.
char kindChar(Git::LineKind k) {
    switch (k) {
    case Git::LineKind::HunkHeader: return '@';
    case Git::LineKind::Context: return ' ';
    case Git::LineKind::Added: return '+';
    case Git::LineKind::Removed: return '-';
    case Git::LineKind::NoNewline: return '\\';
    default: return 0;
    }
}

struct HunkWalk {
    std::vector<Git::DiffLine> lines;
    std::vector<DiffHunk> hunks;
};

HunkWalk walkHunks(const std::string& diff) {
    HunkWalk w;
    w.lines = Git::classifyDiff(diff);
    size_t file = 0, inFile = 0;
    bool anyFile = false;
    for (size_t i = 0; i < w.lines.size(); ++i) {
        const Git::DiffLine& l = w.lines[i];
        if (l.kind == Git::LineKind::FileHeader && startsWith(l.text, "diff ")) {
            if (anyFile) ++file;
            anyFile = true;
            inFile = 0;
            continue;
        }
        const char k = kindChar(l.kind);
        if (k == '@' && anyFile) {
            DiffHunk h;
            h.file = file;
            h.hunk = inFile++;
            h.firstLine = i;
            h.lineCount = 1;
            h.kinds = "@";
            w.hunks.push_back(h);
            continue;
        }
        if (k && k != '@' && !w.hunks.empty() && w.hunks.back().file == file &&
            i == w.hunks.back().firstLine + w.hunks.back().lineCount) {
            w.hunks.back().lineCount++;
            w.hunks.back().kinds += k;
        }
    }
    return w;
}

// A path in a patch header, quoted as git quotes it when it must be.
std::string patchPath(const char* prefix, const std::string& p) {
    bool quote = false;
    for (unsigned char c : p)
        if (c == '"' || c == '\\' || c < 0x20 || c == 0x7f) quote = true;
    if (!quote) return prefix + p;
    std::string out = std::string("\"") + prefix;
    for (unsigned char c : p) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\%03o", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out + "\"";
}

// Whether a file's hunks can be staged apart: a two-sided git diff of a
// text file that exists on both sides.
std::string hunkRefusal(const Git::FileDiff& f) {
    const std::string name = fileName(f.newPath.empty() ? f.oldPath : f.newPath);
    if (f.header.empty() || !startsWith(f.header[0].text, "diff --git "))
        return name + " has a conflict: resolve it in the file, then stage it whole.";
    if (f.binary) return name + " is a binary file: stage or discard it whole.";
    // A submodule's diff is one "Subproject commit" line a side: there is
    // no text to take apart, and reversing it onto the work tree would do
    // nothing to the checkout. The header's mode says so (160000 on the
    // index line, or on a new file, deleted file, old or new mode line).
    // A plain file may hold lines reading "Subproject commit <object
    // name>" (pasted git output, a fixture) and its hunks stage as any
    // other file's: its diff always has an index line naming two objects
    // and a 100644 or 100755 mode. Only a gitlink at the same commit with
    // changes inside (the "-dirty" form) has no index line at all, since
    // git writes none when the two sides name one object; then it is
    // known by its hunk, one such line a side.
    bool gitlink = false, indexLine = false;
    for (const Git::DiffLine& l : f.header) {
        const std::string& t = l.text;
        if (startsWith(t, "index ")) indexLine = true;
        const bool modeLine = startsWith(t, "index ") || startsWith(t, "new file mode ") ||
                              startsWith(t, "deleted file mode ") || startsWith(t, "old mode ") ||
                              startsWith(t, "new mode ");
        if (modeLine && t.size() >= 7 && t.compare(t.size() - 7, 7, " 160000") == 0)
            gitlink = true;
    }
    if (!gitlink && !indexLine && f.hunks.size() == 1) {
        auto isGitlinkLine = [](const std::string& t) {
            // "+Subproject commit <object name>", "-dirty" after it.
            if (t.size() < 19 + 40 || t.compare(1, 18, "Subproject commit ") != 0) return false;
            size_t i = 19;
            while (i < t.size() && std::isxdigit((unsigned char)t[i])) i++;
            return i - 19 >= 40 &&
                   (i == t.size() || t.compare(i, std::string::npos, "-dirty") == 0);
        };
        size_t removed = 0, added = 0, other = 0;
        for (const Git::DiffLine& l : f.hunks[0].lines) {
            if (l.kind == Git::LineKind::Removed && isGitlinkLine(l.text)) removed++;
            else if (l.kind == Git::LineKind::Added && isGitlinkLine(l.text)) added++;
            else if (l.kind == Git::LineKind::Removed || l.kind == Git::LineKind::Added ||
                     l.kind == Git::LineKind::Context)
                other++;
        }
        gitlink = removed == 1 && added == 1 && other == 0;
    }
    if (gitlink)
        return name + " is a submodule: stage or unstage it whole, and discard inside it "
                      "(open it as a folder).";
    if (f.isNew) return name + " is a new file: stage or unstage it whole.";
    if (f.isDeleted) return name + " was deleted: stage or unstage the deletion whole.";
    return "";
}

struct Built {
    std::string patch, path, refusal;
};

Built build(const std::string& diff, HunkOp op, size_t first, size_t last) {
    Built b;
    const HunkWalk w = walkHunks(diff);
    const std::vector<Git::FileDiff> files = Git::parseDiff(diff);
    const bool reverse = op != HunkOp::Stage;
    // The hunks with a changed line in first..last, of the first file that has one.
    std::vector<size_t> touched;
    for (size_t k = 0; k < w.hunks.size(); ++k) {
        const DiffHunk& h = w.hunks[k];
        if (!touched.empty() && h.file != w.hunks[touched[0]].file) break;
        for (size_t j = 1; j < h.lineCount; ++j) {
            const size_t li = h.firstLine + j;
            if (li >= first && li <= last && h.changes(li)) {
                touched.push_back(k);
                break;
            }
        }
    }
    if (touched.empty()) {
        b.refusal = "Choose lines that changed (the + and - lines) first.";
        return b;
    }
    const size_t fi = w.hunks[touched[0]].file;
    if (fi >= files.size()) { b.refusal = "This diff could not be read."; return b; }
    const Git::FileDiff& f = files[fi];
    b.refusal = hunkRefusal(f);
    if (!b.refusal.empty()) return b;
    // Reversed, the patch is applied to what the + side holds (the index for
    // Unstage, the work tree for Discard), so it names the new path: a
    // renamed file's hunk changes the file under its new name.
    b.path = reverse ? f.newPath : f.oldPath;
    std::string out = "diff --git " + patchPath("a/", b.path) + " " + patchPath("b/", b.path) +
                      "\n--- " + patchPath("a/", b.path) + "\n+++ " + patchPath("b/", b.path) +
                      "\n";
    long delta = 0;
    bool any = false;
    for (size_t k : touched) {
        const DiffHunk& h = w.hunks[k];
        if (h.hunk >= f.hunks.size()) continue;
        const Git::Hunk& gh = f.hunks[h.hunk];
        struct Line { char kind; std::string text; std::string marker; };
        std::vector<Line> body;
        bool dropped = false;   // the line before a "\" line was left out
        for (size_t j = 1; j < h.lineCount; ++j) {
            const size_t li = h.firstLine + j;
            const Git::DiffLine& d = w.lines[li];
            const char c = h.kinds[j];
            if (c == '\\') {
                if (!dropped && !body.empty()) body.back().marker = d.text;
                continue;
            }
            const bool chosen = li >= first && li <= last;
            const std::string text = d.text.empty() ? "" : d.text.substr(1);
            char kind = c;
            // Forward, the - side is what the index holds: a - line not
            // chosen stays as context and a + line not chosen is left out.
            // Reversed, the + side is what is there, the other way round.
            if (c == '-' && !chosen) kind = reverse ? 0 : ' ';
            if (c == '+' && !chosen) kind = reverse ? ' ' : 0;
            dropped = kind == 0;
            if (!dropped) body.push_back({kind, text, ""});
        }
        int oldN = 0, newN = 0;
        bool changed = false, oldEnd = false, newEnd = false, bad = false;
        for (const Line& l : body) {
            if (l.kind != '+') { if (oldEnd) bad = true; ++oldN; }
            if (l.kind != '-') { if (newEnd) bad = true; ++newN; }
            if (l.kind != ' ') changed = true;
            if (!l.marker.empty()) {
                if (l.kind != '+') oldEnd = true;
                if (l.kind != '-') newEnd = true;
            }
        }
        if (!changed) continue;
        if (bad) {
            b.refusal = "These lines are at the end of " + fileName(b.path) +
                        ", which has no newline there, and cannot change apart from "
                        "the rest: choose the whole hunk.";
            return b;
        }
        // The side the patch is applied to keeps its numbers; the other
        // side's start moves by what the hunks before it in this patch add.
        int oldStart, newStart;
        if (!reverse) {
            const long before = gh.oldCount == 0 ? gh.oldStart : gh.oldStart - 1;
            oldStart = gh.oldStart;
            newStart = (int)(before + delta + (newN ? 1 : 0));
            delta += newN - oldN;
        } else {
            const long before = gh.newCount == 0 ? gh.newStart : gh.newStart - 1;
            newStart = gh.newStart;
            oldStart = (int)(before + delta + (oldN ? 1 : 0));
            delta += oldN - newN;
        }
        out += "@@ -" + std::to_string(oldStart) + "," + std::to_string(oldN) + " +" +
               std::to_string(newStart) + "," + std::to_string(newN) + " @@\n";
        for (const Line& l : body) {
            out += l.kind;
            out += l.text + "\n";
            if (!l.marker.empty()) out += l.marker + "\n";
        }
        any = true;
    }
    if (!any) {
        b.refusal = "Choose lines that changed (the + and - lines) first.";
        return b;
    }
    b.patch = out;
    return b;
}
}  // namespace

std::vector<DiffHunk> diffHunks(const std::string& diff) { return walkHunks(diff).hunks; }

int hunkAt(const std::vector<DiffHunk>& hunks, size_t line) {
    for (size_t k = 0; k < hunks.size(); ++k)
        if (line >= hunks[k].firstLine && line < hunks[k].firstLine + hunks[k].lineCount)
            return (int)k;
    return -1;
}

std::string buildPatch(const std::string& diff, HunkOp op, size_t first, size_t last,
                       std::string* refusal) {
    Built b = build(diff, op, first, last);
    if (refusal) *refusal = b.refusal;
    return b.patch;
}

HunkPlan hunkPlan(const std::string& diff, DiffSide side, HunkOp op, size_t first, size_t last,
                  bool lines) {
    HunkPlan p;
    const std::vector<HunkOp> ops = hunkOps(side);
    if (std::find(ops.begin(), ops.end(), op) == ops.end()) {
        p.refusal = side == DiffSide::Untracked
            ? "This file is not tracked yet: stage it whole."
            : side == DiffSide::Staged ? "These changes are staged: unstage them first."
                                       : "These changes are not staged.";
        return p;
    }
    if (!lines) {
        const std::vector<DiffHunk> hs = diffHunks(diff);
        const int at = hunkAt(hs, first);
        if (at < 0) { p.refusal = "Put the caret in a hunk first."; return p; }
        first = hs[(size_t)at].firstLine;
        last = first + hs[(size_t)at].lineCount - 1;
    }
    if (last < first) std::swap(first, last);
    Built b = build(diff, op, first, last);
    if (b.patch.empty()) { p.refusal = b.refusal; return p; }
    p.patch = b.patch;
    p.path = b.path;
    p.args = applyArgs(op);
    const std::string name = fileName(b.path);
    const std::string what = lines ? "the selected lines of " + name : "a hunk of " + name;
    switch (op) {
    case HunkOp::Stage: p.doneText = "Staged " + what + "."; break;
    case HunkOp::Unstage: p.doneText = "Unstaged " + what + "."; break;
    case HunkOp::Discard:
        p.question = lines ? "Discard the selected lines of " + name + "?"
                           : "Discard this hunk of " + name + "?";
        p.detail = std::string(lines ? "They go" : "Its lines go") +
                   " back to the version git has (staged, or else in the last commit). "
                   "The file as it is now goes to the Trash first: Undo Discard in this "
                   "panel's menu brings it back.";
        p.button = lines ? "Discard Lines" : "Discard Hunk";
        p.doneText = "Discarded " + what + "; the file as it was is in the Trash.";
        break;
    }
    return p;
}

std::string hunkFailureText(int status, const std::string& out, const std::string& err,
                            const std::string& path) {
    if (err.find("patch does not apply") != std::string::npos ||
        err.find("patch failed") != std::string::npos)
        return fileName(path) + " changed since this diff was shown, so nothing was done. "
               "The diff now shows it as it is.";
    return failureText(status, out, err);
}

std::vector<HunkMenuItem> hunkMenu(const std::string& diff, DiffSide side, size_t clicked,
                                   size_t first, size_t last, bool selection) {
    std::vector<HunkMenuItem> out;
    const std::vector<HunkOp> ops = hunkOps(side);
    if (ops.empty()) return out;
    if (last < first) std::swap(first, last);
    const std::vector<DiffHunk> hunks = diffHunks(diff);
    bool linesChosen = false;
    for (const DiffHunk& h : hunks)
        for (size_t l = std::max(first, h.firstLine);
             selection && !linesChosen && l <= last && l < h.firstLine + h.lineCount; l++)
            if (h.changes(l)) linesChosen = true;
    const bool inHunk = hunkAt(hunks, clicked) >= 0;
    for (HunkOp op : ops)
        for (int lines = 0; lines < 2; lines++) {
            if (lines ? !linesChosen : !inHunk) continue;
            HunkMenuItem it;
            it.op = op;
            it.lines = lines != 0;
            it.first = lines ? first : clicked;
            it.last = lines ? last : clicked;
            it.title = hunkOpTitle(op, it.lines) + (op == HunkOp::Discard ? "…" : "");
            out.push_back(it);
        }
    return out;
}

// ------------------------------------------------------ the branch picker
std::vector<std::string> branchListArgs() {
    return {"for-each-ref", "--sort=-committerdate", std::string("--format=") + Git::refFormat(),
            "refs/heads", "refs/remotes"};
}

std::string localNameFor(const std::string& remoteBranch, const Snapshot& s) {
    size_t best = 0;
    for (const std::string& r : s.remotes)
        if (remoteBranch.size() > r.size() + 1 && remoteBranch.compare(0, r.size(), r) == 0 &&
            remoteBranch[r.size()] == '/' && r.size() + 1 > best)
            best = r.size() + 1;
    if (!best) {
        size_t slash = remoteBranch.find('/');
        best = slash == std::string::npos ? 0 : slash + 1;
    }
    return remoteBranch.substr(best);
}

std::vector<BranchItem> branchItems(const std::string& refsOut, const Snapshot& s) {
    std::vector<BranchItem> locals, remotes;
    std::vector<Git::Ref> refs = Git::parseRefs(refsOut);
    std::vector<std::string> localNames;
    for (const Git::Ref& r : refs)
        if (r.kind == Git::RefKind::Branch) localNames.push_back(r.name);
    for (const Git::Ref& r : refs) {
        // A name git would read as an option never reaches its command line.
        if (r.name.empty() || r.name[0] == '-') continue;
        BranchItem b;
        b.name = r.name;
        const std::string hash = Git::shortHash(r.target);
        if (r.kind == Git::RefKind::Branch) {
            b.current = !s.detached && r.name == s.branch;
            b.detail = b.current ? "current · " + hash : hash;
            if (b.current) locals.insert(locals.begin(), b);
            else locals.push_back(b);
        } else if (r.kind == Git::RefKind::Remote) {
            const std::string local = localNameFor(r.name, s);
            if (local.empty() || local == "HEAD" ||
                std::find(localNames.begin(), localNames.end(), local) != localNames.end())
                continue;
            b.remote = true;
            b.detail = "remote · " + hash;
            remotes.push_back(b);
        }
    }
    locals.insert(locals.end(), remotes.begin(), remotes.end());
    return locals;
}

std::vector<std::string> switchArgs(const BranchItem& b, const Snapshot& s) {
    if (b.current) return {};
    if (b.remote) return {"switch", "-c", localNameFor(b.name, s), "--track", b.name};
    return {"switch", b.name};
}

std::string newBranchName(const std::string& typed) {
    std::string t = trim(typed);
    for (char& c : t)
        if (c == ' ' || c == '\t') c = '-';
    return t;
}

std::string branchNameProblem(const std::string& name) {
    if (name.empty()) return "Type a name for the new branch.";
    const std::string q = "“" + name + "”";
    if (name[0] == '-') return q + " cannot start with a dash.";
    if (name == "@" || name == "HEAD") return q + " is a name git keeps for itself.";
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7F || c == ' ' || c == '~' || c == '^' || c == ':' || c == '?' ||
            c == '*' || c == '[' || c == '\\')
            return q + " has a character git does not allow in a branch name "
                       "(spaces, ~ ^ : ? * [ or \\).";
    if (name.find("..") != std::string::npos || name.find("@{") != std::string::npos ||
        name.find("//") != std::string::npos)
        return q + " cannot hold \"..\", \"@{\" or \"//\".";
    if (name[0] == '/' || name.back() == '/' || name.back() == '.')
        return q + " cannot start or end with a slash, or end with a dot.";
    size_t start = 0;
    while (start <= name.size()) {
        size_t slash = name.find('/', start);
        if (slash == std::string::npos) slash = name.size();
        const std::string part = name.substr(start, slash - start);
        if (!part.empty() && part[0] == '.') return q + ": no part of it may start with a dot.";
        if (part.size() >= 5 && part.compare(part.size() - 5, 5, ".lock") == 0)
            return q + ": no part of it may end with .lock.";
        start = slash + 1;
    }
    return "";
}

std::string createBranchLabel(const std::string& typed, const std::vector<BranchItem>& items,
                              const Snapshot& s) {
    const std::string name = newBranchName(typed);
    if (name.empty()) return "";
    for (const BranchItem& b : items)
        if (!b.remote && b.name == name) return "";
    std::string from = s.detached ? std::string("the detached HEAD")
                                  : s.branch.empty() ? std::string("HEAD") : s.branch;
    return "Create branch “" + name + "” from " + from;
}

std::vector<std::string> createBranchArgs(const std::string& name) {
    return {"switch", "-c", name};
}

std::string switchedText(const std::string& branch, bool created) {
    return created ? "Created " + branch + " and switched to it." : "Switched to " + branch + ".";
}

// ------------------------------------------------ pushing and pulling
std::vector<std::string> remoteArgs() { return {"remote"}; }

std::vector<std::string> parseRemotes(const std::string& out) {
    std::vector<std::string> r;
    for (const std::string& l : nonBlankLines(out))
        if (l[0] != '-' && l.find_first_of(" \t") == std::string::npos) r.push_back(l);
    return r;
}

std::string publishRemote(const Snapshot& s) {
    for (const std::string& r : s.remotes)
        if (r == "origin") return r;
    return s.remotes.size() == 1 ? s.remotes[0] : "";
}

std::string upstreamRemote(const Snapshot& s) {
    std::string best;
    if (s.detached) return best;
    for (const std::string& r : s.remotes)
        if (s.upstream.size() > r.size() + 1 && s.upstream.compare(0, r.size(), r) == 0 &&
            s.upstream[r.size()] == '/' && r.size() > best.size())
            best = r;
    return best;
}

namespace {

// Why a branch cannot be published, or "".
std::string publishRefusal(const Snapshot& s) {
    if (s.remotes.empty()) return "This repository has no remote to push to.";
    if (publishRemote(s).empty())
        return "This repository has several remotes and none is called origin: push "
               "it once from the terminal (git push -u <remote> " + s.branch +
               ") to choose.";
    return "";
}

}  // namespace

SyncPlan syncPlan(SyncOp op, const Snapshot& s) {
    SyncPlan p;
    if (!s.inRepository) { p.refusal = "This folder is not in a git repository."; return p; }
    const bool hasUpstream = !s.upstream.empty();
    if (op == SyncOp::Fetch) {
        if (s.remotes.empty()) { p.refusal = "This repository has no remote to fetch from."; return p; }
        p.fetches = true;
        // The upstream's remote, as a plain fetch would; else origin, else
        // the first.
        std::string remote = upstreamRemote(s);
        if (remote.empty()) remote = publishRemote(s);
        if (remote.empty()) remote = s.remotes[0];
        p.steps.push_back({"fetch", remote});
        p.busyText = "Fetching from " + remote + "…";
        p.doneText = "Fetched from " + remote + ".";
        return p;
    }
    if (s.initial) { p.refusal = "There is no commit yet to push or pull."; return p; }
    if (s.detached) {
        p.refusal = "HEAD is detached: check out a branch to push or pull it.";
        return p;
    }
    if (!hasUpstream) {
        if (op == SyncOp::Pull) {
            p.refusal = s.branch + " has no upstream branch to pull from.";
            return p;
        }
        p.refusal = publishRefusal(s);
        if (!p.refusal.empty()) return p;
        const std::string remote = publishRemote(s);
        p.steps.push_back({"push", "--set-upstream", remote, s.branch});
        p.busyText = "Publishing " + s.branch + " to " + remote + "…";
        p.doneText = "Published " + s.branch + " to " + remote + ".";
        return p;
    }
    if (!s.hasAheadBehind) {
        p.refusal = "The upstream " + s.upstream + " is gone: push it again from the "
                    "terminal (git push -u <remote> " + s.branch + ").";
        return p;
    }
    switch (op) {
    case SyncOp::Pull:
        p.steps.push_back({"pull"});
        p.fetches = true;
        p.busyText = "Pulling from " + s.upstream + "…";
        p.doneText = "Pulled from " + s.upstream + ".";
        break;
    case SyncOp::Push:
        p.steps.push_back({"push"});
        p.busyText = "Pushing to " + s.upstream + "…";
        p.doneText = "Pushed to " + s.upstream + ".";
        break;
    default:   // Sync: what is new there first, then what is new here.
        p.steps.push_back({"pull"});
        p.steps.push_back({"push"});
        p.fetches = true;
        p.busyText = "Syncing with " + s.upstream + "…";
        p.doneText = "Synced with " + s.upstream + ".";
        break;
    }
    return p;
}

std::string syncButtonTitle(const Snapshot& s) {
    if (!s.inRepository || s.initial || s.detached || !s.errorText.empty()) return "";
    if (s.upstream.empty()) return publishRefusal(s).empty() ? "Publish Branch" : "";
    if (!s.hasAheadBehind || (!s.ahead && !s.behind)) return "";
    std::string t = "Sync Changes";
    if (s.behind) t += "  ↓ " + std::to_string(s.behind);
    if (s.ahead) t += "  ↑ " + std::to_string(s.ahead);
    return t;
}

std::string syncButtonToolTip(const Snapshot& s) {
    if (s.upstream.empty())
        return "Push " + s.branch + " to " + publishRemote(s) + " and track it there";
    std::string t;
    if (s.behind) t = "Pull " + std::to_string(s.behind) + (s.behind == 1 ? " commit" : " commits");
    if (s.ahead) {
        t += t.empty() ? "Push " : ", then push ";
        t += std::to_string(s.ahead) + (s.ahead == 1 ? " commit" : " commits");
    }
    return t + (s.behind ? " from and to " : " to ") + s.upstream;
}

std::string networkFailureText(int status, const std::string& out, const std::string& err,
                               bool forLinux) {
    std::string t = failureText(status, out, err);
    auto has = [&](const char* needle) { return t.find(needle) != std::string::npos; };
    if (has("terminal prompts disabled") || has("could not read Username") ||
        has("could not read Password") || has("Authentication failed") ||
        has("Invalid username or password"))
        return t + "\nminicode never asks for a password: set up a credential helper "
                   "(gh auth setup-git, or " +
               (forLinux ? "git-credential-libsecret" : "the macOS keychain") +
               "), or push once from the terminal.";
    if (has("Permission denied (publickey") || has("Host key verification failed") ||
        has("passphrase"))
        return t + "\nminicode cannot type an ssh passphrase or accept a new host key: "
                   "add the key to the agent (" +
               (forLinux ? "ssh-add" : "ssh-add --apple-use-keychain") +
               ") or connect once from the terminal.";
    return t;
}

// ------------------------------------------------------------ autofetch
double autoFetchWait(int period, int failures) {
    double wait = std::max(period, kAutoFetchMinPeriod);
    for (int i = 0; i < failures && wait < kAutoFetchMaxBackoff; i++) wait *= 2;
    return failures ? std::min(wait, (double)std::max(kAutoFetchMaxBackoff, period)) : wait;
}

bool autoFetchDue(bool enabled, bool windowActive, const Snapshot& s, bool busy,
                  const AutoFetchState& st, double now, int period) {
    if (!enabled || !windowActive || busy || !s.inRepository || s.remotes.empty()) return false;
    return st.lastStart < 0 || now - st.lastStart >= autoFetchWait(period, st.failures);
}

const std::vector<EnvVar>& gitEnvironment() {
    static const std::vector<EnvVar> env = {
        {"GIT_OPTIONAL_LOCKS", "0"},   // status must not write the index
        {"GIT_TERMINAL_PROMPT", "0"},  // never wait for a password
        {"GIT_EDITOR", "true"},        // never wait for an editor
        {"GIT_PAGER", "cat"},
    };
    return env;
}

// ---------------------------------------------------------------- graph
bool compareWithUpstream(const Snapshot& s) {
    return !s.upstream.empty() && s.hasAheadBehind && !s.detached;
}

std::string graphKey(const Snapshot& s, int limit, bool all, bool compare,
                     const std::string& refsOutput) {
    return s.headOid + " " + s.branch + " " + (compare ? "1" : "0") + " " +
           std::to_string(limit) + " " + (all ? "1" : "0") + " " + s.upstream + "\n" +
           refsOutput;
}

void buildGraph(Graph& g, const Snapshot& s, const std::string& logOut,
                const std::string& refsOut, int limit) {
    g.limit = limit;
    g.commits = Git::parseLog(logOut);
    g.hasMore = (int)g.commits.size() >= limit;
    g.rows = Git::layoutGraph(g.commits);
    g.maxWidth = 1;
    for (const Git::GraphRow& r : g.rows) g.maxWidth = std::max(g.maxWidth, r.width);
    g.head = s.headOid;
    g.labels = Git::labelsByCommit(Git::parseRefs(refsOut), g.head, s.branch, s.detached);
}

double laneWidth(double rowWidth, int lanes) {
    if (lanes <= 0) return 12;
    return std::max(4.0, std::min(12.0, (rowWidth * 0.4 - 8) / lanes));
}

unsigned laneColor(int i) {
    static const unsigned palette[] = {0x59A4F9, 0xFFB000, 0xDC267F, 0x40B0A6,
                                       0xB66DFF, 0xE0823D, 0x8FCB5A};
    const int n = sizeof(palette) / sizeof(palette[0]);
    return palette[((i % n) + n) % n];
}

unsigned pillColor(Git::RefKind k) {
    switch (k) {
    case Git::RefKind::Head: return 0xEA5C00;
    case Git::RefKind::Remote: return 0xB180D7;
    case Git::RefKind::Tag: return 0xE2C08D;
    case Git::RefKind::Branch: default: return 0x59A4F9;
    }
}

std::string dateText(long long t) {
    time_t tt = (time_t)t;
    struct tm tmv;
    if (!localtime_r(&tt, &tmv)) return "";
    char buf[64];
    if (!strftime(buf, sizeof buf, "%b %e, %Y at %H:%M", &tmv)) return "";
    std::string s = buf;
    size_t dbl = s.find("  ");   // %e pads a one-digit day with a space
    if (dbl != std::string::npos) s.erase(dbl, 1);
    return s;
}

std::string graphToolTip(const Graph& g, size_t row, long long now, DateFormat date) {
    if (row >= g.commits.size())
        return "Load the next " + std::to_string(kGraphBatch) + " commits";
    const Git::Commit& c = g.commits[row];
    std::string t = Git::shortHash(c.hash) + "  " + validUtf8(c.author) + ", " +
                    Git::relativeTime(c.time, now) + " (" + date(c.time) + ")\n" +
                    validUtf8(c.subject);
    if (g.divergence.outgoing.count(c.hash))
        t += "\nNot pushed yet: on this branch but not its upstream.";
    if (g.divergence.incoming.count(c.hash))
        t += "\nNot pulled yet: on the upstream but not this branch.";
    auto it = g.labels.find(c.hash);
    if (it != g.labels.end()) {
        std::string names;
        for (const Git::Ref& r : it->second) names += (names.empty() ? "" : ", ") + r.name;
        t += "\n" + validUtf8(names);
    }
    return t;
}

std::vector<std::string> graphDescriptions(const Graph& g) {
    std::vector<std::string> out;
    for (size_t i = 0; i < g.commits.size(); ++i) {
        const Git::Commit& c = g.commits[i];
        std::string line = Git::shortHash(c.hash) + " lane=" + std::to_string(g.rows[i].lane);
        if (c.hash == g.head) line += " head";
        if (g.divergence.outgoing.count(c.hash)) line += " out";
        if (g.divergence.incoming.count(c.hash)) line += " in";
        line += " " + c.subject;
        auto it = g.labels.find(c.hash);
        if (it != g.labels.end()) {
            std::string names;
            for (const Git::Ref& r : it->second)
                names += (names.empty() ? "" : ",") + std::string(r.current ? "*" : "") + r.name;
            line += " [" + names + "]";
        }
        out.push_back(line);
    }
    if (g.hasMore) out.push_back("(more)");
    return out;
}

// -------------------------------------------------------- styled text
namespace {
// Length of the valid UTF-8 sequence at s[i], or 0.
size_t seqLen(const std::string& s, size_t i) {
    const unsigned char c = (unsigned char)s[i];
    size_t n;
    unsigned min;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; min = 0x10000; }
    else return 0;
    if (i + n > s.size()) return 0;
    unsigned cp = c & (0xFF >> (n + 1));
    for (size_t k = 1; k < n; ++k) {
        const unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (d & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}
}  // namespace

std::string validUtf8(const std::string& s) {
    bool ok = true;
    for (size_t i = 0; i < s.size();) {
        size_t n = seqLen(s, i);
        if (!n || s[i] == '\0') { ok = false; break; }
        i += n;
    }
    if (ok) return s;
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (unsigned char c : s) {
        if (c == 0) continue;   // GtkTextBuffer cannot hold a NUL
        if (c < 0x80) {
            out += (char)c;
        } else {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

void StyledText::add(const std::string& s, Style style) {
    if (s.empty()) return;
    const size_t len = utf8Length(s);
    if (!runs.empty() && runs.back().style == style &&
        runs.back().start + runs.back().length == chars)
        runs.back().length += len;
    else
        runs.push_back({chars, len, style});
    text += s;
    chars += len;
}

namespace {
// The diff's lines, styled; the hunk headers named in `actionsAt` (by line)
// get those actions' words after them, recorded in `actions`.
void appendDiffImpl(StyledText& out, const std::string& input,
                    const std::unordered_map<size_t, std::vector<HunkOp>>* actionsAt,
                    std::vector<DiffAction>* actions) {
    if (input.empty()) {
        out.add("No differences to show.", Style::Muted);
        return;
    }
    std::string bytes = input;
    bool cut = false;
    if (bytes.size() > kMaxDiffBytes) {
        size_t nl = bytes.rfind('\n', kMaxDiffBytes);
        bytes.resize(nl == std::string::npos ? kMaxDiffBytes : nl + 1);
        cut = true;
    }
    const std::vector<Git::DiffLine> lines = Git::classifyDiff(bytes);
    for (size_t i = 0; i < lines.size(); ++i) {
        const Git::DiffLine& l = lines[i];
        Style st = Style::Plain;
        switch (l.kind) {
        case Git::LineKind::Added: st = Style::Added; break;
        case Git::LineKind::Removed: st = Style::Removed; break;
        case Git::LineKind::FileHeader: st = Style::FileHeader; break;
        case Git::LineKind::HunkHeader:
        case Git::LineKind::NoNewline:
        case Git::LineKind::Other: st = Style::Muted; break;
        case Git::LineKind::Context: st = Style::Plain; break;
        }
        const std::vector<HunkOp>* ops = nullptr;
        if (actionsAt) {
            auto at = actionsAt->find(i);
            if (at != actionsAt->end()) ops = &at->second;
        }
        if (!ops) {
            out.add(validUtf8(l.text) + "\n", st);
            continue;
        }
        out.add(validUtf8(l.text), st);
        out.add("    ", Style::Plain);
        for (size_t k = 0; k < ops->size(); ++k) {
            if (k) out.add(" \xC2\xB7 ", Style::Muted);
            const std::string label = hunkOpTitle((*ops)[k], false);
            actions->push_back({out.chars, utf8Length(label), (*ops)[k], i});
            out.add(label, Style::Plain);
        }
        out.add("\n", Style::Plain);
    }
    if (cut) out.add("\n(The diff is longer than 4 MB; the rest is not shown.)\n", Style::Muted);
}
}  // namespace

void appendDiff(StyledText& out, const std::string& input) {
    appendDiffImpl(out, input, nullptr, nullptr);
}

StyledText diffTextWithActions(const std::string& bytes, DiffSide side,
                               std::vector<DiffAction>& actions) {
    actions.clear();
    StyledText out;
    const std::vector<HunkOp> ops = hunkOps(side);
    std::unordered_map<size_t, std::vector<HunkOp>> at;
    if (!ops.empty()) {
        const std::vector<Git::FileDiff> files = Git::parseDiff(bytes);
        for (const DiffHunk& h : diffHunks(bytes))
            if (h.file < files.size() && hunkRefusal(files[h.file]).empty() &&
                h.kinds.find_first_of("+-") != std::string::npos)
                at[h.firstLine] = ops;
    }
    appendDiffImpl(out, bytes, &at, &actions);
    return out;
}

StyledText diffText(const std::string& bytes) {
    StyledText out;
    appendDiff(out, bytes);
    return out;
}

StyledText commitText(const std::string& show, long long now, DateFormat date) {
    Git::CommitDetail d;
    if (!Git::parseShow(show, d)) return diffText(show);
    StyledText out;
    auto line = [&](const std::string& name, const std::string& text) {
        std::string label = name;
        if (label.size() < 10) label.append(10 - label.size(), ' ');
        out.add(label, Style::Muted);
        out.add(validUtf8(text) + "\n", Style::Plain);
    };
    out.add("commit    ", Style::Muted);
    out.add(validUtf8(d.hash), Style::Hash);
    out.add("\n", Style::Plain);
    std::string parents;
    for (const std::string& p : d.parents) parents += (parents.empty() ? "" : " ") + Git::shortHash(p);
    if (d.parents.size() > 1) line("Merge", parents);
    else if (d.parents.size() == 1) line("Parent", parents);
    line("Author", d.author + " <" + d.email + ">");
    if (!d.committer.empty()) line("Committer", d.committer);
    line("Date", date(d.time) + " (" + Git::relativeTime(d.time, now) + ")");
    out.add("\n", Style::Plain);
    const std::string msg = validUtf8(d.message);
    const size_t nl = msg.find('\n');
    out.add(nl == std::string::npos ? msg : msg.substr(0, nl), Style::Bold);
    if (nl != std::string::npos) out.add(msg.substr(nl), Style::Plain);
    out.add("\n\n", Style::Plain);
    if (d.parents.size() > 1)
        out.add("Changes against the first parent, which is what the merge brought in.\n\n",
                Style::Muted);
    if (d.patch.empty()) out.add("No changes in this commit.\n", Style::Muted);
    else appendDiff(out, d.patch);
    return out;
}

std::string commitTitle(const Git::Commit& c) {
    return Git::shortHash(c.hash) + " " + validUtf8(c.subject);
}

}  // namespace GitUi
