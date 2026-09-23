// Editor.cpp — see Editor.h. Mirrors the behavior of the macOS
// EditorController: monospace editor, VS Code dark palette, incremental
// highlighting, and a Markdown preview that swaps the buffer contents.
#include "Editor.h"
#include "Palette.h"
#include "Markdown.h"
#include "SyntaxHighlighter.h"
#include "LineComments.h"
#include "Utf8Offsets.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <climits>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------- helpers

static std::string extOf(const std::string& path) {
    auto slash = path.find_last_of('/');
    auto dot   = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return e;
}

// ---------------------------------------------------------------- construction

Editor::Editor() {
    view_ = gtk_text_view_new();
    buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));

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

    scroller_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller_, "minicode-scroller");
    gtk_widget_add_css_class(scroller_, "minicode-editor-scroller");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), view_);
    gtk_widget_set_hexpand(scroller_, TRUE);
    gtk_widget_set_vexpand(scroller_, TRUE);

    g_signal_connect(buffer_, "changed", G_CALLBACK(onBufferChanged), this);
    // Highlighting follows every edit. The plain handlers run before the
    // buffer changes (both signals are RUN_LAST), while the iterators still
    // describe the old text; the _after ones run once it has changed.
    g_signal_connect(buffer_, "insert-text", G_CALLBACK(onInsertText), this);
    g_signal_connect(buffer_, "delete-range", G_CALLBACK(onDeleteRange), this);
    g_signal_connect_after(buffer_, "insert-text", G_CALLBACK(onInsertTextAfter), this);
    g_signal_connect_after(buffer_, "delete-range", G_CALLBACK(onDeleteRangeAfter), this);
    g_signal_connect(buffer_, "apply-tag", G_CALLBACK(onApplyTag), this);
    g_signal_connect(buffer_, "begin-user-action", G_CALLBACK(onBeginUserAction), this);
    g_signal_connect(buffer_, "end-user-action", G_CALLBACK(onEndUserAction), this);

    ensureTags();
    showWelcome();
}

void Editor::showWelcome() {
    showMessage("\n  MiniCode — a native C++ editor, now on GTK4\n\n"
                "  - Select a file in the sidebar to view it\n"
                "  - Source files are syntax-highlighted by type\n"
                "  - Markdown (.md) files render formatted (Ctrl+Shift+P)\n\n"
                "  Ctrl+O to open a different folder.");
}

Editor::~Editor() {
    g_signal_handlers_disconnect_by_data(buffer_, this);
}

// ---------------------------------------------------------------- tags

void Editor::ensureTags() {
    if (tagsReady_) return;
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    (void)table;

    // One tag per syntax style.
    for (int i = 0; i <= (int)TokenStyle::Function; ++i) {
        TokenStyle s = (TokenStyle)i;
        hlTags_.push_back(gtk_text_buffer_create_tag(buffer_, TagNameForStyle(s),
                                                     "foreground", ColorForStyle(s),
                                                     NULL));
    }

    // Markdown preview tags.
    for (int lvl = 1; lvl <= 6; ++lvl) {
        // Font sizes mirror the macOS build: {26,22,19,17,15,14}.
        static const int sizes[7] = {0, 26, 22, 19, 17, 15, 14};
        char name[16];
        g_snprintf(name, sizeof(name), "h%d", lvl);
        gtk_text_buffer_create_tag(buffer_, name,
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
    gtk_text_buffer_create_tag(buffer_, "md_table",
                               "foreground", pal::EditorText,
                               "family", "monospace",
                               "size-points", 13.0, NULL);   // keeps columns aligned
    gtk_text_buffer_create_tag(buffer_, "md_code",
                               "foreground", pal::MdCode,
                               "background", pal::MdCodeBg,
                               "family", "monospace",
                               "size-points", 13.0, NULL);
    gtk_text_buffer_create_tag(buffer_, "md_quote",
                               "foreground", pal::MdQuote,
                               "left-margin", 16,   // macOS headIndent
                               "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(buffer_, "md_rule",
                               "foreground", pal::MdRule, NULL);
    gtk_text_buffer_create_tag(buffer_, "md_link",
                               "foreground", pal::MdLink,
                               "underline", PANGO_UNDERLINE_SINGLE, NULL);
    gtk_text_buffer_create_tag(buffer_, "md_bold",
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buffer_, "md_italic",
                               "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(buffer_, "plainmsg",
                               "foreground", pal::MdPlainMsg, NULL);

    tagsReady_ = true;
}

// ---------------------------------------------------------------- file I/O

bool Editor::openFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        // The message now stands in for the file; nothing may be saved from it.
        path_ = path;
        source_.clear();
        ext_.clear();
        isMarkdown_ = false;
        preview_ = false;
        markDirty(false);
        showMessage(std::string("\n  Could not open: ") + path);
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    path_ = path;

    // gtk_text_buffer_set_text requires valid UTF-8; handing it a binary file
    // spews GTK criticals and leaves the buffer truncated at the first bad
    // byte. Refuse up front, with the same wording as the macOS build.
    if (!g_utf8_validate(content.data(), (gssize)content.size(), nullptr)) {
        auto slash = path.find_last_of('/');
        std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
        source_.clear();
        ext_.clear();
        isMarkdown_ = false;
        preview_ = false;
        markDirty(false);
        showMessage("\n  Cannot display “" + base + "”.\n\n"
                    "  (Binary file or unsupported encoding.)");
        return false;
    }

    source_ = content;
    ext_  = extOf(path);
    isMarkdown_ = (ext_ == "md" || ext_ == "markdown");
    // Markdown opens rendered, matching the macOS build.
    preview_ = isMarkdown_;
    markDirty(false);

    if (preview_) renderPreview();
    else          loadRawIntoBuffer();
    return true;
}

bool Editor::save(std::string* error) {
    // A message (welcome text, "Cannot display", "Could not open") is not the
    // file's contents. Saving it used to write the message over the file, which
    // for a binary file destroyed it.
    if (path_.empty() || showingMessage_) return true;
    // In preview mode the buffer holds rendered text; persist source_ instead.
    if (!preview_) {
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buffer_, &a, &b);
        char* txt = gtk_text_buffer_get_text(buffer_, &a, &b, FALSE);
        source_ = txt ? txt : "";
        g_free(txt);
    }

    // Write through a symlink to the file it names (a dotfile repo links its
    // files into place), and keep the file's permissions. The write itself is
    // atomic, a temporary file renamed over the old one, so a full disk or a
    // crash midway leaves the previous contents intact, as the macOS build's
    // writeToFile:atomically: does. A read-only file is refused rather than
    // quietly replaced by that rename.
    char* resolved = realpath(path_.c_str(), nullptr);
    const std::string target = resolved ? resolved : path_;
    free(resolved);
    int mode = 0666;
    struct stat st;
    if (stat(target.c_str(), &st) == 0) {
        mode = st.st_mode & 07777;
        if (access(target.c_str(), W_OK) != 0) {
            if (error) *error = std::string("The file is not writable: ") + strerror(errno);
            return false;
        }
    }
    GError* err = nullptr;
    if (!g_file_set_contents_full(target.c_str(), source_.data(), (gssize)source_.size(),
                                  G_FILE_SET_CONTENTS_CONSISTENT, mode, &err)) {
        if (error) *error = err ? err->message : "Unknown error";
        g_clear_error(&err);
        return false;
    }
    markDirty(false);
    return true;
}

void Editor::closeFile() {
    path_.clear();
    source_.clear();
    ext_.clear();
    isMarkdown_ = false;
    preview_ = false;
    markDirty(false);
    showWelcome();
    if (titleCb_) titleCb_(titleUser_);
}

// ---------------------------------------------------------------- go to line

bool Editor::revealLine(int line, std::size_t byteColumn, std::size_t byteLength) {
    if (path_.empty()) return false;
    if (preview_ && isMarkdown_) togglePreview();
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
    // Scroll by the insert mark rather than an iterator: a mark scroll waits
    // for the lines to be laid out, which a file opened a moment ago is not.
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_),
                                 0.1, TRUE, 0.0, 0.3);
    gtk_widget_grab_focus(view_);
    return true;
}

// ---------------------------------------------------------------- buffer fills

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
// cannot be reached. Source keeps GTK_WRAP_NONE, where long lines are wanted
// and lines are bounded by the file's own line breaks. macOS gets the same
// behavior from textContainer.widthTracksTextView = YES.
void Editor::setProseFont(bool prose) {
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view_), prose ? FALSE : TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view_),
                                prose ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    if (prose) gtk_widget_add_css_class(view_, "minicode-prose");
    else       gtk_widget_remove_css_class(view_, "minicode-prose");
}

void Editor::loadRawIntoBuffer() {
    // Temporarily block change signals so filling the buffer doesn't mark dirty.
    // Highlighting is stopped first so the fill is not fed to it as an edit;
    // startHighlighting() lexes the new text once, whole.
    stopHighlighting();
    g_signal_handlers_block_by_func(buffer_, (gpointer)onBufferChanged, this);
    showingMessage_ = false;
    setProseFont(false);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view_), TRUE);
    gtk_text_buffer_set_text(buffer_, source_.c_str(), (int)source_.size());
    g_signal_handlers_unblock_by_func(buffer_, (gpointer)onBufferChanged, this);
    sourceMode_ = true;
    startHighlighting();

    // Scroll to top.
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
}

void Editor::showMessage(const std::string& msg) {
    ensureTags();
    stopHighlighting();
    showingMessage_ = true;
    g_signal_handlers_block_by_func(buffer_, (gpointer)onBufferChanged, this);
    setProseFont(true);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view_), FALSE);
    gtk_text_buffer_set_text(buffer_, msg.c_str(), (int)msg.size());
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    gtk_text_buffer_apply_tag_by_name(buffer_, "plainmsg", &a, &b);
    g_signal_handlers_unblock_by_func(buffer_, (gpointer)onBufferChanged, this);
}

// ---------------------------------------------------------------- highlighting

// The same scheme as the macOS build (textStorage:willProcessEditing: and
// flushHighlighting in EditorController.mm), in UTF-8 bytes rather than UTF-16:
//
// - startHighlighting() lexes the whole buffer once: on file load, when the
//   grammar changes, and when the settings change.
// - onInsertText / onDeleteRange run BEFORE the buffer changes, while the
//   iterators still point into the old text. They turn the edit into bytes,
//   apply it to mirror_ and to the IncrementalHighlighter (which re-lexes the
//   edited lines and any below whose start state changed), and widen the
//   pending byte range.
// - Once the buffer has changed (the _after handlers, or end-user-action when
//   the edit is part of one), flushHighlighting() retags the pending lines:
//   highlight tags off over exactly those lines, then the tokens on.
//
// Retagging is what costs: GTK takes a few microseconds per tag applied, so
// retagging every line of a 50,000-line file takes over a second. A small
// range is retagged at once; a large one (typing "/*" near the top, opening a
// big file) gets the lines on screen at once and the rest in idle time slices
// of a few milliseconds, the way GtkSourceView does it. The lexer's own state
// is always complete; only the tags lag.
//
// Only the tags in hlTags_ (one per token style, plus the settings file's
// swatches) are ever removed, so find matches or any other tag survive.
//
// GtkTextIter speaks characters, and a byte index within its line. When GTK's
// lines are the highlighter's lines, byte offset = the highlighter's line start
// + the iterator's line index, with no scanning. They differ only in a file
// holding a lone '\r' or U+2029, which GTK also ends a line at; then offsets
// are found by counting characters through mirror_ instead.

namespace {
const size_t kSyncLines   = 1000;   // retag at once up to this many lines
const size_t kChunkLines  = 250;    // lines per step of the idle retag
const gint64 kSliceMicros = 5000;   // idle retag budget per main loop turn
const int    kBulkEdits   = 64;     // edits in one user action before resyncing whole

size_t byteOfChar(const std::string& s, long chars) {
    size_t b = 0;
    for (long c = 0; b < s.size(); ++b) {
        if (((unsigned char)s[b] & 0xC0) == 0x80) continue;   // continuation byte
        if (c++ == chars) break;
    }
    return b;
}

// Carry a byte range [s, e) across an edit that replaced [pos, pos + oldLen)
// with newLen bytes: shifted if it lay below, stretched if the edit touched it.
void carry(size_t& s, size_t& e, size_t pos, size_t oldLen, size_t newLen) {
    const size_t editEnd = pos + oldLen;
    if (s >= editEnd) {
        s = s + newLen - oldLen;
        e = e + newLen - oldLen;
    } else if (e > pos) {
        s = std::min(s, pos);
        e = std::max(e, editEnd) + newLen - oldLen;
    }
}
}  // namespace

bool Editor::linesMatch() const {
    return hl_ && (size_t)gtk_text_buffer_get_line_count(buffer_) == hl_->lineCount();
}

size_t Editor::byteOffsetOf(const GtkTextIter* it) const {
    if (linesMatch())
        return hl_->lineStart((size_t)gtk_text_iter_get_line(it)) +
               (size_t)gtk_text_iter_get_line_index(it);
    return byteOfChar(mirror_, gtk_text_iter_get_offset(it));
}

bool Editor::isHighlightTag(GtkTextTag* tag) const {
    return std::find(hlTags_.begin(), hlTags_.end(), tag) != hlTags_.end();
}

void Editor::removeHighlightTags(const GtkTextIter* a, const GtkTextIter* b) {
    for (GtkTextTag* t : hlTags_) gtk_text_buffer_remove_tag(buffer_, t, a, b);
}

void Editor::cancelDeferred() {
    if (idleId_) g_source_remove(idleId_);
    idleId_ = 0;
    deferred_ = false;
}

void Editor::stopHighlighting() {
    hl_.reset();
    std::string().swap(mirror_);
    pending_ = false;
    bulk_ = false;
    cancelDeferred();
    sourceMode_ = false;
}

void Editor::startHighlighting() {
    pending_ = false;
    bulk_ = false;
    cancelDeferred();
    hl_.reset();
    std::string().swap(mirror_);
    if (!sourceMode_ || ext_.empty() || !SyntaxHighlighter::supports(ext_)) {
        // No grammar (any more): no colors. Covers a rename to a plain .txt.
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buffer_, &a, &b);
        applyingTags_ = true;
        removeHighlightTags(&a, &b);
        applyingTags_ = false;
        return;
    }
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* text = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    mirror_ = text ? text : "";
    g_free(text);

    hl_ = std::make_unique<IncrementalHighlighter<char>>(ext_);
    hl_->reset(StringSource<char>(mirror_), nullptr);
    pendStart_ = 0;
    pendEnd_ = mirror_.size();
    pending_ = true;
    flushHighlighting();
}

// Record an edit already applied to mirror_: bytes [pos, pos + oldLen) became
// newLen bytes. The pending and deferred ranges are carried across it, then the
// lines the highlighter re-lexed join the pending range.
void Editor::noteEdit(size_t pos, size_t oldLen, size_t newLen) {
    std::vector<Token> unused;   // recomputed for the final text at flush time
    auto r = hl_->edit(StringSource<char>(mirror_), pos, oldLen, newLen, unused);
    if (deferred_) carry(defStart_, defEnd_, pos, oldLen, newLen);
    if (pending_) {
        carry(pendStart_, pendEnd_, pos, oldLen, newLen);
        pendStart_ = std::min(pendStart_, r.start);
        pendEnd_ = std::max(pendEnd_, r.end);
    } else {
        pendStart_ = r.start;
        pendEnd_ = r.end;
        pending_ = true;
    }
}

// The highlighter's lines on screen, with some margin. Before the view has a
// size, the lines around the cursor.
void Editor::visibleLines(size_t& first, size_t& end) const {
    const size_t margin = 60;
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(view_), &vis);
    GtkTextIter top, bottom;
    if (vis.height > 0) {
        gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(view_), &top, vis.y, nullptr);
        gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(view_), &bottom, vis.y + vis.height, nullptr);
    } else {
        gtk_text_buffer_get_iter_at_mark(buffer_, &top, gtk_text_buffer_get_insert(buffer_));
        bottom = top;
    }
    const size_t t = (size_t)gtk_text_iter_get_line(&top);
    const size_t b = (size_t)gtk_text_iter_get_line(&bottom);
    first = t > margin ? t - margin : 0;
    end = b + margin + 1;
}

void Editor::flushHighlighting() {
    if (!hl_ || !pending_ || bulk_) return;
    pending_ = false;
    const size_t len = hl_->length();
    const size_t s = std::min(pendStart_, len), e = std::min(pendEnd_, len);
    const size_t first = hl_->lineOf(s);
    const size_t end = e > s ? hl_->lineOf(e - 1) + 1 : first + 1;
    if (end - first <= kSyncLines) {
        retagSpan(first, end);
        return;
    }
    // Too many lines to retag in one go: the ones on screen now, all of them
    // later. (The visible ones are retagged twice; that is one chunk's work.)
    if (linesMatch()) {
        size_t vf, ve;
        visibleLines(vf, ve);
        vf = std::max(vf, first);
        ve = std::min(ve, end);
        if (vf < ve) retagSpan(vf, ve);
    } else {
        retagSpan(first, first + kSyncLines);
    }
    const size_t b0 = hl_->lineStart(first);
    const size_t b1 = end < hl_->lineCount() ? hl_->lineStart(end) : len;
    if (deferred_) {
        defStart_ = std::min(defStart_, b0);
        defEnd_ = std::max(defEnd_, b1);
    } else {
        defStart_ = b0;
        defEnd_ = b1;
        deferred_ = true;
    }
    // Below redraw and input, so typing and scrolling stay ahead of it.
    if (!idleId_) idleId_ = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, onIdleRetag, this, nullptr);
}

gboolean Editor::onIdleRetag(gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    if (!self->hl_ || !self->deferred_ || self->bulk_) {
        self->idleId_ = 0;   // resyncAfterBulk schedules it again if needed
        return G_SOURCE_REMOVE;
    }
    const gint64 t0 = g_get_monotonic_time();
    IncrementalHighlighter<char>& hl = *self->hl_;
    while (g_get_monotonic_time() - t0 < kSliceMicros) {
        const size_t len = hl.length();
        const size_t s = std::min(self->defStart_, len), e = std::min(self->defEnd_, len);
        if (e <= s) {
            self->deferred_ = false;
            break;
        }
        const size_t first = hl.lineOf(s);
        const size_t end = std::min(first + kChunkLines, hl.lineOf(e - 1) + 1);
        self->retagSpan(first, end);
        self->defStart_ = end < hl.lineCount() ? hl.lineStart(end) : len;
    }
    if (self->deferred_) return G_SOURCE_CONTINUE;
    self->idleId_ = 0;
    return G_SOURCE_REMOVE;
}

void Editor::retagSpan(size_t firstLine, size_t endLine) {
    endLine = std::min(endLine, hl_->lineCount());
    std::vector<Token> tokens;
    hl_->lineTokens(StringSource<char>(mirror_), firstLine, endLine, tokens);
    retagLines(firstLine, endLine, tokens);
}

// Lines [firstLine, endLine) of the highlighter: highlight tags off over
// exactly those lines, then `tokens` (byte offsets, ascending) on. Adjacent
// tokens of one style are applied together, since a comment spanning lines
// comes as one piece per line.
void Editor::retagLines(size_t firstLine, size_t endLine,
                        const std::vector<Token>& tokens) {
    if (!hl_ || firstLine >= endLine) return;
    const size_t b0 = hl_->lineStart(firstLine);
    const size_t b1 = endLine < hl_->lineCount() ? hl_->lineStart(endLine)
                                                 : hl_->length();
    const bool match = linesMatch();
    GtkTextIter a, b;
    if (match) {
        gtk_text_buffer_get_iter_at_line(buffer_, &a, (int)firstLine);
    } else {
        Utf8OffsetCursor whole(mirror_);
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)whole.charOffset((long)b0));
    }
    const long c0 = gtk_text_iter_get_offset(&a);

    // Byte -> character conversion over just these lines, one forward pass.
    const std::string span = mirror_.substr(b0, b1 - b0);
    gtk_text_buffer_get_iter_at_offset(buffer_, &b,
                                       (int)(c0 + Utf8OffsetCursor(span).totalChars()));
    Utf8OffsetCursor cursor(span);

    applyingTags_ = true;
    removeHighlightTags(&a, &b);
    GtkTextIter ts = a, te = a;
    for (size_t i = 0; i < tokens.size();) {
        const size_t start = tokens[i].start;
        size_t end = start + tokens[i].length;
        const TokenStyle st = tokens[i].style;
        size_t j = i + 1;
        while (j < tokens.size() && tokens[j].style == st && tokens[j].start == end) {
            end = tokens[j].start + tokens[j].length;
            ++j;
        }
        i = j;
        if (start < b0 || end > b1 || end <= start) continue;
        gtk_text_iter_set_offset(&ts, (int)(c0 + cursor.charOffset((long)(start - b0))));
        gtk_text_iter_set_offset(&te, (int)(c0 + cursor.charOffset((long)(end - b0))));
        gtk_text_buffer_apply_tag(buffer_, hlTags_[(int)st], &ts, &te);
    }
    if (isSettingsFile()) {
        if (match) decorateColors((int)firstLine, (int)endLine);
        else       decorateColors(0, INT_MAX);
    }
    applyingTags_ = false;
}

// A user action with many edits in it (a paste from another GtkTextView is
// one insert per run of tags, thousands for a large one) stops being tracked
// edit by edit, since each costs a copy of the mirror. When the action ends,
// the whole of it is found as one edit: the common prefix and suffix of the
// old and new text bound the bytes it changed.
void Editor::resyncAfterBulk() {
    bulk_ = false;
    if (!hl_) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* raw = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    std::string now = raw ? raw : "";
    g_free(raw);
    const size_t n = std::min(now.size(), mirror_.size());
    size_t p = 0;
    while (p < n && now[p] == mirror_[p]) ++p;
    size_t q = 0;
    while (q < n - p && now[now.size() - 1 - q] == mirror_[mirror_.size() - 1 - q]) ++q;
    const size_t oldLen = mirror_.size() - p - q, newLen = now.size() - p - q;
    mirror_ = std::move(now);
    if (oldLen || newLen) noteEdit(p, oldLen, newLen);
    flushHighlighting();
    if (deferred_ && !idleId_)
        idleId_ = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, onIdleRetag, this, nullptr);
}

// ------------------------------------------------ buffer signals

void Editor::onInsertText(GtkTextBuffer*, GtkTextIter* loc, char* text, int len,
                          gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    if (!self->hl_ || len <= 0 || self->bulk_) return;
    if (self->inAction_ && ++self->actionEdits_ > kBulkEdits) {
        self->bulk_ = true;
        return;
    }
    const size_t pos = std::min(self->byteOffsetOf(loc), self->mirror_.size());
    self->mirror_.insert(pos, text, (size_t)len);
    self->noteEdit(pos, 0, (size_t)len);
}

void Editor::onDeleteRange(GtkTextBuffer*, GtkTextIter* start, GtkTextIter* end,
                           gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    if (!self->hl_ || self->bulk_) return;
    if (self->inAction_ && ++self->actionEdits_ > kBulkEdits) {
        self->bulk_ = true;
        return;
    }
    size_t a = self->byteOffsetOf(start), b = self->byteOffsetOf(end);
    if (b < a) std::swap(a, b);
    a = std::min(a, self->mirror_.size());
    b = std::min(b, self->mirror_.size());
    if (a == b) return;
    self->mirror_.erase(a, b - a);
    self->noteEdit(a, b - a, 0);
}

// Inside a user action the retag waits for its end, so a delete-then-insert
// (typing over a selection, a paste) retags once.
void Editor::onInsertTextAfter(GtkTextBuffer*, GtkTextIter*, char*, int, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    if (!self->inAction_) self->flushHighlighting();
}

void Editor::onDeleteRangeAfter(GtkTextBuffer*, GtkTextIter*, GtkTextIter*, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    if (!self->inAction_) self->flushHighlighting();
}

// GtkTextBuffer emits these for the outermost begin/end pair only.
void Editor::onBeginUserAction(GtkTextBuffer*, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    self->inAction_ = true;
    self->actionEdits_ = 0;
}

void Editor::onEndUserAction(GtkTextBuffer*, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    self->inAction_ = false;
    if (self->bulk_) self->resyncAfterBulk();
    else             self->flushHighlighting();
}

// Highlight tags belong to the highlighter alone. Pasting from a GtkTextView
// inserts the copied range with its tags (gtk_text_buffer_insert_range), which
// would lay the source's colors over the lines just retagged; stopping the
// emission keeps them off.
void Editor::onApplyTag(GtkTextBuffer* buf, GtkTextTag* tag, GtkTextIter*, GtkTextIter*,
                        gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    if (self->sourceMode_ && !self->applyingTags_ && self->isHighlightTag(tag))
        g_signal_stop_emission_by_name(buf, "apply-tag");
}

// ------------------------------------------------ rename

void Editor::setPath(const std::string& path) {
    const bool wasSettings = isSettingsFile();
    const std::string ext = extOf(path);
    path_ = path;
    if (ext == ext_ && wasSettings == isSettingsFile()) return;
    ext_ = ext;
    if (sourceMode_) startHighlighting();
}

// ---------------------------------------------------------------- settings

static GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}

static void setTagColor(GtkTextBuffer* buf, const char* name, const char* prop,
                        const Rgba& c) {
    GtkTextTag* tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf), name);
    if (!tag) return;
    GdkRGBA rgba = toGdk(c);
    g_object_set(tag, prop, &rgba, NULL);
}

void Editor::applySettings(const Settings& s) {
    settings_ = s;
    ensureTags();
    for (int i = 0; i <= (int)TokenStyle::Function; ++i)
        setTagColor(buffer_, TagNameForStyle((TokenStyle)i), "foreground-rgba",
                    s.syntax((TokenStyle)i));
    for (int lvl = 1; lvl <= 6; ++lvl) {
        char name[16];
        g_snprintf(name, sizeof(name), "h%d", lvl);
        setTagColor(buffer_, name, "foreground-rgba", s.markdown(MarkdownColor::Heading));
    }
    setTagColor(buffer_, "md_table", "foreground-rgba", s.text(Surface::Editor));
    setTagColor(buffer_, "md_code", "foreground-rgba", s.markdown(MarkdownColor::Code));
    setTagColor(buffer_, "md_code", "background-rgba", s.markdownCodeBackground());
    setTagColor(buffer_, "md_quote", "foreground-rgba", s.markdown(MarkdownColor::Quote));
    setTagColor(buffer_, "md_link", "foreground-rgba", s.markdown(MarkdownColor::Link));
    // Swatch text contrast depends on the editor background.
    if (sourceMode_ && isSettingsFile()) startHighlighting();
}

bool Editor::isSettingsFile() const {
    if (path_.empty() || settingsPath_.empty()) return false;
    if (path_ == settingsPath_) return true;
    char a[PATH_MAX], b[PATH_MAX];
    return realpath(path_.c_str(), a) && realpath(settingsPath_.c_str(), b) &&
           std::string(a) == b;
}

// Every color value in the settings file becomes a swatch of itself: the value
// drawn on its own color, in black or white text, whichever reads.
// Lines [firstLine, endLine), clamped to the buffer.
void Editor::decorateColors(int firstLine, int endLine) {
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    int lines = std::min(endLine, gtk_text_buffer_get_line_count(buffer_));
    for (int ln = std::max(firstLine, 0); ln < lines; ++ln) {
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(buffer_, &ls, ln);
        le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        char* raw = gtk_text_buffer_get_text(buffer_, &ls, &le, FALSE);
        std::u16string line = utf16::fromUtf8(raw ? raw : "");
        g_free(raw);
        ColorSpan span;
        if (!Settings::findColor(line, span)) continue;

        Rgba text = settings_.contrastText(span.color);
        std::string name = "swatch-" + Settings::formatColor(span.color) +
                           (text.rgb() == 0 ? "-k" : "-w");
        GtkTextTag* tag = gtk_text_tag_table_lookup(table, name.c_str());
        if (!tag) {
            GdkRGBA bg = toGdk(span.color), fg = toGdk(text);
            tag = gtk_text_buffer_create_tag(buffer_, name.c_str(),
                                             "background-rgba", &bg,
                                             "foreground-rgba", &fg, NULL);
            hlTags_.push_back(tag);
        }
        GtkTextIter a = ls, b = ls;
        gtk_text_iter_forward_chars(&a, (int)utf16::toCharOffset(line, span.start));
        gtk_text_iter_forward_chars(&b, (int)utf16::toCharOffset(line, span.start + span.length));
        gtk_text_buffer_apply_tag(buffer_, tag, &a, &b);
    }
}

// Is there a swatch under widget point (x, y)? Reports its line.
bool Editor::swatchAt(double x, double y, int* line) const {
    if (preview_ || !isSettingsFile()) return false;
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

void Editor::onMotion(GtkEventControllerMotion*, double x, double y, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    bool over = self->swatchAt(x, y, nullptr);
    if (over == self->overSwatch_) return;
    self->overSwatch_ = over;
    gtk_widget_set_cursor_from_name(self->view_, over ? "pointer" : "text");
}

void Editor::onPressed(GtkGestureClick* g, int n, double x, double y, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    int line;
    if (n != 1 || !self->swatchAt(x, y, &line)) return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    self->pickColor(line);
}

namespace {
struct PickRequest {
    Editor* editor;
    int line;
    std::string path;   // the file the pick was for
};
}  // namespace

// The system color dialog, starting on the swatch's color. The choice is
// written back when the dialog closes (GTK's dialog has no live preview).
void Editor::pickColor(int line) {
    GtkTextIter ls, le;
    gtk_text_buffer_get_iter_at_line(buffer_, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
    char* raw = gtk_text_buffer_get_text(buffer_, &ls, &le, FALSE);
    std::u16string text = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);
    ColorSpan span;
    if (!Settings::findColor(text, span)) return;

    GtkColorDialog* dlg = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(dlg, TRUE);
    gtk_color_dialog_set_modal(dlg, TRUE);
    gtk_color_dialog_set_title(dlg, "Choose a Color");
    GdkRGBA initial = toGdk(span.color);
    GtkRoot* root = gtk_widget_get_root(view_);
    auto* req = new PickRequest{this, line, path_};
    gtk_color_dialog_choose_rgba(
        dlg, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr, &initial, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            PickRequest* r = static_cast<PickRequest*>(data);
            GdkRGBA* c = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(src),
                                                            res, nullptr);
            if (c && r->editor->path_ == r->path) {
                auto byte = [](float v) {
                    return (uint8_t)(std::min(std::max(v, 0.0f), 1.0f) * 255 + 0.5f);
                };
                Rgba rgba;
                rgba.r = byte(c->red); rgba.g = byte(c->green); rgba.b = byte(c->blue);
                rgba.a = std::min(std::max((double)c->alpha, 0.0), 1.0);
                r->editor->setLineColor(r->line, rgba);
            }
            if (c) gdk_rgba_free(c);
            delete r;
        },
        req);
    g_object_unref(dlg);
}

// Rewrite one line's color (uncommenting it if it was a commented-out
// default) and save, so the change applies straight away.
void Editor::setLineColor(int line, const Rgba& c) {
    if (preview_ || line >= gtk_text_buffer_get_line_count(buffer_)) return;
    GtkTextIter ls, le;
    gtk_text_buffer_get_iter_at_line(buffer_, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
    char* raw = gtk_text_buffer_get_text(buffer_, &ls, &le, FALSE);
    std::u16string text = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);
    std::u16string updated = Settings::setColor(text, c);
    if (updated == text) return;
    std::string out = utf16::toUtf8(updated);

    gtk_text_buffer_begin_user_action(buffer_);
    gtk_text_buffer_delete(buffer_, &ls, &le);
    gtk_text_buffer_insert(buffer_, &ls, out.c_str(), (int)out.size());
    gtk_text_buffer_end_user_action(buffer_);
    save();   // the edit above already retagged the line, swatch included
}

// ---------------------------------------------------------------- comments

bool Editor::toggleComment() {
    if (preview_ || path_.empty() || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)))
        return false;
    const std::string marker = LineComments::markerFor(path_);
    if (marker.empty()) return false;

    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* raw = gtk_text_buffer_get_text(buffer_, &a, &b, FALSE);
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
    if (!isMarkdown_) return;
    preview_ = !preview_;
    if (preview_) {
        // Sync source_ from the buffer first, in case of unsaved edits.
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buffer_, &a, &b);
        char* txt = gtk_text_buffer_get_text(buffer_, &a, &b, FALSE);
        source_ = txt ? txt : "";
        g_free(txt);
        renderPreview();
    } else {
        loadRawIntoBuffer();
    }
}

void Editor::renderPreview() {
    stopHighlighting();
    showingMessage_ = false;
    g_signal_handlers_block_by_func(buffer_, (gpointer)onBufferChanged, this);
    setProseFont(true);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view_), FALSE);
    Markdown::render(buffer_, source_);
    g_signal_handlers_unblock_by_func(buffer_, (gpointer)onBufferChanged, this);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
}

// ---------------------------------------------------------------- dirty state

void Editor::markDirty(bool d) {
    if (dirty_ == d) return;
    dirty_ = d;
    if (titleCb_) titleCb_(titleUser_);
}

// ---------------------------------------------------------------- change hook

void Editor::onBufferChanged(GtkTextBuffer* /*buf*/, gpointer selfp) {
    Editor* self = static_cast<Editor*>(selfp);
    self->markDirty(true);
    // Highlighting is not done here: the insert-text and delete-range
    // handlers retag each edit's lines as it happens.
}
