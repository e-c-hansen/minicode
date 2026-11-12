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
    for (Terminal* t : terms_) {
        t->onTitleChanged = nullptr;
        delete t;
    }
    terms_.clear();
    delete strip_;
    strip_ = nullptr;
}

int TerminalDock::indexOf(const Terminal* t) const {
    for (size_t i = 0; i < terms_.size(); i++)
        if (terms_[i] == t) return (int)i;
    return -1;
}

void TerminalDock::add(Terminal* t, int at) {
    terms_.insert(terms_.begin() + at, t);
    gtk_stack_add_child(GTK_STACK(stack_), t->widget());
    t->onTitleChanged = [this] { refresh(); };
}

void TerminalDock::refresh() {
    if (closingAll_ || !strip_) return;
    std::vector<TabStrip::Item> items;
    for (Terminal* t : terms_) {
        TabStrip::Item it;
        it.title = t->title();
        it.tooltip = it.title;
        items.push_back(it);
    }
    strip_->setItems(items, active_);
}

void TerminalDock::activate(int i) {
    active_ = i;
    gtk_stack_set_visible_child(GTK_STACK(stack_), terms_[(size_t)i]->widget());
    refresh();
    if (onActiveChanged) onActiveChanged(terms_[(size_t)i]);
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
    if (focus) terms_[(size_t)i]->focus();
}

void TerminalDock::next() { if (count() > 1) select((active_ + 1) % count()); }
void TerminalDock::previous() { if (count() > 1) select((active_ + count() - 1) % count()); }

void TerminalDock::close(int i, bool ask) {
    if (i < 0 || i >= count()) return;
    Terminal* t = terms_[(size_t)i];
    if (ask && confirmClose && t->busy()) {
        auto alive = std::weak_ptr<int>(alive_);
        confirmClose(t, [this, alive, t] {
            if (alive.expired()) return;
            const int now = indexOf(t);
            if (now >= 0) remove(now);
        });
        return;
    }
    remove(i);
}

void TerminalDock::remove(int i) {
    Terminal* t = terms_[(size_t)i];
    const bool last = count() == 1;
    // The board's mark and id belong to the window's terminals together:
    // a closing tab only lets go of its own.
    if (!last) t->setControlId(0);
    const int was = active_;
    terms_.erase(terms_.begin() + i);
    t->onTitleChanged = nullptr;
    // The stack held the widget's last reference; the terminal's destructor
    // still disconnects from its widgets and the draw function, so keep them
    // alive until it has run (they were freed first: heap corruption that
    // surfaced as a crash suites later).
    GtkWidget* w = GTK_WIDGET(g_object_ref(t->widget()));
    gtk_stack_remove(GTK_STACK(stack_), w);   // before the shell is hung up
    delete t;
    g_object_unref(w);
    if (last) {
        // A fresh terminal whose shell starts when the dock is shown again.
        // Hidden first, so the new one is never mapped (and starts no shell).
        if (onEmpty) onEmpty();
        add(make_(), 0);
        activate(0);
        if (onChanged) onChanged();
        return;
    }
    int to = was;
    if (i < was) to = was - 1;
    else if (i == was) to = std::max(0, i - 1);
    activate(std::min(to, count() - 1));
    if (onChanged) onChanged();
    terms_[(size_t)active_]->focus();
}

void TerminalDock::rename(int i, const std::string& title) {
    if (i < 0 || i >= count()) return;
    terms_[(size_t)i]->setUserTitle(title);   // refreshes through onTitleChanged
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
    gtk_editable_set_text(GTK_EDITABLE(entry), terms_[(size_t)i]->userTitle().c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Terminal name");
    gtk_popover_set_child(GTK_POPOVER(rename_), entry);
    Terminal* t = terms_[(size_t)i];
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
    Terminal* t = terms_[(size_t)from];
    terms_.erase(terms_.begin() + from);
    terms_.insert(terms_.begin() + to, t);
    active_ = indexOf(on);
    refresh();
    if (onChanged) onChanged();
}

void TerminalDock::moveActive(int by) { move(active_, active_ + by); }

#endif
