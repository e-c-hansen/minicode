// Editor.cpp — see Editor.h. Mirrors the behavior of the macOS
// EditorController: monospace editor, VS Code dark palette, debounced full
// re-lex highlighting, and a Markdown preview that swaps the buffer contents.
#include "Editor.h"
#include "Palette.h"
#include "Markdown.h"
#include "SyntaxHighlighter.h"
#include "Utf8Offsets.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

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

    scroller_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), view_);
    gtk_widget_set_hexpand(scroller_, TRUE);
    gtk_widget_set_vexpand(scroller_, TRUE);

    g_signal_connect(buffer_, "changed", G_CALLBACK(onBufferChanged), this);

    ensureTags();
    showMessage("\n  MiniCode — a native C++ editor, now on GTK4\n\n"
                "  - Select a file in the sidebar to view it\n"
                "  - Source files are syntax-highlighted by type\n"
                "  - Markdown (.md) files render formatted (Ctrl+Shift+P)\n\n"
                "  Ctrl+O to open a different folder.");
}

Editor::~Editor() {
    if (rehiTimer_) g_source_remove(rehiTimer_);
}

// ---------------------------------------------------------------- tags

void Editor::ensureTags() {
    if (tagsReady_) return;
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    (void)table;

    // One tag per syntax style.
    for (int i = 0; i <= (int)TokenStyle::Function; ++i) {
        TokenStyle s = (TokenStyle)i;
        gtk_text_buffer_create_tag(buffer_, TagNameForStyle(s),
                                   "foreground", ColorForStyle(s), NULL);
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
    gtk_text_buffer_create_tag(buffer_, "md_table",
                               "foreground", pal::EditorText,
                               "family", "monospace",
                               "size-points", 13.0, NULL);   // keeps columns aligned
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
    if (!f) { showMessage(std::string("\n  Could not open: ") + path); return false; }
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

bool Editor::save() {
    if (path_.empty()) return false;
    // In preview mode the buffer holds rendered text; persist source_ instead.
    if (!preview_) {
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buffer_, &a, &b);
        char* txt = gtk_text_buffer_get_text(buffer_, &a, &b, FALSE);
        source_ = txt ? txt : "";
        g_free(txt);
    }
    std::ofstream out(path_, std::ios::binary);
    if (!out) return false;
    out << source_;
    out.close();
    markDirty(false);
    return true;
}

// ---------------------------------------------------------------- buffer fills

// Source code is monospace; the Markdown preview and the plain messages use the
// proportional UI font, with monospace reapplied per-tag for code and tables.
// The macOS build gets this from per-run NSFonts; GtkTextView needs the widget
// font switched, because a tag can add a family but the view's monospace flag
// would otherwise force every run to it.
void Editor::setProseFont(bool prose) {
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view_), prose ? FALSE : TRUE);
    if (prose) gtk_widget_add_css_class(view_, "minicode-prose");
    else       gtk_widget_remove_css_class(view_, "minicode-prose");
}

void Editor::loadRawIntoBuffer() {
    // Temporarily block change signals so filling the buffer doesn't mark dirty.
    g_signal_handlers_block_by_func(buffer_, (gpointer)onBufferChanged, this);
    setProseFont(false);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view_), TRUE);
    gtk_text_buffer_set_text(buffer_, source_.c_str(), (int)source_.size());
    g_signal_handlers_unblock_by_func(buffer_, (gpointer)onBufferChanged, this);
    rehighlight();

    // Scroll to top.
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
}

void Editor::showMessage(const std::string& msg) {
    ensureTags();
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

// SyntaxHighlighter emits BYTE offsets into the UTF-8 source; GtkTextBuffer
// iterators index by CHARACTER. Utf8OffsetCursor does the conversion with a
// single forward-only pass over the whole token stream (see Utf8Offsets.h — it
// is unit-tested in linux/tests/run_tests.cpp, including against real token
// streams over accented, CJK and emoji text).
void Editor::rehighlight() {
    if (ext_.empty() || !SyntaxHighlighter::supports(ext_)) return;

    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* ctext = gtk_text_buffer_get_text(buffer_, &a, &b, FALSE);
    std::string text = ctext ? ctext : "";
    g_free(ctext);

    // Reset baseline color across the whole buffer, then apply tags.
    gtk_text_buffer_remove_all_tags(buffer_, &a, &b);

    std::vector<Token> tokens = SyntaxHighlighter::highlight(text, ext_);
    const long nbytes = (long)text.size();
    Utf8OffsetCursor cursor(text);

    for (const Token& t : tokens) {
        long sb = (long)t.start;
        long eb = (long)(t.start + t.length);
        if (sb < 0 || eb > nbytes || eb <= sb) continue;
        int cstart = (int)cursor.charOffset(sb);
        int cend   = (int)cursor.charOffset(eb);
        GtkTextIter ts, te;
        gtk_text_buffer_get_iter_at_offset(buffer_, &ts, cstart);
        gtk_text_buffer_get_iter_at_offset(buffer_, &te, cend);
        gtk_text_buffer_apply_tag_by_name(buffer_,
                                          TagNameForStyle(t.style), &ts, &te);
    }
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
    if (self->preview_) return;  // preview buffer isn't user-edited source

    // Debounce: re-highlight ~120ms after the user stops typing.
    if (self->rehiTimer_) g_source_remove(self->rehiTimer_);
    self->rehiTimer_ = g_timeout_add(120, [](gpointer p) -> gboolean {
        Editor* e = static_cast<Editor*>(p);
        e->rehiTimer_ = 0;
        e->rehighlight();
        return G_SOURCE_REMOVE;
    }, self);
}
