// PalettePanel.h — the palette, VS Code's quick input (linux/PARITY.md item
// 1.5): a field over a list at the top centre of the window's panes, the
// counterpart of the Mac's src/PalettePanel.mm. What it lists follows the
// field's first character (CommandPalette::parse, in the core):
//
//   ">"   the port's commands (Keys.h's table of GActions over the
//         registry), each "Menu > Title" with its key in effect, only those
//         whose action is enabled when the palette opens (Ctrl+Shift+P, F1)
//   ":"   a line of the file on show, "42", "42:7" or "42,7" (Ctrl+G)
//   "@"   a symbol of the file on show from minicode's own outline
//         (SymbolOutline.h), "@:" grouped by kind (Ctrl+Shift+O)
//   "#"   the workspace's symbols, which Linux does not have yet
//   else  the window's folder's files, Quick Open (Ctrl+P opens it empty)
//
// Typing filters by the core's rules (CommandPalette::rank and rankFiles:
// word starts first, recently run commands and recently opened files ahead),
// Up and Down choose (wrapping, over the headings of "@:"), Page Up and Page
// Down jump, and Return or a click runs the row on the next turn of the main
// loop. Escape, a click outside the box, or running a row closes it and puts
// the keyboard back where it was, with a text field's selection; the keyboard
// going anywhere else closes it and stays there. Moving through the file's
// symbols shows each in the editor (in a Markdown page, scrolls to the
// heading) while the field keeps the keyboard, and closing without going to
// one puts the selection and the scroll back.
//
// It is two children of a GtkOverlay over the panes, so the window and its
// accelerators stay as they were: a clear sheet under the box that takes a
// click outside it, and the box, placed by the overlay's get-child-position
// at most 600 pixels wide, its list as tall as its rows up to twelve. The
// folder's files are listed on a GLib worker thread (CommandPalette::
// listFiles: Find in Folder's skips and ignore files, dotfiles listed,
// symlinked folders once, at most 20,000), which closing the palette stops.
// The commands run lately and the files Quick Open opened persist in
// $XDG_STATE_HOME/minicode/palette.json (PARITY.md D11), for every window.
#pragma once

#include <gtk/gtk.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "CommandPalette.h"
#include "LspClient.h"
#include "Settings.h"

class Document;
class Editor;

class PalettePanel {
public:
    // What the window gives it each time it opens.
    struct Context {
        std::string root;            // the folder Quick Open lists
        bool useIgnoreFiles = true;  // search.use-ignore-files
        // Open a file of the folder (an absolute path below root), on the
        // turn after the palette closed.
        std::function<void(const std::string& path)> openFile;
    };

    // Adds its children to `overlay`, hidden until shown.
    PalettePanel(GtkWindow* window, GtkOverlay* overlay, Editor* editor);
    // Before the editor goes (main.cpp's teardown deletes it first).
    ~PalettePanel();
    PalettePanel(const PalettePanel&) = delete;
    PalettePanel& operator=(const PalettePanel&) = delete;

    // Open with `text` in the field (">", ":", "@" or ""), or, when it is
    // open already, give it that text and the keyboard. The commands are
    // read now, before the keyboard moves, so their actions' enabled state
    // is the one for where the keyboard was.
    void show(const std::string& text, const Context& context);
    bool isOpen() const { return open_; }
    // Close; with `restoreFocus` the keyboard goes back where it was.
    void close(bool restoreFocus);

    // The colors, from the settings file (the stylesheet is the display's,
    // so this is once for every window).
    static void applySettings(const Settings& settings);

    // ---- for the checks
    GtkWidget* box() const { return box_; }
    GtkWidget* entry() const { return entry_; }
    std::string text() const;
    std::vector<std::string> labels() const;   // the rows, in order
    std::vector<std::string> keys() const;     // each row's keys or note, as shown
    std::string emptyText() const;             // what it says when nothing is listed
    int selected() const { return selected_; }
    bool filesListed() const { return filesListed_; }
    // The last listing ran on a thread other than the main loop's.
    bool filesListedOffMainThread() const { return filesOffMain_; }
    // "outline" once the file's symbols are in, "" before.
    const std::string& symbolSource() const { return symbolSource_; }
    // The file the recent lists persist in.
    static std::string statePath();

private:
    struct Item {
        std::string label;    // the row's text, plain (matched and shown)
        std::string markup;   // the same with its matches marked
        std::string keys;     // at the right, muted
        bool heading = false; // a kind's heading in "@:": never selected
        bool symbol = false;  // a symbol's row: its end gives way, not its start
        std::function<void()> run;       // empty: nothing to run
        std::function<void()> preview;   // when the list comes to it
    };
    struct Cmd {
        std::string id, action, label, title, keys;
    };
    struct ListJob;
    // Where the editor was when the palette opened, for closing without
    // going to a previewed symbol.
    struct Place {
        uint64_t docId = 0;   // Document::id
        GtkTextBuffer* buffer = nullptr;
        int insert = 0, bound = 0;
        double v = 0, h = 0;
        bool valid = false;
    };

    void build();
    void collectCommands();
    void listFiles();
    void refilter();
    void commandItems(const std::u16string& query, std::string* empty);
    void lineItem(const CommandPalette::Parsed& p);
    void fileItems(const std::u16string& query, std::string* empty);
    void symbolItems(const std::u16string& query, bool byKind, std::string* empty);
    void askSymbols();
    void setSymbols(std::vector<Lsp::Symbol> symbols, int caret);
    void select(int row);
    void page(int delta);
    void runRow(int row);
    void takeText(const std::string& text);
    void rememberFocus();
    void restoreFocus();
    void forgetRestore();
    void capturePlace();
    void endPreview();
    bool symbolsAvailable() const;
    void previewSymbol(const Lsp::Symbol& s);
    void goToSymbol(const Lsp::Symbol& s);
    void markRow(int row, bool on);
    std::vector<std::u16string> recentFileKeys() const;
    // Runs `fn` on the next turn of the main loop, if the palette lives.
    void later(std::function<void()> fn);

    static void onSetup(GtkSignalListItemFactory*, GtkListItem* item, gpointer self);
    static void onBind(GtkSignalListItemFactory*, GtkListItem* item, gpointer self);
    static void onUnbind(GtkSignalListItemFactory*, GtkListItem* item, gpointer self);
    static void onRowReleased(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static void onSheetPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static gboolean onKey(GtkEventControllerKey*, guint keyval, guint keycode,
                          GdkModifierType mods, gpointer self);
    static void onChanged(GtkEditable*, gpointer self);
    static void onFocusWidget(GObject*, GParamSpec*, gpointer self);
    static gboolean onChildPosition(GtkOverlay* overlay, GtkWidget* child,
                                    GdkRectangle* allocation, gpointer self);
    static void listWorker(GTask* task, gpointer source, gpointer data, GCancellable*);
    static void onListed(GObject*, GAsyncResult* res, gpointer self);

    GtkWindow*  window_;
    GtkOverlay* overlay_;
    Editor*     editor_;
    GtkWidget* sheet_ = nullptr;   // the clear sheet under the box
    GtkWidget* box_ = nullptr;
    GtkWidget* entry_ = nullptr;
    GtkWidget* scroll_ = nullptr;
    GtkWidget* list_ = nullptr;    // a GtkListView over model_
    GtkWidget* empty_ = nullptr;   // what to say when nothing is listed
    GtkStringList* model_ = nullptr;   // each row's index into items_
    GtkListItemFactory* factory_ = nullptr;
    gulong focusHandler_ = 0, positionHandler_ = 0, changedHandler_ = 0;

    bool open_ = false, closing_ = false;
    unsigned session_ = 0;   // one a showing, for answers that come later
    Context ctx_;
    std::vector<std::u16string> recentKeys_;   // recent files, relative to the root
    std::vector<Item> items_;
    int selected_ = -1;
    std::map<int, GtkWidget*> bound_;   // the row widgets on show, by item

    // Where the keyboard goes back to, and a text field's selection.
    GtkWidget* restore_ = nullptr;   // a weak pointer
    int restoreStart_ = -1, restoreEnd_ = -1;

    std::vector<Cmd> commands_;
    std::vector<CommandPalette::Entry> entries_;

    std::vector<std::string> files_;   // relative to the root, as listed
    std::vector<std::u16string> fileKeys_;
    bool filesListed_ = false, filesOffMain_ = false, filesTruncated_ = false;
    unsigned listGen_ = 0;
    std::shared_ptr<std::atomic<bool>> cancelListing_;

    bool askedSymbols_ = false;
    bool haveSymbols_ = false;
    std::vector<Lsp::Symbol> symbols_;
    int caretSymbol_ = -1;
    std::string symbolSource_;
    int preselect_ = -1;
    bool previewed_ = false, ran_ = false;
    uint64_t openDocId_ = 0;   // the document on show when it opened
    Place place_;

    std::shared_ptr<int> life_ = std::make_shared<int>(0);
};
