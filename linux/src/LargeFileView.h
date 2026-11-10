// LargeFileView.h — big files and long lines in the GTK port (linux/PARITY.md
// item 1.12). The thresholds and rules are the core's (../src/LargeFile.h);
// the Mac's counterpart is EditorController+LargeFile.mm. Four things:
//
//   - Large-file mode, VS Code's: a text over 20 MB or 300,000 lines, with
//     editor.large-file-optimizations on, decided when the tab is read, gets
//     no highlighter, folds, bracket boxes or colors, and no language server
//     (never past VS Code's 50 MB sync limit either). Each Document says so
//     (Document::largeFileFeaturesOff), and every such feature asks it. The
//     status bar's "Large file: features off" (LargeFileUi's note) turns them
//     on for the tab.
//   - A file over 50 MB opened by hand asks Open Anyway or Cancel first
//     (LargeFileUi::mayOpen), and one over kMaxTextBytes is refused.
//   - A file over kBackgroundBytes is read, checked, measured and lexed on a
//     worker thread (Document::beginLoading), then put in its buffer a piece
//     at a time, about 8 ms a turn of the main loop, while its tab says
//     "Opening…". GtkTextBuffer takes about 20 ms a megabyte on this
//     machine, so a 25 MB file in one go held the loop for half a second, and
//     a 200 MB one would have held it past GNOME's five seconds.
//   - A line over 10,000 characters (LargeFile::kLongLineShown, VS Code's
//     editor.stopRenderingLineAfter) is shown 10,000 characters at a time
//     (LongLines): the rest of the line stays in the buffer under an
//     invisible tag, the part shown follows the caret (a search, Go to Line
//     or a click on a link lands in it), and a note after it says how much
//     is left out. GtkTextView lays a line out as one Pango layout: opening
//     a file with a 5 MB line held the main loop for 4 to 7 s of CPU, every
//     keystroke in it for 3 to 9 s, and past about 260,000 characters the
//     line is not even drawn right (Pango's widths overflow). The tag is
//     never in the file, the undo history, or what a copy or the language
//     server gets (D6).
//
// HANDOFF.md item 18 has the measurements.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "EditorTabs.h"

class Editor;

namespace LargeFileView {
// The largest file the editor opens as text: a GtkTextBuffer costs several
// times a file's size, and every window shares the process.
constexpr uint64_t kMaxTextBytes = 256ull * 1024 * 1024;
// Files over this are read and put in their buffer without holding the
// main loop (Document::beginLoading); smaller ones as before, at once.
constexpr uint64_t kBackgroundBytes = 4ull * 1024 * 1024;
// A regular file over kBackgroundBytes.
bool readsInBackground(const std::string& path);

// AutoIndent on a big buffer (Editor::indentKey and electric): the text
// from kIndentLinesAbove lines above `from`'s line to the end of `to`'s
// line, as UTF-16, and the character offset it starts at. Return, Tab,
// Backspace in leading spaces and a typed closer copied the whole buffer
// each time, 0.6 s in a 25 MB file; a bracket or string opened further up
// than the slice is not seen. False, doing nothing, for a buffer of up to
// kIndentWholeChars characters, which is read whole as before.
constexpr int kIndentWholeChars = 1000000;
constexpr int kIndentLinesAbove = 2000;
bool indentSlice(GtkTextBuffer* buffer, const GtkTextIter* from, const GtkTextIter* to,
                 std::u16string& text, long& base);
}  // namespace LargeFileView

// The long lines of one source buffer (every Document's, attached when it is
// made, and owned by the buffer, as BufferLines is). Its handlers run after
// each edit and hide what a long line shows no more of; the part shown of
// the caret's line moves to hold the caret, a moment after it moves.
class LongLines {
public:
    static LongLines* attach(GtkTextBuffer* buffer);
    static LongLines* of(GtkTextBuffer* buffer);   // null when none is attached
    ~LongLines();
    LongLines(const LongLines&) = delete;
    LongLines& operator=(const LongLines&) = delete;

    GtkTextTag* hiddenTag() const { return tag_; }
    // On a long line's first character: the line never wraps, word wrap on
    // or not (PARITY.md 1.11), so End goes on into its hidden rest.
    GtkTextTag* noWrapTag() const { return noWrap_; }

    // A line as shown: its characters (without its break), and the part
    // [start, end) not hidden. False when the line is no long line.
    struct Part {
        size_t length = 0, start = 0, end = 0;
    };
    bool partOf(int line, Part* out) const;
    // The part of the caret's line moved to hold the caret now, rather than
    // a moment later (the follow is otherwise an idle at high priority).
    void followCaret();
    // Told when the caret's line's part moved, or a long line was edited
    // (the view brings the caret on screen); unlisten with the id.
    int listen(std::function<void()> onMoved);
    void unlisten(int id);
    // For checks: how many times a line's hidden range was set, and the CPU
    // time all LongLines' handlers took, in ms.
    size_t applyCount() const { return applies_; }
    static double cpuMs();

private:
    explicit LongLines(GtkTextBuffer* buffer);
    void apply(int line);                 // the line's part from its length and the caret's place
    std::vector<std::pair<size_t, size_t>> visibleRuns(const GtkTextIter* ls,
                                                       const GtkTextIter* le) const;
    void lineEdited(GtkTextIter* at, const char* text, int len, bool deleted);
    void scheduleFollow();
    size_t followedStart(int line) const;
    static void onInsertAfter(GtkTextBuffer* b, GtkTextIter* at, char* text, int len, gpointer self);
    static void onDeleteAfter(GtkTextBuffer* b, GtkTextIter* from, GtkTextIter* to, gpointer self);
    static void onApplyTag(GtkTextBuffer* b, GtkTextTag* tag, GtkTextIter* a, GtkTextIter* z,
                           gpointer self);
    static void onMarkSet(GtkTextBuffer* b, GtkTextIter* at, GtkTextMark* mark, gpointer self);

    // The no-wrap tag on a long line's first character.
    void keepUnwrapped(int line);

    GtkTextBuffer* buffer_;
    GtkTextTag*    tag_ = nullptr;
    GtkTextTag*    noWrap_ = nullptr;   // a long line's first character: no wrapping
    GtkTextMark*   follow_ = nullptr;   // where the caret's line's part starts
    guint          followIdle_ = 0;
    bool           applying_ = false;
    size_t         applies_ = 0;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
    int            nextListener_ = 0;
};

// The editor's half: the status bar's note, the question before a file over
// 50 MB, and the notes drawn after each long line's part. One per Editor,
// made by it; Editor::largeFileNote() is the note's widget, for the status
// bar.
class LargeFileUi {
public:
    explicit LargeFileUi(Editor* editor);
    ~LargeFileUi();
    LargeFileUi(const LargeFileUi&) = delete;
    LargeFileUi& operator=(const LargeFileUi&) = delete;

    GtkWidget* note() const { return note_; }
    // Shows or hides the note for the tab on show.
    void update();
    // Opening `path` in a new tab by hand: true to go on now; false when the
    // file is over 50 MB and the question is up (Open Anyway opens it then,
    // as `from` would have).
    bool mayOpen(const std::string& path, EditorTabs::OpenFrom from);
    // For checks: questions put, and notes drawn after long lines' parts.
    size_t questionsAsked() const { return asked_; }
    size_t notesDrawn() const { return notesDrawn_; }

private:
    void drawNotes(GtkSnapshot* s, const GdkRectangle& visible);
    void scheduleCaretScroll();   // the caret on screen, a turn later
    guint scrollIdle_ = 0;
    // GtkTextView moves the caret over hidden text: End stopped at the end
    // of the part shown, and Right at it jumped to the line's end. Before a
    // move the caret's place is noted; after it, a move that met a part's
    // edge goes where the key meant (End to the line's end, Home to its
    // start, an arrow one character on), and the part follows the caret.
    static void onMoveBefore(GtkTextView* v, GtkMovementStep step, int count, gboolean extend,
                             gpointer self);
    static void onMoveAfter(GtkTextView* v, GtkMovementStep step, int count, gboolean extend,
                            gpointer self);
    static bool copyWhole(GtkTextView* v, bool cut);   // copy-clipboard, cut-clipboard

    Editor*    editor_;
    int        moveFrom_ = -1, moveLine_ = -1, moveLineStart_ = 0;   // onMoveBefore's
    size_t     moveStart_ = 0, moveEnd_ = 0, moveLength_ = 0;
    GtkWidget* note_ = nullptr;
    int        pass_ = -1;
    std::string approved_;   // the file Open Anyway was pressed for, opened next
    LongLines* listening_ = nullptr;   // the buffer on show's
    int        listenId_ = 0;
    size_t     asked_ = 0, notesDrawn_ = 0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};
