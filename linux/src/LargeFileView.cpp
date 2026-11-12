// LargeFileView.cpp — see LargeFileView.h. Besides LongLines and
// LargeFileUi, this file holds the Document and Editor members of item 1.12
// (declared in Document.h and Editor.h under "Big files"), so the shared
// files carry only the hooks.
#include "LargeFileView.h"

#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "Folds.h"
#include "LargeFile.h"   // the core's thresholds and rules
#include "MediaView.h"
#include "Utf8Offsets.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace {

GQuark longLinesQuark() {
    static GQuark q = g_quark_from_static_string("minicode-long-lines");
    return q;
}

// The tag that hides what a long line does not show, one per tag table
// (every document shares the editor's).
GtkTextTag* hiddenTagIn(GtkTextTagTable* table) {
    static const char* kName = "minicode-long-line-rest";
    GtkTextTag* t = gtk_text_tag_table_lookup(table, kName);
    if (!t) {
        t = gtk_text_tag_new(kName);
        g_object_set(t, "invisible", TRUE, NULL);
        gtk_text_tag_table_add(table, t);
        g_object_unref(t);   // the table holds it
    }
    return t;
}

// A long line never wraps, word wrap on or not (PARITY.md 1.11): GTK takes
// a paragraph's wrap mode from the tags on its first character, so this
// tag on that one character keeps the part shown on one row, End going to
// its end and on into the hidden rest, as before wrapping.
GtkTextTag* noWrapTagIn(GtkTextTagTable* table) {
    static const char* kName = "minicode-long-line-nowrap";
    GtkTextTag* t = gtk_text_tag_table_lookup(table, kName);
    if (!t) {
        t = gtk_text_tag_new(kName);
        g_object_set(t, "wrap-mode", GTK_WRAP_NONE, NULL);
        gtk_text_tag_table_add(table, t);
        g_object_unref(t);   // the table holds it
    }
    return t;
}

// Line `line`'s start and end (before its break), and its characters.
size_t lineSpan(GtkTextBuffer* b, int line, GtkTextIter* start, GtkTextIter* end) {
    gtk_text_buffer_get_iter_at_line(b, start, line);
    *end = *start;
    if (!gtk_text_iter_ends_line(end)) gtk_text_iter_forward_to_line_end(end);
    return (size_t)(gtk_text_iter_get_offset(end) - gtk_text_iter_get_offset(start));
}

// Whether `tag` covers any of [start, end).
bool tagIn(GtkTextTag* tag, const GtkTextIter* start, const GtkTextIter* end) {
    if (gtk_text_iter_has_tag(start, tag)) return true;
    GtkTextIter it = *start;
    return gtk_text_iter_forward_to_tag_toggle(&it, tag) && gtk_text_iter_compare(&it, end) < 0;
}

std::string baseName(const std::string& p) {
    const auto slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

double gLongLinesCpuMs = 0;

double threadCpuMs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// The CPU time of the work below, added to gLongLinesCpuMs when it ends
// (outermost only: apply runs inside lineEdited and followCaret).
struct CpuSpan {
    static int depth;
    double t0;
    CpuSpan() : t0(depth++ == 0 ? threadCpuMs() : 0) {}
    ~CpuSpan() {
        if (--depth == 0) gLongLinesCpuMs += threadCpuMs() - t0;
    }
};
int CpuSpan::depth = 0;

}  // namespace

double LongLines::cpuMs() { return gLongLinesCpuMs; }

// ================================================================ reading

bool LargeFileView::readsInBackground(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           (uint64_t)st.st_size > kBackgroundBytes && (uint64_t)st.st_size <= kMaxTextBytes;
}

bool LargeFileView::indentSlice(GtkTextBuffer* buffer, const GtkTextIter* from,
                                const GtkTextIter* to, std::u16string& text, long& base) {
    if (gtk_text_buffer_get_char_count(buffer) <= kIndentWholeChars) return false;
    GtkTextIter a = *from, b = *to;
    gtk_text_iter_order(&a, &b);
    gtk_text_buffer_get_iter_at_line(buffer, &a,
                                     std::max(0, gtk_text_iter_get_line(&a) - kIndentLinesAbove));
    if (!gtk_text_iter_ends_line(&b)) gtk_text_iter_forward_to_line_end(&b);
    char* raw = gtk_text_buffer_get_text(buffer, &a, &b, TRUE);
    text = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);
    base = gtk_text_iter_get_offset(&a);
    return true;
}

// ================================================================ LongLines
//
// The handlers run after GtkTextBuffer's own (connect_after), so the edit is
// in the buffer; a tag changed here invalidates iterators, so the ones the
// signal passes are set again from their offsets before returning, for the
// handlers after this one and for the caller.

LongLines* LongLines::attach(GtkTextBuffer* buffer) {
    if (LongLines* had = of(buffer)) return had;
    LongLines* l = new LongLines(buffer);
    // Freed when the buffer is finalized (its handlers are gone by then).
    g_object_set_qdata_full(G_OBJECT(buffer), longLinesQuark(), l,
                            [](gpointer p) { delete static_cast<LongLines*>(p); });
    return l;
}

LongLines* LongLines::of(GtkTextBuffer* buffer) {
    if (!buffer) return nullptr;
    return static_cast<LongLines*>(g_object_get_qdata(G_OBJECT(buffer), longLinesQuark()));
}

LongLines::LongLines(GtkTextBuffer* buffer) : buffer_(buffer) {
    tag_ = hiddenTagIn(gtk_text_buffer_get_tag_table(buffer));
    noWrap_ = noWrapTagIn(gtk_text_buffer_get_tag_table(buffer));
    g_signal_connect_after(buffer, "insert-text", G_CALLBACK(onInsertAfter), this);
    g_signal_connect_after(buffer, "delete-range", G_CALLBACK(onDeleteAfter), this);
    g_signal_connect(buffer, "apply-tag", G_CALLBACK(onApplyTag), this);
    g_signal_connect_after(buffer, "mark-set", G_CALLBACK(onMarkSet), this);
}

// Runs while the buffer is finalized: nothing of it may be touched.
LongLines::~LongLines() {
    if (followIdle_) g_source_remove(followIdle_);
}

int LongLines::listen(std::function<void()> onMoved) {
    listeners_.push_back({++nextListener_, std::move(onMoved)});
    return nextListener_;
}

void LongLines::unlisten(int id) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                    [id](const auto& l) { return l.first == id; }),
                     listeners_.end());
}

size_t LongLines::followedStart(int line) const {
    if (!follow_) return 0;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buffer_, &it, follow_);
    return gtk_text_iter_get_line(&it) == line ? (size_t)gtk_text_iter_get_line_offset(&it) : 0;
}

bool LongLines::partOf(int line, Part* out) const {
    if (line < 0 || line >= gtk_text_buffer_get_line_count(buffer_)) return false;
    GtkTextIter ls, le;
    const size_t len = lineSpan(buffer_, line, &ls, &le);
    if (len <= LargeFile::kLongLineShown) return false;
    // As the tag lies: the first run not hidden.
    GtkTextIter it = ls;
    if (gtk_text_iter_has_tag(&it, tag_) && !gtk_text_iter_forward_to_tag_toggle(&it, tag_))
        it = le;
    if (gtk_text_iter_compare(&it, &le) > 0) it = le;
    const size_t start = (size_t)(gtk_text_iter_get_offset(&it) - gtk_text_iter_get_offset(&ls));
    GtkTextIter stop = it;
    if (!gtk_text_iter_forward_to_tag_toggle(&stop, tag_) || gtk_text_iter_compare(&stop, &le) > 0)
        stop = le;
    out->length = len;
    out->start = start;
    out->end = (size_t)(gtk_text_iter_get_offset(&stop) - gtk_text_iter_get_offset(&ls));
    return true;
}

// The runs of [ls, le) the tag does not hide, as offsets from ls.
std::vector<std::pair<size_t, size_t>> LongLines::visibleRuns(const GtkTextIter* ls,
                                                              const GtkTextIter* le) const {
    std::vector<std::pair<size_t, size_t>> runs;
    const int base = gtk_text_iter_get_offset(ls), end = gtk_text_iter_get_offset(le);
    GtkTextIter it = *ls;
    bool hidden = gtk_text_iter_has_tag(&it, tag_);
    int from = base;
    while (from < end && runs.size() < 64) {
        GtkTextIter next = it;
        const bool toggled = gtk_text_iter_forward_to_tag_toggle(&next, tag_);
        const int to = toggled ? std::min(gtk_text_iter_get_offset(&next), end) : end;
        if (!hidden && to > from) runs.push_back({(size_t)(from - base), (size_t)(to - base)});
        if (!toggled) break;
        it = next;
        from = to;
        hidden = gtk_text_iter_has_tag(&it, tag_);
    }
    return runs;
}

// The line's hidden ranges from its length and where its part starts (the
// caret's line keeps its own): all of it shown up to kLongLineShown
// characters, and nothing done for a short line the tag never touched.
//
// Typing in the part shown makes it grow (GTK puts text typed at either end
// of a tagged range outside it), and that is left as it is up to twice the
// part's size: hiding the rest again on every keystroke cost 20 ms a key in
// a 5 MB line, most of it GTK joining and splitting the line's text. Text
// put in past the part (a file read a piece at a time, a paste) has the tag
// put over it alone.
void LongLines::apply(int line) {
    CpuSpan span;
    if (line < 0 || line >= gtk_text_buffer_get_line_count(buffer_)) return;
    GtkTextIter ls, le;
    const size_t len = lineSpan(buffer_, line, &ls, &le);
    const bool longLine = len > LargeFile::kLongLineShown;
    if (!longLine && !tagIn(tag_, &ls, &le) && !tagIn(noWrap_, &ls, &le)) return;
    const int at = gtk_text_iter_get_offset(&ls);
    const LargeFile::Shown s = LargeFile::shownPart(len, followedStart(line));
    if (longLine) {
        const auto runs = visibleRuns(&ls, &le);
        const size_t w = LargeFile::kLongLineShown;
        if (!runs.empty() && runs[0].first == s.start && runs[0].second - runs[0].first >= w / 2 &&
            runs[0].second - runs[0].first <= 2 * w &&
            !(runs.size() == 1 && runs[0].first == 0 && runs[0].second == len)) {
            if (runs.size() == 1) {   // as it should be
                keepUnwrapped(line);
                return;
            }
            applying_ = true;
            ++applies_;
            for (size_t i = 1; i < runs.size(); i++) {
                GtkTextIter a, b;
                gtk_text_buffer_get_iter_at_offset(buffer_, &a, at + (int)runs[i].first);
                gtk_text_buffer_get_iter_at_offset(buffer_, &b, at + (int)runs[i].second);
                gtk_text_buffer_apply_tag(buffer_, tag_, &a, &b);
            }
            applying_ = false;
            keepUnwrapped(line);
            return;
        }
    }
    applying_ = true;
    ++applies_;
    gtk_text_buffer_remove_tag(buffer_, tag_, &ls, &le);
    if (!longLine) {   // the iterators again: a tag changed
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, at);
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, at + (int)len);
        gtk_text_buffer_remove_tag(buffer_, noWrap_, &a, &b);
    }
    if (longLine) {
        GtkTextIter a, b;
        if (s.start > 0) {
            gtk_text_buffer_get_iter_at_offset(buffer_, &a, at);
            gtk_text_buffer_get_iter_at_offset(buffer_, &b, at + (int)s.start);
            gtk_text_buffer_apply_tag(buffer_, tag_, &a, &b);
        }
        if (s.end < len) {
            gtk_text_buffer_get_iter_at_offset(buffer_, &a, at + (int)s.end);
            gtk_text_buffer_get_iter_at_offset(buffer_, &b, at + (int)len);
            gtk_text_buffer_apply_tag(buffer_, tag_, &a, &b);
        }
        if (follow_ && followedStart(line) != s.start) {
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_mark(buffer_, &it, follow_);
            if (gtk_text_iter_get_line(&it) == line) {
                gtk_text_buffer_get_iter_at_offset(buffer_, &it, at + (int)s.start);
                gtk_text_buffer_move_mark(buffer_, follow_, &it);
            }
        }
    }
    applying_ = false;
    if (longLine) keepUnwrapped(line);
}

// The no-wrap tag on the long line's first character (noWrapTagIn); text
// typed before it, or its character deleted, leaves the line's start
// without it, so it is looked for after every edit of the line.
void LongLines::keepUnwrapped(int line) {
    GtkTextIter a;
    gtk_text_buffer_get_iter_at_line(buffer_, &a, line);
    if (gtk_text_iter_ends_line(&a) || gtk_text_iter_has_tag(&a, noWrap_)) return;
    GtkTextIter b = a;
    gtk_text_iter_forward_char(&b);
    applying_ = true;
    gtk_text_buffer_apply_tag(buffer_, noWrap_, &a, &b);
    applying_ = false;
}

// An edit at `at` (after it: the end of an insertion, or where a deletion
// was): the lines it touched, long ones hidden past their part, and a line
// no longer long shown whole. `text` is what was inserted (null for a
// deletion), whose own long lines are found by reading its bytes, so a whole
// file put in costs one pass over it and no iterator per line.
void LongLines::lineEdited(GtkTextIter* at, const char* text, int len, bool deleted) {
    CpuSpan span;
    const int keep = gtk_text_iter_get_offset(at);
    const int last = gtk_text_iter_get_line(at);
    std::vector<int> lines;
    if (deleted || !text || len <= 0) {
        lines.push_back(last);
    } else {
        const size_t n = (size_t)len;
        const size_t breaks = LargeFile::countLinesUtf8(text, n, (size_t)-1) - 1;
        const int first = last - (int)breaks;
        lines.push_back(last);
        if (first != last) lines.push_back(first);
        for (const auto& [k, chars] : LargeFile::longLinesUtf8(text, n, LargeFile::kLongLineShown)) {
            (void)chars;
            if (k != 0 && k != breaks) lines.push_back(first + (int)k);
        }
    }
    for (int line : lines) apply(line);
    // An edit in a long line: GTK's own scroll to the caret after a key
    // measures the line as it was before the key (with text hidden in it),
    // and left the caret past the view's edge, so the listeners hear of it.
    GtkTextIter ls, le;
    const bool longLine = lineSpan(buffer_, last, &ls, &le) > LargeFile::kLongLineShown;
    gtk_text_buffer_get_iter_at_offset(buffer_, at, keep);
    scheduleFollow();
    if (longLine && !listeners_.empty())
        for (const auto& l : std::vector<std::pair<int, std::function<void()>>>(listeners_)) l.second();
}

void LongLines::onInsertAfter(GtkTextBuffer*, GtkTextIter* at, char* text, int len, gpointer p) {
    static_cast<LongLines*>(p)->lineEdited(at, text, len, false);
}

void LongLines::onDeleteAfter(GtkTextBuffer* b, GtkTextIter* from, GtkTextIter* to, gpointer p) {
    LongLines* self = static_cast<LongLines*>(p);
    const int keep = gtk_text_iter_get_offset(from);
    self->lineEdited(from, nullptr, 0, true);
    gtk_text_buffer_get_iter_at_offset(b, to, keep);
}

// The hidden tag is this object's alone: a paste from a GtkTextView brings
// the copied range's tags, which would hide the pasted text.
void LongLines::onApplyTag(GtkTextBuffer* b, GtkTextTag* tag, GtkTextIter*, GtkTextIter*,
                           gpointer p) {
    LongLines* self = static_cast<LongLines*>(p);
    if ((tag == self->tag_ || tag == self->noWrap_) && !self->applying_)
        g_signal_stop_emission_by_name(b, "apply-tag");
}

void LongLines::onMarkSet(GtkTextBuffer* b, GtkTextIter*, GtkTextMark* mark, gpointer p) {
    if (mark == gtk_text_buffer_get_insert(b)) static_cast<LongLines*>(p)->scheduleFollow();
}

// Before GTK lays out or draws (G_PRIORITY_HIGH_IDLE runs ahead of both), so
// a line is never laid out with the caret in its hidden part for long; and
// only once for a burst of moves.
void LongLines::scheduleFollow() {
    if (followIdle_) return;
    followIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE, [](gpointer p) -> gboolean {
        LongLines* self = static_cast<LongLines*>(p);
        self->followIdle_ = 0;
        self->followCaret();
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

void LongLines::followCaret() {
    CpuSpan span;
    if (followIdle_) g_source_remove(followIdle_);
    followIdle_ = 0;
    GtkTextIter caret, ls, le;
    gtk_text_buffer_get_iter_at_mark(buffer_, &caret, gtk_text_buffer_get_insert(buffer_));
    const int line = gtk_text_iter_get_line(&caret);
    int was = -1;
    if (follow_) {
        GtkTextIter f;
        gtk_text_buffer_get_iter_at_mark(buffer_, &f, follow_);
        was = gtk_text_iter_get_line(&f);
    }
    const size_t len = lineSpan(buffer_, line, &ls, &le);
    if (len <= LargeFile::kLongLineShown) {
        if (!follow_) return;
        // The caret left the long line it was on: that line shows its start
        // again, as every line the caret is not on does.
        gtk_text_buffer_delete_mark(buffer_, follow_);
        follow_ = nullptr;
        apply(was);
        return;
    }
    const size_t start = was == line ? followedStart(line) : 0;
    const size_t col = (size_t)(gtk_text_iter_get_offset(&caret) - gtk_text_iter_get_offset(&ls));
    const LargeFile::Shown s = LargeFile::shownPart(len, start, col);
    if (was == line && s.start == start) return;
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, gtk_text_iter_get_offset(&ls) + (int)s.start);
    if (follow_) gtk_text_buffer_move_mark(buffer_, follow_, &at);
    else follow_ = gtk_text_buffer_create_mark(buffer_, nullptr, &at, TRUE);
    if (was >= 0 && was != line) apply(was);   // back to its start
    apply(line);
    for (const auto& l : std::vector<std::pair<int, std::function<void()>>>(listeners_)) l.second();
}

// ================================================================ Document

namespace {

// What the worker hands back. The document is touched only on the main
// thread, and only while `cancelled` is false (set on the main thread when
// the document lets go of the load).
struct LoadJob {
    std::atomic<bool> cancelled{false};
    Document* doc = nullptr;
    std::string path, ext;   // ext: the grammar to lex with, "" for none
    bool optimizations = true;
    // Results.
    Document::Read result = Document::Read::Failed;
    std::string content;
    Document::DiskStamp stamp;
    size_t chars = 0, utf16 = 0;
    bool large = false, blank = false;
    std::unique_ptr<IncrementalHighlighter<char>> hl;
    std::string mirror;
};

// What a load keeps on the main thread.
constexpr size_t kPieceBytes = 64 * 1024;
constexpr size_t kLinePieceBytes = 1024 * 1024;   // a piece inside one long line
constexpr gint64 kPieceBudgetMicros = 8000;   // a turn of the main loop, as the tree's splices

}  // namespace

struct Document::Load {
    std::shared_ptr<LoadJob> job;
    std::string content;
    size_t at = 0;
    guint idle = 0;
    GtkTextBuffer* placeholder = nullptr;
    std::vector<std::function<void()>> then;
};

GtkTextBuffer* Document::loadingBuffer() const { return load_ ? load_->placeholder : nullptr; }

void Document::whenLoaded(std::function<void()> f) {
    if (load_) load_->then.push_back(std::move(f));
    else f();
}

void Document::cancelLoading() {
    if (!load_) return;
    if (load_->job) load_->job->cancelled = true;
    if (load_->idle) g_source_remove(load_->idle);
    if (load_->placeholder) g_object_unref(load_->placeholder);
    filling_ = false;
    delete load_;
    load_ = nullptr;
}

void Document::beginLoading(const std::string& path) {
    clearSource();
    kind_ = Kind::Text;
    path_ = path;
    ext_ = extensionOf(path);
    markdown_ = ext_ == "md" || ext_ == "markdown";
#ifdef MINICODE_ENABLE_PDF
    latex_ = ext_ == "tex" || ext_ == "ltx" || ext_ == "latex";
#endif
    gtk_text_buffer_set_enable_undo(buffer_, FALSE);
    fill("");
    load_ = new Load;
    load_->placeholder = gtk_text_buffer_new(tags_->table);
    struct stat st;
    std::string size;
    if (stat(path.c_str(), &st) == 0) {
        char* s = g_format_size((guint64)st.st_size);
        size = std::string(" (") + s + ")";
        g_free(s);
    }
    const std::string note = "\n  Opening “" + baseName(path) + "”" + size + "…";
    gtk_text_buffer_set_text(load_->placeholder, note.c_str(), -1);
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(load_->placeholder, &a, &b);
    gtk_text_buffer_apply_tag_by_name(load_->placeholder, "plainmsg", &a, &b);

    auto job = std::make_shared<LoadJob>();
    job->doc = this;
    job->path = path;
    job->optimizations = host_->documentSettings().largeFileOptimizations();
    if (!ext_.empty() && SyntaxHighlighter::supports(ext_) && !isSettingsFile()) job->ext = ext_;
    load_->job = job;
    // Read, check, measure and lex off the main thread; the result goes back
    // to it through the default main context.
    std::thread([job] {
        struct stat st;
        if (stat(job->path.c_str(), &st) == 0) {   // before the read, as the Mac's stamp
            job->stamp.exists = true;
            job->stamp.mtime = (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
            job->stamp.inode = (uint64_t)st.st_ino;
            job->stamp.size = (int64_t)st.st_size;
        }
        job->result = Document::readText(job->path, job->content);
        if (job->result == Document::Read::Ok && !job->cancelled) {
            const std::string& c = job->content;
            for (unsigned char ch : c)
                if ((ch & 0xC0) != 0x80) job->chars++;
            job->utf16 = LargeFile::utf16Length(c.data(), c.size());
            job->large = LargeFile::isLargeUtf8(c.data(), c.size());
            job->blank = std::all_of(c.begin(), c.end(), [](unsigned char ch) { return std::isspace(ch); });
            if (!job->ext.empty() && !(job->large && job->optimizations)) {
                job->mirror = c;
                job->hl = std::make_unique<IncrementalHighlighter<char>>(job->ext);
                job->hl->reset(StringSource<char>(job->mirror), nullptr);
            }
        }
        auto* hold = new std::shared_ptr<LoadJob>(job);
        g_idle_add_full(G_PRIORITY_DEFAULT, &Document::onLoadRead, hold,
                        [](gpointer p) { delete static_cast<std::shared_ptr<LoadJob>*>(p); });
    }).detach();
}

gboolean Document::onLoadRead(gpointer p) {
    const std::shared_ptr<LoadJob>& job = *static_cast<std::shared_ptr<LoadJob>*>(p);
    if (!job->cancelled) job->doc->loadRead();
    return G_SOURCE_REMOVE;
}

void Document::loadRead() {
    std::shared_ptr<LoadJob> job = load_->job;
    if (job->result != Read::Ok) {
        // Why not, in the tab (Editor::textRead), as for a file read at once.
        std::vector<std::function<void()>> then = std::move(load_->then);
        const Read r = job->result;
        cancelLoading();
        host_->documentLoaded(this, (int)r, false);
        for (auto& f : then) f();
        return;
    }
    load_->content = std::move(job->content);
    load_->idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, onLoadIdle, this, nullptr);
}

gboolean Document::onLoadIdle(gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (!self->putPieces()) return G_SOURCE_CONTINUE;
    self->load_->idle = 0;
    self->finishLoading();
    return G_SOURCE_REMOVE;
}

// Pieces at the buffer's end until the turn's budget is spent, cut where
// LargeFile::pieceEnd says (after a line break, never inside "\r\n" or a
// character; up to 1 MB inside a long line). No handler counts them as
// edits (filling_), and there is no undo while they go in.
bool Document::putPieces() {
    Load* L = load_;
    const std::string& c = L->content;
    const gint64 t0 = g_get_monotonic_time();
    filling_ = true;
    while (L->at < c.size() && g_get_monotonic_time() - t0 < kPieceBudgetMicros) {
        const size_t end = LargeFile::pieceEnd(c.data(), c.size(), L->at, kPieceBytes, kLinePieceBytes);
        GtkTextIter last;
        gtk_text_buffer_get_end_iter(buffer_, &last);
        gtk_text_buffer_insert(buffer_, &last, c.data() + L->at, (int)(end - L->at));
        L->at = end;
    }
    filling_ = false;
    return L->at >= c.size();
}

// All of it is in: what loadText does after its fill, from what the worker
// found, then the host shows it.
void Document::finishLoading() {
    Load* L = load_;
    std::shared_ptr<LoadJob> job = L->job;
    diskKnown_ = true;
    saved_ = std::move(L->content);
    savedChars_ = (long)job->chars;
    diskQuestion_ = false;
    diskStamp_ = job->stamp;
    largeFile_ = job->large;
    largeOff_ = job->large && job->optimizations;
    utf16Length_ = job->utf16;
    ++edits_;
    gtk_text_buffer_set_enable_undo(buffer_, TRUE);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
    if (job->hl && !largeFileFeaturesOff()) {
        // Lexed on the worker over the same bytes: only the tags are left,
        // the lines on screen at once and the rest in idle slices.
        cancelDeferred();
        hl_ = std::move(job->hl);
        mirror_ = std::move(job->mirror);
        bulk_ = false;
        pendStart_ = 0;
        pendEnd_ = mirror_.size();
        pending_ = true;
        flushHighlighting();
    } else {
        startHighlighting();
    }
    watch();
    refreshTaskCount();
    const bool blank = job->blank;
    std::vector<std::function<void()>> then = std::move(L->then);
    job->cancelled = true;   // nothing more of it
    cancelLoading();
    host_->documentLoaded(this, (int)Read::Ok, blank);
    for (auto& f : then) f();
}

void Document::noteLargeFile(const std::string& content) {
    largeFile_ = LargeFile::isLargeUtf8(content.data(), content.size());
    largeOff_ = largeFile_ && host_->documentSettings().largeFileOptimizations();
    largeOn_ = false;
    // Bytes are never fewer than UTF-16 units, so a text within the sync
    // limit in bytes is within it in units.
    utf16Length_ = content.size() > LargeFile::kSyncLimit
                       ? LargeFile::utf16Length(content.data(), content.size())
                       : content.size();
}

bool Document::languageServerMayRead() const {
    if (loading()) return false;
    return LargeFile::modeFor(utf16Length_, largeFile_, largeOff_, largeOn_).languageServer;
}

void Document::turnOnLargeFileFeatures() {
    if (!largeFileFeaturesOff()) return;
    largeOn_ = true;
    // The user asked for it: the whole text is lexed now, as on the Mac, and
    // its regions found (Folds.h).
    startHighlighting();
    folds_->sourceChanged();
}

// ================================================================ Editor

GtkWidget* Editor::largeFileNote() const { return large_->note(); }

void Editor::turnOnLargeFileFeatures() {
    if (!active_ || !active_->largeFileFeaturesOff()) return;
    active_->turnOnLargeFileFeatures();
    notifyDocument();   // a language server may have it now
    applyWordWrap();    // wrapping is one of them (Editor::setProseFont)
    large_->update();
}

void Editor::documentLoaded(Document* d, int result, bool blank) {
    if ((Document::Read)result != Document::Read::Ok)
        textRead(d, d->path(), result, std::string());
    else
        d->setShowsPreview((d->isMarkdown() || d->isLatex() || d->isNotebook()) && !blank);
    if (d->place().haveRestore && !d->unread()) {
        d->place().haveRestore = false;
        placeTab(d, d->place().restore);
    }
    if (d == active_) showActive();
    else refreshStrip();
}

// ================================================================ LargeFileUi

LargeFileUi::LargeFileUi(Editor* editor) : editor_(editor) {
    note_ = gtk_label_new("Large file: features off");
    gtk_widget_set_cursor_from_name(note_, "pointer");
    gtk_widget_set_visible(note_, FALSE);
    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "released", G_CALLBACK(+[](GtkGestureClick*, int, double, double,
                                                     gpointer self) {
        static_cast<LargeFileUi*>(self)->editor_->turnOnLargeFileFeatures();
    }), this);
    gtk_widget_add_controller(note_, GTK_EVENT_CONTROLLER(click));
    // A tab switch, and a file read in the background coming on show, put
    // another buffer in the view.
    g_signal_connect_swapped(editor_->textView(), "notify::buffer", G_CALLBACK(+[](gpointer self) {
        static_cast<LargeFileUi*>(self)->update();
    }), this);
    pass_ = editor_->codeView()->addPass(CodeView::Layer::AboveText, 40,
        [this](GtkSnapshot* s, const GdkRectangle& visible) { drawNotes(s, visible); });
    g_signal_connect(editor_->textView(), "move-cursor", G_CALLBACK(onMoveBefore), this);
    g_signal_connect_after(editor_->textView(), "move-cursor", G_CALLBACK(onMoveAfter), this);
    g_signal_connect(editor_->textView(), "copy-clipboard", G_CALLBACK(+[](GtkTextView* v, gpointer) {
        if (copyWhole(v, false)) g_signal_stop_emission_by_name(v, "copy-clipboard");
    }), this);
    g_signal_connect(editor_->textView(), "cut-clipboard", G_CALLBACK(+[](GtkTextView* v, gpointer) {
        if (copyWhole(v, true)) g_signal_stop_emission_by_name(v, "cut-clipboard");
    }), this);
}

// GTK's copy hands over the selection's text without what an invisible tag
// hides: Select All and Copy on a minified file gave its first 10,001
// characters. A selection holding hidden text is copied (and cut) here,
// whole, as plain text; any other is left to GTK.
bool LargeFileUi::copyWhole(GtkTextView* v, bool cut) {
    GtkTextBuffer* b = gtk_text_view_get_buffer(v);
    GtkTextIter a, z;
    if (!gtk_text_buffer_get_selection_bounds(b, &a, &z)) return false;
    char* all = gtk_text_buffer_get_text(b, &a, &z, TRUE);
    char* shown = gtk_text_buffer_get_text(b, &a, &z, FALSE);
    const bool hidden = strcmp(all, shown) != 0;
    g_free(shown);
    if (!hidden) {
        g_free(all);
        return false;
    }
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(v)), all);
    g_free(all);
    if (cut && gtk_text_view_get_editable(v)) {
        gtk_text_buffer_begin_user_action(b);
        gtk_text_buffer_delete_selection(b, TRUE, TRUE);
        gtk_text_buffer_end_user_action(b);
    }
    return true;
}

void LargeFileUi::onMoveBefore(GtkTextView* v, GtkMovementStep, int, gboolean, gpointer p) {
    LargeFileUi* self = static_cast<LargeFileUi*>(p);
    self->moveFrom_ = -1;
    GtkTextBuffer* b = gtk_text_view_get_buffer(v);
    LongLines* lines = LongLines::of(b);
    if (!lines) return;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    LongLines::Part part;
    if (!lines->partOf(gtk_text_iter_get_line(&it), &part)) return;
    GtkTextIter ls = it;
    gtk_text_iter_set_line_offset(&ls, 0);
    self->moveFrom_ = gtk_text_iter_get_offset(&it);
    self->moveLine_ = gtk_text_iter_get_line(&it);
    self->moveLineStart_ = gtk_text_iter_get_offset(&ls);
    self->moveStart_ = part.start;
    self->moveEnd_ = part.end;
    self->moveLength_ = part.length;
}

void LargeFileUi::onMoveAfter(GtkTextView* v, GtkMovementStep step, int count, gboolean extend,
                              gpointer p) {
    LargeFileUi* self = static_cast<LargeFileUi*>(p);
    if (self->moveFrom_ < 0 || count == 0) return;
    const int from = self->moveFrom_;
    self->moveFrom_ = -1;
    GtkTextBuffer* b = gtk_text_view_get_buffer(v);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    const int now = gtk_text_iter_get_offset(&it);
    const int base = self->moveLineStart_;
    const int fromCol = from - base, nowCol = now - base;
    const int start = (int)self->moveStart_, end = (int)self->moveEnd_, len = (int)self->moveLength_;
    int target = -1;
    if (step == GTK_MOVEMENT_DISPLAY_LINE_ENDS || step == GTK_MOVEMENT_PARAGRAPH_ENDS) {
        if (count > 0 && nowCol == end && end < len) target = base + len;
        if (count < 0 && nowCol == start && start > 0) target = base;
    } else if (step == GTK_MOVEMENT_VISUAL_POSITIONS || step == GTK_MOVEMENT_LOGICAL_POSITIONS) {
        // Past the part's edge in one step: one character on, not over the
        // hidden text to the line's end or start (or the next line).
        if (count > 0 && fromCol >= end - 1 && end < len && (nowCol > fromCol + count || now > base + len))
            target = std::min(from + count, base + len);
        if (count < 0 && fromCol <= start + 1 && start > 0 && (nowCol < fromCol + count || now < base))
            target = std::max(from + count, base);
    }
    if (target < 0 || target == now) return;
    GtkTextIter to;
    gtk_text_buffer_get_iter_at_offset(b, &to, target);
    if (extend) gtk_text_buffer_move_mark(b, gtk_text_buffer_get_insert(b), &to);
    else gtk_text_buffer_place_cursor(b, &to);
    LongLines::of(b)->followCaret();   // the part to it now, then the view to it
    gtk_text_view_scroll_mark_onscreen(v, gtk_text_buffer_get_insert(b));
}

LargeFileUi::~LargeFileUi() {
    alive_.reset();
    if (scrollIdle_) g_source_remove(scrollIdle_);
    if (listening_ && listenId_) listening_->unlisten(listenId_);
    g_signal_handlers_disconnect_by_data(editor_->textView(), this);
    GListModel* ctrls = gtk_widget_observe_controllers(note_);
    for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
        GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
        g_signal_handlers_disconnect_by_data(c, this);
        g_object_unref(c);
    }
    g_object_unref(ctrls);
    if (pass_ >= 0) editor_->codeView()->removePass(pass_);
}

void LargeFileUi::update() {
    Document* d = editor_->active();
    // The view's own buffer: this runs from notify::buffer, before the
    // editor has noted the new one.
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor_->textView()));
    const bool show = d && d->largeFileFeaturesOff() && !d->loading() && b == d->buffer();
    if (show) {
        const std::string tip = "“" + baseName(d->path()) +
            "” is over 20 MB or 300,000 lines, so syntax colors, folding, bracket matching and "
            "the language server are off for it, as in VS Code "
            "(editor.large-file-optimizations). Click to turn them on for this tab.";
        gtk_widget_set_tooltip_text(note_, tip.c_str());
    }
    gtk_widget_set_visible(note_, show);
    // When the part of the caret's line moves, the caret is brought on
    // screen: a search or Go to Line put it in text that was hidden.
    LongLines* lines = LongLines::of(b);
    if (lines != listening_) {
        if (listening_ && listenId_) listening_->unlisten(listenId_);
        listening_ = lines;
        listenId_ = 0;
        if (lines) listenId_ = lines->listen([this] { scheduleCaretScroll(); });
    }
}

// After GTK's own scroll for the same change, which comes later in the same
// turn (and measured the line as it was): once, a turn later.
void LargeFileUi::scheduleCaretScroll() {
    if (scrollIdle_) return;
    scrollIdle_ = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer p) -> gboolean {
        LargeFileUi* self = static_cast<LargeFileUi*>(p);
        self->scrollIdle_ = 0;
        GtkTextView* v = GTK_TEXT_VIEW(self->editor_->textView());
        GtkTextBuffer* b = gtk_text_view_get_buffer(v);
        if (LongLines::of(b) == self->listening_)
            gtk_text_view_scroll_mark_onscreen(v, gtk_text_buffer_get_insert(b));
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

// After the part each long line on screen shows, how much is left out.
void LargeFileUi::drawNotes(GtkSnapshot* s, const GdkRectangle& visible) {
    GtkTextView* tv = GTK_TEXT_VIEW(editor_->textView());
    GtkTextBuffer* b = gtk_text_view_get_buffer(tv);
    LongLines* lines = LongLines::of(b);
    if (!lines) return;
    GtkTextIter it;
    int top = 0;
    gtk_text_view_get_line_at_y(tv, &it, visible.y, &top);
    GdkRGBA muted;
    gdk_rgba_parse(&muted, "#9CA3AF");
    for (int guard = 0; guard < 1000; guard++) {
        int y = 0, h = 0;
        gtk_text_view_get_line_yrange(tv, &it, &y, &h);
        if (y > visible.y + visible.height) break;
        LongLines::Part p;
        if (lines->partOf(gtk_text_iter_get_line(&it), &p) && p.end > p.start) {
            GtkTextIter last = it;
            gtk_text_iter_set_line_offset(&last, (int)(p.end - 1));
            GdkRectangle r;
            gtk_text_view_get_iter_location(tv, &last, &r);
            const std::string note = "… " + LargeFile::hiddenNote(p.start, p.length - p.end);
            PangoLayout* layout = gtk_widget_create_pango_layout(GTK_WIDGET(tv), note.c_str());
            gtk_snapshot_save(s);
            graphene_point_t at = GRAPHENE_POINT_INIT((float)(r.x + r.width + 12), (float)r.y);
            gtk_snapshot_translate(s, &at);
            gtk_snapshot_append_layout(s, layout, &muted);
            gtk_snapshot_restore(s);
            g_object_unref(layout);
            notesDrawn_++;
        }
        if (!gtk_text_iter_forward_line(&it)) break;
    }
}

bool LargeFileUi::mayOpen(const std::string& path, EditorTabs::OpenFrom from) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || MediaView::handles(path)) return true;
    if (LargeFile::openingFor((uint64_t)st.st_size, LargeFileView::kMaxTextBytes) !=
        LargeFile::Opening::Ask)
        return true;   // over the limit, its tab says so
    if (approved_ == path) {
        approved_.clear();
        return true;
    }
    char* size = g_format_size((guint64)st.st_size);
    GtkAlertDialog* a = gtk_alert_dialog_new("“%s” is %s. Open it anyway?", baseName(path).c_str(), size);
    g_free(size);
    gtk_alert_dialog_set_detail(a,
        "A file this large takes a while to read and show. It opens without syntax colors, "
        "folding or a language server, which the status bar can turn on for it.");
    const char* buttons[] = {"Open Anyway", "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(a, buttons);
    gtk_alert_dialog_set_cancel_button(a, 1);
    gtk_alert_dialog_set_default_button(a, 0);
    struct Ask {
        std::weak_ptr<int> alive;
        LargeFileUi* ui;
        std::string path;
        EditorTabs::OpenFrom from;
    };
    GtkRoot* root = gtk_widget_get_root(editor_->widget());
    gtk_alert_dialog_choose(a, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            Ask* ask = static_cast<Ask*>(data);
            const int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
            if (button == 0 && !ask->alive.expired()) {
                ask->ui->approved_ = ask->path;
                ask->ui->editor_->openFile(ask->path, ask->from);
            }
            delete ask;
        },
        new Ask{alive_, this, path, from});
    g_object_unref(a);
    asked_++;
    return false;
}
