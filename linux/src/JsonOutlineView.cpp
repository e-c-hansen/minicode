// JsonOutlineView.cpp — see JsonOutlineView.h.
#include "JsonOutlineView.h"

#include "Utf8Offsets.h"

#include <algorithm>

namespace {

constexpr guint kDebounceMs = 300;
constexpr size_t kMaxNodes = 200000;

std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}

long nodeOf(gpointer item) {
    return item ? (long)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(item), "node")) - 1 : -1;
}

GObject* itemFor(long node) {
    GObject* o = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
    g_object_set_data(o, "node", GSIZE_TO_POINTER((gsize)node + 1));
    return o;
}

}  // namespace

JsonOutlineView::JsonOutlineView() {
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box_, "minicode-json-outline");
    gtk_widget_set_size_request(box_, 240, -1);
    errorLabel_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(errorLabel_), 0);
    gtk_label_set_wrap(GTK_LABEL(errorLabel_), TRUE);
    gtk_widget_add_css_class(errorLabel_, "error");
    gtk_widget_set_margin_start(errorLabel_, 6);
    gtk_widget_set_margin_end(errorLabel_, 6);
    gtk_widget_set_visible(errorLabel_, FALSE);
    gtk_box_append(GTK_BOX(box_), errorLabel_);

    list_ = gtk_list_view_new(nullptr, nullptr);
    gtk_widget_add_css_class(list_, "minicode-json-list");
    GtkListItemFactory* f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(setupRow), this);
    g_signal_connect(f, "bind", G_CALLBACK(bindRow), this);
    gtk_list_view_set_factory(GTK_LIST_VIEW(list_), f);
    g_object_unref(f);
    g_signal_connect(list_, "activate", G_CALLBACK(activated), this);
    GtkWidget* sc = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), list_);
    gtk_box_append(GTK_BOX(box_), sc);
    g_object_ref_sink(box_);
}

JsonOutlineView::~JsonOutlineView() {
    if (timer_) g_source_remove(timer_);
    if (selection_) g_signal_handlers_disconnect_by_data(selection_, this);
    g_object_unref(box_);
}

void JsonOutlineView::setupRow(GtkListItemFactory*, GtkListItem* li, gpointer) {
    GtkWidget* ex = gtk_tree_expander_new();
    GtkWidget* l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(ex), l);
    gtk_list_item_set_child(li, ex);
}

void JsonOutlineView::bindRow(GtkListItemFactory*, GtkListItem* li, gpointer p) {
    JsonOutlineView* self = static_cast<JsonOutlineView*>(p);
    GtkTreeListRow* row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(li));
    GtkWidget* ex = gtk_list_item_get_child(li);
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(ex), row);
    gpointer item = gtk_tree_list_row_get_item(row);
    const long n = nodeOf(item);
    if (item) g_object_unref(item);
    GtkWidget* l = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(ex));
    if (n < 0 || (size_t)n >= self->outline_.nodes.size()) {
        gtk_label_set_text(GTK_LABEL(l), "");
        return;
    }
    const JsonOutline::Node& nd = self->outline_.nodes[n];
    char* a = g_markup_escape_text(oneLine(JsonOutline::label(nd)).c_str(), -1);
    char* b = g_markup_escape_text(oneLine(JsonOutline::summary(nd)).c_str(), -1);
    char* m = g_strdup_printf("<b>%s</b>  <span alpha=\"65%%\">%s</span>", a, b);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(a);
    g_free(b);
    g_free(m);
}

GListModel* JsonOutlineView::childrenOf(gpointer item, gpointer p) {
    JsonOutlineView* self = static_cast<JsonOutlineView*>(p);
    const long n = nodeOf(item);
    if (n < 0 || (size_t)n >= self->outline_.nodes.size()) return nullptr;
    const JsonOutline::Node& nd = self->outline_.nodes[n];
    if (nd.children.empty()) return nullptr;
    GListStore* st = g_list_store_new(G_TYPE_OBJECT);
    for (size_t c : nd.children) {
        GObject* o = itemFor((long)c);
        g_list_store_append(st, o);
        g_object_unref(o);
    }
    return G_LIST_MODEL(st);
}

void JsonOutlineView::chosen(GObject*, GParamSpec*, gpointer p) {
    JsonOutlineView* self = static_cast<JsonOutlineView*>(p);
    if (self->quiet_) return;
    self->emitFor(gtk_single_selection_get_selected(self->selection_));
}

void JsonOutlineView::activated(GtkListView*, guint pos, gpointer p) {
    static_cast<JsonOutlineView*>(p)->emitFor(pos);
}

void JsonOutlineView::emitFor(guint position) {
    if (stale_ || !error_.empty() || position == GTK_INVALID_LIST_POSITION || !tree_) return;
    GtkTreeListRow* row = gtk_tree_list_model_get_row(tree_, position);
    if (!row) return;
    gpointer item = gtk_tree_list_row_get_item(row);
    const long n = nodeOf(item);
    if (item) g_object_unref(item);
    g_object_unref(row);
    long s, e;
    if (charRange(n, &s, &e) && onSelect) onSelect(s, e);
}

bool JsonOutlineView::charRange(long node, long* start, long* end) const {
    if (node < 0 || (size_t)node >= outline_.nodes.size()) return false;
    const JsonOutline::Node& n = outline_.nodes[node];
    const size_t s16 = n.isMember ? n.memberStart16 : n.start16;
    *start = utf16::toCharOffset(text16_, s16);
    *end = utf16::toCharOffset(text16_, n.end16);
    return true;
}

std::string JsonOutlineView::rowText(long node) const {
    if (node < 0 || (size_t)node >= outline_.nodes.size()) return "";
    const JsonOutline::Node& n = outline_.nodes[node];
    return oneLine(JsonOutline::label(n)) + "  " + oneLine(JsonOutline::summary(n));
}

void JsonOutlineView::rebuild() {
    quiet_ = true;
    if (selection_) g_signal_handlers_disconnect_by_data(selection_, this);
    gtk_list_view_set_model(GTK_LIST_VIEW(list_), nullptr);
    tree_ = nullptr;
    selection_ = nullptr;
    if (!outline_.nodes.empty()) {
        GListStore* root = g_list_store_new(G_TYPE_OBJECT);
        GObject* o = itemFor(0);
        g_list_store_append(root, o);
        g_object_unref(o);
        tree_ = gtk_tree_list_model_new(G_LIST_MODEL(root), FALSE, FALSE, childrenOf, this,
                                        nullptr);
        selection_ = gtk_single_selection_new(G_LIST_MODEL(tree_));
        gtk_single_selection_set_autoselect(selection_, FALSE);
        gtk_single_selection_set_can_unselect(selection_, TRUE);
        gtk_single_selection_set_selected(selection_, GTK_INVALID_LIST_POSITION);
        g_signal_connect(selection_, "notify::selected", G_CALLBACK(chosen), this);
        gtk_list_view_set_model(GTK_LIST_VIEW(list_), GTK_SELECTION_MODEL(selection_));
        g_object_unref(selection_);   // the view holds it
        if (GtkTreeListRow* r = gtk_tree_list_model_get_row(tree_, 0)) {
            gtk_tree_list_row_set_expanded(r, TRUE);
            g_object_unref(r);
        }
    }
    quiet_ = false;
}

void JsonOutlineView::show(const std::string& text, bool jsonc) {
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    jsonc_ = jsonc;
    text_ = text;
    text16_ = utf16::fromUtf8(text);
    outline_ = JsonOutline::parse(text, kMaxNodes, jsonc);
    stale_ = false;
    error_.clear();
    if (!outline_.ok && !outline_.error.empty())
        error_ = "Line " + std::to_string(outline_.errorLine) + ", column " +
                 std::to_string(outline_.errorColumn) + ": " + outline_.error;
    gtk_label_set_text(GTK_LABEL(errorLabel_), error_.c_str());
    gtk_widget_set_visible(errorLabel_, !error_.empty());
    gtk_widget_set_opacity(list_, error_.empty() ? 1.0 : 0.5);
    rebuild();
}

void JsonOutlineView::parseNow() {
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    if (!getText) return;
    const std::string text = getText();
    JsonOutline::Outline o = JsonOutline::parse(text, kMaxNodes, jsonc_);
    if (o.ok || outline_.nodes.empty()) {
        show(text, jsonc_);
        if (onParsed) onParsed();
        return;
    }
    // Not valid now: the last good outline stays, dimmed, selecting nothing.
    error_ = "Line " + std::to_string(o.errorLine) + ", column " + std::to_string(o.errorColumn) +
             ": " + o.error;
    stale_ = true;
    gtk_label_set_text(GTK_LABEL(errorLabel_), error_.c_str());
    gtk_widget_set_visible(errorLabel_, TRUE);
    gtk_widget_set_opacity(list_, 0.5);
}

void JsonOutlineView::schedule() {
    stale_ = true;
    if (timer_) g_source_remove(timer_);
    timer_ = g_timeout_add(kDebounceMs, +[](gpointer p) -> gboolean {
        JsonOutlineView* self = static_cast<JsonOutlineView*>(p);
        self->timer_ = 0;
        self->parseNow();
        return G_SOURCE_REMOVE;
    }, this);
}

void JsonOutlineView::flush() {
    if (timer_) parseNow();
}

void JsonOutlineView::clear() {
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    outline_ = JsonOutline::Outline();
    text_.clear();
    text16_.clear();
    error_.clear();
    stale_ = false;
    gtk_widget_set_visible(errorLabel_, FALSE);
    gtk_widget_set_opacity(list_, 1.0);
    rebuild();
}

bool JsonOutlineView::dimmed() const { return gtk_widget_get_opacity(list_) < 0.9; }

long JsonOutlineView::selectedNode() const {
    if (!selection_ || !tree_) return -1;
    const guint pos = gtk_single_selection_get_selected(selection_);
    if (pos == GTK_INVALID_LIST_POSITION) return -1;
    GtkTreeListRow* row = gtk_tree_list_model_get_row(tree_, pos);
    if (!row) return -1;
    gpointer item = gtk_tree_list_row_get_item(row);
    const long n = nodeOf(item);
    if (item) g_object_unref(item);
    g_object_unref(row);
    return n;
}

// The row of a node, its ancestors opened on the way.
long JsonOutlineView::positionOf(long node) {
    if (!tree_ || node < 0 || (size_t)node >= outline_.nodes.size()) return -1;
    std::vector<long> path;
    for (long n = node; n >= 0; n = outline_.nodes[n].parent) path.insert(path.begin(), n);
    long pos = 0;
    for (size_t i = 1; i < path.size(); ++i) {
        GtkTreeListRow* cur = gtk_tree_list_model_get_row(tree_, (guint)pos);
        if (!cur) return -1;
        gtk_tree_list_row_set_expanded(cur, TRUE);
        const guint depth = gtk_tree_list_row_get_depth(cur);
        g_object_unref(cur);
        const guint total = g_list_model_get_n_items(G_LIST_MODEL(tree_));
        long found = -1;
        for (guint p = (guint)pos + 1; p < total; ++p) {
            GtkTreeListRow* r = gtk_tree_list_model_get_row(tree_, p);
            if (!r) break;
            const guint d = gtk_tree_list_row_get_depth(r);
            gpointer item = gtk_tree_list_row_get_item(r);
            const long n = nodeOf(item);
            if (item) g_object_unref(item);
            g_object_unref(r);
            if (d <= depth) break;
            if (d == depth + 1 && n == path[i]) { found = p; break; }
        }
        if (found < 0) return -1;
        pos = found;
    }
    return pos;
}

bool JsonOutlineView::selectNode(long node, bool notify) {
    const long pos = positionOf(node);
    if (pos < 0) return false;
    quiet_ = !notify;
    gtk_single_selection_set_selected(selection_, (guint)pos);
    quiet_ = false;
    gtk_list_view_scroll_to(GTK_LIST_VIEW(list_), (guint)pos, GTK_LIST_SCROLL_NONE, nullptr);
    return true;
}

void JsonOutlineView::selectAtChar(long chars) {
    if (stale_ || !error_.empty() || outline_.nodes.empty()) return;
    const long n = JsonOutline::nodeAt16(outline_, utf16::fromCharOffset(text16_, chars));
    if (n >= 0 && n != selectedNode()) selectNode(n, false);
}
