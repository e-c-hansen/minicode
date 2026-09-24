// Editor.h — the text editing surface: a GtkTextView + GtkTextBuffer with
// syntax highlighting driven by the shared IncrementalHighlighter core, plus a
// Markdown preview toggle backed by Markdown.{h,cpp}.
#pragma once

#include <gtk/gtk.h>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Settings.h"
#include "SyntaxHighlighter.h"

class MediaView;
class LatexPreview;

// Told what the buffer holds and how it changes, for the language server
// session (Lsp.h). Three calls, so the editor knows nothing about LSP.
class EditorObserver {
public:
    virtual ~EditorObserver() = default;
    // The buffer now holds another document: `path` when it is that file's
    // editable source, "" for a message or a Markdown preview.
    virtual void documentChanged(const std::string& path) = 0;
    virtual void documentSaved() = 0;
    // The source was edited; the buffer has already changed. `inserted` is
    // the text of an insertion (len bytes), or null for a deletion.
    virtual void textEdited(const char* inserted, int len) = 0;
};

class Editor {
public:
    Editor();
    ~Editor();

    // The widget to drop into the layout: a stack holding the text editor
    // and the image / PDF viewer, which take the same slot.
    GtkWidget* widget() const { return slot_; }

    // The GtkTextView / GtkTextBuffer themselves, for the shell's find bar.
    GtkWidget*     textView() const { return view_; }
    GtkTextBuffer* buffer()   const { return buffer_; }

    // Load a file from disk into the buffer, detect its extension and
    // re-highlight. Returns false if the file could not be read.
    bool openFile(const std::string& path);

    // Write the current buffer (or the raw source, in preview mode) to disk.
    // Returns false, with the reason in *error, when the write failed; the
    // buffer then stays marked unsaved. When there is nothing to save (no
    // file, or the slot shows something that is not the file's text: an image,
    // a PDF, the "Cannot display" message) it writes nothing and returns true.
    bool save(std::string* error = nullptr);
    // False while an image, a PDF or a binary file's message is shown.
    bool canSave() const { return !path_.empty() && !readOnly_; }

    // Close the file and show the welcome text, dropping any unsaved edits.
    // The shell asks "Save changes?" before calling this.
    void closeFile();

    // Changes made to the open text file by something else (the Mac's
    // checkExternalChange). The file is watched while it is open, and the
    // shell also calls checkExternalChange when the window becomes active.
    // What is on disk is compared with what was last loaded or saved, by
    // content, so MiniCode's own atomic saves never count. When it differs:
    // a clean buffer takes the new text in place, keeping the caret and the
    // scroll position; a buffer with unsaved edits is left alone and the
    // callback is asked to put the question to the user. It returns false if
    // it cannot ask now (another question is up), and then the change is
    // reported again on the next check. Images and PDFs reload on their own
    // (MediaView) and are not watched here.
    using ExternalCb = bool(*)(void* user);
    void setExternalChangeCallback(ExternalCb cb, void* user) { extCb_ = cb; extUser_ = user; }
    void checkExternalChange();
    // The user chose Reload: the disk's text replaces the buffer and its
    // edits, in place. False if the file can no longer be read as text.
    bool reloadFromDisk();

    // Images and PDFs (MediaView). isMedia() is true while one is shown, and
    // titleSuffix() is what the window title adds for it ("  640 × 480",
    // "  12 pages"), empty otherwise.
    bool isMedia() const;
    std::string titleSuffix() const;
    MediaView* media() const { return media_; }

    // Toggle between the raw editable buffer and the rendered Markdown preview,
    // or for LaTeX between the source and the typeset pages. No-op for other
    // files.
    void togglePreview();
    bool isMarkdown() const { return isMarkdown_; }
    bool inPreview() const { return preview_; }
    // A .tex/.ltx/.latex file, in a build with poppler: it opens in the LaTeX
    // preview (Latex.h), which takes the slot while the buffer keeps the
    // source. latex() is null until the first one opens.
    bool isLatex() const { return isLatex_; }
    LatexPreview* latex() const { return latex_; }
    // Ctrl+Shift+S: save the typeset PDF somewhere of the user's choosing.
    // False when the open file is not LaTeX.
    bool exportPdf(GtkWindow* parent);

    const std::string& currentPath() const { return path_; }
    bool dirty() const { return dirty_ && !readOnly_; }

    // Optional: called whenever the dirty/title state changes so the shell can
    // refresh the window title. Set by main.cpp.
    using TitleCb = void(*)(void* user);
    void setTitleCallback(TitleCb cb, void* user) { titleCb_ = cb; titleUser_ = user; }

    // Show a plain gray message (welcome screen / errors), not editable.
    void showMessage(const std::string& msg);
    void showWelcome();

    // Colors from the settings file: syntax and Markdown tags, and the swatches
    // shown while the settings file itself is open. The panel background and
    // text come from the stylesheet (ThemeCss).
    void applySettings(const Settings& s);
    // The settings file's path. While that file is open its color values are
    // clickable swatches that open a color picker.
    void setSettingsPath(const std::string& path) { settingsPath_ = path; }

    // Ctrl+/ : comment or uncomment the lines the selection touches. Returns
    // false when there is nothing to do it to (preview, message, a file type
    // with no line comments).
    bool toggleComment();

    // Put the caret on a 1-based line and select `byteLength` bytes starting
    // `byteColumn` bytes into it (a Find in Folder match), scrolled into view
    // and focused. A Markdown preview switches to the source first. A column
    // that no longer fits the line (the file changed since the search) selects
    // nothing and leaves the caret at the line's start. Returns false when no
    // editable text is showing.
    bool revealLine(int line, std::size_t byteColumn, std::size_t byteLength);

    // The open file now lives at `path` (a rename or save-as). Keeps the
    // buffer and its undo history, and re-highlights the whole buffer only if
    // the extension, and so the grammar, changed.
    void setPath(const std::string& path);

    // One observer (the language server session), not owned.
    void setObserver(EditorObserver* o) { observer_ = o; }
    // True while the buffer holds a file's editable source.
    bool isSource() const { return sourceMode_; }
    // The whole source as UTF-8: the highlighter's mirror when there is one,
    // else a copy out of the buffer.
    std::string text() const;

private:
    // Highlighting. startHighlighting() lexes the whole buffer and retags it
    // (file load, grammar change, settings change). After that every edit is
    // fed to the IncrementalHighlighter from the buffer's insert-text and
    // delete-range signals, and only the lines it reports are retagged: at
    // once if they are few or on screen, otherwise in idle time slices.
    void startHighlighting();
    void stopHighlighting();     // buffer no longer holds source (preview, message)
    void flushHighlighting();    // retag the lines pending since the last flush
    void retagSpan(size_t firstLine, size_t endLine);   // lex from stored states, retag
    void retagLines(size_t firstLine, size_t endLine, const std::vector<Token>& tokens);
    void visibleLines(size_t& first, size_t& end) const;
    void cancelDeferred();
    void resyncAfterBulk();      // one prefix/suffix diff for a many-edit user action
    void removeHighlightTags(const GtkTextIter* a, const GtkTextIter* b);
    size_t byteOffsetOf(const GtkTextIter* it) const;
    bool linesMatch() const;     // GTK's lines are the highlighter's '\n' lines
    bool isHighlightTag(GtkTextTag* tag) const;
    void noteEdit(size_t pos, size_t oldLen, size_t newLen);

    static void onInsertText(GtkTextBuffer* buf, GtkTextIter* loc, char* text,
                             int len, gpointer self);
    static void onDeleteRange(GtkTextBuffer* buf, GtkTextIter* a, GtkTextIter* b,
                              gpointer self);
    static void onInsertTextAfter(GtkTextBuffer* buf, GtkTextIter* loc, char* text,
                                  int len, gpointer self);
    static void onDeleteRangeAfter(GtkTextBuffer* buf, GtkTextIter* a, GtkTextIter* b,
                                   gpointer self);
    static void onApplyTag(GtkTextBuffer* buf, GtkTextTag* tag, GtkTextIter* a,
                           GtkTextIter* b, gpointer self);
    static void onBeginUserAction(GtkTextBuffer* buf, gpointer self);
    static void onEndUserAction(GtkTextBuffer* buf, gpointer self);
    static gboolean onIdleRetag(gpointer self);

    void renderPreview();        // build the Markdown preview into the buffer
    void loadRawIntoBuffer();    // put source_ back as editable, highlighted text
    void ensureTags();           // create the per-style + markdown GtkTextTags once
    void setProseFont(bool prose);  // proportional for preview/messages, mono for code
    void markDirty(bool d);

    static void onBufferChanged(GtkTextBuffer* buf, gpointer self);

    bool isSettingsFile() const;
    void decorateColors(int firstLine, int endLine);   // swatch tags over color values
    bool swatchAt(double x, double y, int* line) const;   // widget coords
    void pickColor(int line);
    void setLineColor(int line, const Rgba& c);
    void dropColorPopover();
    void scheduleColorSave();
    static void onColorPicked(GObject* chooser, GParamSpec* pspec, gpointer self);
    static void onViewUnrealize(GtkWidget* view, gpointer self);
    static void onPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static void onMotion(GtkEventControllerMotion* m, double x, double y, gpointer self);

    // External changes (checkExternalChange).
    void watchText();                     // start watching path_ as text
    void stopWatchingText();
    void rememberDisk(const std::string& content);
    bool diskMatches(const std::string& content) const;
    void applyDiskText(const std::string& content);   // in place, caret and scroll kept
    static void onTextFileChanged(GFileMonitor* m, GFile* f, GFile* other,
                                  GFileMonitorEvent ev, gpointer self);

    void showTextSlot();                 // the text view back in the slot
    void showLatex(bool on);             // the LaTeX preview in the slot, or the source
    static void onMediaChanged(void* self);   // reloaded from disk

    GtkWidget*     slot_     = nullptr;   // GtkStack: scroller_ or the media view
    MediaView*     media_    = nullptr;
    bool           readOnly_ = false;     // the buffer is not the file's text
    GtkWidget*     scroller_ = nullptr;
    GtkWidget*     view_     = nullptr;   // GtkTextView
    GtkTextBuffer* buffer_   = nullptr;

    std::string path_;
    std::string ext_;         // lowercase, no dot
    std::string source_;      // authoritative UTF-8 source text
    bool        isMarkdown_ = false;
    bool        isLatex_    = false;
    LatexPreview* latex_    = nullptr;
    bool        preview_    = false;
    bool        dirty_      = false;
    bool        tagsReady_  = false;
    bool        sourceMode_ = false;   // the buffer holds editable source text

    // Highlighting state, live only while the buffer holds source of a
    // language the lexer knows. mirror_ is a UTF-8 copy of the buffer kept in
    // step edit by edit, the TextSource the highlighter reads.
    std::unique_ptr<IncrementalHighlighter<char>> hl_;
    std::string  mirror_;
    size_t       pendStart_ = 0, pendEnd_ = 0;   // bytes still to retag
    bool         pending_ = false;
    size_t       defStart_ = 0, defEnd_ = 0;     // bytes left to the idle retag
    bool         deferred_ = false;
    guint        idleId_ = 0;
    bool         inAction_ = false;    // between begin- and end-user-action
    int          actionEdits_ = 0;     // edits seen in the current user action
    bool         bulk_ = false;        // stopped tracking edits one by one
    bool         applyingTags_ = false;
    std::vector<GtkTextTag*> hlTags_;   // syntax tags plus swatches: all a retag owns

    std::string settingsPath_;
    Settings    settings_;    // for tag colors and swatch text contrast
    bool        overSwatch_ = false;

    // The live color picker: a popover on the text view holding a
    // GtkColorChooserWidget. Every pick rewrites the swatch's line at once;
    // the file is saved 150 ms after the last one, as on the Mac.
    GtkWidget*  colorPop_ = nullptr;
    int         colorLine_ = -1;
    std::string colorPath_;             // the file the popover was opened for
    guint       colorSaveTimer_ = 0;

    // What the open text file held on disk when it was last loaded or saved
    // (size and hash; the content itself is not kept twice).
    bool          diskKnown_ = false;
    std::size_t   diskSize_ = 0;
    std::size_t   diskHash_ = 0;
    GFileMonitor* textMonitor_ = nullptr;
    guint         textCheckTimer_ = 0;
    ExternalCb    extCb_ = nullptr;
    void*         extUser_ = nullptr;

    EditorObserver* observer_ = nullptr;
    void notifyDocument();    // tell the observer what the buffer holds now

    TitleCb titleCb_ = nullptr;
    void*   titleUser_ = nullptr;
};
