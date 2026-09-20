// Editor.cpp — see Editor.h. Mirrors the behavior of the macOS
// EditorController: monospace editor, VS Code dark palette, debounced full
// re-lex highlighting, and a Markdown preview that swaps the buffer contents.
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
    if (isSettingsFile()) decorateColors();
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
    if (!preview_ && isSettingsFile()) rehighlight();
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
void Editor::decorateColors() {
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    int lines = gtk_text_buffer_get_line_count(buffer_);
    for (int ln = 0; ln < lines; ++ln) {
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
    save();
    rehighlight();
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
