// Editor.cpp — see Editor.h. Mirrors the behavior of the macOS
// EditorController: monospace editor, VS Code dark palette, and a Markdown
// preview. The view shows the active tab's document; what is per file
// (the buffer, the highlighter, the disk) is Document.cpp, and the tabs are
// Tabs.cpp.
#include "Editor.h"
#include "AutoCloseInput.h"
#include "BracketColors.h"
#include "BracketView.h"
#include "Carets.h"
#include "CodeView.h"
#include "Folds.h"
#include "Palette.h"
#include "Markdown.h"
#include "MarkdownEdit.h"
#include "MarkdownTasks.h"
#include "SyntaxHighlighter.h"
#include "LineComments.h"
#include "AutoIndent.h"
#include "KeepSorted.h"
#include "LargeFileView.h"
#include "Utf8Offsets.h"
#include "MediaView.h"
#include "StartupLog.h"
#include "GitPanel.h"
#include "Latex.h"
#include "PdfView.h"
#include "ScrollSettle.h"
#include "TabStrip.h"
#include "WebImages.h"
#include "Zoom.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <sys/stat.h>

// ---------------------------------------------------------------- construction

Editor::Editor() {
    // One tag table for every document's buffers (PARITY.md, D2), so a tag's
    // color, set once, is every tab's.
    tags_.table = gtk_text_tag_table_new();
    welcome_ = gtk_text_buffer_new(tags_.table);
    gtk_text_buffer_set_enable_undo(welcome_, FALSE);
    ensureTags();
    const char* welcome =
        "\n  MiniCode — a native C++ editor, now on GTK4\n\n"
        "  - Select a file in the sidebar to view it\n"
        "  - Source files are syntax-highlighted by type\n"
        "  - Markdown (.md) files render formatted (Ctrl+Shift+V)\n\n"
        "  Ctrl+O to open a different folder.";
    gtk_text_buffer_set_text(welcome_, welcome, -1);
    GtkTextIter wa, wb;
    gtk_text_buffer_get_bounds(welcome_, &wa, &wb);
    gtk_text_buffer_apply_tag_by_name(welcome_, "plainmsg", &wa, &wb);

    code_ = CodeView::create(welcome_);
    view_ = code_->widget();
    buffer_ = welcome_;
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view_), GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view_), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view_), 6);
    gtk_widget_add_css_class(view_, "minicode-editor");

    // Color swatches in the settings file: a click on one opens the color
    // picker (capture phase, so it wins over the text view's own click
    // handling), and the pointer turns into a hand over them.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click));
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(onMotion), this);
    gtk_widget_add_controller(view_, motion);
    // Return, noted for its indent (onViewKey), and Ctrl+Z and Ctrl+Shift+Z
    // in a Markdown page, whose buffer is never undone into: they are the
    // source buffer's undo there.
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onViewKey), this);
    gtk_widget_add_controller(view_, keys);
    // An input method composing text: auto-save waits for it (Persistence.h).
    g_signal_connect(view_, "preedit-changed", G_CALLBACK(+[](GtkTextView*, char* preedit,
                                                            gpointer self) {
        static_cast<Editor*>(self)->composing_ = preedit && *preedit;
    }), this);
    // A scroll by the user ends holding the top line in place (holdTopLine).
    GtkEventController* wheel =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(wheel, GTK_PHASE_CAPTURE);
    g_signal_connect_swapped(wheel, "scroll-begin", G_CALLBACK(+[](gpointer self) {
        static_cast<Editor*>(self)->stopHold();
        static_cast<Editor*>(self)->stopPlace();
        static_cast<Editor*>(self)->stopSettle();
    }), this);
    g_signal_connect(wheel, "scroll", G_CALLBACK(+[](GtkEventControllerScroll*, double, double,
                                                   gpointer self) -> gboolean {
        static_cast<Editor*>(self)->stopHold();
        static_cast<Editor*>(self)->stopPlace();
        static_cast<Editor*>(self)->stopSettle();
        return FALSE;
    }), this);
    gtk_widget_add_controller(view_, wheel);
    // The color picker's popover is the view's child only by
    // gtk_widget_set_parent, so it must go before the view is disposed (the
    // same trap as the LSP popovers, see Lsp.cpp).
    g_signal_connect(view_, "unrealize", G_CALLBACK(onViewUnrealize), this);

    scroller_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller_, "minicode-scroller");
    gtk_widget_add_css_class(scroller_, "minicode-editor-scroller");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), view_);
    settleScrollingOnUnmap(GTK_SCROLLED_WINDOW(scroller_));
    // A press anywhere in the scroller, a scroll bar's included, is the user
    // taking the view over: a tab's place coming back stops (settleOnPlace).
    GtkGesture* press = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
    g_signal_connect_swapped(press, "pressed", G_CALLBACK(+[](gpointer self) {
        static_cast<Editor*>(self)->stopPlace();
        static_cast<Editor*>(self)->stopSettle();
    }), this);
    gtk_widget_add_controller(scroller_, GTK_EVENT_CONTROLLER(press));
    // A preview's tables and pictures follow the pane's width.
    g_signal_connect_swapped(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)),
                             "notify::page-size", G_CALLBACK(+[](gpointer self) {
        static_cast<Editor*>(self)->fitPage();
    }), this);
    gtk_widget_set_hexpand(scroller_, TRUE);
    gtk_widget_set_vexpand(scroller_, TRUE);

    // Images and PDFs take the editor's slot, the way the macOS build swaps
    // an NSImageView or PDFView in for the text view.
    media_ = new MediaView();
    media_->setChangedCallback(onMediaChanged, this);
#ifdef MINICODE_ENABLE_PDF
    media_->pdfView()->setShownCallback(onPdfShown, this);
#endif
    slot_ = gtk_stack_new();
    gtk_widget_set_hexpand(slot_, TRUE);
    gtk_widget_set_vexpand(slot_, TRUE);
    gtk_stack_add_named(GTK_STACK(slot_), scroller_, "text");
    gtk_stack_add_named(GTK_STACK(slot_), media_->widget(), "media");
    gtk_stack_set_visible_child(GTK_STACK(slot_), scroller_);

    // The tabs (Tabs.cpp): a click selects, the close button or a middle
    // click asks the shell to close (it asks about unsaved changes first), a
    // double-click keeps a preview tab, a right click opens the tab's menu,
    // whose actions are the "tab." group on the strip.
    strip_ = new TabStrip();
    strip_->onSelect = [this](int i) { selectTab(i); };
    strip_->onClose = [this](int i) {
        Document* d = document(i);
        if (!d) return;
        if (onCloseRequest) onCloseRequest(d);
        else closeTab(d);
    };
    strip_->onDoubleClick = [this](int i) {
        if (Document* d = document(i)) keepPreviewTab(d);
    };
    strip_->menuFor = [this](int i) { return menuForTab(i); };
    tabActions_ = g_simple_action_group_new();
    for (const char* name : {"keep", "close", "closeothers", "closeright", "closeall", "copypath",
                             "reveal", "revealtree"}) {
        GSimpleAction* a = g_simple_action_new(name, G_VARIANT_TYPE_UINT64);
        g_signal_connect(a, "activate", G_CALLBACK(+[](GSimpleAction* act, GVariant* param,
                                                     gpointer self) {
            static_cast<Editor*>(self)->tabMenuCommand(g_action_get_name(G_ACTION(act)), param);
        }), this);
        g_action_map_add_action(G_ACTION_MAP(tabActions_), G_ACTION(a));
        g_object_unref(a);
    }
    gtk_widget_insert_action_group(strip_->widget(), "tab", G_ACTION_GROUP(tabActions_));
    gtk_widget_set_visible(strip_->widget(), FALSE);   // until a tab is open

    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(box_, TRUE);
    gtk_widget_set_vexpand(box_, TRUE);
    gtk_box_append(GTK_BOX(box_), strip_->widget());
    // The slot in an overlay, so the find bar (FindBar.h) can hang over the
    // text's top right as VS Code's find widget does.
    slotOverlay_ = gtk_overlay_new();
    gtk_widget_set_hexpand(slotOverlay_, TRUE);
    gtk_widget_set_vexpand(slotOverlay_, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(slotOverlay_), slot_);
    gtk_box_append(GTK_BOX(box_), slotOverlay_);

    brackets_ = new BracketView(this);       // PARITY.md 1.7
    autoClose_ = new AutoCloseInput(this);   // PARITY.md 1.8
    carets_ = new Carets(this);              // PARITY.md 1.9
    large_ = new LargeFileUi(this);   // big files and long lines (LargeFileView.h)
    folds_ = new FoldUi(this);        // PARITY.md 1.10
    showActive();   // the welcome text
}

// Runs while the window is being torn down but its widgets still exist
// (main.cpp deletes the editor from GtkApplication's window-removed), so
// every handler that names this object comes off before GTK disposes them.
Editor::~Editor() {
    delete folds_;
    folds_ = nullptr;
    delete carets_;
    carets_ = nullptr;
    delete autoClose_;
    autoClose_ = nullptr;
    delete brackets_;
    brackets_ = nullptr;
    stopSettle();
    stopPlace();
    if (previewEntryTimer_) g_source_remove(previewEntryTimer_);
    previewEntryTimer_ = 0;
    alive_.reset();   // fetches still running find the editor gone
    stopHold();
    if (arrivedTimer_) g_source_remove(arrivedTimer_);
    arrivedTimer_ = 0;
    dropMdPopover();
    g_signal_handlers_disconnect_by_data(
        gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)), this);
    if (colorSaveTimer_) g_source_remove(colorSaveTimer_);
    colorSaveTimer_ = 0;
    dropColorPopover();
    active_ = nullptr;
    for (Document* d : tabs_) destroy(d);
    tabs_.clear();
    delete large_;
    large_ = nullptr;
    g_signal_handlers_disconnect_by_data(view_, this);
    for (GtkWidget* w : {view_, scroller_}) {
        GListModel* ctrls = gtk_widget_observe_controllers(w);
        for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
            GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
            g_signal_handlers_disconnect_by_data(c, this);
            g_object_unref(c);
        }
        g_object_unref(ctrls);
    }
    gtk_widget_insert_action_group(strip_->widget(), "tab", nullptr);
    g_object_unref(tabActions_);
    delete strip_;
    delete media_;
    delete diff_;
    g_object_unref(welcome_);
    g_object_unref(tags_.table);
}

std::vector<void*> Editor::handlerOwners() const {
    std::vector<void*> out = {(void*)this,      (void*)media_,     (void*)strip_,
                              (void*)brackets_, (void*)autoClose_, (void*)large_,
                              (void*)carets_,   (void*)folds_};
    for (Document* d : tabs_)
        if (d->latexPreview()) out.push_back(d->latexPreview());
    return out;
}

// ---------------------------------------------------------------- tags

// Every document's buffer and page share these (one table), so their colors
// are set once for all of them (applySettings).
void Editor::ensureTags() {
    GtkTextBuffer* b = welcome_;   // any buffer on the shared table

    // One tag per syntax style.
    for (int i = 0; i < kTokenStyleCount; ++i) {
        TokenStyle s = (TokenStyle)i;
        tags_.highlight.push_back(gtk_text_buffer_create_tag(b, TagNameForStyle(s), "foreground",
                                                             ColorForStyle(s), NULL));
    }

    // Markdown preview tags.
    for (int lvl = 1; lvl <= 6; ++lvl) {
        // Font sizes mirror the macOS build: {26,22,19,17,15,14}.
        static const int sizes[7] = {0, 26, 22, 19, 17, 15, 14};
        char name[16];
        g_snprintf(name, sizeof(name), "h%d", lvl);
        gtk_text_buffer_create_tag(b, name,
                                   "foreground", pal::MdHeading,
                                   "weight", PANGO_WEIGHT_BOLD,
                                   "size-points", (double)sizes[lvl],
                                   "pixels-above-lines", lvl <= 2 ? 18 : 12,
                                   "pixels-below-lines", 8,
                                   NULL);
    }
    // Code and tables are monospace at 13pt; everything else inherits the
    // proportional preview font. Mirrors the macOS body/mono split.
    // md_table is created before md_code/md_link: later tags win, so a code
    // span or link inside a table cell keeps its own color.
    gtk_text_buffer_create_tag(b, "md_table",
                               "foreground", pal::EditorText,
                               "family", "monospace",
                               "size-points", 13.0, NULL);   // keeps columns aligned
    // A checked task's text (MdRun::task == 2). Before md_code and md_link,
    // so a code span or link in it keeps its color and is struck through.
    gtk_text_buffer_create_tag(b, "md_done",
                               "foreground", pal::MdDone,
                               "strikethrough", TRUE, NULL);
    gtk_text_buffer_create_tag(b, "md_code",
                               "foreground", pal::MdCode,
                               "background", pal::MdCodeBg,
                               "family", "monospace",
                               "size-points", 13.0, NULL);
    gtk_text_buffer_create_tag(b, "md_quote",
                               "foreground", pal::MdQuote,
                               "left-margin", 16,   // macOS headIndent
                               "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(b, "md_rule",
                               "foreground", pal::MdRule, NULL);
    gtk_text_buffer_create_tag(b, "md_link",
                               "foreground", pal::MdLink,
                               "underline", PANGO_UNDERLINE_SINGLE, NULL);
    gtk_text_buffer_create_tag(b, "md_bold",
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(b, "md_italic",
                               "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(b, "md_strike",
                               "strikethrough", TRUE, NULL);
    // The empty line between two blocks (MdRun::gap), shorter than a line
    // of text, as the macOS build draws it.
    gtk_text_buffer_create_tag(b, "md_gap",
                               "size-points", 8.0, NULL);
    gtk_text_buffer_create_tag(b, "plainmsg",
                               "foreground", pal::MdPlainMsg, NULL);
}

// ---------------------------------------------------------------- the view

// Source code is monospace; the Markdown preview and the plain messages use the
// proportional UI font, with monospace reapplied per-tag for code and tables.
// The macOS build gets this from per-run NSFonts; GtkTextView needs the widget
// font switched, because a tag can add a family but the view's monospace flag
// would otherwise force every run to it.
//
// Prose also has to WRAP, and that is not cosmetic. MarkdownParser joins every
// consecutive non-blank line of a paragraph into one logical line, so with
// GTK_WRAP_NONE a paragraph became a single unwrapped line as wide as the whole
// paragraph is long. Pango measures in 1/1024 px stored in a signed int, which
// caps a line at 2^31/1024 = 2,097,152 px, about 281k characters at the preview
// font. Past that the width calculation overflows and the paragraph's layout
// collapses: the text goes missing and the view misreports its own size.
// Wrapping bounds every display line to the viewport width, so the overflow
// cannot be reached. Source wraps as editor.word-wrap and Alt+Z say
// (Zoom::wordWrap, PARITY.md 1.11), as the Mac's editor does, except in
// large-file mode, where wrapping is one of the features off, as in VS
// Code (GTK took half as long again to lay out 470,000 lines wrapped);
// a line too long to lay out is shown a part at a time, unwrapped, either
// way (LongLines, LargeFileView.h). macOS gets the same behavior from
// textContainer.widthTracksTextView = YES.
void Editor::setProseFont(bool prose) {
    prose_ = prose;
    const bool wraps = prose || (Zoom::wordWrap() && !(active_ && active_->largeFileFeaturesOff()));
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view_), prose ? FALSE : TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view_), wraps ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    if (prose) gtk_widget_add_css_class(view_, "minicode-prose");
    else       gtk_widget_remove_css_class(view_, "minicode-prose");
}

// Switching tabs is this: the view takes the document's buffer (D2), which
// keeps its own text, selection, undo and colors.
void Editor::setViewBuffer(GtkTextBuffer* b, bool prose, bool editable) {
    if (buffer_ != b) {
        code_->setBuffer(b);   // never gtk_text_view_set_buffer itself (CodeView.h)
        buffer_ = b;
        composing_ = false;   // a new buffer resets the input method
    }
    setProseFont(prose);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view_), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view_), editable);
}

GtkTextBuffer* Editor::sourceBuffer() const { return active_ ? active_->buffer() : buffer_; }

bool Editor::isSource() const {
    return active_ && active_->kind() == Document::Kind::Text && buffer_ == active_->buffer();
}

bool Editor::inMarkdownPage() const {
    return active_ && active_->isMarkdown() && active_->showsPreview() &&
           buffer_ == active_->page();
}

std::string Editor::text() const { return active_ ? active_->text() : std::string(); }

const std::string& Editor::currentPath() const {
    static const std::string none;
    return active_ ? active_->path() : none;
}

const std::string& Editor::diffTitle() const {
    static const std::string none;
    return isDiff() ? active_->diffTitle() : none;
}

// A large file with its features off, or one past the sync limit, is no
// document to a language server (LargeFileView.h), like a message.
void Editor::notifyDocument() {
    if (observer_)
        observer_->documentChanged(isSource() && active_->languageServerMayRead() ? active_->path()
                                                                                 : std::string());
}

void Editor::titleChanged() {
    if (titleCb_) titleCb_(titleUser_);
}

// The active tab leaves the screen: where its view was is kept in the
// document, and what the shared views hold for it goes (a playing video
// stops, the page's widgets come off the view), as the Mac's stashActiveTab.
void Editor::stash() {
    Document* d = active_;
    if (!d) return;
    // A color picked a moment ago belongs to the file it was picked in.
    if (colorSaveTimer_) saveColorNow();
    dropColorPopover();
    dropMdPopover();
    stopSettle();
    stopHold();
    // An animated scroll ends here, where it had got to, before the view
    // takes another buffer (ScrollSettle.h has the criticals it caused).
    settleScrolling(nullptr, scroller_);
    if (previewEntryTimer_) g_source_remove(previewEntryTimer_);
    previewEntryTimer_ = 0;
    if (arrivedTimer_) g_source_remove(arrivedTimer_);
    arrivedTimer_ = 0;
    arrived_.clear();

    capturePlace(d);
    stopPlace();
    if (d->isMarkdown()) clearPage(d);
#ifdef MINICODE_ENABLE_PDF
    if (d->latexPreview()) d->latexPreview()->setActive(false);
    if (d->kind() == Document::Kind::Media && media_->kind() == MediaView::Kind::Pdf) {
        d->place().pdf = media_->pdfView()->anchor();
        d->place().havePdf = true;
    }
#endif
    if (d->kind() == Document::Kind::Media) media_->clear();
    d->setShownIn(nullptr);
}

// Where the view is: its first line on show and how far into it the view
// starts. A tab left while its place is still coming back (settleOnPlace)
// keeps that place instead: what the view shows then is GTK's guess, and
// flicking through the tabs once moved every place passed to the top.
// While the caret was being brought on screen, the place is around it.
void Editor::capturePlace(Document* d) {
    Document::Place& p = d->place();
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv, &vis);
    GtkTextIter top;
    int into = 0;
    bool known = false;
    if (placeTick_ && placeMark_ && gtk_text_mark_get_buffer(placeMark_) == buffer_) {
        if (placeFollowCaret_) {
            gtk_text_buffer_get_iter_at_mark(buffer_, &top, gtk_text_buffer_get_insert(buffer_));
            gtk_text_iter_set_line_offset(&top, 0);
            into = -(int)(vis.height * 0.3);
        } else {
            gtk_text_buffer_get_iter_at_mark(buffer_, &top, placeMark_);
            into = placeInto_;
        }
        known = true;
    } else if (vis.height > 0) {
        int lineTop = 0;
        gtk_text_view_get_line_at_y(tv, &top, vis.y, &lineTop);
        into = vis.y - lineTop;
        known = true;
    }
    if (known && buffer_ == d->buffer() && d->kind() == Document::Kind::Text) {
        if (!p.top) p.top = gtk_text_buffer_create_mark(d->buffer(), nullptr, &top, TRUE);
        else gtk_text_buffer_move_mark(d->buffer(), p.top, &top);
        p.topInto = into;
        p.hscroll = gtk_adjustment_get_value(
            gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)));
        p.haveView = true;
    } else if (known && d->isMarkdown() && buffer_ == d->page()) {
        p.pageTop = gtk_text_iter_get_offset(&top);
        p.pageInto = into;
        p.havePage = true;
    }
}

// The active tab on screen, from what it kept (the Mac's showActiveTab); the
// welcome text with no tab open.
void Editor::showActive() {
    Document* d = active_;
    // A tab come back from a session is read when it is first shown, and
    // given the selection and view it came back with.
    if (d && d->unread()) {
        loadInto(d, d->path());
        if (d->place().haveRestore) {
            d->place().haveRestore = false;
            placeTab(d, d->place().restore);
        }
    }
    if (!d) {
        setViewBuffer(welcome_, true, false);
        gtk_stack_set_visible_child(GTK_STACK(slot_), scroller_);
        notifyDocument();
        refreshStrip();
        titleChanged();
        return;
    }
    switch (d->kind()) {
    case Document::Kind::Text:
        if (d->loading()) {   // read in the background: "Opening…" until it is in
            // In the source's font and wrapping, so the text coming on show
            // changes no style: in the prose font, the change cost 20 to 30
            // ms more at 470,000 lines.
            setViewBuffer(d->loadingBuffer(), false, false);
            gtk_stack_set_visible_child(GTK_STACK(slot_), scroller_);
        } else if (d->isMarkdown() && d->showsPreview()) {
            gtk_stack_set_visible_child(GTK_STACK(slot_), scroller_);
            renderPreview();
            const Document::Place& p = d->place();
            if (p.havePage) {
                GtkTextIter it;
                gtk_text_buffer_get_iter_at_offset(buffer_, &it,
                    std::min(p.pageTop, gtk_text_buffer_get_char_count(buffer_)));
                settleOnPlace(gtk_text_buffer_create_mark(buffer_, nullptr, &it, TRUE),
                              p.pageInto);
            }
        } else {
            showSource(d);
            if (d->isLatex() && d->showsPreview()) showLatex(true, false);
            else gtk_stack_set_visible_child(GTK_STACK(slot_), scroller_);
        }
        break;
    case Document::Kind::Message:
        setViewBuffer(d->buffer(), true, false);
        gtk_stack_set_visible_child(GTK_STACK(slot_), scroller_);
        break;
    case Document::Kind::Media:
        setViewBuffer(d->buffer(), true, false);
        if (!media_->show(d->path())) {
            // It no longer decodes: what the text path makes of it.
            loadText(d, d->path());
            showActive();
            return;
        }
        gtk_stack_set_visible_child(GTK_STACK(slot_), media_->widget());
#ifdef MINICODE_ENABLE_PDF
        if (d->place().havePdf && media_->kind() == MediaView::Kind::Pdf)
            media_->pdfView()->scrollTo(d->place().pdf);
#endif
        break;
    case Document::Kind::Diff:
        setViewBuffer(d->buffer(), true, false);
        if (!diff_) {
            diff_ = new GitDiffView();
            gtk_stack_add_named(GTK_STACK(slot_), diff_->widget(), "diff");
        }
        diff_->show(d->diffBytes(), d->diffIsCommit());
        gtk_stack_set_visible_child(GTK_STACK(slot_), diff_->widget());
        break;
    }
    notifyDocument();
    refreshStrip();
    titleChanged();
}

void Editor::showSource(Document* d) {
    const bool already = buffer_ == d->buffer();
    setViewBuffer(d->buffer(), false, true);
    d->setShownIn(GTK_TEXT_VIEW(view_));
    if (!already) restorePlace(d);
}

// The source's place as the tab left it: its first line back at the top,
// as far into it as it was, once the view has laid it out (holdTopLine),
// and the sideways scroll. A tab shown for the first time starts at the top,
// where set_buffer leaves the view.
void Editor::restorePlace(Document* d) {
    const Document::Place& p = d->place();
    if (!p.haveView || !p.top) return;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(d->buffer(), &it, p.top);
    settleOnPlace(gtk_text_buffer_create_mark(d->buffer(), nullptr, &it, TRUE), p.topInto);
    gtk_adjustment_set_value(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)),
                             p.hscroll);
}

// ---------------------------------------------------------------- documents

void Editor::documentStateChanged(Document* d) {
    // An edit makes the preview tab an ordinary one, as on the Mac.
    if (d->dirty() && d->isPreviewTab()) d->setPreviewTab(false);
    refreshStrip();
    if (d == active_) titleChanged();
}

// The language server hears of edits to the source on show, and a closer or
// ':' typed from the keyboard (see onViewKey) may move its line back
// (AutoIndent::typed). Return, Tab and Backspace are handled whole in
// onViewKey.
void Editor::documentEdited(Document* d, GtkTextIter* at, const char* inserted, int len) {
    if (onDocumentsChanged) onDocumentsChanged(d);
    if (d != active_ || buffer_ != d->buffer()) return;
    if (observer_) observer_->textEdited(inserted, len);
    if (!inserted) return;
    const char typed = typedKey_;
    typedKey_ = 0;
    if (typed && len == 1 && inserted[0] == typed && !d->showsPreview()) electric(at);
}

bool Editor::documentChangedOnDisk(Document* d) {
    return d == active_ && extCb_ && extCb_(extUser_);
}

void Editor::documentWillReload(Document* d) {
    reloadKeep_ = d == active_ && buffer_ == d->buffer();
    if (!reloadKeep_) return;
    reloadH_ = gtk_adjustment_get_value(
        gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)));
    reloadV_ = gtk_adjustment_get_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_)));
}

namespace {
// Scroll offsets, put back once the view has laid out the new text as well
// as straight away (the adjustment's range may lag the buffer by a frame).
struct ScrollKeep {
    GtkAdjustment* h;
    GtkAdjustment* v;
    double hv, vv;
};
void restoreScroll(ScrollKeep* k) {
    gtk_adjustment_set_value(k->h, k->hv);
    gtk_adjustment_set_value(k->v, k->vv);
}
}  // namespace

void Editor::documentReloaded(Document* d) {
    // A Markdown page shows the file's new text.
    if (d == active_ && inMarkdownPage()) rerenderPreview();
    if (reloadKeep_) {
        reloadKeep_ = false;
        auto* keep = new ScrollKeep{
            gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)),
            gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_)), reloadH_,
            reloadV_};
        g_object_ref(keep->h);
        g_object_ref(keep->v);
        restoreScroll(keep);
        g_idle_add_full(G_PRIORITY_LOW, [](gpointer p) -> gboolean {
            restoreScroll(static_cast<ScrollKeep*>(p));
            return G_SOURCE_REMOVE;
        }, keep, [](gpointer p) {
            auto* k = static_cast<ScrollKeep*>(p);
            g_object_unref(k->h);
            g_object_unref(k->v);
            delete k;
        });
    }
    if (d == active_) titleChanged();
}

// ---------------------------------------------------------------- saving and the disk

bool Editor::save(std::string* error) { return saveDocument(active_, error); }

bool Editor::saveDocument(Document* d, std::string* error) {
    if (!d || !d->canSave()) return true;
    if (!d->save(error)) return false;
    if (d->isPreviewTab()) keepPreviewTab(d);   // saved: kept, as in VS Code
    if (d == active_ && isSource() && observer_) observer_->documentSaved();
    refreshStrip();
    if (d == active_) titleChanged();
    return true;
}

void Editor::checkExternalChange() {
    const std::vector<Document*> docs = tabs_;   // a question may come up meanwhile
    for (Document* d : docs) d->checkExternalChange();
}

bool Editor::reloadFromDisk() { return active_ && active_->reloadFromDisk(); }

// ---------------------------------------------------------------- go to line

bool Editor::revealLine(int line, std::size_t byteColumn, std::size_t byteLength) {
    if (active_ && active_->loading()) {   // read in the background: once it is in
        Document* d = active_;
        d->whenLoaded([this, d, line, byteColumn, byteLength] {
            if (d == active_) revealLine(line, byteColumn, byteLength);
        });
        return true;
    }
    if (!active_ || active_->kind() != Document::Kind::Text) return false;
    if (active_->showsPreview()) togglePreview();
    if (!gtk_text_view_get_editable(GTK_TEXT_VIEW(view_))) return false;   // a message

    const int lines = gtk_text_buffer_get_line_count(buffer_);
    const int index = std::max(0, std::min(line - 1, lines - 1));
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(buffer_, &start, index);
    end = start;
    if (!gtk_text_iter_ends_line(&end)) gtk_text_iter_forward_to_line_end(&end);
    char* text = gtk_text_buffer_get_slice(buffer_, &start, &end, TRUE);
    const std::string lineText = text ? text : "";
    g_free(text);

    // Both ends must fall on character boundaries of the line as it is now.
    auto boundary = [&](std::size_t b) {
        return b == lineText.size() ||
               (b < lineText.size() &&
                (static_cast<unsigned char>(lineText[b]) & 0xC0) != 0x80);
    };
    if (line - 1 != index || !boundary(byteColumn) ||
        !boundary(byteColumn + byteLength)) {
        byteColumn = 0;
        byteLength = 0;
    }
    GtkTextIter a = start, b = start;
    gtk_text_iter_set_line_index(&a, static_cast<int>(byteColumn));
    gtk_text_iter_set_line_index(&b, static_cast<int>(byteColumn + byteLength));
    gtk_text_buffer_select_range(buffer_, &a, &b);
    stopPlace();   // a tab's place coming back gives way to the line asked for
    // Scroll by the insert mark rather than an iterator, so GTK scrolls once
    // the line is laid out, and then keep checking (settleOnCaret).
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_),
                                 0.1, TRUE, 0.0, 0.3);
    settleOnCaret();
    gtk_widget_grab_focus(view_);
    return true;
}

// A scroll to a line of a file opened a moment ago lands in the wrong place.
// GtkTextView works out where the line is from estimated heights for the
// lines above it that it has not laid out yet, and animates the scroll
// towards that point; the lines are laid out while it moves, their real
// heights differ, and the animation still ends at the old point. A Find in
// Folder match on line 250 of a new file ended with lines 166 to 208 on
// screen, the match selected but out of sight, which looks like the file
// opened at the top. So for up to two seconds the caret is checked every
// frame: once the view has stopped moving with the caret off screen, it is
// scrolled to again, now from real heights. It ends when the caret has been
// on screen with the view still for three frames, or when the caret moves
// (the user took over).
void Editor::settleOnCaret() {
    stopSettle();
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buffer_, &it, gtk_text_buffer_get_insert(buffer_));
    settleOffset_ = gtk_text_iter_get_offset(&it);
    settleUntil_ = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    settleLastV_ = -1;
    settleStill_ = 0;
    settleTries_ = 0;
    settleUpper_ = gtk_adjustment_get_upper(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view_)));
    settleCap_ = g_get_monotonic_time() + 180 * G_USEC_PER_SEC;
    settleTop_ = false;
    settleTick_ = gtk_widget_add_tick_callback(view_, onSettleTick, this, nullptr);
}

void Editor::stopSettle() {
    if (settleTick_) gtk_widget_remove_tick_callback(view_, settleTick_);
    settleTick_ = 0;
}

gboolean Editor::onSettleTick(GtkWidget*, GdkFrameClock*, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    GtkTextView* tv = GTK_TEXT_VIEW(self->view_);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(self->buffer_, &it,
                                     gtk_text_buffer_get_insert(self->buffer_));
    // The caret moved: the user has taken over, unless GTK is still laying
    // out a big buffer (below), when typing moves the caret and the view
    // must still follow it there.
    if (gtk_text_iter_get_offset(&it) != self->settleOffset_ && self->settleTop_)
        self->settleOffset_ = gtk_text_iter_get_offset(&it);
    if (gtk_text_iter_get_offset(&it) != self->settleOffset_ ||
        g_get_monotonic_time() > self->settleUntil_ || !gtk_widget_get_mapped(self->view_)) {
        self->settleTick_ = 0;
        return G_SOURCE_REMOVE;
    }
    GdkRectangle vis, at;
    gtk_text_view_get_visible_rect(tv, &vis);
    gtk_text_view_get_iter_location(tv, &it, &at);
    const bool seen = vis.height > 0 && at.y >= vis.y && at.y + at.height <= vis.y + vis.height;
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv));
    const double v = gtk_adjustment_get_value(adj);
    const bool still = v == self->settleLastV_;
    self->settleLastV_ = v;
    // While GTK is still laying out a big buffer its height grows, and lines
    // it has not laid out have no height, so a line far up can share the
    // caret's y. GTK keeps the view on its top line meanwhile: with the
    // caret a third of the way down, that was such a line far up, and the
    // caret drifted off as the lines between got their heights. So until
    // the height stops growing the caret's line is put at the very top,
    // where it is the one GTK keeps, and a third of the way down after.
    if (gtk_adjustment_get_upper(adj) != self->settleUpper_) {
        self->settleUpper_ = gtk_adjustment_get_upper(adj);
        self->settleTries_ = 0;
        self->settleStill_ = 0;
        self->settleUntil_ = std::min(self->settleCap_,
                                      std::max(self->settleUntil_, g_get_monotonic_time() + G_USEC_PER_SEC));
        int y = 0, h = 0;
        gtk_text_view_get_line_yrange(tv, &it, &y, &h);
        // Shown means among the lines drawn: seen, with the top line no
        // more lines up than fit.
        GtkTextIter top;
        gtk_text_view_get_line_at_y(tv, &top, vis.y + 1, nullptr);
        const int below = gtk_text_iter_get_line(&it) - gtk_text_iter_get_line(&top);
        const bool shown = seen && below >= 0 && h > 0 && below <= vis.height / h;
        // A pixel into the line, past the top margin: at its top edge, lines
        // with no height yet share the pixel, and GTK keeps the first of
        // them instead.
        const int in = gtk_text_view_get_top_margin(tv) + 1;
        if (!shown || self->settleTop_) {
            if (y + in != vis.y) {
                gtk_adjustment_set_value(adj, v + (y + in - vis.y));
                self->settleLastV_ = gtk_adjustment_get_value(adj);
            }
            self->settleTop_ = true;
        }
        return G_SOURCE_CONTINUE;
    }
    if (still && seen && self->settleTop_) {
        self->settleTop_ = false;
        self->settleStill_ = 0;
        gtk_text_view_scroll_to_mark(tv, gtk_text_buffer_get_insert(self->buffer_), 0.1, TRUE, 0.0, 0.3);
        return G_SOURCE_CONTINUE;
    }
    if (!still) {
        self->settleStill_ = 0;
    } else if (seen) {
        if (++self->settleStill_ >= 3) {
            self->settleTick_ = 0;
            return G_SOURCE_REMOVE;
        }
    } else if (self->settleTries_++ < 5) {
        self->settleStill_ = 0;
        gtk_text_view_scroll_to_mark(tv, gtk_text_buffer_get_insert(self->buffer_),
                                     0.1, TRUE, 0.0, 0.3);
    }
    return G_SOURCE_CONTINUE;
}

void Editor::stopSettling() {
    stopPlace();
    stopSettle();
    stopHold();
}

bool Editor::revealLineColumn(int line, int column) {
    if (active_ && active_->loading()) {   // read in the background: once it is in
        Document* d = active_;
        d->whenLoaded([this, d, line, column] {
            if (d == active_) revealLineColumn(line, column);
        });
        return true;
    }
    // revealLine first: it switches a preview back to the source, so the
    // buffer holds the lines the column counts in.
    if (!revealLine(line, 0, 0)) return false;
    if (column <= 1) return true;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buffer_, &it, gtk_text_buffer_get_insert(buffer_));
    for (int c = 1; c < column && !gtk_text_iter_ends_line(&it); c++)
        gtk_text_iter_forward_char(&it);
    gtk_text_buffer_place_cursor(buffer_, &it);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_),
                                 0.1, TRUE, 0.0, 0.3);
    settleOnCaret();   // the caret moved; watch it where it is now
    return true;
}

// ---------------------------------------------------------------- media, PDFs, LaTeX

bool Editor::isMedia() const {
    return active_ && active_->kind() == Document::Kind::Media &&
           media_->kind() != MediaView::Kind::None;
}

bool Editor::canPlayMedia() const { return isMedia() && media_->canPlay(); }

void Editor::togglePlayMedia() {
    if (isMedia()) media_->togglePlay();
}

std::string Editor::titleSuffix() const { return isMedia() ? media_->titleSuffix() : ""; }

// The active tab's LaTeX preview: its own, made on first use and kept with
// its pages until the tab closes, as each Mac tab has its own LatexView.
LatexPreview* Editor::makeLatex(Document* d) {
#ifdef MINICODE_ENABLE_PDF
    if (!d->latexPreview()) {
        LatexPreview* l = new LatexPreview(d->buffer());
        l->pdfView()->setShownCallback(onPdfShown, this);
        gtk_stack_add_child(GTK_STACK(slot_), l->widget());
        d->setLatexPreview(l);
    }
    return d->latexPreview();
#else
    (void)d;
    return nullptr;
#endif
}

void Editor::showLatex(bool on, bool focus) {
#ifdef MINICODE_ENABLE_PDF
    Document* d = active_;
    if (!d) return;
    LatexPreview* l = makeLatex(d);
    if (on) {
        l->setPath(d->path());
        gtk_stack_set_visible_child(GTK_STACK(slot_), l->widget());
        l->setActive(true);
    } else {
        l->setActive(false);
        gtk_stack_set_visible_child(GTK_STACK(slot_), scroller_);
        if (focus) gtk_widget_grab_focus(view_);
    }
#else
    (void)on;
    (void)focus;
#endif
}

bool Editor::exportPdf(GtkWindow* parent) {
#ifdef MINICODE_ENABLE_PDF
    Document* d = active_;
    if (!d || !d->isLatex() || d->path().empty()) return false;
    LatexPreview* l = makeLatex(d);   // made hidden, to typeset for the export
    l->setPath(d->path());
    l->exportPdf(parent);
    return true;
#else
    (void)parent;
    return false;
#endif
}

void Editor::onMediaChanged(void* selfp) {
    static_cast<Editor*>(selfp)->titleChanged();   // page count may differ
}

void Editor::onPdfShown(void* selfp) {
    static_cast<Editor*>(selfp)->titleChanged();   // the zoom keys follow
}

bool Editor::showingPdf() const {
#ifdef MINICODE_ENABLE_PDF
    if (media_->pdfView()->shown()) return true;
    if (latex() && latex()->pdfView()->shown()) return true;
#endif
    return false;
}

bool Editor::zoomPdf(int step) {
#ifdef MINICODE_ENABLE_PDF
    PdfView* pdf = nullptr;
    if (media_->pdfView()->shown()) pdf = media_->pdfView();
    else if (latex() && latex()->pdfView()->shown()) pdf = latex()->pdfView();
    if (!pdf) return false;
    if (step > 0) pdf->zoomIn();
    else if (step < 0) pdf->zoomOut();
    else pdf->zoomToFit();
    return true;
#else
    (void)step;
    return false;
#endif
}

// ---------------------------------------------------------------- settings

static GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}

static void setTagColor(GtkTextTagTable* table, const char* name, const char* prop,
                        const Rgba& c) {
    GtkTextTag* tag = gtk_text_tag_table_lookup(table, name);
    if (!tag) return;
    GdkRGBA rgba = toGdk(c);
    g_object_set(tag, prop, &rgba, NULL);
}

void Editor::applySettings(const Settings& s) {
    const bool webBefore = settings_.webImages();
    settings_ = s;
    code_->applySettings(s);   // the ruler and the gutter
    // Bracket boxes, and the bracket colors' tags (every tab's) and whether
    // each tab has them.
    brackets_->applySettings(s);
    BracketColors::applySettings(tags_.table, s);
    for (Document* d : tabs_) d->bracketSettingChanged();
    // The tags are every tab's: each recolors with them.
    GtkTextTagTable* t = tags_.table;
    for (int i = 0; i < kTokenStyleCount; ++i)
        setTagColor(t, TagNameForStyle((TokenStyle)i), "foreground-rgba", s.syntax((TokenStyle)i));
    for (int lvl = 1; lvl <= 6; ++lvl) {
        char name[16];
        g_snprintf(name, sizeof(name), "h%d", lvl);
        setTagColor(t, name, "foreground-rgba", s.markdown(MarkdownColor::Heading));
    }
    setTagColor(t, "md_table", "foreground-rgba", s.text(Surface::Editor));
    setTagColor(t, "md_code", "foreground-rgba", s.markdown(MarkdownColor::Code));
    setTagColor(t, "md_code", "background-rgba", s.markdownCodeBackground());
    setTagColor(t, "md_quote", "foreground-rgba", s.markdown(MarkdownColor::Quote));
    setTagColor(t, "md_link", "foreground-rgba", s.markdown(MarkdownColor::Link));
    setTagColor(t, "md_done", "foreground-rgba", s.markdown(MarkdownColor::Done));
    // Swatch text contrast depends on the editor background.
    for (Document* d : tabs_)
        if (d->isSettingsFile()) d->restartHighlighting();
    // Web pictures turned on or off: the preview shows them, or their alt
    // text, from now on.
    if (s.webImages() != webBefore && inMarkdownPage()) {
        GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
        const double entry = active_->place().previewEntryV;
        const bool scrolled = entry >= 0 && std::fabs(gtk_adjustment_get_value(v) - entry) > 2;
        rerenderPreview();
        if (!scrolled) notePreviewRest();   // still where Ctrl+Shift+V left it
    }
}

bool Editor::isSettingsFile() const { return active_ && active_->isSettingsFile(); }

// Is there a swatch under widget point (x, y)? Reports its line.
bool Editor::swatchAt(double x, double y, int* line) const {
    if (!isSource() || !isSettingsFile()) return false;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET,
                                          (int)x, (int)y, &bx, &by);
    GtkTextIter it;
    if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(view_), &it, bx, by))
        return false;
    GSList* tags = gtk_text_iter_get_tags(&it);
    bool found = false;
    for (GSList* l = tags; l; l = l->next) {
        char* name = nullptr;
        g_object_get(l->data, "name", &name, NULL);
        if (name && g_str_has_prefix(name, "swatch-")) found = true;
        g_free(name);
    }
    g_slist_free(tags);
    if (found && line) *line = gtk_text_iter_get_line(&it);
    return found;
}

// The span of a Markdown preview under a point in the view, or null. A
// table or picture is its own widget and takes its own clicks.
static const Markdown::Span* previewSpanAt(GtkWidget* view, const Markdown::Page& page,
                                           double x, double y, GtkTextIter* iter) {
    if (page.spans.empty()) return nullptr;
    GtkWidget* picked = gtk_widget_pick(view, x, y, GTK_PICK_DEFAULT);
    for (const Markdown::Embed& e : page.embeds)
        if (picked && (picked == e.widget || gtk_widget_is_ancestor(picked, e.widget)))
            return nullptr;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(view), GTK_TEXT_WINDOW_WIDGET,
                                          (int)x, (int)y, &bx, &by);
    GtkTextIter it;
    if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(view), &it, bx, by)) return nullptr;
    // The iterator is the character nearest the point, or the end of the
    // line when the point is past it: that one does not count.
    GdkRectangle box;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view), &it, &box);
    if (gtk_text_iter_ends_line(&it) && bx > box.x + std::max(box.width, 1)) return nullptr;
    if (iter) *iter = it;
    return page.spanAt(gtk_text_iter_get_offset(&it));
}

void Editor::onMotion(GtkEventControllerMotion*, double x, double y, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    bool over = self->swatchAt(x, y, nullptr);
    if (!over && self->inMarkdownPage()) {
        const Markdown::Span* sp =
            previewSpanAt(self->view_, self->active_->pageInfo(), x, y, nullptr);
        over = sp && !sp->url.empty();
    }
    if (over == self->overSwatch_) return;
    self->overSwatch_ = over;
    gtk_widget_set_cursor_from_name(self->view_, over ? "pointer" : "text");
}

void Editor::onPressed(GtkGestureClick* g, int n, double x, double y, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    if (self->inMarkdownPage()) {
        GtkTextIter it;
        const Markdown::Span* sp =
            previewSpanAt(self->view_, self->active_->pageInfo(), x, y, &it);
        if (!sp) return;
        if (n == 1 && !sp->url.empty()) {
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
            // Once the click is over: the text view still puts the caret
            // where it landed, which would undo a scroll to "#section".
            struct Job {
                std::weak_ptr<int> alive;
                Editor* editor;
                std::string url;
            };
            auto* job = new Job{self->alive_, self, sp->url};
            g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer d) -> gboolean {
                auto* j = static_cast<Job*>(d);
                if (!j->alive.expired()) j->editor->openMarkdownLink(j->url);
                return G_SOURCE_REMOVE;
            }, job, [](gpointer d) { delete static_cast<Job*>(d); });
            return;
        }
        if (n == 2) {
            GdkRectangle box;
            gtk_text_view_get_iter_location(GTK_TEXT_VIEW(self->view_), &it, &box);
            int wx, wy;
            gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(self->view_),
                                                  GTK_TEXT_WINDOW_WIDGET, box.x, box.y, &wx, &wy);
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
            self->editMarkdownBlock(sp->line, -1, GdkRectangle{wx, wy, std::max(box.width, 1),
                                                                std::max(box.height, 1)});
        }
        return;
    }
    int line;
    if (n != 1 || !self->swatchAt(x, y, &line)) return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    self->pickColor(line);
}

static Rgba fromGdk(const GdkRGBA& c) {
    auto byte = [](float v) {
        return (uint8_t)(std::min(std::max(v, 0.0f), 1.0f) * 255 + 0.5f);
    };
    Rgba rgba;
    rgba.r = byte(c.red);
    rgba.g = byte(c.green);
    rgba.b = byte(c.blue);
    rgba.a = std::min(std::max((double)c.alpha, 0.0), 1.0);
    return rgba;
}

// The color picker, live like the Mac's NSColorPanel: a popover under the
// swatch holding GTK's color chooser, opened on its editor (the plane, hue and
// alpha sliders and the hex entry) at the swatch's color. Every change the
// chooser reports rewrites the line at once, and the file is saved 150 ms
// after the last one, so the window follows the drag. GtkColorDialog, used
// before, reports a color only when it closes.
//
// GtkColorChooserWidget is deprecated since GTK 4.10 with no replacement that
// can be embedded (GtkColorDialog is a separate window and reports once), so
// the few calls that need it are wrapped to silence that warning.
void Editor::pickColor(int line) {
    GtkTextIter ls, le;
    gtk_text_buffer_get_iter_at_line(buffer_, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
    char* raw = gtk_text_buffer_get_text(buffer_, &ls, &le, TRUE);
    std::u16string text = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);
    ColorSpan span;
    if (!Settings::findColor(text, span)) return;
    dropColorPopover();

    // Point at the swatch itself.
    GtkTextIter a = ls, b = ls;
    gtk_text_iter_forward_chars(&a, (int)utf16::toCharOffset(text, span.start));
    gtk_text_iter_forward_chars(&b, (int)utf16::toCharOffset(text, span.start + span.length));
    GdkRectangle ra, rb;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view_), &a, &ra);
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view_), &b, &rb);
    int x, y;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(view_), GTK_TEXT_WINDOW_WIDGET,
                                          ra.x, ra.y, &x, &y);
    GdkRectangle at = {x, y, std::max(1, rb.x - ra.x), std::max(1, ra.height)};

    GdkRGBA initial = toGdk(span.color);
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkWidget* chooser = gtk_color_chooser_widget_new();
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(chooser), TRUE);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(chooser), &initial);
G_GNUC_END_IGNORE_DEPRECATIONS
    // The editor rather than the palette: the point is to adjust the color
    // that is there, and the editor starts on it.
    g_object_set(chooser, "show-editor", TRUE, NULL);
    // Connected after the color is set, so opening changes nothing.
    g_signal_connect(chooser, "notify::rgba", G_CALLBACK(onColorPicked), this);

    GtkWidget* pop = gtk_popover_new();
    gtk_widget_add_css_class(pop, "minicode-color-popover");
    gtk_popover_set_child(GTK_POPOVER(pop), chooser);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &at);
    gtk_widget_set_parent(pop, view_);
    // Double-clicking a color in the palette means "this one, done".
    g_signal_connect_swapped(chooser, "color-activated",
                             G_CALLBACK(gtk_popover_popdown), pop);
    // Closing: the last pick is saved now rather than 150 ms later, and the
    // popover comes off the view from an idle, since it is still emitting
    // its own signal here.
    g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer selfp) {
        Editor* self = static_cast<Editor*>(selfp);
        if (self->colorSaveTimer_) self->saveColorNow();
        if (self->colorPop_ == GTK_WIDGET(p)) self->colorPop_ = nullptr;
        g_idle_add([](gpointer w) -> gboolean {
            GtkWidget* widget = GTK_WIDGET(w);
            if (gtk_widget_get_parent(widget)) gtk_widget_unparent(widget);
            g_object_unref(widget);
            return G_SOURCE_REMOVE;
        }, g_object_ref(p));
    }), this);

    colorPop_ = pop;
    colorLine_ = line;
    colorDoc_ = active_;
    gtk_popover_popup(GTK_POPOVER(pop));
}

void Editor::onColorPicked(GObject* chooser, GParamSpec*, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    if (!self->colorPop_ || self->active_ != self->colorDoc_ || !self->isSource()) return;
    GdkRGBA c;
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(chooser), &c);
G_GNUC_END_IGNORE_DEPRECATIONS
    self->setLineColor(self->colorLine_, fromGdk(c));
    self->scheduleColorSave();
}

void Editor::scheduleColorSave() {
    if (colorSaveTimer_) g_source_remove(colorSaveTimer_);
    colorSaveTimer_ = g_timeout_add(150, [](gpointer p) -> gboolean {
        Editor* self = static_cast<Editor*>(p);
        self->colorSaveTimer_ = 0;
        self->saveColorNow();
        return G_SOURCE_REMOVE;
    }, this);
}

// The settings file the picker edits, written now (saving it is what
// applies the color).
void Editor::saveColorNow() {
    if (colorSaveTimer_) g_source_remove(colorSaveTimer_);
    colorSaveTimer_ = 0;
    if (colorDoc_) saveDocument(colorDoc_, nullptr);
}

// Take the popover down at once, without its "closed" handler: the file is
// being switched or closed, or the view is going away.
void Editor::dropColorPopover() {
    if (!colorPop_) return;
    GtkWidget* pop = colorPop_;
    colorPop_ = nullptr;
    g_signal_handlers_disconnect_by_data(pop, this);
    if (GtkWidget* chooser = gtk_popover_get_child(GTK_POPOVER(pop)))
        g_signal_handlers_disconnect_by_data(chooser, this);
    if (gtk_widget_get_parent(pop)) gtk_widget_unparent(pop);
}

void Editor::onViewUnrealize(GtkWidget*, gpointer selfp) {
    static_cast<Editor*>(selfp)->dropColorPopover();
    static_cast<Editor*>(selfp)->dropMdPopover();
}

// Rewrite one line's color (uncommenting it if it was a commented-out
// default). The caller saves, which is what applies it.
void Editor::setLineColor(int line, const Rgba& c) {
    if (!isSource() || line < 0 || line >= gtk_text_buffer_get_line_count(buffer_)) return;
    GtkTextIter ls, le;
    gtk_text_buffer_get_iter_at_line(buffer_, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
    char* raw = gtk_text_buffer_get_text(buffer_, &ls, &le, TRUE);
    std::u16string text = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);
    std::u16string updated = Settings::setColor(text, c);
    if (updated == text) return;
    std::string out = utf16::toUtf8(updated);

    // The caret stays where it was rather than jumping to the line's end.
    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(buffer_, &cur, gtk_text_buffer_get_insert(buffer_));
    const int curLine = gtk_text_iter_get_line(&cur);
    const int curCol = gtk_text_iter_get_line_offset(&cur);

    gtk_text_buffer_begin_user_action(buffer_);
    gtk_text_buffer_delete(buffer_, &ls, &le);
    gtk_text_buffer_insert(buffer_, &ls, out.c_str(), (int)out.size());
    gtk_text_buffer_end_user_action(buffer_);

    if (curLine == line) {
        GtkTextIter back;
        gtk_text_buffer_get_iter_at_line(buffer_, &back, line);
        const int len = gtk_text_iter_get_chars_in_line(&back);
        gtk_text_iter_set_line_offset(&back, std::min(curCol, std::max(0, len - 1)));
        gtk_text_buffer_place_cursor(buffer_, &back);
    }
}

// ---------------------------------------------------------------- comments

bool Editor::toggleComment() {
    if (!isSource() || inPreview() || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)))
        return false;
    const std::string marker = LineComments::markerFor(active_->path());
    if (marker.empty()) return false;

    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    // Hidden text too (a fold, a long line's rest): the offsets below are
    // the buffer's.
    char* raw = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    const std::u16string text = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);

    GtkTextIter selA, selB;
    gtk_text_buffer_get_selection_bounds(buffer_, &selA, &selB);
    const size_t s = utf16::fromCharOffset(text, gtk_text_iter_get_offset(&selA));
    const size_t e = utf16::fromCharOffset(text, gtk_text_iter_get_offset(&selB));
    LineComments::Result r = LineComments::toggle(text, s, e, marker);
    if (!r.changed) return true;

    GtkTextIter from, to;
    gtk_text_buffer_get_iter_at_offset(buffer_, &from,
        (int)utf16::toCharOffset(text, r.replaceStart));
    gtk_text_buffer_get_iter_at_offset(buffer_, &to,
        (int)utf16::toCharOffset(text, r.replaceStart + r.replaceLength));
    const std::string replacement = utf16::toUtf8(r.replacement);
    // One user action, so one Ctrl+Z undoes the whole toggle.
    gtk_text_buffer_begin_user_action(buffer_);
    gtk_text_buffer_delete(buffer_, &from, &to);
    gtk_text_buffer_insert(buffer_, &from, replacement.c_str(), (int)replacement.size());
    gtk_text_buffer_end_user_action(buffer_);

    GtkTextIter ns, ne;
    gtk_text_buffer_get_iter_at_offset(buffer_, &ns, (int)utf16::toCharOffset(r.text, r.selStart));
    gtk_text_buffer_get_iter_at_offset(buffer_, &ne, (int)utf16::toCharOffset(r.text, r.selEnd));
    gtk_text_buffer_select_range(buffer_, &ns, &ne);
    return true;
}

// ---------------------------------------------------------------- preview

void Editor::togglePreview() {
    Document* d = active_;
    if (!d) return;
    if (d->isLatex()) {
        d->setShowsPreview(!d->showsPreview());
        showLatex(d->showsPreview(), true);
        return;
    }
    if (!d->isMarkdown()) return;
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    Document::Place& p = d->place();
    int caretLine = -1, previewLine = -1;
    if (!d->showsPreview()) {
        // Leaving the source: remember the caret's height in the pane. The
        // selection stays in the source buffer, which the page leaves alone.
        GtkTextIter a, b;
        gtk_text_buffer_get_selection_bounds(d->buffer(), &a, &b);
        caretLine = gtk_text_iter_get_line(&a);
        GdkRectangle loc, vis;
        gtk_text_view_get_iter_location(tv, &a, &loc);
        gtk_text_view_get_visible_rect(tv, &vis);
        p.srcCaretFrac = vis.height > 0
            ? std::min(0.9, std::max(0.0, (double)(loc.y - vis.y) / vis.height)) : 0.3;
        p.haveSrcPos = true;
    } else if (p.previewEntryV >= 0 &&
               std::fabs(gtk_adjustment_get_value(v) - p.previewEntryV) > 2) {
        // Leaving a preview that was scrolled: the source opens where it was.
        GdkRectangle vis;
        gtk_text_view_get_visible_rect(tv, &vis);
        GtkTextIter top;
        gtk_text_view_get_iter_at_location(tv, &top, vis.x, vis.y + 4);
        const int at = gtk_text_iter_get_offset(&top);
        for (const Markdown::Span& sp : d->pageInfo().spans)
            if (sp.end > at && sp.line >= 0) { previewLine = sp.line; break; }
    }
    if (previewEntryTimer_) g_source_remove(previewEntryTimer_);
    previewEntryTimer_ = 0;
    stopSettle();
    stopHold();
    stopPlace();
    settleScrolling(nullptr, scroller_);

    if (!d->showsPreview()) {
        d->setShowsPreview(true);
        d->setShownIn(nullptr);
        p.havePage = false;
        renderPreview();
        d->refreshTaskCount();   // typing a moment ago may not be counted yet
        // The part of the page the caret was in, a third of the way down.
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(buffer_, &at,
            d->pageInfo().offsetForLine(caretLine, gtk_text_buffer_get_char_count(buffer_)));
        gtk_text_buffer_place_cursor(buffer_, &at);
        gtk_text_view_scroll_to_mark(tv, gtk_text_buffer_get_insert(buffer_), 0.0, TRUE, 0.0,
                                     caretLine <= 0 ? 0.0 : 0.33);
        settleOnCaret();
        // Where the preview came to rest, once it has: a scroll after that
        // is the user's.
        notePreviewRest();
    } else {
        dropMdPopover();
        clearPage(d);
        d->setShowsPreview(false);
        setViewBuffer(d->buffer(), false, true);
        d->setShownIn(tv);
        GtkTextIter a, b;
        double yalign = 0.0;
        if (previewLine >= 0) {
            gtk_text_buffer_get_iter_at_line(buffer_, &a, previewLine);
            gtk_text_buffer_select_range(buffer_, &a, &a);
        } else if (p.haveSrcPos) {
            // The source's own selection, kept in its buffer, as it was.
            gtk_text_buffer_get_selection_bounds(buffer_, &a, &b);
            yalign = p.srcCaretFrac;
        } else {
            gtk_text_buffer_get_start_iter(buffer_, &a);
            gtk_text_buffer_select_range(buffer_, &a, &a);
        }
        gtk_text_view_scroll_to_mark(tv, gtk_text_buffer_get_insert(buffer_), 0.0, TRUE, 0.0,
                                     yalign);
        settleOnCaret();
        notifyDocument();
    }
}

// The active Markdown file's page, rendered into a buffer of its own (D3)
// that the view then shows. The source buffer, its undo and its colors are
// not touched.
void Editor::renderPreview() {
    Document* d = active_;
    if (!d) return;
    stopHold();
    stopPlace();
    clearPage(d);
    GtkTextBuffer* page = d->page();
    // The view shows the page before the page's widgets are anchored in it:
    // an anchor's widget is added through the view of its buffer.
    setViewBuffer(page, true, false);
    Markdown::Hooks hooks;
    char* dir = g_path_get_dirname(d->path().c_str());
    hooks.folder = dir ? dir : ".";
    g_free(dir);
    hooks.webImages = settings_.webImages() && WebImages::available();
    std::weak_ptr<int> alive = alive_;
    hooks.link = [this, alive](const std::string& url) {
        if (!alive.expired()) openMarkdownLink(url);
    };
    hooks.cellDoubleClick = [this, alive](int line, int row, int column, GtkWidget* cell) {
        if (alive.expired()) return;
        graphene_rect_t r;
        GdkRectangle at{0, 0, 1, 1};
        if (gtk_widget_compute_bounds(cell, view_, &r))
            at = GdkRectangle{(int)r.origin.x, (int)r.origin.y, std::max(1, (int)r.size.width),
                              std::max(1, (int)r.size.height)};
        // The separator row sits between the header and the first body row.
        editMarkdownBlock(line + row + (row > 0 ? 1 : 0), column, at);
    };
    hooks.taskToggle = [this, alive](int line) {
        if (!alive.expired()) toggleTaskBox(line);
    };
    const double renderStart = StartupLog::cpuMs();
    d->pageInfo() = Markdown::render(GTK_TEXT_VIEW(view_), page, d->text(), hooks);
    StartupLog::once("first-page", "editor: first Markdown page rendered (%.1f ms of cpu)",
                     StartupLog::cpuMs() - renderStart);
    fitPage();
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(page, &start);
    gtk_text_buffer_place_cursor(page, &start);
    notifyDocument();
    fetchWebPictures();
}

// The page's tables, pictures and task boxes come off the view before the
// text holding their anchors is replaced, or the view shows another buffer.
// Left to the buffer's deletion, each child is unmapped from inside
// gtk_text_buffer_set_text, and a child that is hovered or pressed changes
// the view's state flags there; the view's handler reads the selection from
// a buffer half deleted and crashes (GTK 4.22). A task box re-renders the
// page while the pointer is on it and its press is in progress, which
// reached this every time after the edit popover had been open once.
void Editor::clearPage(Document* d) {
    if (!d) return;
    for (const Markdown::Embed& e : d->pageInfo().embeds)
        if (e.widget && gtk_widget_get_parent(e.widget) == view_)
            gtk_text_view_remove(GTK_TEXT_VIEW(view_), e.widget);
    d->pageInfo() = Markdown::Page();
}

void Editor::notePreviewRest() {
    if (previewEntryTimer_) g_source_remove(previewEntryTimer_);
    if (active_) active_->place().previewEntryV = -1;
    previewEntryTimer_ = g_timeout_add(700, [](gpointer selfp) -> gboolean {
        Editor* self = static_cast<Editor*>(selfp);
        self->previewEntryTimer_ = 0;
        if (self->active_)
            self->active_->place().previewEntryV = gtk_adjustment_get_value(
                gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scroller_)));
        return G_SOURCE_REMOVE;
    }, this);
}

// Rendered again in place (an edit from the preview, its undo, a setting):
// the same scroll offset, set now and again once the new text is laid out.
void Editor::rerenderPreview() {
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double keep = gtk_adjustment_get_value(v);
    renderPreview();
    gtk_adjustment_set_value(v, keep);
    auto* again = new std::pair<GtkAdjustment*, double>(GTK_ADJUSTMENT(g_object_ref(v)), keep);
    g_idle_add_full(G_PRIORITY_LOW, [](gpointer d) -> gboolean {
        auto* p = static_cast<std::pair<GtkAdjustment*, double>*>(d);
        gtk_adjustment_set_value(p->first, p->second);
        return G_SOURCE_REMOVE;
    }, again, [](gpointer d) {
        auto* p = static_cast<std::pair<GtkAdjustment*, double>*>(d);
        g_object_unref(p->first);
        delete p;
    });
}

// ---------------------------------------------------------------- web pictures
//
// The render showed the alt text of every web picture it did not have; each
// address is asked for once (WebImages shares one fetch among all who ask).
// Arrivals are collected and placed 100 ms after the first, so a page of
// badges settles in a few steps rather than one per badge. Each is put in
// place of its alt text in the buffer as it stands, not by a new render:
// GtkTextView keeps the first paragraph on screen where it is when lines
// above it change height, so the part of the page being read stays put.
// A result for a page that is gone (another tab, the source, a new render
// without that picture) finds nothing pending and is dropped; the page
// rendered again later finds the picture kept in memory.

void Editor::fetchWebPictures() {
    if (!active_) return;
    std::weak_ptr<int> alive = alive_;
    std::vector<std::string> asked;
    for (const Markdown::PendingPicture& p : active_->pageInfo().pending) {
        if (std::find(asked.begin(), asked.end(), p.url) != asked.end()) continue;
        asked.push_back(p.url);
        const std::string url = p.url;
        WebImages::fetch(url, [this, alive, url](WebImages::PicturePtr pic) {
            if (alive.expired()) return;
            arrived_.emplace_back(url, std::move(pic));
            if (!arrivedTimer_)
                arrivedTimer_ = g_timeout_add(100, [](gpointer selfp) -> gboolean {
                    Editor* self = static_cast<Editor*>(selfp);
                    self->arrivedTimer_ = 0;
                    self->placeArrivedPictures();
                    return G_SOURCE_REMOVE;
                }, this);
        });
    }
}

void Editor::placeArrivedPictures() {
    auto batch = std::move(arrived_);
    arrived_.clear();
    if (!inMarkdownPage() || active_->pageInfo().pending.empty()) return;
    Markdown::Page& page = active_->pageInfo();
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    // A preview the user has not scrolled yet is not scrolled by this
    // either, as far as Ctrl+Shift+V is concerned.
    const double entry = active_->place().previewEntryV;
    const bool scrolled = entry >= 0 && std::fabs(gtk_adjustment_get_value(v) - entry) > 2;
    // The line at the top of the view, and how far into it the view starts.
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv, &vis);
    GtkTextIter top;
    int topY = 0;
    gtk_text_view_get_line_at_y(tv, &top, vis.y, &topY);
    GtkTextMark* topMark = gtk_text_buffer_create_mark(buffer_, nullptr, &top, TRUE);

    int placed = 0;
    for (const auto& a : batch)
        placed += Markdown::placeWebPicture(tv, buffer_, page, a.first, a.second);
    g_log("minicode-web", G_LOG_LEVEL_DEBUG, "placed %d pictures from %zu arrivals, %zu pending",
          placed, batch.size(), page.pending.size());
    if (!placed) {
        gtk_text_buffer_delete_mark(buffer_, topMark);
        return;
    }
    fitPage();
    if (!scrolled && !previewEntryTimer_) notePreviewRest();
    // Not while a scroll to the caret is settling (the preview has just
    // come up), which is placing the view itself, nor at the very top.
    if (vis.y > 0 && !settleTick_) holdTopLine(topMark, vis.y - topY);
    else gtk_text_buffer_delete_mark(buffer_, topMark);
}

// GtkTextView keeps the line at the top of the view in place by itself when
// lines above it change height: it moves the scroll offset down as far as
// they grew. But when two pictures are laid out in one pass it moves for
// the second before the adjustment's upper bound has grown with the first,
// and near the end of a page that move is cut short (two 300-pixel
// pictures left the text 35 pixels lower). So for a few frames after
// pictures are placed, the line that was at the top is put back where it
// was, until it stays there. A scroll by the user ends it at once.
// (In the container the first move put it right every time.)
void Editor::holdTopLine(GtkTextMark* mark, int into) {
    stopHold();
    holdMark_ = mark;
    holdInto_ = into;
    holdUntil_ = g_get_monotonic_time() + G_USEC_PER_SEC / 2;
    holdStill_ = 0;
    holdMoves_ = 0;
    holdTick_ = gtk_widget_add_tick_callback(view_, onHoldTick, this, nullptr);
}

void Editor::stopHold() {
    if (holdTick_) gtk_widget_remove_tick_callback(view_, holdTick_);
    holdTick_ = 0;
    if (holdMark_ && !gtk_text_mark_get_deleted(holdMark_))
        gtk_text_buffer_delete_mark(gtk_text_mark_get_buffer(holdMark_), holdMark_);
    holdMark_ = nullptr;
}

gboolean Editor::onHoldTick(GtkWidget*, GdkFrameClock*, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    GtkTextView* tv = GTK_TEXT_VIEW(self->view_);
    if (!self->holdMark_ || g_get_monotonic_time() > self->holdUntil_ ||
        gtk_text_mark_get_buffer(self->holdMark_) != self->buffer_ ||
        !gtk_widget_get_mapped(self->view_)) {
        self->holdTick_ = 0;
        self->stopHold();
        return G_SOURCE_REMOVE;
    }
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(self->buffer_, &it, self->holdMark_);
    int y = 0, h = 0;
    gtk_text_view_get_line_yrange(tv, &it, &y, &h);
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv, &vis);
    const int off = y + self->holdInto_ - vis.y;
    // A few moves at most: GTK lays the pictures out in a frame or two, and
    // anything still moving the view after that (a flick of a touchpad
    // coasting on) is not to be fought.
    if (off != 0 && self->holdMoves_++ >= 3) {
        self->holdTick_ = 0;
        self->stopHold();
        return G_SOURCE_REMOVE;
    }
    if (off != 0) {
        GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv));
        g_log("minicode-web", G_LOG_LEVEL_DEBUG, "top line held: moved %d px", off);
        gtk_adjustment_set_value(v, gtk_adjustment_get_value(v) + off);
        self->holdStill_ = 0;
    } else if (++self->holdStill_ >= 3) {
        self->holdTick_ = 0;
        self->stopHold();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// A tab shown again comes back to its place, as the Mac's restoreTextPlaceOf:
// does: its first line at the top, as far into it as it was. Right after
// gtk_text_view_set_buffer GTK lays out and moves the view more than once,
// from heights it estimates for lines it has not laid out yet, so for up to
// two seconds the line is looked at every frame, and once the view has
// stopped moving it is put there, on the adjustment, until it stays for a
// few frames. Not by gtk_text_view_scroll_to_mark: GTK keeps that pending
// and can carry it out after the user has moved on (a Ctrl+End then ended
// at the top). It waits while the view is off screen (the
// browser over it), and gives way to the user scrolling, or pressing the
// mouse anywhere in the scroller (its scroll bars included). The caret moving
// meanwhile (typing, Ctrl+End, an undo) makes the caret the target instead:
// it is kept on screen until the view stays still with it there, since GTK's
// own scroll to it lands short while it lays out the lines it had only
// estimated (settleOnCaret), and an undo does not scroll to it at all; once
// the view was left near the top with the caret, and the edit, far below.
void Editor::settleOnPlace(GtkTextMark* mark, int into) {
    stopPlace();
    placeMark_ = mark;
    placeInto_ = into;
    placeUntil_ = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    placeLastV_ = -1;
    placeStill_ = 0;
    placeTries_ = 0;
    placeUpper_ = gtk_adjustment_get_upper(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view_)));
    placeCap_ = g_get_monotonic_time() + 180 * G_USEC_PER_SEC;
    placeFollowCaret_ = false;
    GtkTextBuffer* b = gtk_text_mark_get_buffer(mark);
    GtkTextIter caret;
    gtk_text_buffer_get_iter_at_mark(b, &caret, gtk_text_buffer_get_insert(b));
    placeCaret_ = gtk_text_iter_get_offset(&caret);
    placeTick_ = gtk_widget_add_tick_callback(view_, onPlaceTick, this, nullptr);
}

void Editor::stopPlace() {
    if (placeTick_) gtk_widget_remove_tick_callback(view_, placeTick_);
    placeTick_ = 0;
    if (placeMark_ && !gtk_text_mark_get_deleted(placeMark_))
        gtk_text_buffer_delete_mark(gtk_text_mark_get_buffer(placeMark_), placeMark_);
    placeMark_ = nullptr;
}

gboolean Editor::onPlaceTick(GtkWidget*, GdkFrameClock*, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    GtkTextView* tv = GTK_TEXT_VIEW(self->view_);
    auto done = [self] {
        self->placeTick_ = 0;
        self->stopPlace();
        return G_SOURCE_REMOVE;
    };
    if (!self->placeMark_ || g_get_monotonic_time() > self->placeUntil_ ||
        gtk_text_mark_get_buffer(self->placeMark_) != self->buffer_)
        return done();
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(self->buffer_, &it, gtk_text_buffer_get_insert(self->buffer_));
    if (gtk_text_iter_get_offset(&it) != self->placeCaret_) {
        self->placeCaret_ = gtk_text_iter_get_offset(&it);
        self->placeFollowCaret_ = true;
        self->placeStill_ = 0;
        self->placeTries_ = 0;
        self->placeUntil_ = std::max(self->placeUntil_,
                                     g_get_monotonic_time() + G_USEC_PER_SEC * 3 / 2);
    }
    if (!gtk_widget_get_mapped(self->view_)) return G_SOURCE_CONTINUE;
    GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv));
    const double now = gtk_adjustment_get_value(v);
    const bool still = now == self->placeLastV_;
    self->placeLastV_ = now;
    // GTK gives a line no height until it has laid it out, and lays a big
    // buffer out in idle slices for seconds after it comes on show (13 s at
    // 470,000 lines): until then a line far down sits far too high, and the
    // view cannot be put there. While the height still grows the place is
    // tried again, for up to three minutes (LargeFileView.h).
    const bool growing = gtk_adjustment_get_upper(v) != self->placeUpper_;
    self->placeUpper_ = gtk_adjustment_get_upper(v);
    if (growing) {
        self->placeTries_ = 0;
        self->placeStill_ = 0;
        self->placeUntil_ = std::min(self->placeCap_,
                                     std::max(self->placeUntil_, g_get_monotonic_time() + G_USEC_PER_SEC));
    }
    if (!still) {
        self->placeStill_ = 0;
        return G_SOURCE_CONTINUE;
    }
    if (self->placeFollowCaret_) {
        // The caret on screen, a third of the way down when it was not.
        GdkRectangle vis, at;
        gtk_text_view_get_visible_rect(tv, &vis);
        gtk_text_view_get_iter_location(tv, &it, &at);
        if (vis.height > 0 && at.y >= vis.y && at.y + at.height <= vis.y + vis.height) {
            if (++self->placeStill_ >= 3) return done();
            return G_SOURCE_CONTINUE;
        }
        if (self->placeTries_++ >= 8) return done();
        self->placeStill_ = 0;
        gtk_adjustment_set_value(v, now + at.y - vis.y - vis.height * 0.3);
        self->placeLastV_ = gtk_adjustment_get_value(v);
        return G_SOURCE_CONTINUE;
    }
    gtk_text_buffer_get_iter_at_mark(self->buffer_, &it, self->placeMark_);
    int y = 0, h = 0;
    gtk_text_view_get_line_yrange(tv, &it, &y, &h);
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv, &vis);
    const int off = y + self->placeInto_ - vis.y;
    if (off == 0) {
        if (++self->placeStill_ >= 3) return done();
        return G_SOURCE_CONTINUE;
    }
    // Near the end of the text the place may not be reachable any more (the
    // file shrank): a few tries, then it stays where GTK can put it.
    if (self->placeTries_++ >= 8) return done();
    self->placeStill_ = 0;
    gtk_adjustment_set_value(v, now + off);
    self->placeLastV_ = gtk_adjustment_get_value(v);
    return G_SOURCE_CONTINUE;
}

void Editor::fitPage() {
    if (!active_ || active_->pageInfo().embeds.empty()) return;
    const double page = gtk_adjustment_get_page_size(
        gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)));
    // Before the first layout the pane has no width yet; a notify follows.
    const int width = page > 0 ? (int)page : 600;
    Markdown::fit(active_->pageInfo(), width - gtk_text_view_get_left_margin(GTK_TEXT_VIEW(view_)) -
                                           gtk_text_view_get_right_margin(GTK_TEXT_VIEW(view_)) -
                                           16);
}

// ---------------------------------------------------------------- preview links

// "#section" scrolls to that heading; a web address, or a path relative to
// the Markdown file (GitHub's "#L12" names a line), goes to the shell.
void Editor::openMarkdownLink(const std::string& url) {
    if (url.empty() || !active_) return;
    if (url[0] == '#') {
        if (!scrollToAnchor(url.substr(1))) gtk_widget_error_bell(view_);
        return;
    }
    char* scheme = g_uri_parse_scheme(url.c_str());
    const std::string sch = scheme ? scheme : "";
    g_free(scheme);
    if (!sch.empty() && sch != "file") {
        if (sch == "http" || sch == "https") {
            Link link;
            link.isUrl = true;
            link.target = url;
            if (linkHandler_) linkHandler_(link);
        } else {
            GtkUriLauncher* launcher = gtk_uri_launcher_new(url.c_str());
            gtk_uri_launcher_launch(launcher, GTK_WINDOW(gtk_widget_get_root(view_)),
                                    nullptr, nullptr, nullptr);
            g_object_unref(launcher);
        }
        return;
    }
    std::string path = url, fragment;
    if (sch == "file") {
        char* f = g_filename_from_uri(url.c_str(), nullptr, nullptr);
        path = f ? f : "";
        g_free(f);
    }
    const size_t hash = path.find('#');
    if (hash != std::string::npos) {
        fragment = path.substr(hash + 1);
        path = path.substr(0, hash);
    }
    if (sch != "file") {
        char* un = g_uri_unescape_string(path.c_str(), nullptr);
        if (un) path = un;
        g_free(un);
    }
    if (path.rfind("~/", 0) == 0) path = std::string(g_get_home_dir()) + path.substr(1);
    if (path.empty()) {   // "#section" after an empty path: this file
        if (!scrollToAnchor(fragment)) gtk_widget_error_bell(view_);
        return;
    }
    const std::string& here = active_->path();
    if (path[0] != '/') {
        char* dir = g_path_get_dirname(here.c_str());
        path = std::string(dir) + "/" + path;
        g_free(dir);
    }
    // Spelled like the tree's paths: "..", "." and doubled slashes folded.
    GFile* f = g_file_new_for_path(path.c_str());
    char* canon = g_file_get_path(f);
    g_object_unref(f);
    if (canon) path = canon;
    g_free(canon);
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        gtk_widget_error_bell(view_);
        return;
    }
    if (path == here && !fragment.empty() && fragment[0] != 'L') {
        if (!scrollToAnchor(fragment)) gtk_widget_error_bell(view_);
        return;
    }
    Link link;
    link.target = path;
    link.isDir = S_ISDIR(st.st_mode);
    if (fragment.size() > 1 && fragment[0] == 'L') link.line = atoi(fragment.c_str() + 1);
    if (linkHandler_) linkHandler_(link);
}

bool Editor::scrollToAnchor(const std::string& anchorIn) {
    if (!inMarkdownPage()) return false;
    char* un = g_uri_unescape_string(anchorIn.c_str(), nullptr);
    std::string key = un ? un : anchorIn;
    g_free(un);
    for (char& c : key) c = (char)std::tolower((unsigned char)c);
    const Markdown::Page& page = active_->pageInfo();
    auto it = page.anchors.find(key);
    if (it == page.anchors.end()) return false;
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, it->second);
    gtk_text_buffer_place_cursor(buffer_, &at);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_),
                                 0.0, TRUE, 0.0, 0.0);
    settleOnCaret();
    return true;
}

// ---------------------------------------------------------------- preview edits
//
// A double-click on rendered Markdown opens the block behind it (a
// paragraph, heading, list item, quote, code block or table cell) in a
// popover holding its Markdown, as the LaTeX preview does for LaTeX. Saving
// splices just those bytes into the source buffer, as one undoable edit
// (D3), which is then unsaved until Ctrl+S, as typing leaves it.

void Editor::editMarkdownBlock(int line, int column, const GdkRectangle& at) {
    if (!inMarkdownPage()) return;
    const std::string source = active_->text();
    MarkdownEdit::Block block = MarkdownEdit::blockAt(source, line, column);
    if (block.kind == MarkdownEdit::Block::None) return;
    static const char* const kinds[] = {"", "Paragraph", "Heading", "List item", "Quote",
                                        "Code block", "Table cell", "Table row", "Formula",
                                        "HTML"};
    mdBlock_ = block;
    mdEditSrc_ = source;
    mdEditDoc_ = active_;
    mdEditValid_ = true;
    mdAdding_ = false;
    mdAnchor_ = at;
    showMdPopover(std::string(kinds[block.kind]) +
                      " (Markdown; Enter saves, Shift+Enter for a new line)",
                  source.substr(block.start, block.end - block.start),
                  block.kind == MarkdownEdit::Block::ListItem);
}

void Editor::showMdPopover(const std::string& title, const std::string& text, bool allowsItem) {
    if (!mdPop_) {
        mdPop_ = gtk_popover_new();
        gtk_popover_set_position(GTK_POPOVER(mdPop_), GTK_POS_BOTTOM);
        gtk_widget_set_parent(mdPop_, view_);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_size_request(box, 440, -1);
        mdPopLabel_ = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(mdPopLabel_), 0.0);
        gtk_widget_add_css_class(mdPopLabel_, "dim-label");
        mdPopText_ = gtk_text_view_new();
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(mdPopText_), TRUE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(mdPopText_), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(mdPopText_), 4);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(mdPopText_), 4);
        GtkWidget* scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), mdPopText_);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 74);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 260);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroll), TRUE);
        // Enter saves and Shift+Enter types a newline, as on the Mac; Escape
        // closes the popover by itself.
        GtkEventController* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint key,
                                                           guint, GdkModifierType mods,
                                                           gpointer selfp) -> gboolean {
            if ((key != GDK_KEY_Return && key != GDK_KEY_KP_Enter) || (mods & GDK_SHIFT_MASK))
                return FALSE;
            static_cast<Editor*>(selfp)->commitMdEdit();
            return TRUE;
        }), this);
        gtk_widget_add_controller(mdPopText_, keys);

        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        mdPopAdd_ = gtk_button_new_with_label("Add item");
        gtk_widget_set_tooltip_text(mdPopAdd_, "Add a new entry to this list, below this one");
        g_signal_connect_swapped(mdPopAdd_, "clicked", G_CALLBACK(+[](gpointer selfp) {
            Editor* self = static_cast<Editor*>(selfp);
            if (!self->mdEditValid_) return;
            self->mdAdding_ = true;
            self->showMdPopover("New list entry", "", false);
        }), this);
        GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        GtkWidget* cancel = gtk_button_new_with_label("Cancel");
        g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(+[](gpointer selfp) {
            Editor* self = static_cast<Editor*>(selfp);
            self->mdEditValid_ = false;
            gtk_popover_popdown(GTK_POPOVER(self->mdPop_));
        }), this);
        GtkWidget* save = gtk_button_new_with_label("Save");
        gtk_widget_add_css_class(save, "suggested-action");
        g_signal_connect_swapped(save, "clicked", G_CALLBACK(+[](gpointer selfp) {
            static_cast<Editor*>(selfp)->commitMdEdit();
        }), this);
        gtk_box_append(GTK_BOX(buttons), mdPopAdd_);
        gtk_box_append(GTK_BOX(buttons), spacer);
        gtk_box_append(GTK_BOX(buttons), cancel);
        gtk_box_append(GTK_BOX(buttons), save);
        gtk_box_append(GTK_BOX(box), mdPopLabel_);
        gtk_box_append(GTK_BOX(box), scroll);
        gtk_box_append(GTK_BOX(box), buttons);
        gtk_popover_set_child(GTK_POPOVER(mdPop_), box);
    }
    gtk_label_set_text(GTK_LABEL(mdPopLabel_), title.c_str());
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(mdPopText_));
    gtk_text_buffer_set_text(b, text.c_str(), (int)text.size());
    gtk_widget_set_visible(mdPopAdd_, allowsItem);
    gtk_popover_set_pointing_to(GTK_POPOVER(mdPop_), &mdAnchor_);
    gtk_popover_popup(GTK_POPOVER(mdPop_));
    gtk_widget_grab_focus(mdPopText_);
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    gtk_text_buffer_select_range(b, &s, &e);
}

void Editor::commitMdEdit() {
    if (!mdEditValid_ || !mdPop_) return;
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(mdPopText_));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    const std::string text = raw ? raw : "";
    g_free(raw);
    const bool adding = mdAdding_;
    mdEditValid_ = false;
    gtk_popover_popdown(GTK_POPOVER(mdPop_));
    // The block's bytes index the source it was found in; if that changed
    // while the popover was open, they may no longer be the same text.
    if (!inMarkdownPage() || active_ != mdEditDoc_ || active_->text() != mdEditSrc_) return;
    std::string edited;
    if (adding) {
        if (text.empty()) return;
        edited = MarkdownEdit::addItem(mdEditSrc_, mdBlock_, text);
    } else {
        edited = MarkdownEdit::replace(mdEditSrc_, mdBlock_, text);
    }
    if (edited != mdEditSrc_) applyMarkdownSource(edited);
}

void Editor::dropMdPopover() {
    mdEditValid_ = false;
    mdEditDoc_ = nullptr;
    if (!mdPop_) return;
    g_signal_handlers_disconnect_by_data(mdPop_, this);
    if (gtk_widget_get_parent(mdPop_)) gtk_widget_unparent(mdPop_);
    mdPop_ = mdPopLabel_ = mdPopText_ = mdPopAdd_ = nullptr;
}

void Editor::applyMarkdownSource(const std::string& source) {
    Document* d = active_;
    if (!d || source == d->text()) return;
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double keep = gtk_adjustment_get_value(v);
    d->spliceText(source);
    rerenderPreview();
    d->place().previewEntryV = keep;
    d->refreshTaskCount();
}

// The page is never undone into; Ctrl+Z there takes back the last change to
// the source, the preview's edits and typing alike, and shows the page again.
void Editor::undoFromPage(bool redo) {
    Document* d = active_;
    GtkTextBuffer* src = d->buffer();
    if (!(redo ? gtk_text_buffer_get_can_redo(src) : gtk_text_buffer_get_can_undo(src))) {
        gtk_widget_error_bell(view_);
        return;
    }
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double keep = gtk_adjustment_get_value(v);
    if (redo) gtk_text_buffer_redo(src);
    else gtk_text_buffer_undo(src);
    rerenderPreview();
    d->place().previewEntryV = keep;
    d->refreshTaskCount();
}

gboolean Editor::onViewKey(GtkEventControllerKey*, guint key, guint, GdkModifierType mods,
                           gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    // Return, Tab, Shift+Tab and Backspace in source follow the language's
    // indentation (AutoIndent), unless a completion list is open and wants
    // the key.
    const bool plain = !(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK));
    const bool completion = self->observer_ && self->observer_->takesReturn();
    self->typedKey_ = 0;
    // Typing, Return, Tab, Backspace and Escape at several carets (Carets.h).
    if (self->carets_ && self->carets_->keyPressed(key, mods)) return TRUE;
    // Beside a fold (Folds.h): Backspace or Delete opens it, and Return at
    // the end of a folded line takes the caret below it for what follows.
    if (!completion && self->folds_ && self->folds_->keyPressed(key, mods)) return TRUE;
    // Brackets and quotes closing, typed over, deleted in pairs, or
    // surrounding a selection (AutoCloseInput.h), before the text view or
    // its input method takes the key.
    if (self->autoClose_ && self->autoClose_->keyPressed(key, mods)) return TRUE;
    if (plain && self->isSource() && !completion) {
        const bool shift = mods & GDK_SHIFT_MASK;
        if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter || key == GDK_KEY_ISO_Enter) {
            // In a Markdown list it starts the next item, or ends the list.
            // Shift+Return is a plain new line (with the indent), for a
            // second line inside the same item.
            if (!shift && self->isMarkdown() && self->continueList()) return TRUE;
            if (self->indentKey(GDK_KEY_Return)) return TRUE;
        } else if ((key == GDK_KEY_Tab || key == GDK_KEY_KP_Tab) && !shift) {
            if (self->indentKey(GDK_KEY_Tab)) return TRUE;
        } else if (key == GDK_KEY_ISO_Left_Tab || key == GDK_KEY_Tab || key == GDK_KEY_KP_Tab) {
            if (self->indentKey(GDK_KEY_ISO_Left_Tab)) return TRUE;
        } else if (key == GDK_KEY_BackSpace && !shift) {
            if (self->indentKey(GDK_KEY_BackSpace)) return TRUE;
        } else {
            const gunichar c = gdk_keyval_to_unicode(key);
            if (c == ':' || c == ')' || c == ']' || c == '}') self->typedKey_ = (char)c;
        }
    }
    if (!self->inMarkdownPage()) return FALSE;
    const bool ctrl = mods & GDK_CONTROL_MASK, shift = mods & GDK_SHIFT_MASK;
    const bool undo = ctrl && !shift && (key == GDK_KEY_z || key == GDK_KEY_Z);
    const bool redo = ctrl && ((shift && (key == GDK_KEY_z || key == GDK_KEY_Z)) ||
                               (!shift && (key == GDK_KEY_y || key == GDK_KEY_Y)));
    if (!undo && !redo) return FALSE;
    self->undoFromPage(redo);
    return TRUE;
}

// ---------------------------------------------------------------- tasks
//
// Task lists ("- [ ] x"), over the shared MarkdownTasks core: a click on a
// box in the preview, the task key on the source's lines, and Return
// continuing a list. The count in the title is the document's.

void Editor::toggleTaskBox(int line) {
    if (!inMarkdownPage()) return;
    const std::string source = active_->text();
    const std::string edited = MarkdownTasks::toggleBox(source, line);
    if (edited == source) {
        gtk_widget_error_bell(view_);
        return;
    }
    // A preview edit like the popover's: dirty until Ctrl+S, undone with
    // Ctrl+Z in the preview, and the page shown again at the same scroll.
    applyMarkdownSource(edited);
}

void Editor::applyTaskEdit(const std::u16string& text, std::size_t replaceStart,
                           std::size_t replaceLength, const std::u16string& replacement,
                           std::size_t selStart, std::size_t selEnd, long base) {
    GtkTextIter from, to;
    gtk_text_buffer_get_iter_at_offset(buffer_, &from,
        (int)(base + utf16::toCharOffset(text, replaceStart)));
    gtk_text_buffer_get_iter_at_offset(buffer_, &to,
        (int)(base + utf16::toCharOffset(text, replaceStart + replaceLength)));
    const std::string bytes = utf16::toUtf8(replacement);
    // One user action, so one Ctrl+Z takes it back.
    gtk_text_buffer_begin_user_action(buffer_);
    gtk_text_buffer_delete(buffer_, &from, &to);
    gtk_text_buffer_insert(buffer_, &from, bytes.c_str(), (int)bytes.size());
    gtk_text_buffer_end_user_action(buffer_);

    std::u16string after = text;
    after.replace(replaceStart, replaceLength, replacement);
    GtkTextIter ns, ne;
    gtk_text_buffer_get_iter_at_offset(buffer_, &ns,
                                       (int)(base + utf16::toCharOffset(after, selStart)));
    gtk_text_buffer_get_iter_at_offset(buffer_, &ne,
                                       (int)(base + utf16::toCharOffset(after, selEnd)));
    gtk_text_buffer_select_range(buffer_, &ns, &ne);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_));
}

bool Editor::toggleTask() {
    if (!isMarkdown() || inPreview() || !isSource() ||
        !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)))
        return false;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* raw = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    const std::u16string text = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);
    GtkTextIter selA, selB;
    gtk_text_buffer_get_selection_bounds(buffer_, &selA, &selB);
    const MarkdownTasks::Edit r = MarkdownTasks::toggle(
        text, utf16::fromCharOffset(text, gtk_text_iter_get_offset(&selA)),
        utf16::fromCharOffset(text, gtk_text_iter_get_offset(&selB)));
    if (!r.changed) return false;
    applyTaskEdit(text, r.replaceStart, r.replaceLength, r.replacement, r.selStart, r.selEnd);
    active_->refreshTaskCount();
    return true;
}

bool Editor::continueList() {
    if (!isMarkdown() || inPreview() || !isSource() ||
        !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)))
        return false;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* raw = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    const std::u16string text = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);
    GtkTextIter selA, selB;
    gtk_text_buffer_get_selection_bounds(buffer_, &selA, &selB);
    const MarkdownTasks::Edit r = MarkdownTasks::newline(
        text, utf16::fromCharOffset(text, gtk_text_iter_get_offset(&selA)),
        utf16::fromCharOffset(text, gtk_text_iter_get_offset(&selB)));
    if (!r.changed) return false;
    applyTaskEdit(text, r.replaceStart, r.replaceLength, r.replacement, r.selStart, r.selEnd);
    return true;
}

// ---------------------------------------------------------------- indentation
//
// Return, Tab, Shift+Tab and Backspace over the shared AutoIndent core, and
// line sorting over KeepSorted. Each edit is one user action, one Ctrl+Z.

bool Editor::sourceText(std::u16string& text, std::size_t& a, std::size_t& b,
                        long* base) const {
    if (!isSource() || inPreview() || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)))
        return false;
    GtkTextIter selA, selB;
    gtk_text_buffer_get_selection_bounds(buffer_, &selA, &selB);
    long start = 0;
    if (!base || !LargeFileView::indentSlice(buffer_, &selA, &selB, text, start)) {
        GtkTextIter s, e;
        gtk_text_buffer_get_bounds(buffer_, &s, &e);
        char* raw = gtk_text_buffer_get_text(buffer_, &s, &e, TRUE);
        text = utf16::fromUtf8(raw ? raw : "");
        g_free(raw);
    }
    if (base) *base = start;
    a = utf16::fromCharOffset(text, gtk_text_iter_get_offset(&selA) - start);
    b = utf16::fromCharOffset(text, gtk_text_iter_get_offset(&selB) - start);
    return true;
}

bool Editor::indentKey(guint key) {
    if (key == GDK_KEY_BackSpace) {
        // Only ever in leading spaces: look before reading the whole buffer.
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_mark(buffer_, &at, gtk_text_buffer_get_insert(buffer_));
        if (gtk_text_buffer_get_has_selection(buffer_) || gtk_text_iter_starts_line(&at))
            return false;
        GtkTextIter before = at;
        gtk_text_iter_backward_char(&before);
        if (gtk_text_iter_get_char(&before) != ' ') return false;
    }
    std::u16string text;
    std::size_t a = 0, b = 0;
    long base = 0;   // a big buffer gives only the lines around the caret
    if (!sourceText(text, a, b, &base)) return false;
    const std::string& path = active_->path();
    AutoIndent::Edit e;
    switch (key) {
        case GDK_KEY_Return: e = AutoIndent::newline(text, a, b, path); break;
        case GDK_KEY_Tab: e = AutoIndent::tab(text, a, b, path); break;
        case GDK_KEY_ISO_Left_Tab: e = AutoIndent::backtab(text, a, b, path); break;
        case GDK_KEY_BackSpace: e = AutoIndent::backspace(text, a, b, path); break;
        default: return false;
    }
    // Shift+Tab never moves the focus out of the editor.
    if (!e.changed) return key == GDK_KEY_ISO_Left_Tab;
    applyTaskEdit(text, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd,
                  base);
    return true;
}

void Editor::electric(GtkTextIter* loc) {
    std::u16string text;
    long base = 0;   // a big buffer gives only the lines around the caret
    if (!LargeFileView::indentSlice(buffer_, loc, loc, text, base)) {
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buffer_, &a, &b);
        char* raw = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
        text = utf16::fromUtf8(raw ? raw : "");
        g_free(raw);
    }
    const size_t caret = utf16::fromCharOffset(text, gtk_text_iter_get_offset(loc) - base);
    const AutoIndent::Edit e = AutoIndent::typed(text, caret, active_->path());
    if (!e.changed) return;
    // Inside the key's own user action, so one Ctrl+Z takes back the key and
    // the move together. The mark keeps loc valid for the handlers after us.
    GtkTextMark* keep = gtk_text_buffer_create_mark(buffer_, nullptr, loc, FALSE);
    GtkTextIter from, to;
    gtk_text_buffer_get_iter_at_offset(buffer_, &from,
        (int)(base + utf16::toCharOffset(text, e.replaceStart)));
    gtk_text_buffer_get_iter_at_offset(buffer_, &to,
        (int)(base + utf16::toCharOffset(text, e.replaceStart + e.replaceLength)));
    gtk_text_buffer_delete(buffer_, &from, &to);
    const std::string bytes = utf16::toUtf8(e.replacement);
    gtk_text_buffer_insert(buffer_, &from, bytes.c_str(), (int)bytes.size());
    gtk_text_buffer_get_iter_at_mark(buffer_, loc, keep);
    gtk_text_buffer_delete_mark(buffer_, keep);
}

bool Editor::runKeepSorted(bool all, std::string* message) {
    std::u16string text;
    std::size_t a = 0, b = 0;
    if (!sourceText(text, a, b)) return false;
    const KeepSorted::Edit e = all ? KeepSorted::fixCommand(text, a, b)
                                   : KeepSorted::sortCommand(text, a, b);
    if (e.changed)
        applyTaskEdit(text, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd);
    if (message) *message = !e.message.empty() ? e.message : e.changed ? "" : "Already sorted";
    return e.changed || !e.error;
}

bool Editor::sortLines(std::string* message) { return runKeepSorted(false, message); }
bool Editor::fixKeepSorted(std::string* message) { return runKeepSorted(true, message); }

std::string Editor::sortBlocksBeforeSave() {
    std::u16string text;
    std::size_t a = 0, b = 0;
    if (!sourceText(text, a, b) || !KeepSorted::hasBlocks(text)) return "";
    const KeepSorted::Edit e = KeepSorted::fixCommand(text, a, b);
    if (e.changed)
        applyTaskEdit(text, e.replaceStart, e.replaceLength, e.replacement, e.selStart, e.selEnd);
    return e.error ? e.message : "";
}
