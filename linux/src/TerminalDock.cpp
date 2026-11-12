// TerminalDock.cpp — see TerminalDock.h.
#include "TerminalDock.h"

#ifdef MINICODE_ENABLE_TERMINAL

#include <algorithm>

#include "TabStrip.h"

TerminalDock::TerminalDock(std::function<Terminal*()> make) : make_(std::move(make)) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    strip_ = new TabStrip();
    plus_ = gtk_button_new_with_label("+");
    gtk_widget_set_tooltip_text(plus_, "New Terminal");
    gtk_widget_add_css_class(plus_, "flat");
    gtk_widget_set_focus_on_click(plus_, FALSE);
    gtk_widget_set_hexpand(strip_->widget(), TRUE);
    gtk_box_append(GTK_BOX(row), strip_->widget());
    gtk_box_append(GTK_BOX(row), plus_);
    stack_ = gtk_stack_new();
    gtk_widget_set_vexpand(stack_, TRUE);
    gtk_box_append(GTK_BOX(root_), row);
    gtk_box_append(GTK_BOX(root_), stack_);
    g_signal_connect(plus_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        static_cast<TerminalDock*>(p)->newTerminal();
    }), this);
    strip_->onSelect = [this](int i) { select(i); };
    strip_->onClose = [this](int i) { close(i); };
    strip_->onMove = [this](int from, int to) { move(from, to); };
    strip_->menuFor = nullptr;
    add(make_(), 0);
    activate(0);
}

TerminalDock::~TerminalDock() {
    closingAll_ = true;
    if (rename_) gtk_widget_unparent(rename_);
    for (auto& g : groups_)
        for (Terminal* t : g->panes()) {
            t->onTitleChanged = nullptr;
            t->onFocusIn = nullptr;
        }
    groups_.clear();   // each group deletes its terminals
    delete strip_;
    strip_ = nullptr;
}

int TerminalDock::indexOf(const Terminal* t) const {
    for (size_t i = 0; i < groups_.size(); i++)
        if (groups_[i]->contains(t)) return (int)i;
    return -1;
}

std::vector<Terminal*> TerminalDock::all() const {
    std::vector<Terminal*> out;
    for (const auto& g : groups_) {
        const auto p = g->panes();
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

void TerminalDock::hook(Terminal* t) {
    t->onTitleChanged = [this] { refresh(); };
    t->onFocusIn = [this, t] {
        if (closingAll_) return;
        const int i = indexOf(t);
        if (i < 0 || i != active_) return;   // a hidden tab's pane gets no keyboard
        if (groups_[(size_t)i]->active() == t) return;
        groups_[(size_t)i]->setActive(t);
        refresh();
        if (onActiveChanged) onActiveChanged(t);
        if (onChanged) onChanged();
    };
}

void TerminalDock::add(Terminal* t, int at) {
    groups_.insert(groups_.begin() + at, std::make_unique<TerminalGroup>(t));
    gtk_stack_add_child(GTK_STACK(stack_), groups_[(size_t)at]->widget());
    hook(t);
}

void TerminalDock::refresh() {
    if (closingAll_ || !strip_) return;
    std::vector<TabStrip::Item> items;
    for (const auto& g : groups_) {
        TabStrip::Item it;
        it.title = g->active()->title();
        it.tooltip = it.title;
        items.push_back(it);
    }
    strip_->setItems(items, active_);
}

void TerminalDock::activate(int i) {
    active_ = i;
    gtk_stack_set_visible_child(GTK_STACK(stack_), groups_[(size_t)i]->widget());
    refresh();
    if (onActiveChanged) onActiveChanged(groups_[(size_t)i]->active());
}

Terminal* TerminalDock::newTerminal(const std::string& title) {
    Terminal* t = make_();
    if (!title.empty()) t->setUserTitle(title);
    add(t, active_ + 1);
    activate(active_ + 1);
    if (onChanged) onChanged();
    return t;
}

void TerminalDock::select(int i, bool focus) {
    if (i < 0 || i >= count()) return;
    if (i != active_) {
        activate(i);
        if (onChanged) onChanged();
    }
    if (focus) groups_[(size_t)i]->active()->focus();
}

// Next and Previous walk every pane, tab by tab, wrapping.
void TerminalDock::step(int by) {
    const std::vector<Terminal*> v = all();
    if (v.size() < 2) return;
    size_t at = 0;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == active()) at = i;
    focusPane(v[(at + v.size() + (size_t)(by < 0 ? v.size() - 1 : 1)) % v.size()]);
}
void TerminalDock::next() { step(1); }
void TerminalDock::previous() { step(-1); }

void TerminalDock::focusPane(Terminal* t) {
    const int i = indexOf(t);
    if (i < 0) return;
    groups_[(size_t)i]->setActive(t);
    if (i != active_) {
        activate(i);
        if (onChanged) onChanged();
    } else if (onActiveChanged && active() == t) {
        onActiveChanged(t);
    }
    t->focus();
}

// A whole tab. A program running in any pane asks first (the first such pane).
void TerminalDock::close(int i, bool ask) {
    if (i < 0 || i >= count()) return;
    if (ask && confirmClose) {
        for (Terminal* t : groups_[(size_t)i]->panes()) {
            if (!t->busy()) continue;
            auto alive = std::weak_ptr<int>(alive_);
            confirmClose(t, [this, alive, t] {
                if (alive.expired()) return;
                const int now = indexOf(t);
                if (now >= 0) remove(now);
            });
            return;
        }
    }
    remove(i);
}

void TerminalDock::closePane(Terminal* t, bool ask) {
    const int i = indexOf(t);
    if (i < 0) return;
    if (groups_[(size_t)i]->count() == 1) {
        close(i, ask);
        return;
    }
    if (ask && confirmClose && t->busy()) {
        auto alive = std::weak_ptr<int>(alive_);
        confirmClose(t, [this, alive, t] {
            if (alive.expired()) return;
            if (indexOf(t) >= 0) removePane(t);
        });
        return;
    }
    removePane(t);
}

// One pane of a tab that has others: its share goes to a neighbour, which
// has the keyboard (when the pane had it).
void TerminalDock::removePane(Terminal* t) {
    const int i = indexOf(t);
    if (i < 0) return;
    TerminalGroup* g = groups_[(size_t)i].get();
    const bool hadKeyboard = i == active_ && g->active() == t;
    t->setControlId(0);
    t->onTitleChanged = nullptr;
    t->onFocusIn = nullptr;
    Terminal* next = nullptr;
    GtkWidget* w = GTK_WIDGET(g_object_ref(t->widget()));
    next = g->take(t);
    if (hadKeyboard && next && onActiveChanged) onActiveChanged(next);
    delete t;
    g_object_unref(w);
    refresh();
    if (onChanged) onChanged();
    if (hadKeyboard && next) next->focus();
}

void TerminalDock::remove(int i) {
    TerminalGroup* g = groups_[(size_t)i].get();
    const bool last = count() == 1;
    // The board's mark and id belong to the window's terminals together:
    // a closing tab only lets go of its own.
    for (Terminal* t : g->panes()) {
        if (!last) t->setControlId(0);
        t->onTitleChanged = nullptr;
        t->onFocusIn = nullptr;
    }
    const int was = active_;
    // The last tab leaves a fresh terminal whose shell starts when the dock
    // is shown again; it joins the stack before the old one goes.
    Terminal* fresh = last ? make_() : nullptr;
    if (fresh) add(fresh, 1);
    std::unique_ptr<TerminalGroup> gone = std::move(groups_[(size_t)i]);
    groups_.erase(groups_.begin() + i);
    // app->terminal must never name a terminal being deleted: removing the
    // widget moves the focus, and the focus handlers ask app->terminal (an
    // ASan use-after-free in Terminal::owns, TerminalTabsSuite).
    if (i == was && onActiveChanged) {
        const int near = fresh ? 0 : std::max(0, std::min(i, count() - 1));
        onActiveChanged(groups_[(size_t)near]->active());
    }
    // Hidden before the old widget leaves, so the new one is never mapped.
    if (last && onEmpty) onEmpty();
    // The stack held the widget's last reference; the terminals' destructors
    // still disconnect from their widgets and the draw function, so keep
    // the group's widget (and theirs) alive until they have run.
    GtkWidget* w = GTK_WIDGET(g_object_ref(gone->widget()));
    gtk_stack_remove(GTK_STACK(stack_), w);   // before the shells are hung up
    gone.reset();
    g_object_unref(w);
    if (last) {
        activate(0);
        if (onChanged) onChanged();
        return;
    }
    int to = was;
    if (i < was) to = was - 1;
    else if (i == was) to = std::max(0, i - 1);
    activate(std::min(to, count() - 1));
    if (onChanged) onChanged();
    groups_[(size_t)active_]->active()->focus();
}

Terminal* TerminalDock::split(bool down, bool focus) {
    if (groups_.empty()) return nullptr;
    TerminalGroup* g = groups_[(size_t)active_].get();
    Terminal* of = g->active();
    if (g->plan(of, down) == TerminalLayout::Split::Refused) {
        GdkDisplay* d = gdk_display_get_default();
        if (d) gdk_display_beep(d);
        return nullptr;
    }
    Terminal* fresh = make_();
    g->split(of, fresh, down);
    hook(fresh);
    g->setActive(fresh);
    refresh();
    if (onActiveChanged) onActiveChanged(fresh);
    if (onChanged) onChanged();
    if (focus) fresh->focus();
    return fresh;
}

bool TerminalDock::resizeActive(TerminalGroup::Dir d) {
    if (groups_.empty()) return false;
    TerminalGroup* g = groups_[(size_t)active_].get();
    return g->resize(g->active(), d, 4);
}

bool TerminalDock::activeAloneInColumn() const {
    if (groups_.empty()) return false;
    const TerminalGroup* g = groups_[(size_t)active_].get();
    return g->aloneInColumn(g->active());
}

void TerminalDock::rename(int i, const std::string& title) {
    if (i < 0 || i >= count()) return;
    groups_[(size_t)i]->active()->setUserTitle(title);   // refreshes through onTitleChanged
    if (onChanged) onChanged();
}

void TerminalDock::askRename(int i) {
    if (i < 0 || i >= count()) return;
    if (rename_) {
        gtk_widget_unparent(rename_);
        rename_ = nullptr;
    }
    GtkWidget* anchor = strip_->tabWidget(i) ? strip_->tabWidget(i) : strip_->widget();
    rename_ = gtk_popover_new();
    gtk_widget_set_parent(rename_, anchor);
    GtkWidget* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), groups_[(size_t)i]->active()->userTitle().c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Terminal name");
    gtk_popover_set_child(GTK_POPOVER(rename_), entry);
    Terminal* t = groups_[(size_t)i]->active();
    auto alive = std::weak_ptr<int>(alive_);
    struct Ctx { TerminalDock* d; Terminal* t; std::weak_ptr<int> alive; };
    Ctx* ctx = new Ctx{this, t, alive};
    g_object_set_data_full(G_OBJECT(entry), "ctx", ctx, +[](gpointer p) { delete static_cast<Ctx*>(p); });
    g_signal_connect(entry, "activate", G_CALLBACK(+[](GtkEntry* e, gpointer p) {
        Ctx* c = static_cast<Ctx*>(p);
        if (c->alive.expired()) return;
        const char* text = gtk_editable_get_text(GTK_EDITABLE(e));
        const int now = c->d->indexOf(c->t);
        if (now >= 0) c->d->rename(now, text ? text : "");
        gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(e), GTK_TYPE_POPOVER)));
        if (now >= 0) c->t->focus();
    }), ctx);
    gtk_popover_popup(GTK_POPOVER(rename_));
    gtk_widget_grab_focus(entry);
}

void TerminalDock::move(int from, int to) {
    const int n = count();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    Terminal* on = active();
    auto g = std::move(groups_[(size_t)from]);
    groups_.erase(groups_.begin() + from);
    groups_.insert(groups_.begin() + to, std::move(g));
    active_ = indexOf(on);
    refresh();
    if (onChanged) onChanged();
}

void TerminalDock::moveActive(int by) { move(active_, active_ + by); }

#endif
