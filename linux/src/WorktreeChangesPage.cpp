// WorktreeChangesPage.cpp — see WorktreeChangesPage.h.
#include "WorktreeChangesPage.h"

WorktreeChangesPage::WorktreeChangesPage(AgentWorktrees* model, const WorktreeInfo& worktree)
    : model_(model), wt_(worktree) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    g_object_ref_sink(root_);
    gtk_widget_set_margin_start(root_, 12);
    gtk_widget_set_margin_end(root_, 12);
    gtk_widget_set_margin_top(root_, 10);
    gtk_widget_set_margin_bottom(root_, 10);
    title_ = gtk_label_new((wt_.label + " compared with " + model_->mainBranch()).c_str());
    gtk_widget_set_halign(title_, GTK_ALIGN_START);
    gtk_widget_add_css_class(title_, "title-3");
    info_ = gtk_label_new("");
    gtk_widget_set_halign(info_, GTK_ALIGN_START);
    gtk_widget_add_css_class(info_, "dim-label");
    notice_ = gtk_label_new("Reading the changes…");
    gtk_widget_set_halign(notice_, GTK_ALIGN_START);
    gtk_widget_add_css_class(notice_, "dim-label");
    scroll_ = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll_, TRUE);
    list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list_), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), list_);
    gtk_box_append(GTK_BOX(root_), title_);
    gtk_box_append(GTK_BOX(root_), info_);
    gtk_box_append(GTK_BOX(root_), notice_);
    gtk_box_append(GTK_BOX(root_), scroll_);
    g_signal_connect(list_, "row-activated",
                     G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* r, gpointer p) {
                         static_cast<WorktreeChangesPage*>(p)->openChangeAtRow(
                             gtk_list_box_row_get_index(r));
                     }),
                     this);
    std::string line = wt_.path;
    if (!wt_.branch.empty()) line += "  ·  " + wt_.branch;
    if (!wt_.badge().empty()) line += "  ·  " + wt_.badge();
    gtk_label_set_text(GTK_LABEL(info_), line.c_str());
    auto alive = alive_;
    model_->changes(wt_, [this, alive](std::vector<AgentView::Change> c, std::string err) {
        if (*alive) show(std::move(c), err);
    });
}

WorktreeChangesPage::~WorktreeChangesPage() {
    *alive_ = false;
    g_object_unref(root_);
}

void WorktreeChangesPage::show(std::vector<AgentView::Change> changes, const std::string& error) {
    changes_ = std::move(changes);
    loaded_ = true;
    while (GtkWidget* c = gtk_widget_get_first_child(list_)) gtk_list_box_remove(GTK_LIST_BOX(list_), c);
    for (const AgentView::Change& c : changes_) {
        char* mk = g_markup_printf_escaped("<tt><b>%c</b></tt>   %s", c.status, c.path.c_str());
        std::string m = mk;
        g_free(mk);
        if (!c.from.empty()) {
            char* f = g_markup_printf_escaped("  <small>from %s</small>", c.from.c_str());
            m += f;
            g_free(f);
        }
        GtkWidget* l = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(l), m.c_str());
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
        gtk_list_box_append(GTK_LIST_BOX(list_), l);
    }
    std::string notice = !error.empty() ? error : changes_.empty() ? "No changes against the main branch." : "";
    gtk_label_set_text(GTK_LABEL(notice_), notice.c_str());
    gtk_widget_set_visible(notice_, !notice.empty());
    std::string info = gtk_label_get_text(GTK_LABEL(info_));
    const size_t n = changes_.size();
    gtk_label_set_text(GTK_LABEL(info_),
                       (info + "  ·  " + std::to_string(n) + " changed " + (n == 1 ? "file" : "files")).c_str());
    if (onLoaded) onLoaded();
}

std::vector<std::string> WorktreeChangesPage::rowDescriptions() const {
    std::vector<std::string> out;
    for (const AgentView::Change& c : changes_) out.push_back(std::string(1, c.status) + " " + c.path);
    return out;
}

std::string WorktreeChangesPage::noticeText() const {
    return gtk_widget_get_visible(notice_) ? gtk_label_get_text(GTK_LABEL(notice_)) : "";
}

void WorktreeChangesPage::openChangeAtRow(int row) {
    if (row < 0 || row >= (int)changes_.size()) return;
    auto alive = alive_;
    model_->comparison(wt_, changes_[(size_t)row], [this, alive](CompareSpec spec) {
        if (*alive && onShowComparison) onShowComparison(spec);
    });
}
