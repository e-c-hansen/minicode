// AgentWorktrees.cpp — see AgentWorktrees.h.
#include "AgentWorktrees.h"

#include <glib.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <ctime>
#include <fstream>
#include <sstream>

#include "AgentIndex.h"
#include "GitRun.h"

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string textOf(const std::string& bytes) {
    if (bytes.find('\0') != std::string::npos && bytes.find('\0') < 8192) return "(a binary file)";
    if (g_utf8_validate(bytes.data(), (gssize)bytes.size(), nullptr)) return bytes;
    char* u = g_convert(bytes.data(), (gssize)bytes.size(), "UTF-8", "ISO-8859-1", nullptr, nullptr, nullptr);
    std::string out = u ? u : "";
    g_free(u);
    return out;
}

struct Named {
    std::string label;
    bool running;
};

void collect(const std::vector<AgentModelData::Work>& works, double now,
             std::vector<std::pair<std::string, Named>>* out) {
    for (const AgentModelData::Work& w : works) {
        if (!w.worktreePath.empty()) out->push_back({w.worktreePath, {w.name, w.runningAt(now)}});
        collect(w.subagents, now, out);
    }
}

struct Pack {
    AgentWorktrees* self;
    std::shared_ptr<bool> alive;
    std::function<void(AgentWorktrees*)> fn;
};

void toMain(AgentWorktrees* self, std::shared_ptr<bool> alive, std::function<void(AgentWorktrees*)> fn) {
    Pack* p = new Pack{self, std::move(alive), std::move(fn)};
    g_idle_add(
        [](gpointer d) -> gboolean {
            std::unique_ptr<Pack> pk(static_cast<Pack*>(d));
            if (*pk->alive) pk->fn(pk->self);
            return G_SOURCE_REMOVE;
        },
        p);
}
}  // namespace

AgentWorktrees::AgentWorktrees(const std::string& folder, AgentModel& model) : model_(model) {
    setFolder(folder);
}

AgentWorktrees::~AgentWorktrees() {
    *alive_ = false;
    if (timer_) g_source_remove(timer_);
}

void AgentWorktrees::setFolder(const std::string& folder) {
    folder_ = folder;
    char buf[PATH_MAX];
    real_ = realpath(folder.c_str(), buf) ? buf : folder;
    list_.clear();
    loaded_ = false;
}

const WorktreeInfo* AgentWorktrees::find(const std::string& path) const {
    char buf[PATH_MAX];
    const std::string real = realpath(path.c_str(), buf) ? buf : path;
    for (const WorktreeInfo& w : list_) {
        if (w.path == path) return &w;
        const std::string wr = realpath(w.path.c_str(), buf) ? buf : w.path;
        if (wr == real) return &w;
    }
    return nullptr;
}

void AgentWorktrees::refresh() {
    if (busy_) { again_ = true; return; }
    busy_ = true;
    run();
}

void AgentWorktrees::refreshSoon() {
    if (timer_) return;
    timer_ = g_timeout_add(2000, [](gpointer p) -> gboolean {
        auto* s = static_cast<AgentWorktrees*>(p);
        s->timer_ = 0;
        s->refresh();
        return G_SOURCE_REMOVE;
    }, this);
}

void AgentWorktrees::run() {
    const std::string folder = folder_, real = real_;
    auto snap = model_.snapshot();
    std::vector<std::pair<std::string, Named>> named;
    const double now = (double)time(nullptr);
    collect(snap->sessions, now, &named);
    auto alive = alive_;
    std::thread([this, alive, folder, real, named] {
        std::vector<WorktreeInfo> out;
        std::string mainRef;
        if (!gitExecutable().empty()) {
            GitResult list = runGit(folder, {"worktree", "list", "--porcelain"});
            std::vector<AgentIndex::Worktree> all;
            if (list.ok()) {
                all = AgentIndex::parseWorktreeList(list.out);
                mainRef = AgentView::mainRef(all);
            }
            for (const AgentIndex::Worktree& w :
                 AgentIndex::worktreesUnder(all, real, nullptr, (double)time(nullptr))) {
                if (w.bare || w.prunable) continue;
                WorktreeInfo t;
                t.path = w.path;
                t.branch = w.branch;
                t.head = w.head;
                size_t slash = w.path.find_last_of('/');
                t.label = slash == std::string::npos ? w.path : w.path.substr(slash + 1);
                for (const auto& n : named)
                    if (n.first == w.path || AgentIndex::isInside(w.path, n.first)) {
                        if (!n.second.label.empty()) t.label = n.second.label;
                        t.running = n.second.running;
                        break;
                    }
                if (!mainRef.empty() && !w.head.empty()) {
                    GitResult r = runGit(folder, {"rev-list", "--count", mainRef + ".." + w.head, "--"});
                    if (r.ok()) t.ahead = atoi(r.out.c_str());
                }
                GitResult st = runGit(w.path, {"status", "--porcelain", "--untracked-files=normal"});
                t.dirty = st.ok() && !st.out.empty();
                out.push_back(std::move(t));
            }
        }
        toMain(this, alive, [out, mainRef](AgentWorktrees* s) {
            s->list_ = out;
            s->mainBranch_ = mainRef;
            s->loaded_ = true;
            s->busy_ = false;
            if (s->onChange) s->onChange();
            if (s->again_) {
                s->again_ = false;
                s->refresh();
            }
        });
    }).detach();
}

void AgentWorktrees::changes(const WorktreeInfo& w, ChangesDone done) {
    const std::string main = mainBranch_, path = w.path;
    if (main.empty() || path.empty()) {
        done({}, "There is no main branch to compare with.");
        return;
    }
    auto alive = alive_;
    auto cb = std::make_shared<ChangesDone>(std::move(done));
    std::thread([this, alive, main, path, cb] {
        std::string error;
        std::vector<AgentView::Change> changes;
        GitResult base = runGit(path, {"merge-base", main, "HEAD"});
        const std::string baseRef = base.ok() ? trim(base.out) : main;
        GitResult diff = runGit(path, {"-c", "core.quotePath=false", "diff", "--name-status", "-z",
                                       "-M", "--no-ext-diff", baseRef.empty() ? main : baseRef, "--"});
        GitResult un = runGit(path, {"ls-files", "--others", "--exclude-standard", "-z"});
        if (!diff.ok()) error = diff.err.empty() ? "git diff failed." : diff.err;
        else changes = AgentView::parseChanges(diff.out, un.ok() ? un.out : std::string());
        toMain(this, alive, [cb, changes, error](AgentWorktrees*) { (*cb)(changes, error); });
    }).detach();
}

void AgentWorktrees::comparison(const WorktreeInfo& w, const AgentView::Change& c,
                                std::function<void(CompareSpec)> done) {
    const std::string main = mainBranch_.empty() ? "main" : mainBranch_;
    const WorktreeInfo wt = w;
    auto alive = alive_;
    auto cb = std::make_shared<std::function<void(CompareSpec)>>(std::move(done));
    std::thread([this, alive, main, wt, c, cb] {
        const std::string oldPath = c.from.empty() ? c.path : c.from;
        std::string left, right;
        // The list is the worktree against where its branch left main, so the
        // left pane is main there too: main's later work is not the agent's
        // and would show as the agent taking it out.
        GitResult mb = runGit(wt.path, {"merge-base", main, "HEAD"});
        const AgentView::CompareBase base = AgentView::compareBase(main, mb.ok() ? mb.out : std::string());
        bool onMain = c.status != 'A' && c.status != '?';
        if (onMain) {
            GitResult show = runGit(wt.path, {"show", base.ref + ":" + oldPath});
            if (show.ok()) left = textOf(show.out);
            else onMain = false;
        }
        const std::string file = wt.path + "/" + c.path;
        struct stat st;
        const bool onDisk = c.status != 'D' && stat(file.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        if (onDisk) {
            std::ifstream in(file, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            right = textOf(ss.str());
        }
        CompareSpec spec;
        std::string label = wt.label.size() > 40 ? wt.label.substr(0, 40) + "…" : wt.label;
        size_t slash = c.path.find_last_of('/');
        spec.title = (slash == std::string::npos ? c.path : c.path.substr(slash + 1)) + " (" +
                     label + " vs " + main + ")";
        const std::string branch = wt.branch.empty() ? wt.label : wt.branch;
        spec.leftLabel = onMain ? base.label + ": " + oldPath : base.label + ": (not there)";
        spec.rightLabel = onDisk ? branch + ": " + c.path : branch + ": (deleted)";
        spec.leftText = left;
        spec.rightText = right;
        spec.path = onDisk ? file : std::string();
        toMain(this, alive, [cb, spec](AgentWorktrees*) { (*cb)(spec); });
    }).detach();
}
