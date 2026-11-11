// AgentNotes.h — notes an agent puts over the code (`minicode open --note`,
// `minicode tour`), the Mac's AgentNotes. A range of lines is highlighted,
// scrolled to the upper third of the view, with a small box above it
// labelled with who wrote it ("Claude"). A tour is a sequence of such
// notes, stepped through with Alt+] and Alt+[ and counted in the box
// ("Claude · 2 of 5").
//
// Notes are display only: a text tag over the buffer and a box over the
// editor's slot, never text in the file or a step in its undo history, and
// they never take the keyboard. A note closes on Escape (with the editor's
// keyboard), its x, or an edit that touches its lines; edits elsewhere
// move it along with its lines. Closing a tour's note ends the tour. Each
// tab keeps its note while another tab is shown; nothing is kept across
// restarts. One AgentNotes per window (Editor::notes()).
//
// The user's keyboard is never moved into the agent's file: while they are
// typing in the editor, an agent's file opens in a tab beside theirs
// without being shown, and its note waits there, placed (and scrolled to)
// when they switch to that tab. A note moves the caret only in the agent
// preview tab, never in a tab of the user's.
#pragma once

#include <gtk/gtk.h>

#include <map>
#include <string>
#include <vector>

class Document;
class Editor;

class AgentNotes {
public:
    explicit AgentNotes(Editor* editor);
    ~AgentNotes();
    AgentNotes(const AgentNotes&) = delete;
    AgentNotes& operator=(const AgentNotes&) = delete;

    struct Stop {
        std::string path;
        int line = 1, column = 0, toLine = 0;   // 1-based; 0 when not given
        std::string note;
    };

    // Highlights line:column to toLine:toColumn of `doc` (a missing column is
    // the line's start, or the last line's end; no toLine is `line` alone)
    // with `text` in a box above, labelled `from` ("Claude" when empty): now
    // when `doc` is on show, else when it is next shown. Empty text: the
    // highlight alone.
    void show(Document* doc, const std::string& text, const std::string& from, int line,
              int column, int toLine, int toColumn);

    // A tour replaces any earlier one; its first stop opens at once (beside
    // the user's tab, unshown, when `quiet`). False, with the reason, when
    // that stop could not be opened. Stepping is the user's own doing, so a
    // step always shows its stop.
    bool startTour(const std::vector<Stop>& stops, const std::string& from, bool quiet,
                   std::string* error);
    bool step(int delta);   // +1 next, -1 previous; false at either end
    bool tourActive() const { return tourIndex_ >= 0; }
    int tourIndex() const { return tourIndex_; }   // 0-based
    int tourCount() const { return (int)stops_.size(); }

    // Closes the note on show (and the tour), as its x does.
    void close();
    // Escape: closes the note on show; false when there is none.
    bool closeFromKey();

    // The note on show, for `state` and tests.
    bool shown() const { return shownDoc_ != nullptr; }
    std::string text() const { return shownNote_.text; }
    std::string label() const;
    int firstLine() const;   // 1-based, the highlighted lines; 0 when none
    int lastLine() const;
    GtkWidget* box() const { return box_; }
    // A pending note of `doc` (waiting for its tab), for tests.
    bool pendingFor(const Document* doc) const;

    // The editor's hooks: around a change of the tab on show (the text is
    // swapped then, which is not an edit), and a tab going away.
    void activeWillChange();
    void activeChanged();
    void forget(Document* doc);

    // win.tournext and win.tourprevious, enabled only during a tour.
    void installActions(GActionMap* window);

private:
    struct Note {
        std::string text, from;
        int line = 1, column = 0, toLine = 0, toColumn = 0;
        bool tour = false;
    };
    void place(Document* doc);
    void unplace(bool keep);   // the shown note off the screen; kept for its tab when `keep`
    void endTour();
    bool openStop(int index, bool quiet, std::string* error);
    void reposition();
    void updateActions();
    void buildBox();
    bool touches(long from, long to) const;
    static void onInsert(GtkTextBuffer*, GtkTextIter* at, gchar*, gint, gpointer self);
    static void onDelete(GtkTextBuffer*, GtkTextIter* a, GtkTextIter* b, gpointer self);

    Editor* ed_;
    std::map<Document*, Note> pending_;
    std::vector<Stop> stops_;
    std::string tourFrom_;
    int tourIndex_ = -1;

    Document* shownDoc_ = nullptr;
    Note shownNote_;
    GtkTextBuffer* buf_ = nullptr;
    GtkTextMark *start_ = nullptr, *end_ = nullptr;   // the first line's start, the last line's end
    GtkTextTag* tag_ = nullptr;
    gulong insertHandler_ = 0, deleteHandler_ = 0, scrollHandler_ = 0;
    GtkAdjustment* adjustment_ = nullptr;
    GtkWidget *box_ = nullptr, *head_ = nullptr, *body_ = nullptr;
    GActionMap* actions_ = nullptr;
    guint idle_ = 0;
};
