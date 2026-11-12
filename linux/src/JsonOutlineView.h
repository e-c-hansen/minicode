// JsonOutlineView.h — a JSON file's outline beside its text, the GTK side of
// the Mac's JsonOutlineView over the core's JsonOutline.
//
// A GtkListView over a GtkTreeListModel: one row a node, "name  value" with
// a container's size. Choosing a row (click, arrows, Enter) calls onSelect
// with the node's characters in the text, which the editor selects without
// taking the keyboard from the outline; selectAtChar() does the reverse for
// the caret. The text is parsed again 0.3 s after schedule() was last called;
// when it no longer parses, the last good outline stays, dimmed under a line
// naming the error, and selects nothing, since its offsets are stale (as
// they are in the 0.3 s before the new parse is in).
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <string>

#include "JsonOutline.h"

class JsonOutlineView {
public:
    JsonOutlineView();
    ~JsonOutlineView();
    JsonOutlineView(const JsonOutlineView&) = delete;
    JsonOutlineView& operator=(const JsonOutlineView&) = delete;

    GtkWidget* widget() const { return box_; }

    // Parses `text` now and shows it. `jsonc` allows comments.
    void show(const std::string& text, bool jsonc);
    // The text changed: parse again once nothing has changed for 0.3 s.
    void schedule();
    // Parse now, if one is waiting.
    void flush();
    void clear();

    // The deepest node holding the caret at this character of the text is
    // chosen (nothing happens while the outline is stale).
    void selectAtChar(long chars);
    // Chooses a node as a click would (calls onSelect), or only marks it.
    bool selectNode(long node, bool notify);
    long selectedNode() const;

    bool stale() const { return stale_; }
    bool hasError() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    bool dimmed() const;
    size_t nodeCount() const { return outline_.nodes.size(); }
    const JsonOutline::Outline& outline() const { return outline_; }
    std::string rowText(long node) const;   // "name  value", plain
    // The node's range, in characters of the text parsed.
    bool charRange(long node, long* start, long* end) const;

    // The text to parse again (the editor's buffer).
    std::function<std::string()> getText;
    // A row chosen: the characters [start, end) of the text.
    std::function<void(long start, long end)> onSelect;
    // A new parse is in (the editor finds the caret's node again).
    std::function<void()> onParsed;

private:
    void rebuild();
    void parseNow();
    long positionOf(long node);
    static GListModel* childrenOf(gpointer item, gpointer self);
    static void setupRow(GtkListItemFactory*, GtkListItem*, gpointer);
    static void bindRow(GtkListItemFactory*, GtkListItem*, gpointer);
    static void chosen(GObject*, GParamSpec*, gpointer);
    static void activated(GtkListView*, guint, gpointer);
    void emitFor(guint position);

    GtkWidget* box_ = nullptr;
    GtkWidget* errorLabel_ = nullptr;
    GtkWidget* list_ = nullptr;
    GtkTreeListModel* tree_ = nullptr;
    GtkSingleSelection* selection_ = nullptr;
    JsonOutline::Outline outline_;
    std::string text_;           // as parsed (UTF-8)
    std::u16string text16_;      // the same in UTF-16
    bool jsonc_ = false;
    bool stale_ = false;         // the buffer moved on from what was parsed
    bool quiet_ = false;         // selection changed by us: say nothing
    std::string error_;
    guint timer_ = 0;
};
