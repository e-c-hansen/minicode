// Gutter.h — the line numbers beside the editor, linux/PARITY.md item 1.4
// (D4): a widget the editor's GtkTextView holds in its left border window
// (gtk_text_view_set_gutter), so it scrolls with the text and takes its
// width from the text's side of the pane. The Mac's counterpart is
// MCLineGutter (src/Gutter.h), a ruler on the editor's scroll view.
//
// Each logical line is numbered on the display line it starts on (a wrapped
// line's continuations stay blank), the carets' lines in the active color.
// Only the display lines on screen are visited, and the numbers come from
// the buffer's BufferLines (a LineIndex kept edit by edit), so neither
// drawing nor typing counts lines through the file. A click selects a
// line, a drag the lines it passes, and Shift+click extends the selection
// to whole lines.
//
// Lanes, left to right:
//   - dots: a dot in the error or warning color by each line a diagnostic
//     starts on, from the buffer's tags (the language server session's
//     squiggles, lsp-error and lsp-warning, unless setDiagnosticTags says);
//   - change bars, while changeLane is on (item 2.10): the buffer's
//     BufferLines::changeBars, a 3 px bar beside each added or changed line
//     and a small triangle where lines were deleted; a click on one calls
//     onChangeBar instead of selecting;
//   - the numbers, right-aligned, as wide as the line count's digits, three
//     at least;
//   - folds (item 1.10): the buffer's BufferLines::folds, a chevron by each
//     line a region starts on while the pointer is over the gutter, always
//     by a closed fold; a click on one toggles it.
//
// It shows while the view is editable (the source, never a preview, a
// message or the welcome text), its buffer has BufferLines, and
// editor.line-numbers is not off. editor.line-numbers = relative counts up
// and down from the caret's line, which shows its own number, as VS Code's.
// Its background is editor.gutter-background (the editor's own unless set),
// painted once here, so it is right at any opacity.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class Settings;
class BufferLines;

class Gutter {
public:
    // Sets itself as `view`'s left gutter; the view owns the widget. The
    // editor's CodeView makes it and deletes it before the view goes.
    explicit Gutter(GtkTextView* view);
    ~Gutter();
    Gutter(const Gutter&) = delete;
    Gutter& operator=(const Gutter&) = delete;

    GtkWidget* widget() const { return widget_; }

    enum class Mode { Off, On, Relative };
    // editor.line-numbers and the colors.
    void applySettings(const Settings& s);
    void setMode(Mode mode);
    Mode mode() const { return mode_; }
    void setColors(const GdkRGBA& background, const GdkRGBA& number, const GdkRGBA& active);
    const GdkRGBA& backgroundColor() const { return background_; }
    const GdkRGBA& numberColor() const { return number_; }
    const GdkRGBA& activeNumberColor() const { return active_; }

    // Whether it is showing now.
    bool shown() const { return gtk_widget_get_visible(widget_); }

    // ---- lanes
    static constexpr double kDotLane = 12;
    static constexpr double kChangeLane = 5;
    static constexpr double kChangeBarWidth = 3;
    static constexpr double kFoldLane = 14;
    static constexpr int kMinDigits = 3;
    // The change bars' lane, between the dots and the numbers (GitGutter.h
    // turns it on with editor.git-gutter).
    void setChangeLane(bool on);
    bool changeLane() const { return changeLane_; }
    // The bars' colors: added, modified, deleted (editor.git-added and the
    // rest; VS Code's until set).
    void setChangeColors(const GdkRGBA& added, const GdkRGBA& modified, const GdkRGBA& deleted);
    // A click on change bar `index` (of the buffer's changeBars), with the
    // rect it is drawn in, in the gutter's coordinates.
    std::function<void(int index, const graphene_rect_t& rect)> onChangeBar;
    // The rect bar `index` is drawn in (for a deletion, the boundary it
    // marks), in the gutter's coordinates; empty when its lines are not laid
    // out.
    graphene_rect_t rectOfChangeBar(int index) const;
    // The tags whose starts get a dot, with their severities (1 error, 2
    // warning: the worst wins), colored by the tag's underline color.
    void setDiagnosticTags(std::vector<std::pair<std::string, int>> tags);

    // The x of the numbers' right edge, and the width beside the digits.
    double numbersRight() const;
    double laneWidth() const;
    int digits() const { return digits_; }
    // The width its lanes and digits take in the view's font now.
    int wantedWidth() const;
    // The logical line at a y of the gutter (0-based; the last line below
    // the text).
    size_t lineAtY(double y) const;

    // Draw again (the view's size or layout changed).
    void queueDraw();

    // ---- for the checks: what the last snapshot drew, top to bottom
    struct Row {
        size_t line = 0;          // 0-based logical line
        std::string label;        // what was drawn: the number, or the distance
        double y = 0, height = 0; // the display line, in the gutter's coordinates
        bool active = false;      // in the active color (a caret's line)
        int dot = 0;              // 1 error, 2 warning, 0 none
        int chevron = 0;          // 1 open region, 2 closed fold, 0 none
    };
    const std::vector<Row>& drawnRows() const { return rows_; }
    unsigned drawCount() const { return draws_; }
    int changeBarsDrawn() const { return barsDrawn_; }
    // GTK lines the last snapshot visited, those passed over included: no
    // more than are on screen, however many a fold hides (D8).
    unsigned linesWalked() const { return walked_; }

    // GtkWidget vfuncs (Gutter.cpp's widget type calls them).
    void measure(GtkOrientation o, int* min, int* nat);
    void snapshot(GtkSnapshot* s, int width, int height);

private:
    BufferLines* lines() const;
    void attachBuffer(GtkTextBuffer* b);
    void attachAdjustment(GtkAdjustment* a);
    void updateVisibility();
    void updateWidth();
    int digitsFor(size_t lineCount) const;
    PangoLayout* numberLayout(const char* text) const;
    double digitWidth() const;
    guint fontHash() const;   // the view's font, as last measured in measuredFont_
    void checkFont();
    size_t caretLine() const;
    int dotOn(const GtkTextIter& start, const GtkTextIter& end) const;
    bool isDiagnosticTag(GtkTextTag* tag) const;
    GdkRGBA colorOfSeverity(int severity) const;
    // Buffer y to the gutter's, and a line's y range in the gutter.
    double toGutterY(int bufferY) const;
    bool linesYRange(size_t first, size_t last, double* y, double* height) const;
    void drawChangeBars(GtkSnapshot* s, size_t firstLine, size_t lastLine);
    void drawChevron(GtkSnapshot* s, double width, double midY, bool closed);
    void selectLines(size_t anchor, size_t line);

    static void onDragBegin(GtkGestureDrag* g, double x, double y, gpointer self);
    static void onDragUpdate(GtkGestureDrag* g, double dx, double dy, gpointer self);

    GtkTextView*   view_ = nullptr;
    GtkWidget*     widget_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;      // the view's, held while shown in it
    GtkAdjustment* vadj_ = nullptr;
    int            listenId_ = 0;          // on the buffer's BufferLines
    BufferLines*   listening_ = nullptr;
    Mode           mode_ = Mode::On;
    GdkRGBA        background_{0.118f, 0.118f, 0.118f, 1};
    GdkRGBA        number_{0.522f, 0.522f, 0.522f, 1};
    GdkRGBA        active_{0.776f, 0.776f, 0.776f, 1};
    bool           changeLane_ = false;
    GdkRGBA        changeColors_[3] = {{0.282f, 0.494f, 0.008f, 1},    // #487E02
                                       {0.106f, 0.506f, 0.659f, 1},    // #1B81A8
                                       {0.945f, 0.298f, 0.298f, 1}};   // #F14C4C
    int            digits_ = kMinDigits;
    size_t         caretLine_ = 0;
    bool           hovering_ = false;
    guint          measuredFont_ = 0;
    guint          resizeIdle_ = 0;
    std::vector<std::pair<std::string, int>> diagTags_;
    mutable std::vector<GtkTextTag*> diagTagPtrs_;   // diagTags_ found in the table
    mutable GtkTextTagTable* diagTagTable_ = nullptr;
    // The drag: the line it began on (none on a chevron or a bar), and where.
    bool   dragging_ = false;
    size_t dragAnchor_ = 0;
    double dragX_ = 0, dragY_ = 0;

    std::vector<Row> rows_;
    unsigned draws_ = 0;
    unsigned walked_ = 0;
    int barsDrawn_ = 0;
};
