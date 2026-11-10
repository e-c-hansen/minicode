// TreeGitColors.cpp — see TreeGitColors.h. The Mac's src/TreeGitColors.mm,
// step for step, with a watch on the git folder added (GIO's monitors are
// not recursive).
#include "TreeGitColors.h"

#include "GitRun.h"

#include <stdlib.h>

#include <cctype>

namespace {

void deliver(std::shared_ptr<bool> alive, std::function<void()> fn) {
    struct Delivery {
        std::shared_ptr<bool> alive;
        std::function<void()> fn;
    };
    auto* d = new Delivery{std::move(alive), std::move(fn)};
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        auto* d = static_cast<Delivery*>(p);
        if (*d->alive) d->fn();
        delete d;
        return G_SOURCE_REMOVE;
    }, d, nullptr);
}

void runJob(gpointer data, gpointer) {
    auto* job = static_cast<std::function<void()>*>(data);
    (*job)();
    delete job;
}

std::string withoutTrailingSlashes(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

// The path from `top`, false when it is not inside it ("" for top itself).
bool relativeTo(const std::string& path, const std::string& top, std::string* rel) {
    const std::string p = withoutTrailingSlashes(path), t = withoutTrailingSlashes(top);
    if (t.empty()) return false;
    if (p == t) {
        *rel = "";
        return true;
    }
    const std::string prefix = t == "/" ? t : t + "/";
    if (p.compare(0, prefix.size(), prefix) != 0) return false;
    *rel = p.substr(prefix.size());
    return true;
}

bool under(const std::string& rel, const char* dir) {
    const std::string d = dir;
    return rel == d || rel.compare(0, d.size() + 1, d + "/") == 0;
}

}  // namespace

TreeGitColors::TreeGitColors() {
    alive_ = std::make_shared<bool>(true);
    pool_ = g_thread_pool_new(runJob, nullptr, 1, FALSE, nullptr);
}

TreeGitColors::~TreeGitColors() {
    *alive_ = false;
    onChange = nullptr;
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    watch_.reset();
    g_thread_pool_free(pool_, FALSE, FALSE);
}

void TreeGitColors::post(std::function<void()> job) {
    g_thread_pool_push(pool_, new std::function<void()>(std::move(job)), nullptr);
}

void TreeGitColors::setRoot(const std::string& root) {
    if (root == root_) return;
    generation_++;
    root_ = root;
    char* real = root.empty() ? nullptr : realpath(root.c_str(), nullptr);
    realRoot_ = real ? std::string(real) : root;
    free(real);
    top_.clear();
    prefix_.clear();
    gitDir_.clear();
    watch_.reset();
    looked_ = false;
    again_ = false;
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    const bool had = decorations_ && !decorations_->empty();
    decorations_.reset();
    if (had && onChange) onChange();
    if (!root_.empty()) look();
}

void TreeGitColors::changedOnDisk(const std::string& path) {
    if (root_.empty()) return;
    bool wanted = false;
    std::string fromRoot;
    if (!looked_) {
        wanted = true;
    } else if (!relativeTo(path, root_, &fromRoot) && !relativeTo(path, realRoot_, &fromRoot)) {
        return;
    } else if (top_.empty()) {
        // Not in a repository: only a git init in the folder itself (an
        // entry of its own top folder changing) can make it one.
        wanted = !fromRoot.empty() && fromRoot.find('/') == std::string::npos;
    } else {
        const std::string rel = prefix_ + fromRoot;
        // git writes these while it works; the index, HEAD and refs, which
        // change what status says, are elsewhere in .git.
        if (under(rel, ".git/objects") || under(rel, ".git/logs")) return;
        if (!rel.empty() && decorations_ && decorations_->ignored(rel)) return;
        wanted = true;
    }
    if (wanted) schedule();
}

// Half a second after the first change, so a burst of writes costs one
// status; while one runs, another follows it.
void TreeGitColors::schedule() {
    if (running_) {
        again_ = true;
        return;
    }
    if (timer_) return;
    timer_ = g_timeout_add(500, [](gpointer p) -> gboolean {
        TreeGitColors* self = static_cast<TreeGitColors*>(p);
        self->timer_ = 0;
        self->look();
        return G_SOURCE_REMOVE;
    }, this);
}

void TreeGitColors::look() {
    if (root_.empty()) return;
    if (running_) {
        again_ = true;
        return;
    }
    running_ = true;
    const unsigned gen = generation_;
    const std::string realRoot = realRoot_, knownTop = top_, knownPrefix = prefix_,
                      knownGitDir = gitDir_;
    std::shared_ptr<const Git::Decorations> old = decorations_;   // compared off the main loop
    std::shared_ptr<bool> alive = alive_;
    post([this, alive, gen, realRoot, knownTop, knownPrefix, knownGitDir, old] {
        std::string top = knownTop, prefix = knownPrefix, gitDir = knownGitDir;
        if (top.empty()) {
            GitResult r = runGit(realRoot, {"rev-parse", "--show-toplevel", "--show-prefix",
                                            "--absolute-git-dir"});
            if (r.ok()) {
                std::vector<std::string> lines;
                size_t start = 0;
                while (start <= r.out.size()) {
                    size_t nl = r.out.find('\n', start);
                    if (nl == std::string::npos) nl = r.out.size();
                    lines.push_back(r.out.substr(start, nl - start));
                    start = nl + 1;
                }
                if (!lines.empty() && !lines[0].empty()) {
                    top = lines[0];
                    prefix = lines.size() >= 2 ? lines[1] : "";
                    gitDir = lines.size() >= 3 ? lines[2] : "";
                }
            }
        }
        std::shared_ptr<const Git::Decorations> decorations;
        if (!top.empty()) {
            GitResult st = runGit(top, Git::decorationArgs());
            if (st.ok()) {
                decorations = std::make_shared<const Git::Decorations>(
                    Git::Decorations::fromStatus(Git::parseStatus(st.out)));
            } else {
                // The repository went (or status failed): look again next time.
                top.clear();
                prefix.clear();
                gitDir.clear();
            }
        }
        const bool changed = (bool)decorations != (bool)old || (decorations && *decorations != *old);
        deliver(alive, [this, gen, top, prefix, gitDir, decorations, changed] {
            running_ = false;
            if (gen != generation_) {
                if (!root_.empty()) look();   // a new folder came while this ran
                return;
            }
            looked_ = true;
            runs_++;
            top_ = top;
            prefix_ = prefix;
            gitDir_ = gitDir;
            decorations_ = decorations;
            watchGitDir(gitDir);
            if (changed && onChange) onChange();
            if (again_) {
                again_ = false;
                schedule();
            }
        });
    });
}

// A stage, commit or checkout writes only in the git folder, which the
// tree's monitors never see (it is hidden, and they watch a folder each).
void TreeGitColors::watchGitDir(const std::string& gitDir) {
    if (watch_ && watch_->dir() == gitDir) return;
    watch_.reset();
    if (gitDir.empty()) return;
    watch_ = std::make_unique<GitDirWatch>(gitDir, gitStampOf(gitDir + "/index"), [this](bool gone) {
        if (gone) {
            top_.clear();   // look for the repository again
            watch_.reset();
        }
        schedule();
    });
}

Git::Decoration TreeGitColors::decorationFor(const std::string& path, bool isDir) const {
    if (!decorations_ || root_.empty()) return Git::Decoration::None;
    std::string tail;
    if (!relativeTo(path, root_, &tail) && !relativeTo(path, realRoot_, &tail))
        return Git::Decoration::None;
    // The agents' worktrees are repositories of their own.
    if (under(tail, ".claude/worktrees")) return Git::Decoration::None;
    const std::string rel = withoutTrailingSlashes(prefix_ + tail);
    return isDir ? decorations_->folder(rel) : decorations_->file(rel);
}

const char* TreeGitColors::cssClass(Git::Decoration d) {
    switch (d) {
    case Git::Decoration::None: return nullptr;
    case Git::Decoration::Ignored: return "minicode-git-ignored";
    case Git::Decoration::Untracked: return "minicode-git-untracked";
    case Git::Decoration::Added: return "minicode-git-added";
    case Git::Decoration::Deleted: return "minicode-git-deleted";
    case Git::Decoration::Modified: return "minicode-git-modified";
    case Git::Decoration::Conflict: return "minicode-git-conflict";
    }
    return nullptr;
}

std::string TreeGitColors::tooltip(Git::Decoration d) {
    std::string name = Git::decorationName(d);
    if (name.empty()) return name;
    name[0] = (char)std::toupper((unsigned char)name[0]);
    return name + " (git)";
}
