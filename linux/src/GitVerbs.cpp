// GitVerbs.cpp — the Source Control panel's commands (PARITY.md 2.9), the
// Mac's in src/GitPanel.mm: Fetch, Pull, Push and Sync, and autofetch; Stage
// All and Unstage All; Discard Changes, one row or several, with the file as
// it was in the Trash (GitTrash.h), and Undo Discard; a hunk's or the
// selected lines' Stage, Unstage and Discard; the branch picker; Commit and
// Amend, Undo Last Commit, Stash and Pop Stash. The plans and their words
// are the core's (GitModel.h); this file runs them. Each reads the status
// now on the git thread, so it acts on the repository as it is, with the
// panel shown or not; asks when the plan asks (an alert over the window,
// its first button the plan's); and says what happened under the Commit
// button, and in the status bar while the panel is hidden.
#include "GitPanel.h"
#include "GitTrash.h"

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <unordered_map>

#define GIT_LOG_DOMAIN "minicode-git"

namespace {

std::string nameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

const char* opName(GitUi::SyncOp op) {
    switch (op) {
    case GitUi::SyncOp::Fetch: return "fetch";
    case GitUi::SyncOp::Pull: return "pull";
    case GitUi::SyncOp::Push: return "push";
    case GitUi::SyncOp::Sync: return "sync";
    }
    return "";
}

// Each repository's autofetch state, by its top level, shared by the
// windows on it so two of them do not fetch twice. Main thread only.
std::unordered_map<std::string, GitUi::AutoFetchState>& autoFetchStates() {
    static std::unordered_map<std::string, GitUi::AutoFetchState> states;
    return states;
}

double nowSeconds() { return g_get_monotonic_time() / 1e6; }

}  // namespace

// ---------------------------------------------------------------- pieces
void GitPanel::say(const std::string& text, bool info) {
    setError(text, info);
    if (!text.empty() && !active_ && onNote) onNote(text);
}

// Refused before anything ran: why, and the bell.
void GitPanel::refuse(const GitUi::Snapshot& snap, const std::string& refusal) {
    say(snap.inRepository ? refusal : (!snap.notice.empty() ? snap.notice : snap.errorText), false);
    gtk_widget_error_bell(root_);
    actionDone();
}

// A command finished, or was refused: the checks count them, and Undo
// Discard's command follows what there is to undo.
void GitPanel::actionDone() {
    actionsDone_++;
    updateActions();
}

// The plan's question over the window: its button first and the default,
// Cancel the Escape; `go` runs on the first.
void GitPanel::ask(const GitUi::ActionPlan& plan, std::function<void()> go) {
    GtkWindow* parent = hostWindow;
    if (!parent) {
        GtkRoot* r = gtk_widget_get_root(root_);
        if (r && GTK_IS_WINDOW(r)) parent = GTK_WINDOW(r);
    }
    GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", plan.question.c_str());
    if (!plan.detail.empty()) gtk_alert_dialog_set_detail(dlg, plan.detail.c_str());
    const char* buttons[] = {plan.button.c_str(), "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 1);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_modal(dlg, TRUE);
    g_log(GIT_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "question: %s", plan.question.c_str());
    struct Pending {
        std::shared_ptr<bool> alive;
        std::function<void()> go;
    };
    gtk_alert_dialog_choose(dlg, parent, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            auto* p = static_cast<Pending*>(data);
            const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
            if (choice == 0 && *p->alive) p->go();
            delete p;
        }, new Pending{alive_, std::move(go)});
    g_object_unref(dlg);
}

// ---------------------------------------------------- staging and commits
void GitPanel::commit() {
    if (!inRepository_) return;
    runAction(GitUi::commitArgs(message()), topLevel_, [this](const GitResult& r) {
        if (!r.ok()) return;   // the message is kept when a commit fails
        setMessage("");
        setError(GitUi::firstLine(r.out), true);   // "[main 1a2b3c4] Subject"
    });
}

// add -A (a deletion staged as one); restore --staged, both halves of a
// rename; rm --cached --force before the first commit, which has no HEAD.
// Several rows chosen: the Changes rows staged and the Staged rows unstaged,
// one git each.
void GitPanel::toggleStage(int row) {
    const GitUi::Row* r = fileRow(row);
    if (!r) {
        gtk_widget_error_bell(list_);
        return;
    }
    const std::vector<GitUi::Row> rows = rowsActingWith(row);
    if (rows.size() > 1) {
        const std::vector<std::string> stage = GitUi::stageRowsArgs(rows, initial_, true);
        const std::vector<std::string> unstage = GitUi::stageRowsArgs(rows, initial_, false);
        if (!stage.empty()) runAction(stage, topLevel_, nullptr);
        if (!unstage.empty()) runAction(unstage, topLevel_, nullptr);
        return;
    }
    runAction(GitUi::stageArgs(*r, initial_), topLevel_, nullptr);
}

void GitPanel::stageRows(int row, bool stage) {
    const std::vector<std::string> args = GitUi::stageRowsArgs(rowsActingWith(row), initial_, stage);
    if (args.empty()) {
        gtk_widget_error_bell(list_);
        return;
    }
    runAction(args, topLevel_, nullptr);
}

void GitPanel::stageAll() { runAllStaging(true); }
void GitPanel::unstageAll() { runAllStaging(false); }

// Planned from a status read now, so the counts are current and the panel
// need not be shown.
void GitPanel::runAllStaging(bool stage) {
    const std::string root = rootDir_;
    if (root.empty()) return;
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        auto snap = std::make_shared<GitUi::Snapshot>(gitSnapshotAt(root));
        auto plan = std::make_shared<GitUi::ActionPlan>(stage ? GitUi::stageAllPlan(*snap)
                                                              : GitUi::unstageAllPlan(*snap));
        deliver(alive, self, [=](GitPanel* p) {
            if (root != p->rootDir_) return;
            if (plan->args.empty()) {
                p->refuse(*snap, plan->refusal);
                return;
            }
            const std::string top = snap->topLevel;
            auto go = [p, plan, top] {
                const std::string done = plan->doneText;
                p->runAction(plan->args, top, [p, done](const GitResult& r) {
                    if (r.ok()) p->say(done, true);
                    p->actionDone();
                });
            };
            if (plan->question.empty()) go();
            else p->ask(*plan, go);
        });
    });
}

// Commit and Amend, Undo Last Commit, Stash and Pop Stash: the plan made on
// the git thread from a status read now (and what else it reads there, in
// `extra`, while it is still there: the message of the commit about to be
// undone); asked when it asks; run; said.
void GitPanel::runVerb(
    std::function<GitUi::ActionPlan(const GitUi::Snapshot&, std::string* extra)> plan,
    std::function<void(const GitResult&, const std::string& extra)> after) {
    const std::string root = rootDir_;
    if (root.empty()) return;
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        auto snap = std::make_shared<GitUi::Snapshot>(gitSnapshotAt(root));
        auto extra = std::make_shared<std::string>();
        auto p = std::make_shared<GitUi::ActionPlan>(plan(*snap, extra.get()));
        deliver(alive, self, [=](GitPanel* panel) {
            if (root != panel->rootDir_) return;
            if (p->args.empty()) {
                panel->refuse(*snap, p->refusal);
                return;
            }
            const std::string top = snap->topLevel;
            auto go = [panel, p, top, extra, after] {
                panel->runAction(p->args, top, [panel, p, extra, after](const GitResult& r) {
                    if (r.ok())
                        panel->say(!p->doneText.empty() ? p->doneText : GitUi::firstLine(r.out),
                                   true);
                    if (after) after(r, *extra);
                    panel->actionDone();
                });
            };
            if (p->question.empty()) go();
            else panel->ask(*p, go);
        });
    });
}

void GitPanel::commitAmend() {
    const std::string text = message();
    runVerb([text](const GitUi::Snapshot& s, std::string*) { return GitUi::amendPlan(s, text); },
            [this](const GitResult& r, const std::string&) {
                if (r.ok()) setMessage("");
            });
}

// HEAD's parents first (a first commit, a merge), then the plan; the
// undone commit's message goes back in the box, as VS Code does.
void GitPanel::undoLastCommit() {
    runVerb([](const GitUi::Snapshot& s, std::string* extra) {
                int parents = -1;
                if (s.inRepository) {
                    GitResult r = runGit(s.topLevel, GitUi::headParentsArgs());
                    if (r.ok()) parents = GitUi::parentCount(r.out);
                }
                GitUi::ActionPlan p = GitUi::undoCommitPlan(s, parents);
                if (!p.args.empty()) {
                    GitResult m = runGit(s.topLevel, GitUi::headMessageArgs());
                    if (m.ok()) *extra = m.out;
                }
                return p;
            },
            [this](const GitResult& r, const std::string& message) {
                const std::string m = trimmed(message);
                if (r.ok() && !m.empty()) setMessage(GitUi::validUtf8(m));
            });
}

void GitPanel::stash() {
    const std::string text = message();
    runVerb([text](const GitUi::Snapshot& s, std::string*) { return GitUi::stashPlan(s, text); },
            nullptr);
}

void GitPanel::popStash() {
    runVerb([](const GitUi::Snapshot& s, std::string*) {
                int n = 0;
                if (s.inRepository) {
                    GitResult r = runGit(s.topLevel, GitUi::stashListArgs());
                    if (r.ok()) n = (int)GitUi::countLines(r.out);
                }
                return GitUi::popStashPlan(s, n);
            },
            nullptr);
}

// ---------------------------------------------------------------- discard
// The row, or the rows chosen with it: one question naming the file or the
// count, refused whole if any one would be (GitUi::discardBatchPlan).
void GitPanel::discardRow(int row) {
    if (!fileRow(row) || topLevel_.empty()) {
        gtk_widget_error_bell(root_);
        return;
    }
    discardRows(rowsActingWith(row), topLevel_);
}

void GitPanel::discardRows(const std::vector<GitUi::Row>& rows, const std::string& top) {
    const GitUi::DiscardBatch batch = GitUi::discardBatchPlan(rows);
    if (!batch.refusal.empty()) {
        say(batch.refusal, false);
        gtk_widget_error_bell(root_);
        actionDone();
        return;
    }
    ask(batch, [this, batch, top] { runDiscards(batch.plans, batch.paths, top, batch.doneText); });
}

// Discard Changes from the menu bar or the palette: the file on show, if
// it has changes in the Changes list, by a status read now.
void GitPanel::discardPath(const std::string& path) {
    const std::string root = rootDir_;
    if (root.empty() || path.empty()) {
        gtk_widget_error_bell(root_);
        return;
    }
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        auto snap = std::make_shared<GitUi::Snapshot>(gitSnapshotAt(root));
        std::string rel;
        if (snap->inRepository) {
            // git names the top level with links resolved; the file may be
            // gone, so its folder is resolved instead.
            char dirBuf[PATH_MAX], topBuf[PATH_MAX];
            gchar* dir = g_path_get_dirname(path.c_str());
            if (realpath(dir, dirBuf) && realpath(snap->topLevel.c_str(), topBuf)) {
                const std::string full = std::string(dirBuf) + "/" + nameOf(path);
                const std::string top = topBuf;
                if (full.compare(0, top.size() + 1, top + "/") == 0) rel = full.substr(top.size() + 1);
            }
            g_free(dir);
        }
        const int at = rel.empty() ? -1 : GitUi::changesRowFor(snap->rows, rel);
        deliver(alive, self, [=](GitPanel* p) {
            if (root != p->rootDir_) return;
            if (at < 0) {
                p->say(!snap->inRepository
                           ? (!snap->notice.empty() ? snap->notice : snap->errorText)
                           : nameOf(path) + " has no changes to discard.",
                       false);
                gtk_widget_error_bell(p->root_);
                p->actionDone();
                return;
            }
            p->discardRows({snap->rows[(size_t)at]}, snap->topLevel);
        });
    });
}

// Each in turn on the git thread: the file as it is goes to the Trash
// first, then git restores it; if git fails, the Trash's copy comes back,
// so a refusal leaves the file alone. The first failure stops the rest, and
// the ones done before it stay done (and undoable), each its own Undo
// Discard.
void GitPanel::runDiscards(const std::vector<GitUi::DiscardPlan>& plans,
                           const std::vector<std::string>& rels, const std::string& top,
                           const std::string& doneText) {
    const std::string root = rootDir_;
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        std::vector<Discard> made;
        std::string failure;
        for (size_t i = 0; i < plans.size() && i < rels.size() && failure.empty(); i++) {
            const GitUi::DiscardPlan& p = plans[i];
            const std::string path = top + "/" + rels[i];
            Discard d;
            d.path = path;
            d.rel = rels[i];
            if (p.trashFirst) {
                std::string err;
                if (!GitTrash::trash(path, &d.trashed, &err)) {
                    failure = nameOf(rels[i]) + " could not be moved to the Trash (" + err +
                              "), so nothing was discarded.";
                    break;
                }
                d.kept = true;
            }
            if (!p.args.empty()) {
                GitResult r = runGit(top, p.args);
                if (!r.ok()) {
                    failure = gitFailure(r);
                    if (d.kept && !d.trashed.empty()) GitTrash::putBack(d.trashed, path, nullptr);
                    break;
                }
            }
            made.push_back(d);
        }
        deliver(alive, self, [=](GitPanel* panel) {
            panel->discards_.insert(panel->discards_.end(), made.begin(), made.end());
            while (panel->discards_.size() > 20) panel->discards_.erase(panel->discards_.begin());
            if (root == panel->rootDir_) {
                panel->say(failure.empty() ? doneText : failure, failure.empty());
                panel->runRefresh();
            }
            panel->actionDone();
        });
    });
}

std::string GitPanel::undoDiscardTitle() const {
    return discards_.empty() ? std::string() : GitUi::undoDiscardTitle(discards_.back().rel);
}

// Undo Discard: what is at the path now (git's version, or edits since)
// goes to the Trash, and the discarded copy comes back from it.
void GitPanel::undoDiscard() {
    if (discards_.empty()) {
        gtk_widget_error_bell(root_);
        return;
    }
    const Discard d = discards_.back();
    discards_.pop_back();
    updateActions();
    const std::string root = rootDir_;
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        std::string failure, err;
        const std::string name = nameOf(d.rel);
        if (d.kept && !GitTrash::stillThere(d.trashed))
            failure = "The discarded copy of " + name + " is no longer in the Trash.";
        struct stat st;
        if (failure.empty() && lstat(d.path.c_str(), &st) == 0 &&
            !GitTrash::trash(d.path, nullptr, &err))
            failure = name + " could not be moved to the Trash (" + err + ").";
        if (failure.empty() && d.kept && !GitTrash::putBack(d.trashed, d.path, &err))
            failure = name + " could not be put back (" + err + ").";
        deliver(alive, self, [=](GitPanel* p) {
            if (root == p->rootDir_) {
                p->say(failure.empty() ? GitUi::undoneDiscardText(d.rel) : failure,
                       failure.empty());
                p->runRefresh();
            }
            p->actionDone();
        });
    });
}

// git.clean: the row selected in the panel while its list has the
// keyboard, else the file on show (VS Code's from the palette acts on the
// active editor's file).
void GitPanel::discardFromCommand() {
    const int row = listHasKeyboard() ? selectedRow() : -1;
    if (fileRow(row)) {
        discardRow(row);
        return;
    }
    const std::string file = fileOnShow ? fileOnShow() : std::string();
    if (!file.empty()) discardPath(file);
    else gtk_widget_error_bell(root_);
}

// ---------------------------------------------------------- hunks and lines
void GitPanel::runHunkOp(GitUi::HunkOp op, size_t first, size_t last, bool lines) {
    if (!shownDiff_.valid) {
        gtk_widget_error_bell(root_);
        actionDone();
        return;
    }
    const ShownDiff d = shownDiff_;
    const GitUi::HunkPlan plan =
        GitUi::hunkPlan(d.bytes, GitUi::diffSideOf(d.row), op, first, last, lines);
    if (!plan.refusal.empty()) {
        say(plan.refusal, false);
        gtk_widget_error_bell(root_);
        actionDone();
        return;
    }
    const bool discard = op == GitUi::HunkOp::Discard;
    auto go = [this, plan, discard, d] { runHunkPlan(plan, discard, d); };
    if (plan.question.empty()) go();
    else ask(plan, go);
}

// git apply with the patch on stdin. Discard first puts a copy of the file
// as it is in the Trash (as Discard Changes does), so Undo Discard can put
// it back; if git refuses, the copy is taken out again. The diff is then
// shown again: what is left, or, for a diff gone stale, what is there now.
void GitPanel::runHunkPlan(const GitUi::HunkPlan& plan, bool discard, const ShownDiff& d) {
    const std::string root = rootDir_;
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        const std::string path = d.top + "/" + plan.path;
        std::string trashed, failure;
        if (discard) {
            std::string err;
            if (!GitTrash::trashCopy(path, &trashed, &err))
                failure = nameOf(plan.path) + " could not be copied to the Trash (" + err +
                          "), so nothing was discarded.";
        }
        if (failure.empty()) {
            GitResult r = runGit(d.top, plan.args, &plan.patch);
            if (!r.ok()) {
                failure = GitUi::hunkFailureText(r.status, r.out, r.err, plan.path);
                GitTrash::remove(trashed);
                trashed.clear();
            }
        }
        deliver(alive, self, [=](GitPanel* p) {
            if (failure.empty() && discard) {
                Discard u;
                u.path = path;
                u.rel = plan.path;
                u.trashed = trashed;
                u.kept = true;
                p->discards_.push_back(u);
                if (p->discards_.size() > 20) p->discards_.erase(p->discards_.begin());
            }
            if (root == p->rootDir_) {
                p->say(failure.empty() ? plan.doneText : failure, failure.empty());
                p->runRefresh();
                p->showDiffOf(d.row, d.top);
            }
            p->actionDone();
        });
    });
}

// ------------------------------------------------------ the branch picker
// The palette over the window, listing the branches (the current one first,
// then by their last commit, then remote ones); typing filters them, and a
// name that is no branch gets a last row that makes it.
void GitPanel::chooseBranch() {
    const std::string root = rootDir_;
    if (root.empty()) return;
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        auto snap = std::make_shared<GitUi::Snapshot>(gitSnapshotAt(root));
        auto items = std::make_shared<std::vector<GitUi::BranchItem>>();
        std::string failure;
        if (!snap->inRepository) {
            failure = !snap->notice.empty() ? snap->notice : snap->errorText;
        } else {
            GitResult refs = runGit(snap->topLevel, GitUi::branchListArgs());
            if (!refs.ok()) failure = gitFailure(refs);
            else *items = GitUi::branchItems(refs.out, *snap);
        }
        deliver(alive, self, [=](GitPanel* p) {
            if (root != p->rootDir_) return;
            if (!failure.empty()) {
                p->say(failure, false);
                gtk_widget_error_bell(p->root_);
                return;
            }
            if (!p->showPick) return;
            PalettePanel::Pick pick;
            pick.placeholder = "Select a branch to switch to, or type a name for a new one";
            for (const GitUi::BranchItem& b : *items) {
                pick.labels.push_back(GitUi::validUtf8(b.name));
                pick.details.push_back(GitUi::validUtf8(b.detail));
            }
            pick.emptyText = "No branch matches";
            std::shared_ptr<bool> alive2 = p->alive_;
            pick.choose = [alive2, p, items, snap](int i) {
                if (*alive2 && i >= 0 && i < (int)items->size())
                    p->switchToBranch((*items)[(size_t)i], *snap);
            };
            pick.typedRow = [items, snap](const std::string& text) {
                return GitUi::createBranchLabel(text, *items, *snap);
            };
            pick.chooseTyped = [alive2, p, snap](const std::string& text) {
                if (*alive2) p->createBranch(text, *snap);
            };
            p->showPick(pick);
        });
    });
}

// A refused switch (local changes git would overwrite) says what git said.
void GitPanel::switchToBranch(const GitUi::BranchItem& b, const GitUi::Snapshot& snap) {
    const std::vector<std::string> args = GitUi::switchArgs(b, snap);
    if (args.empty()) {
        say("Already on " + b.name + ".", true);
        actionDone();
        return;
    }
    const std::string text = b.remote ? GitUi::switchedText(GitUi::localNameFor(b.name, snap), true)
                                      : GitUi::switchedText(b.name, false);
    runAction(args, snap.topLevel, [this, text](const GitResult& r) {
        if (r.ok()) say(text, true);
        actionDone();
    });
}

void GitPanel::createBranch(const std::string& typed, const GitUi::Snapshot& snap) {
    const std::string name = GitUi::newBranchName(typed);
    const std::string problem = GitUi::branchNameProblem(name);
    if (!problem.empty()) {
        say(problem, false);
        gtk_widget_error_bell(root_);
        actionDone();
        return;
    }
    const std::string text = GitUi::switchedText(name, true);
    runAction(GitUi::createBranchArgs(name), snap.topLevel, [this, text](const GitResult& r) {
        if (r.ok()) say(text, true);
        actionDone();
    });
}

// ---------------------------------------------------- pushing and pulling
void GitPanel::fetch() { runSync(GitUi::SyncOp::Fetch); }
void GitPanel::pull() { runSync(GitUi::SyncOp::Pull); }
void GitPanel::push() { runSync(GitUi::SyncOp::Push); }
void GitPanel::sync() { runSync(GitUi::SyncOp::Sync); }

void GitPanel::updateSyncButton() {
    if (!syncButton_) return;
    const std::string title = haveShown_ ? GitUi::validUtf8(GitUi::syncButtonTitle(shown_)) : "";
    const bool show = inRepository_ && gtk_widget_get_visible(messageFrame_) &&
                      (!title.empty() || syncing_);
    if (!syncing_) {
        gtk_button_set_label(GTK_BUTTON(syncButton_), title.c_str());
        const std::string tip = title.empty() ? "" : GitUi::validUtf8(GitUi::syncButtonToolTip(shown_));
        gtk_widget_set_tooltip_text(syncButton_, tip.empty() ? nullptr : tip.c_str());
    }
    gtk_widget_set_sensitive(syncButton_, !syncing_);
    if (gtk_widget_get_visible(syncButton_) != show) {
        gtk_widget_set_visible(syncButton_, show);
        gtk_widget_queue_allocate(root_);
    }
}

// Fetch, Pull, Push or Sync, from the button, the menus and the commands.
// The plan is made from a status read just now, so the counts and the
// upstream are current even when the panel has not been shown; nothing is
// asked first (VS Code's git.confirmSync off). One at a time; a second while
// one runs rings the bell. `quietly` is autofetch's: no button, no words.
void GitPanel::runSync(GitUi::SyncOp op, bool quietly) {
    const std::string root = rootDir_;
    if (root.empty()) return;
    // One asked for at a time; one asked for while a background fetch runs
    // waits behind it on the git thread.
    if (quietly ? (syncing_ || autoFetching_) : syncing_) {
        if (!quietly) gtk_widget_error_bell(root_);
        return;
    }
    if (quietly) autoFetching_ = true;
    else syncing_ = true;
    updateSyncButton();
    std::shared_ptr<bool> alive = alive_;
    GitPanel* self = this;
    post([=] {
        const GitUi::Snapshot snap = gitSnapshotAt(root);
        GitUi::SyncPlan plan = GitUi::syncPlan(op, snap);
        if (quietly && (!snap.inRepository || snap.remotes.empty())) plan = GitUi::SyncPlan();
        const std::string top = snap.topLevel;
        if (plan.steps.empty()) {
            deliver(alive, self, [=](GitPanel* p) {
                p->finishSync(op, plan, nullptr, top, root, quietly);
            });
            return;
        }
        if (!quietly) {
            const std::string busy = plan.busyText;
            deliver(alive, self, [=](GitPanel* p) {
                if (root != p->rootDir_) return;
                gtk_button_set_label(GTK_BUTTON(p->syncButton_), busy.c_str());
                p->setError(busy, true);
            });
        }
        auto last = std::make_shared<GitResult>();
        for (const std::vector<std::string>& step : plan.steps) {
            *last = runGit(top, step);
            if (!last->ok()) break;
        }
        deliver(alive, self, [=](GitPanel* p) {
            p->finishSync(op, plan, last.get(), top, root, quietly);
        });
    });
}

void GitPanel::finishSync(GitUi::SyncOp op, const GitUi::SyncPlan& plan, const GitResult* r,
                          const std::string& top, const std::string& root, bool quietly) {
    if (quietly) autoFetching_ = false;
    else syncing_ = false;
    if (!top.empty()) autoKey_ = top;
    const bool ran = r != nullptr;
    const bool ok = ran && r->ok();
    // Any fetch counts for autofetch's wait; an autofetch that found no
    // remote too (it was keyed by the folder until the top level was known).
    if (!top.empty() && (quietly || (ran && plan.fetches))) {
        GitUi::AutoFetchState& st = autoFetchStates()[top];
        st.lastStart = quietly ? autoNow_ : nowSeconds();
        if (ran && ok) st.failures = 0;
        else if (ran && quietly) st.failures++;
    }
    if (ran && quietly) autoFetches_++;
    lastSync_ = std::string(opName(op)) + " " + (!ran ? "refused" : ok ? "ok" : "failed");
    g_log(GIT_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "sync: %s", lastSync_.c_str());
    if (root != rootDir_) {
        updateSyncButton();
        return;
    }
    std::string text;
    bool info = true, haveText = false;
    if (!ran) {
        info = false;
        if (!quietly) {
            text = plan.refusal;
            haveText = true;
            gtk_widget_error_bell(root_);
        }
    } else if (!ok) {
        text = r->timedOut ? r->err : GitUi::networkFailureText(r->status, r->out, r->err, true);
        if (quietly) text = "Autofetch: " + text;
        info = false;
        haveText = true;
    } else if (!quietly) {
        text = plan.doneText;
        haveText = true;
    }
    if (!quietly) say(text, info);
    else if (haveText) setError(text, info);
    // A background fetch that works again takes its old failure away.
    else if (ok && errorText_.rfind("Autofetch: ", 0) == 0) setError("", false);
    updateSyncButton();
    // The counts, the graph and the lists as they are now.
    if (ran && active_) runRefresh();
}

// git.autofetch: called every 30 seconds by the panel's timer (and by the
// checks with a time of their own). By what is known so far: the
// repository's top level once a status has said it, else the folder; the
// fetch itself reads the remotes again and fetches nothing without one.
bool GitPanel::autoFetchTickAt(double now) {
    if (!autoFetchOn_ || rootDir_.empty() || syncing_ || autoFetching_) return false;
    if (!(isActive && isActive())) return false;
    const std::string key = autoKey_.empty() ? rootDir_ : autoKey_;
    GitUi::AutoFetchState& st = autoFetchStates()[key];
    GitUi::Snapshot probe;
    probe.inRepository = true;
    probe.remotes = {"?"};
    if (!GitUi::autoFetchDue(true, true, probe, false, st, now, autoFetchPeriod_)) return false;
    // A folder with no repository or no remote is looked at again only when
    // the wait is over.
    st.lastStart = now;
    autoNow_ = now;
    runSync(GitUi::SyncOp::Fetch, true);
    return true;
}
