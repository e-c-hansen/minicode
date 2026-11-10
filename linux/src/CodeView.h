// CodeView.h — the editor's GtkTextView, linux/PARITY.md item 1.4 (D4): a
// subclass that does the editor's own drawing in snapshot_layer, and holds
// the line-number gutter (Gutter.h) in its left border window. The Mac's
// counterpart is CodeTextView (src/CodeTextView.mm), whose
// drawViewBackgroundInRect: and drawRect: do the same jobs.
//
// Drawing passes: what the editor draws itself goes in a pass, a function
// given a GtkSnapshot in buffer coordinates (GTK translates it) and the
// visible rect in the same coordinates, run every time the view draws, in
// one of two layers:
//   - BelowText: over the text's background, under the text and the
//     selection (the ruler, at order 0; bracket boxes, item 1.7);
//   - AboveText: over the text (the carets of item 1.9, inlay labels of
//     item 2.7, fold placeholders of item 1.10).
// Passes run in ascending order within a layer; addPass returns an id for
// removePass. A pass draws only what is in the visible rect, reads what it
// needs from its own state (never by laying out the whole buffer), and asks
// for a redraw with queueDraw when that state changes. Nothing a pass draws
// is in the buffer (D6).
//
// The ruler (editor.ruler, editor.ruler-color) is the first pass: a 1 px
// line at each column, where that column's character starts (the text's
// left edge plus the width of that many '0's in the view's font), drawn
// only while the view is editable, as on the Mac.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <vector>

class Gutter;
class Settings;

class CodeView {
public:
    // A new view showing `buffer`. The view owns the CodeView, which goes
    // when the view is disposed.
    static CodeView* create(GtkTextBuffer* buffer);
    // The CodeView of a view made by create, else null.
    static CodeView* of(GtkWidget* view);

    CodeView(const CodeView&) = delete;
    CodeView& operator=(const CodeView&) = delete;

    GtkWidget* widget() const { return view_; }
    GtkTextView* textView() const { return GTK_TEXT_VIEW(view_); }
    Gutter* gutter() const { return gutter_.get(); }

    // Show another buffer: what gtk_text_view_set_buffer does, after
    // emptying the view's cache of laid-out lines while the old buffer still
    // exists. GtkTextLayout keeps a display per line it laid out, and on
    // leaving a buffer drops only those of lines it had validated; a display
    // of any other line (one asked for by a scroll to a far line, a caret
    // check, a lookup past the screen) outlived its buffer, and the next
    // display the cache sorted in read the freed line (CI's GTK job crashed
    // in `tabs`, 2026-11-10; `gutter_switch` crashes without this under
    // MALLOC_PERTURB_, and under ASan, which sees only the segfault, GTK not
    // being instrumented). The cache is emptied the one public way GTK has:
    // a tag of the buffer's table reported changed with no change of size
    // makes each layout showing that table's buffers throw its displays away
    // (gtk_text_tag_changed; GTK's tag_changed_cb invalidates y 0 with equal
    // heights, which empties the cache whole).
    void setBuffer(GtkTextBuffer* buffer);

    // ---- drawing passes
    enum class Layer { BelowText, AboveText };
    using Pass = std::function<void(GtkSnapshot* snapshot, const GdkRectangle& visible)>;
    int addPass(Layer layer, int order, Pass pass);
    void removePass(int id);
    void queueDraw() { gtk_widget_queue_draw(view_); }

    // The ruler, and the gutter's settings.
    void applySettings(const Settings& s);
    void setRulers(std::vector<int> columns, const GdkRGBA& color);
    const std::vector<int>& rulers() const { return rulers_; }
    const GdkRGBA& rulerColor() const { return rulerColor_; }
    // Where column `column` starts, in buffer coordinates (the text's left
    // edge plus `column` '0's of the view's font).
    double columnX(int column) const;
    // For the checks: the x of each ruler as last drawn, in buffer
    // coordinates; empty when none was.
    const std::vector<double>& rulerXs() const { return rulerXs_; }

    // The GtkTextView subclass's vfuncs (CodeView.cpp's widget type).
    void snapshotLayer(GtkTextViewLayer layer, GtkSnapshot* snapshot);
    void allocated();
    ~CodeView();

private:
    explicit CodeView(GtkWidget* view);
    void drawRulers(GtkSnapshot* s, const GdkRectangle& visible);

    struct Entry {
        int id;
        Layer layer;
        int order;
        Pass pass;
    };
    GtkWidget* view_;
    std::unique_ptr<Gutter> gutter_;
    std::vector<Entry> passes_;   // sorted by layer, then order
    int nextId_ = 0;
    std::vector<int> rulers_{100};
    GdkRGBA rulerColor_{0x5A / 255.0f, 0x5A / 255.0f, 0x5A / 255.0f, 1};
    std::vector<double> rulerXs_;
};
