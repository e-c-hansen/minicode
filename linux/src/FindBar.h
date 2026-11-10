// FindBar.h — the editor's find and replace bar (linux/PARITY.md, item
// 1.6), after VS Code's find widget as the Mac's MCEditorFind
// (src/EditorFind.h) is: a box over the top right of the text, with the
// chevron that shows or hides the replace row down its left edge; the query
// with Match Case (Aa), Whole Word (ab) and Regular Expression (.*) inside
// its right end; the count ("3 of 17", or "No results" in red with the
// query outlined); Previous and Next; Find in Selection; and Close. The
// replace row holds the replacement with Preserve Case (AB) inside it, then
// Replace and Replace All. Every button's tooltip names its key. GTK draws
// it from a stylesheet of its own in the theme's find widget colors
// (FindRules::palette); its measures are not VS Code's to the pixel.
//
// What it decides is FindRules.h's, the matching TextFind.h's, as on the
// Mac:
//   - Ctrl+F seeds the query from a selection within one line, else from
//     the word at the caret (escaped while Regular Expression is on), and
//     leaves it for a selection over several lines; from the bar's own
//     field it only selects the query.
//   - Typing searches as you go, off the main thread (a GTask): each change
//     of the query, an option or the text raises the running search's
//     cancel flag, and results are taken only while the query and the text
//     are still those they were found for. The text is copied once per edit
//     of it, never per keystroke in the query, and turned into UTF-16 on the
//     worker. An edit searches again 0.1 s after typing stops.
//   - The first match from the caret is selected as the query changes;
//     Enter and F3 go on, Shift+Enter and Shift+F3 back, wrapping.
//   - Matches on the lines on screen (and 50 either side) carry a tag,
//     painted again as the view scrolls; the selected one has its own tag.
//     Never a snapshot pass, so the gutter's GtkTextView subclass (item
//     1.4) and this stay apart.
//   - Find in Selection keeps the matches to the selection it was turned
//     on with (VS Code's: one ending after a line break ends before it), a
//     pair of marks that follow edits, tinted; it goes when the bar closes.
//   - Replace replaces the selected match and selects the next; Replace All
//     replaces every match (in the scope) as one user action through
//     MultiEdit::apply, one undo step, the caret kept in the text around
//     it. A Markdown page or any read-only text finds but does not replace.
//   - Escape closes it, in the bar or in the editor, and gives the editor
//     the keyboard back if the bar had it.
//
// Keys while a field of the bar has the keyboard: Enter (in the
// replacement, Replace), Shift+Enter, Escape, and Tab between the two
// fields. The rest are the registry's (CommandsLinux.cpp), so they work
// from the editor too: Ctrl+F, Ctrl+H, F3, Shift+F3, Alt+C, Alt+W, Alt+R,
// Alt+P, Alt+L, Ctrl+Shift+1, Ctrl+Alt+Enter.
#pragma once

#include <gtk/gtk.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "FindRules.h"
#include "TextFind.h"

class Editor;
class Settings;

class FindBar {
public:
    // Hangs itself over `editor`'s slot (Editor::addOverlay).
    explicit FindBar(Editor* editor);
    // Runs while the window's widgets still exist (main.cpp deletes it
    // before the editor, from window-removed).
    ~FindBar();
    FindBar(const FindBar&) = delete;
    FindBar& operator=(const FindBar&) = delete;

    GtkWidget* widget() const { return box_; }

    // ---- the commands (main.cpp's actions)
    void showFind();                 // Ctrl+F: seeded, the query's field focused
    void showReplace();              // Ctrl+H: the replace row, its field focused
    void findNext(bool forward);     // F3, Shift+F3 (no query: as Ctrl+F)
    void replaceOne();               // Ctrl+Shift+1
    void replaceAll();               // Ctrl+Alt+Enter
    void toggleMatchCase();
    void toggleWholeWord();
    void toggleRegex();
    void togglePreserveCase();
    void toggleFindInSelection();
    void toggleReplaceRow();         // the chevron
    void close();
    // Escape in the editor: closes the bar if it is open.
    bool closeFromEditor();

    // The theme's colors, for the bar and the highlights.
    void applySettings(const Settings& s);
    // A line for the status bar ("Replaced 17 matches").
    std::function<void(const std::string&)> onNote;
    // Every object a handler on a widget names, for main.cpp's teardown.
    void* handlerOwner() { return this; }

    // ---- what it shows, for the checks
    bool shown() const { return shown_; }
    bool replaceShown() const { return replaceShown_; }
    bool matchCase() const;
    bool wholeWord() const;
    bool regex() const;
    bool preserveCase() const;
    bool findInSelection() const { return scopeA_ != nullptr; }
    // The scope, in characters; false when Find in Selection is off.
    bool scope(int* start, int* end) const;
    // There are results for the query and the text as they are.
    bool resultsCurrent() const;
    // The matches found for them, in characters.
    const std::vector<TextFind::Match>& matches() const { return found_.points; }
    long currentIndex() const;
    std::string query() const;
    std::string countText() const;
    bool countInErrorColor() const;
    bool queryOutlined() const;      // the red outline of "No results"
    std::size_t textCopies() const { return textCopies_; }
    int highlighted() const { return highlighted_; }   // matches tagged now
    // The main thread's CPU time for the last Replace All's edits, and the
    // worker's for finding them, in milliseconds.
    double lastReplaceAllMs() const { return lastReplaceAllMs_; }
    double lastReplaceAllWorkerMs() const { return lastReplaceAllWorkerMs_; }
    // A field of the bar has the keyboard.
    bool editing() const;
    GtkWidget* findField() const { return findText_; }
    GtkWidget* replaceField() const { return replaceText_; }
    GtkWidget* countLabel() const { return count_; }
    GtkWidget* caseButton() const { return case_; }
    GtkWidget* wordButton() const { return word_; }
    GtkWidget* regexButton() const { return regex_; }
    GtkWidget* preserveButton() const { return preserve_; }
    GtkWidget* selectionButton() const { return selection_; }
    GtkWidget* previousButton() const { return prev_; }
    GtkWidget* nextButton() const { return next_; }
    GtkWidget* closeButton() const { return close_; }
    GtkWidget* chevronButton() const { return chevron_; }
    GtkWidget* replaceButton() const { return replaceOne_; }
    GtkWidget* replaceAllButton() const { return replaceAll_; }

private:
    struct Job;
    struct Text;

    void build();
    void show(GtkWidget* field, bool replace);
    void refreshTips();
    void setReplaceShown(bool on);
    void updateVisibility();
    bool canReplace() const;
    void focusField(GtkWidget* field);
    void selectInText(int insert, int bound);

    // The buffer the view shows: searched, tagged, followed.
    void attach(GtkTextBuffer* buffer);
    void detach();
    void textChanged();
    void selectionChanged();

    // Searching (see the Mac's MCEditorFind, whose steps these are).
    void queryChanged();
    void search();
    std::shared_ptr<Text> currentText();
    void arrived(Job* job);
    void replaced(Job* job);
    void whenCurrent(std::function<void()> block);
    void selectMatch(std::size_t index);
    void showCount();
    void setFindInSelection(bool on);
    void updateSelectionButton();
    void setScope(int start, int end);
    void dropScope();

    // The highlights: tags on the matches near the lines on screen.
    void ensureTags();
    void schedulePaint();
    void paint();
    void clearMatchTags();
    void removeAllTags();

    static void worker(GTask* task, gpointer source, gpointer data, GCancellable* c);
    static void onDone(GObject* source, GAsyncResult* res, gpointer data);

    Editor*        editor_ = nullptr;
    GtkWidget*     view_ = nullptr;          // the editor's GtkTextView
    GtkTextBuffer* buf_ = nullptr;           // what it shows, referenced while attached
    GtkAdjustment* vadj_ = nullptr;
    GtkEventController* viewKeys_ = nullptr;   // Escape in the editor
    GtkEventController* boxKeys_ = nullptr;    // the fields' keys

    GtkWidget* box_ = nullptr;
    GtkWidget* chevron_ = nullptr;
    GtkWidget* findInput_ = nullptr;
    GtkWidget* findText_ = nullptr;
    GtkWidget* case_ = nullptr;
    GtkWidget* word_ = nullptr;
    GtkWidget* regex_ = nullptr;
    GtkWidget* count_ = nullptr;
    GtkWidget* prev_ = nullptr;
    GtkWidget* next_ = nullptr;
    GtkWidget* selection_ = nullptr;
    GtkWidget* close_ = nullptr;
    GtkWidget* replaceInput_ = nullptr;
    GtkWidget* replaceText_ = nullptr;
    GtkWidget* preserve_ = nullptr;
    GtkWidget* replaceOne_ = nullptr;
    GtkWidget* replaceAll_ = nullptr;
    GtkWidget* replaceButtons_ = nullptr;

    bool shown_ = false;
    bool replaceShown_ = false;
    bool settingField_ = false;   // the query is being set, not typed

    TextFind::Query query_;
    unsigned long queryGen_ = 0;       // every change of the query or an option
    unsigned long textVersion_ = 0;    // every edit of the text, and a new buffer
    unsigned long resultsGen_ = (unsigned long)-1, resultsVersion_ = (unsigned long)-1;
    FindRules::Found found_;
    std::shared_ptr<Text> foundText_;  // the text found_ was found in
    bool selectOnArrival_ = false;     // the query changed: select from the caret
    std::size_t anchor_ = 0;           // where, in characters
    std::function<void()> whenCurrent_;
    std::shared_ptr<std::atomic<bool>> cancel_;   // the running search's
    bool searching_ = false;
    guint searchTimer_ = 0;            // 0.1 s after an edit
    std::shared_ptr<Text> text_;       // the text at textAt_
    unsigned long textAt_ = (unsigned long)-1;
    std::size_t textCopies_ = 0;

    // Find in Selection's scope: marks in buf_, or null.
    GtkTextMark* scopeA_ = nullptr;
    GtkTextMark* scopeB_ = nullptr;
    bool scopeTagged_ = false;

    GtkTextTag* matchTag_ = nullptr;
    GtkTextTag* currentTag_ = nullptr;
    GtkTextTag* scopeTag_ = nullptr;
    GdkRGBA matchColor_{0.92f, 0.36f, 0.0f, 0.33f};
    GdkRGBA currentColor_{0.32f, 0.36f, 0.42f, 1.0f};
    GdkRGBA scopeColor_{0.23f, 0.24f, 0.25f, 0.4f};
    GtkTextMark* paintA_ = nullptr;    // the range the match tags were put on
    GtkTextMark* paintB_ = nullptr;
    guint paintIdle_ = 0;
    guint selectionIdle_ = 0;          // the selection moved: the count and button follow
    int highlighted_ = 0;

    double lastReplaceAllMs_ = 0, lastReplaceAllWorkerMs_ = 0;
    // Searches still running when the bar goes find it gone through this.
    std::shared_ptr<int> life_ = std::make_shared<int>(0);
};
