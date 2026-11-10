// GitMenus.cpp — the Source Control panel's right click menus and the
// window's commands, in one place (PARITY.md 2.9): a file row's menu (Open
// File, Open Changes, Stage or Unstage, Discard, Reveal in File Tree; with
// several rows chosen, what acts on all of them, with their count), and the
// panel's own (Pull, Push, Fetch and Sync; Commit and Amend and Undo Last
// Commit; Stage All, Unstage All and Undo Discard; Stash and Pop Stash;
// Switch Branch), the Mac's menuForRow: and menuForEvent: in src/
// GitPanel.mm, item for item. Every menu goes up through popupMenu, which
// ends it with Adjust Settings… (PARITY.md 2.11) as the Mac's are ended; a
// part with nothing of its own gets that item alone, and the message box's
// text menu ends with it too.
//
// The window's commands are View > Source Control's (Keys.cpp: win.gitfetch
// and the rest), which the menu bar and the palette offer; installActions
// puts them in the window's action map and updateActions keeps Undo Discard
// and Discard Changes enabled only when they can act, so the palette offers
// them only then.
#include "Appearance.h"
#include "GitPanel.h"

#include <glib/gstdio.h>

#include <cstring>

namespace {

// The menu item showing `label`: its tooltip says why it is disabled (a
// popover menu's model has no tooltips of its own).
void setItemTip(GtkWidget* menu, const std::string& label, const std::string& tip) {
    std::vector<GtkWidget*> todo = {menu};
    while (!todo.empty()) {
        GtkWidget* w = todo.back();
        todo.pop_back();
        if (GTK_IS_LABEL(w) && label == gtk_label_get_text(GTK_LABEL(w))) {
            GtkWidget* item = w;
            for (GtkWidget* up = w; up && up != menu; up = gtk_widget_get_parent(up))
                if (std::strcmp(G_OBJECT_TYPE_NAME(up), "GtkModelButton") == 0) {
                    item = up;
                    break;
                }
            gtk_widget_set_tooltip_text(item, tip.c_str());
            return;
        }
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            todo.push_back(c);
    }
}

void append(GMenu* section, const std::string& label, const char* action) {
    g_menu_append(section, label.c_str(), (std::string("gitpanel.") + action).c_str());
}

// The window's commands: the action's name in Keys.cpp's table.
const char* const kWindowActions[] = {
    "gitfetch",  "gitpull",       "gitpush",     "gitsync",  "gitstageall",
    "gitunstageall", "gitclean",  "gitundodiscard", "gitcheckout", "gitamend",
    "gitundocommit", "gitstash",  "gitstashpop",
};

const char* const kMenuActions[] = {
    "openfile", "openchanges", "togglestage", "stage", "unstage", "discard", "reveal",
    "pull", "push", "fetch", "sync", "amend", "undocommit", "stageall", "unstageall",
    "undodiscard", "stash", "popstash", "switchbranch",
};

}  // namespace

// ---------------------------------------------------------------- actions
void GitPanel::buildActions() {
    menuActions_ = g_simple_action_group_new();
    for (const char* name : kMenuActions) {
        GSimpleAction* a = g_simple_action_new(name, nullptr);
        g_signal_connect(a, "activate", G_CALLBACK(onMenuAction), this);
        g_action_map_add_action(G_ACTION_MAP(menuActions_), G_ACTION(a));
        g_object_unref(a);
    }
    gtk_widget_insert_action_group(root_, "gitpanel", G_ACTION_GROUP(menuActions_));
    // The message box keeps its text's own menu, which ends with Adjust
    // Settings… like every menu of the panel (the Mac's MCCommitTextView).
    Appearance::addToTextMenus(message_, "git");
}

void GitPanel::onMenuAction(GSimpleAction* a, GVariant*, gpointer selfp) {
    GitPanel* self = static_cast<GitPanel*>(selfp);
    const std::string name = g_action_get_name(G_ACTION(a));
    const int row = self->menuRow_;
    const GitUi::Row* r = self->fileRow(row);
    if (name == "openfile") {
        if (r && self->onOpenFile) self->onOpenFile(self->inRoot(self->topLevel_ + "/" + r->path));
    } else if (name == "openchanges") {
        self->showDiff(row);
    } else if (name == "togglestage") {
        self->toggleStage(row);
    } else if (name == "stage" || name == "unstage") {
        self->stageRows(row, name == "stage");
    } else if (name == "discard") {
        self->discardRow(row);
    } else if (name == "reveal") {
        if (r && self->onRevealFile) self->onRevealFile(self->inRoot(self->topLevel_ + "/" + r->path));
    } else if (name == "pull") {
        self->pull();
    } else if (name == "push") {
        self->push();
    } else if (name == "fetch") {
        self->fetch();
    } else if (name == "sync") {
        self->sync();
    } else if (name == "amend") {
        self->commitAmend();
    } else if (name == "undocommit") {
        self->undoLastCommit();
    } else if (name == "stageall") {
        self->stageAll();
    } else if (name == "unstageall") {
        self->unstageAll();
    } else if (name == "undodiscard") {
        self->undoDiscard();
    } else if (name == "stash") {
        self->stash();
    } else if (name == "popstash") {
        self->popStash();
    } else if (name == "switchbranch") {
        self->chooseBranch();
    }
}

void GitPanel::installActions(GActionMap* window) {
    windowActions_ = window;
    for (const char* name : kWindowActions) {
        GSimpleAction* a = g_simple_action_new(name, nullptr);
        g_signal_connect(a, "activate", G_CALLBACK(onWindowAction), this);
        g_action_map_add_action(window, G_ACTION(a));
        ownActions_.push_back(a);   // the reference is kept, to disconnect later
    }
    updateActions();
}

void GitPanel::onWindowAction(GSimpleAction* a, GVariant*, gpointer selfp) {
    GitPanel* self = static_cast<GitPanel*>(selfp);
    const std::string name = g_action_get_name(G_ACTION(a));
    if (name == "gitfetch") self->fetch();
    else if (name == "gitpull") self->pull();
    else if (name == "gitpush") self->push();
    else if (name == "gitsync") self->sync();
    else if (name == "gitstageall") self->stageAll();
    else if (name == "gitunstageall") self->unstageAll();
    else if (name == "gitclean") self->discardFromCommand();
    else if (name == "gitundodiscard") self->undoDiscard();
    else if (name == "gitcheckout") self->chooseBranch();
    else if (name == "gitamend") self->commitAmend();
    else if (name == "gitundocommit") self->undoLastCommit();
    else if (name == "gitstash") self->stash();
    else if (name == "gitstashpop") self->popStash();
}

void GitPanel::updateActions() {
    if (!windowActions_) return;
    auto set = [&](const char* name, bool on) {
        if (GAction* a = g_action_map_lookup_action(windowActions_, name))
            g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
    };
    set("gitundodiscard", !discards_.empty());
    const std::string file = fileOnShow ? fileOnShow() : std::string();
    set("gitclean", !file.empty() || (listHasKeyboard() && selectedRow() >= 0));
}

void GitPanel::enable(const char* action, bool on) {
    if (GAction* a = g_action_map_lookup_action(G_ACTION_MAP(menuActions_), action))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

// ------------------------------------------------------------------ menus
// The right click, part by part as the Mac's (GitPanel.mm): a file row's
// menu on a file (the row chosen first unless it is one of several chosen,
// which the menu then acts on, as in VS Code); Adjust Settings… alone
// elsewhere in the change list (a heading, the empty part) and on the
// graph, as the Mac's MCGitTable off a file row; the panel's own on the
// rest (the branch, the buttons, the graph's heading, the edges). The
// message box keeps its text's own menu (buildActions).
void GitPanel::onMenuPressed(GtkGestureClick* g, int, double x, double y, gpointer selfp) {
    GitPanel* self = static_cast<GitPanel*>(selfp);
    GtkWidget* hit = gtk_widget_pick(self->root_, x, y, GTK_PICK_DEFAULT);
    auto inside = [hit](GtkWidget* w) {
        return hit && (hit == w || gtk_widget_is_ancestor(hit, w));
    };
    if (inside(self->message_)) return;
    int row = -1;
    if (inside(self->list_)) {
        graphene_point_t in = GRAPHENE_POINT_INIT((float)x, (float)y), out;
        if (gtk_widget_compute_point(self->root_, self->list_, &in, &out))
            row = self->listRowAt(out.x, out.y);
    }
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    if (self->fileRow(row)) {
        if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(self->listSel_), (guint)row))
            self->selectRow(row, false);
        self->showRowMenu(row, x, y);
    } else if (inside(self->list_) || inside(self->graphList_)) {
        self->menuRow_ = -1;
        GMenu* menu = g_menu_new();
        self->popupMenu(menu, x, y);   // Adjust Settings… alone
        g_object_unref(menu);
    } else {
        self->showPanelMenu(x, y);
    }
}

void GitPanel::showRowMenu(int row, double x, double y) {
    const GitUi::Row* r = fileRow(row);
    if (!r) return;
    menuRow_ = row;
    const std::vector<GitUi::Row> rows = rowsActingWith(row);
    GMenu* menu = g_menu_new();
    std::string discardLabel, why;
    GStatBuf st;
    const bool exists = !topLevel_.empty() &&
                        g_lstat((topLevel_ + "/" + r->path).c_str(), &st) == 0;
    if (rows.size() > 1) {
        // Several chosen: what acts on all of them, then the clicked file's own.
        size_t changes = 0, staged = 0;
        for (const GitUi::Row& x : rows) (x.staged ? staged : changes)++;
        GMenu* all = g_menu_new();
        if (changes) append(all, "Stage Changes (" + std::to_string(changes) + " Files)", "stage");
        if (staged) append(all, "Unstage Changes (" + std::to_string(staged) + " Files)", "unstage");
        discardLabel = "Discard Changes (" + std::to_string(rows.size()) + " Files)…";
        append(all, discardLabel, "discard");
        why = GitUi::discardBatchPlan(rows).refusal;
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(all));
        g_object_unref(all);
        GMenu* own = g_menu_new();
        append(own, "Open File", "openfile");
        append(own, "Open Changes", "openchanges");
        append(own, "Reveal in File Tree", "reveal");
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(own));
        g_object_unref(own);
    } else {
        GMenu* own = g_menu_new();
        append(own, "Open File", "openfile");
        append(own, "Open Changes", "openchanges");
        append(own, r->staged ? "Unstage Changes" : "Stage Changes", "togglestage");
        if (!r->staged) {
            discardLabel = r->untracked ? "Discard (Move to Trash)…" : "Discard Changes…";
            append(own, discardLabel, "discard");
            why = GitUi::discardPlan(*r).refusal;
        }
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(own));
        g_object_unref(own);
        GMenu* tree = g_menu_new();
        append(tree, "Reveal in File Tree", "reveal");
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(tree));
        g_object_unref(tree);
    }
    enable("openfile", exists);
    enable("discard", why.empty());
    popupMenu(menu, x, y);
    g_object_unref(menu);
    if (!why.empty() && !discardLabel.empty() && menu_)
        setItemTip(menu_, discardLabel, GitUi::validUtf8(why));
}

// VS Code's "..." menu: Pull, Push, Fetch and Sync, then the changes as a
// whole, the last commit, the stash and the branch (its Commit, Changes and
// Stash submenus, flattened), as the Mac's.
void GitPanel::showPanelMenu(double x, double y) {
    menuRow_ = -1;
    GMenu* menu = g_menu_new();
    if (!inRepository_) {   // outside a repository, Adjust Settings… alone
        popupMenu(menu, x, y);
        g_object_unref(menu);
        return;
    }
    auto section = [&](std::initializer_list<std::pair<std::string, const char*>> items) {
        GMenu* s = g_menu_new();
        for (const auto& it : items) append(s, it.first, it.second);
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(s));
        g_object_unref(s);
    };
    section({{"Pull", "pull"}, {"Push", "push"}, {"Fetch", "fetch"}, {"Sync", "sync"}});
    section({{"Commit and Amend", "amend"}, {"Undo Last Commit", "undocommit"}});
    const std::string undo = discards_.empty() ? "Undo Discard" : undoDiscardTitle();
    section({{"Stage All Changes", "stageall"}, {"Unstage All Changes", "unstageall"},
             {undo, "undodiscard"}});
    section({{"Stash", "stash"}, {"Pop Stash", "popstash"}});
    section({{"Switch Branch…", "switchbranch"}});
    for (const char* op : {"pull", "push", "fetch", "sync"}) enable(op, !syncing_);
    enable("amend", inRepository_ && !initial_);
    enable("undocommit", inRepository_ && !initial_);
    enable("stash", inRepository_ && !initial_);
    enable("popstash", inRepository_);
    enable("undodiscard", !discards_.empty());
    popupMenu(menu, x, y);
    g_object_unref(menu);
}

// Every menu of the panel goes up here, under the pointer: a popover
// parented to the panel, so its items' actions ("gitpanel.") resolve
// through it. Adjust Settings… (PARITY.md 2.11) ends every one, as a last
// section of `menu`, and is all of a menu with nothing of its own, as the
// Mac's MCAdjustSettingsMenu.
void GitPanel::popupMenu(GMenu* menu, double x, double y) {
    dropMenu();
    Appearance::addItem(menu, "git");
    if (g_menu_model_get_n_items(G_MENU_MODEL(menu)) == 0) return;
    menu_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_has_arrow(GTK_POPOVER(menu_), FALSE);
    gtk_widget_set_halign(menu_, GTK_ALIGN_START);
    gtk_widget_set_parent(menu_, root_);
    GdkRectangle at = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(menu_), &at);
    gtk_popover_popup(GTK_POPOVER(menu_));
}

// The popover is the panel's child only by gtk_widget_set_parent, so it
// comes off by hand (the LSP popovers' trap, Lsp.cpp).
void GitPanel::dropMenu() {
    if (!menu_) return;
    GtkWidget* m = menu_;
    menu_ = nullptr;
    gtk_popover_popdown(GTK_POPOVER(m));
    if (gtk_widget_get_parent(m)) gtk_widget_unparent(m);
}
