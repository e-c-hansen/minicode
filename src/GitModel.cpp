// GitModel.cpp — see GitModel.h.
#include "GitModel.h"

#include <algorithm>
#include <ctime>

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
            staged.push_back(r);
        }
        if (Git::hasUnstaged(e)) {
            Row r;
            r.letter = Git::unstagedLetter(e);
            r.path = e.path;
            r.untracked = e.untracked;
            r.unmerged = e.unmerged;
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
    std::vector<std::string> a = {"-c", "core.quotePath=false", "--literal-pathspecs",
                                  "diff", "--no-color", "--no-ext-diff",
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

std::string networkFailureText(int status, const std::string& out, const std::string& err) {
    std::string t = failureText(status, out, err);
    auto has = [&](const char* needle) { return t.find(needle) != std::string::npos; };
    if (has("terminal prompts disabled") || has("could not read Username") ||
        has("could not read Password") || has("Authentication failed") ||
        has("Invalid username or password"))
        return t + "\nminicode never asks for a password: set up a credential helper "
                   "(gh auth setup-git, or the macOS keychain), or push once from the "
                   "terminal.";
    if (has("Permission denied (publickey") || has("Host key verification failed") ||
        has("passphrase"))
        return t + "\nminicode cannot type an ssh passphrase or accept a new host key: "
                   "add the key to the agent (ssh-add --apple-use-keychain) or connect "
                   "once from the terminal.";
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

void appendDiff(StyledText& out, const std::string& input) {
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
    for (const Git::DiffLine& l : Git::classifyDiff(bytes)) {
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
        out.add(validUtf8(l.text) + "\n", st);
    }
    if (cut) out.add("\n(The diff is longer than 4 MB; the rest is not shown.)\n", Style::Muted);
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
