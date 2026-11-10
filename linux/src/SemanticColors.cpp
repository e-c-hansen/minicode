// SemanticColors.cpp — see SemanticColors.h.
#include "SemanticColors.h"

#include "BufferLines.h"
#include "Palette.h"
#include "Settings.h"

#include <algorithm>
#include <ctime>

namespace {

// Lsp::SemanticStyle less None, in its order.
const char* const kTagNames[6] = {"semantic-class",     "semantic-function",
                                  "semantic-variable",  "semantic-parameter",
                                  "semantic-property",  "semantic-constant"};

GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}

Rgba colorOf(const Settings& s, int i) {
    switch ((Lsp::SemanticStyle)(i + 1)) {
    case Lsp::SemanticStyle::Class: return s.semanticColor(SemanticColor::Class);
    case Lsp::SemanticStyle::Function: return s.syntax(TokenStyle::Function);
    case Lsp::SemanticStyle::Variable: return s.semanticColor(SemanticColor::Variable);
    case Lsp::SemanticStyle::Parameter: return s.semanticColor(SemanticColor::Parameter);
    case Lsp::SemanticStyle::Property: return s.semanticColor(SemanticColor::Property);
    default: return s.semanticColor(SemanticColor::Constant);
    }
}

double threadCpuMs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

double gCpuMs = 0;

struct Timed {
    double t0 = threadCpuMs();
    ~Timed() { gCpuMs += threadCpuMs() - t0; }
};

// The six made together, just above the lexer's tags: a semantic color wins
// over the lexer's and loses to the bracket colors and everything else.
void makeTags(GtkTextTagTable* table) {
    int above = -1;
    for (int k = 0; k < kTokenStyleCount; ++k)
        if (GtkTextTag* t = gtk_text_tag_table_lookup(table, TagNameForStyle((TokenStyle)k)))
            above = std::max(above, gtk_text_tag_get_priority(t));
    const Settings defaults;
    for (int i = 0; i < 6; ++i) {
        GtkTextTag* t = gtk_text_tag_new(kTagNames[i]);
        const GdkRGBA rgba = toGdk(colorOf(defaults, i));
        g_object_set(t, "foreground-rgba", &rgba, NULL);
        gtk_text_tag_table_add(table, t);
        if (above >= 0) gtk_text_tag_set_priority(t, ++above);
        g_object_unref(t);
    }
}

}  // namespace

double SemanticColors::cpuMs() { return gCpuMs; }

GtkTextTag* SemanticColors::tag(GtkTextTagTable* table, Lsp::SemanticStyle style) {
    const int i = (int)style - 1;
    if (i < 0 || i >= 6) return nullptr;
    GtkTextTag* t = gtk_text_tag_table_lookup(table, kTagNames[i]);
    if (t) return t;
    makeTags(table);
    return gtk_text_tag_table_lookup(table, kTagNames[i]);
}

bool SemanticColors::isTag(GtkTextTagTable* table, GtkTextTag* t) {
    for (int i = 0; i < 6; ++i)
        if (gtk_text_tag_table_lookup(table, kTagNames[i]) == t) return t != nullptr;
    return false;
}

void SemanticColors::applySettings(GtkTextTagTable* table, const Settings& s) {
    for (int i = 0; i < 6; ++i) {
        const GdkRGBA rgba = toGdk(colorOf(s, i));
        g_object_set(tag(table, (Lsp::SemanticStyle)(i + 1)), "foreground-rgba", &rgba, NULL);
    }
}

SemanticColors::SemanticColors(GtkTextBuffer* buffer) : buffer_(buffer) {}

void SemanticColors::takeOff(const GtkTextIter* a, const GtkTextIter* b) {
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    for (int i = 0; i < 6; ++i)
        gtk_text_buffer_remove_tag(buffer_, tag(table, (Lsp::SemanticStyle)(i + 1)), a, b);
}

void SemanticColors::set(std::vector<Lsp::SemanticSpan> spans) {
    spans_ = std::move(spans);
    current_ = true;
    painted_.clear();
}

void SemanticColors::edited() {
    if (!current_) return;
    current_ = false;
    std::vector<Lsp::SemanticSpan>().swap(spans_);
    std::vector<bool>().swap(painted_);
}

void SemanticColors::clear() {
    edited();
    if (!any_) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    takeOff(&a, &b);
    any_ = false;
}

void SemanticColors::lexed(const GtkTextIter* a, const GtkTextIter* b, size_t firstLine,
                           size_t endLine) {
    if (painting_) return;
    if (any_) takeOff(a, b);
    if (current_)
        for (size_t l = firstLine; l < endLine && l < painted_.size(); ++l) painted_[l] = false;
}

void SemanticColors::paintLine(size_t line, IncrementalHighlighter<char>& hl,
                               const std::string& text) {
    BufferLines* lines = BufferLines::of(buffer_);
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    const size_t c0 = lines->lineStart(line), c1 = lines->lineEnd(line);
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)c0);
    gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)c1);
    if (any_) takeOff(&a, &b);
    linesPainted_++;
    const size_t b0 = hl.lineStart(line);
    const size_t b1 = line + 1 < hl.lineCount() ? hl.lineStart(line + 1) : hl.length();
    // A line too long to lex keeps its plain color, as the lexer leaves it.
    if (b1 - b0 > kMaxTokenizationLineLength + 2) return;
    // The line's spans: the first ending past its start, on to its end.
    auto it = std::upper_bound(spans_.begin(), spans_.end(), b0,
                               [](size_t at, const Lsp::SemanticSpan& s) {
                                   return at < (size_t)s.start + s.length;
                               });
    std::vector<Lsp::SemanticSpan> here;
    for (; it != spans_.end() && it->start < b1; ++it) here.push_back(*it);
    if (here.empty()) return;
    StringSource<char> src(text);
    std::vector<Token> tokens;
    hl.lineTokens(src, line, line + 1, tokens);
    const std::vector<Lsp::SemanticSpan> cut = Lsp::semanticSpansOverCode(here, tokens);
    // Bytes to characters, through the line once.
    size_t byte = b0, chars = c0;
    auto charAt = [&](size_t to) {
        for (; byte < to && byte < text.size(); ++byte)
            if (((unsigned char)text[byte] & 0xC0) != 0x80) chars++;
        return chars;
    };
    for (const Lsp::SemanticSpan& s : cut) {
        if (s.start < b0) continue;
        const size_t end = std::min((size_t)s.start + s.length, b1);
        GtkTextIter ts = a, te = a;
        gtk_text_iter_set_offset(&ts, (int)charAt(s.start));
        gtk_text_iter_set_offset(&te, (int)charAt(end));
        if (gtk_text_iter_compare(&ts, &te) >= 0) continue;
        gtk_text_buffer_apply_tag(buffer_, tag(table, s.style), &ts, &te);
        any_ = true;
    }
}

void SemanticColors::paintVisible(GtkTextView* view, IncrementalHighlighter<char>& hl,
                                  const std::string& text) {
    if (!current_ || painting_ || !view || gtk_text_view_get_buffer(view) != buffer_) return;
    BufferLines* lines = BufferLines::of(buffer_);
    if (!lines || lines->lineCount() != hl.lineCount() || hl.length() != text.size() ||
        (!spans_.empty() && (size_t)spans_.back().start + spans_.back().length > text.size()))
        return;
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(view, &vis);
    if (vis.width <= 0 || vis.height <= 0) return;
    Timed timed;
    if (painted_.size() != hl.lineCount()) painted_.assign(hl.lineCount(), false);
    GtkTextIter top, bottom;
    gtk_text_view_get_line_at_y(view, &top, vis.y, nullptr);
    gtk_text_view_get_line_at_y(view, &bottom, vis.y + vis.height - 1, nullptr);
    const size_t first = lines->lineOf((size_t)gtk_text_iter_get_offset(&top));
    const size_t last = std::min(lines->lineOf((size_t)gtk_text_iter_get_offset(&bottom)),
                                 hl.lineCount() - 1);
    painting_ = true;
    for (size_t l = first; l <= last; ++l) {
        if (painted_[l]) continue;
        paintLine(l, hl, text);
        painted_[l] = true;
    }
    painting_ = false;
}
