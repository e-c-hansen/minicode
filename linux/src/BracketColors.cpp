// BracketColors.cpp — see BracketColors.h.
#include "BracketColors.h"

#include "BufferLines.h"
#include "Settings.h"

#include <algorithm>
#include <ctime>

namespace {

const char* const kTagNames[4] = {"bracket-pair-1", "bracket-pair-2", "bracket-pair-3",
                                  "bracket-pair-unexpected"};

GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}

double threadCpuMs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

double gCpuMs = 0;

// Adds the CPU time of a scope to gCpuMs.
struct Timed {
    double t0 = threadCpuMs();
    ~Timed() { gCpuMs += threadCpuMs() - t0; }
};

inline bool isBracketByte(char c) {
    return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}';
}

}  // namespace

double BracketColors::cpuMs() { return gCpuMs; }

GtkTextTag* BracketColors::tag(GtkTextTagTable* table, int i) {
    if (i < 0 || i > 3) return nullptr;
    GtkTextTag* t = gtk_text_tag_table_lookup(table, kTagNames[i]);
    if (t) return t;
    // Made after the highlighter's tags (Editor::ensureTags), so a bracket's
    // color wins over the style its token has.
    t = gtk_text_tag_new(kTagNames[i]);
    const GdkRGBA rgba = toGdk(Settings().bracketColor(i));
    g_object_set(t, "foreground-rgba", &rgba, NULL);
    gtk_text_tag_table_add(table, t);
    g_object_unref(t);
    return t;
}

void BracketColors::applySettings(GtkTextTagTable* table, const Settings& s) {
    for (int i = 0; i < 4; ++i) {
        const GdkRGBA rgba = toGdk(s.bracketColor(i));
        g_object_set(tag(table, i), "foreground-rgba", &rgba, NULL);
    }
}

BracketColors::BracketColors(GtkTextBuffer* buffer) : buffer_(buffer) {}

bool BracketColors::current(size_t line) const {
    auto it = std::upper_bound(current_.begin(), current_.end(), line,
                               [](size_t l, const std::pair<size_t, size_t>& r) {
                                   return l < r.first;
                               });
    if (it == current_.begin()) return false;
    --it;
    return line < it->second;
}

void BracketColors::markCurrent(size_t first, size_t end) {
    if (end <= first) return;
    std::vector<std::pair<size_t, size_t>> out;
    out.reserve(current_.size() + 1);
    bool placed = false;
    for (const auto& r : current_) {
        if (r.second < first) {
            out.push_back(r);
        } else if (r.first > end) {
            if (!placed) out.push_back({first, end});
            placed = true;
            out.push_back(r);
        } else {   // touching or overlapping: joined
            first = std::min(first, r.first);
            end = std::max(end, r.second);
        }
    }
    if (!placed) out.push_back({first, end});
    current_ = std::move(out);
}

void BracketColors::takeAllOff() {
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    for (int i = 0; i < 4; ++i) gtk_text_buffer_remove_tag(buffer_, tag(table, i), &a, &b);
}

void BracketColors::reset(IncrementalHighlighter<char>* hl, const std::string& text,
                          const std::vector<Token>& tokens) {
    Timed timed;
    takeAllOff();
    current_.clear();
    on_ = hl != nullptr;
    if (!hl) {
        depths_.assign({});
        return;
    }
    depths_.assign(BracketPairs::summarizeLines(StringSource<char>(text), *hl, 0,
                                                hl->lineCount(), tokens));
}

void BracketColors::edited(const IncrementalHighlighter<char>& hl, const TextSource<char>& text,
                           const IncrementalHighlighter<char>::Range& r, size_t linesBefore,
                           const std::vector<Token>& relexed) {
    if (!on_) return;
    Timed timed;
    const size_t first = r.firstLine;
    const size_t now = r.endLine - r.firstLine;
    const size_t replaced = now + linesBefore - hl.lineCount();
    const bool moved = depths_.replace(
        first, replaced,
        BracketPairs::summarizeLines(text, hl, first, r.endLine, relexed));
    // The edited lines are not current; those after them move with their
    // text, unless the edit changed the depth they start at.
    std::vector<std::pair<size_t, size_t>> out;
    out.reserve(current_.size() + 1);
    for (const auto& c : current_) {
        if (c.second <= first) {
            out.push_back(c);
            continue;
        }
        if (c.first < first) out.push_back({c.first, first});
        if (moved) continue;
        const size_t a = std::max(c.first, first + replaced);
        if (c.second > a) out.push_back({a - replaced + now, c.second - replaced + now});
    }
    current_ = std::move(out);
}

void BracketColors::paintLine(size_t line, int32_t depth, IncrementalHighlighter<char>& hl,
                              const std::string& text) {
    BufferLines* lines = BufferLines::of(buffer_);
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    const size_t c0 = lines->lineStart(line), c1 = lines->lineEnd(line);
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)c0);
    gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)c1);
    for (int i = 0; i < 4; ++i) gtk_text_buffer_remove_tag(buffer_, tag(table, i), &a, &b);
    linesPainted_++;
    const size_t b0 = hl.lineStart(line);
    const size_t b1 = line + 1 < hl.lineCount() ? hl.lineStart(line + 1) : hl.length();
    // A line with no bracket needs no lexing.
    bool any = false;
    for (size_t k = b0; k < b1 && k < text.size() && !any; ++k) any = isBracketByte(text[k]);
    if (!any) return;
    StringSource<char> src(text);
    std::vector<Token> tokens;
    hl.lineTokens(src, line, line + 1, tokens);
    std::vector<BracketPairs::Bracket> brackets;
    BracketPairs::colorLines(src, hl, line, line + 1, tokens, depth, &brackets);
    // Bytes to characters, through the line once.
    size_t byte = b0, chars = c0;
    for (const BracketPairs::Bracket& br : brackets) {
        for (; byte < br.at && byte < text.size(); ++byte)
            if (((unsigned char)text[byte] & 0xC0) != 0x80) chars++;
        GtkTextIter s = a, e;
        gtk_text_iter_set_offset(&s, (int)chars);
        e = s;
        gtk_text_iter_forward_char(&e);
        gtk_text_buffer_apply_tag(buffer_, tag(table, BracketPairs::colorIndex(br.depth)), &s, &e);
    }
}

void BracketColors::paintVisible(GtkTextView* view, IncrementalHighlighter<char>& hl,
                                 const std::string& text) {
    if (!on_ || painting_ || !view || gtk_text_view_get_buffer(view) != buffer_) return;
    BufferLines* lines = BufferLines::of(buffer_);
    if (!lines || lines->lineCount() != hl.lineCount() || depths_.lineCount() != hl.lineCount() ||
        hl.length() != text.size())
        return;
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(view, &vis);
    if (vis.width <= 0 || vis.height <= 0) return;
    Timed timed;
    GtkTextIter top, bottom;
    gtk_text_view_get_line_at_y(view, &top, vis.y, nullptr);
    gtk_text_view_get_line_at_y(view, &bottom, vis.y + vis.height - 1, nullptr);
    const size_t first = lines->lineOf((size_t)gtk_text_iter_get_offset(&top));
    const size_t last = std::min(lines->lineOf((size_t)gtk_text_iter_get_offset(&bottom)),
                                 hl.lineCount() - 1);
    bool all = true;
    for (size_t l = first; l <= last && all; ++l) all = current(l);
    if (all) return;
    painting_ = true;
    int32_t depth = depths_.depthAt(first);
    for (size_t l = first; l <= last; ++l) {
        const int32_t here = depth;
        depth = depths_.line(l).after(depth);
        if (current(l)) continue;
        paintLine(l, here, hl, text);
        markCurrent(l, l + 1);
    }
    painting_ = false;
}
