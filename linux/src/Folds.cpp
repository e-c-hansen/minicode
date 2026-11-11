// Folds.cpp — see Folds.h. The Mac's src/FoldView.mm is the model, command
// for command.
#include "Folds.h"

#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "Settings.h"
#include "Utf8Offsets.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace {

GQuark foldsQuark() {
    static GQuark q = g_quark_from_static_string("minicode-document-folds");
    return q;
}

// The tag that hides folded lines, one per tag table (every document
// shares the editor's).
GtkTextTag* foldTagIn(GtkTextTagTable* table) {
    static const char* kName = "minicode-fold";
    GtkTextTag* t = gtk_text_tag_table_lookup(table, kName);
    if (!t) {
        t = gtk_text_tag_new(kName);
        g_object_set(t, "invisible", TRUE, NULL);
        gtk_text_tag_table_add(table, t);
        g_object_unref(t);   // the table holds it
    }
    return t;
}

size_t offsetOf(const GtkTextIter* it) { return (size_t)gtk_text_iter_get_offset(it); }

bool isBreak(gunichar c) { return c == '\n' || c == '\r' || c == 0x2029; }

// A unit a code point, as GtkTextIter counts (BufferText.h): what
// Fold::Collapsed::edit reads of an insertion (its length, its first
// character, whether it holds a line break).
std::u16string unitsOf(const char* text, size_t len) {
    std::u16string out;
    out.reserve(len);
    const char* end = text + len;
    for (const char* p = text; p < end; p = g_utf8_next_char(p)) {
        const gunichar c = g_utf8_get_char_validated(p, end - p);
        out += c < 0x10000 && c != (gunichar)-1 && c != (gunichar)-2 ? (char16_t)c : (char16_t)0xFFFD;
    }
    return out;
}

char16_t unitOf(gunichar c) { return c < 0x10000 ? (char16_t)c : (char16_t)0xFFFD; }

std::string withCommas(size_t n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}

}  // namespace

// ================================================================ DocumentFolds

DocumentFolds::DocumentFolds(Document* doc, GtkTextBuffer* buffer) : doc_(doc), buffer_(buffer) {
    tag_ = foldTagIn(gtk_text_buffer_get_tag_table(buffer));
    g_object_set_qdata(G_OBJECT(buffer), foldsQuark(), this);
    // The plain handlers run before the buffer changes (both signals are
    // RUN_LAST), after BufferLines' and Document's own; the _after ones once
    // it has changed.
    g_signal_connect(buffer, "insert-text", G_CALLBACK(onInsertBefore), this);
    g_signal_connect(buffer, "delete-range", G_CALLBACK(onDeleteBefore), this);
    g_signal_connect_after(buffer, "insert-text", G_CALLBACK(onInsertAfter), this);
    g_signal_connect_after(buffer, "delete-range", G_CALLBACK(onDeleteAfter), this);
    g_signal_connect(buffer, "apply-tag", G_CALLBACK(onApplyTag), this);
    g_signal_connect(buffer, "mark-set", G_CALLBACK(onMarkSet), this);
    if (BufferLines* l = lines()) l->setFolds(this);
}

// From Document's destructor, while the buffer still exists.
DocumentFolds::~DocumentFolds() {
    alive_.reset();
    for (guint* id : {&timer_, &returnIdle_, &revealIdle_})
        if (*id) g_source_remove(*id);
    g_signal_handlers_disconnect_by_data(buffer_, this);
    if (BufferLines* l = lines(); l && l->folds() == this) l->setFolds(nullptr);
    g_object_set_qdata(G_OBJECT(buffer_), foldsQuark(), nullptr);
}

DocumentFolds* DocumentFolds::of(GtkTextBuffer* buffer) {
    if (!buffer) return nullptr;
    return static_cast<DocumentFolds*>(g_object_get_qdata(G_OBJECT(buffer), foldsQuark()));
}

BufferLines* DocumentFolds::lines() const { return BufferLines::of(buffer_); }

size_t DocumentFolds::charCount() const { return (size_t)gtk_text_buffer_get_char_count(buffer_); }

gunichar DocumentFolds::charAt(size_t offset) const {
    if (offset >= charCount()) return 0;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buffer_, &it, (int)offset);
    return gtk_text_iter_get_char(&it);
}

size_t DocumentFolds::breakLength(size_t offset) const {
    const gunichar c = charAt(offset);
    if (c == '\r' && charAt(offset + 1) == '\n') return 2;
    return isBreak(c) ? 1 : 0;
}

// Where a line's text ends: before its "\n", "\r\n", "\r" or U+2029.
size_t DocumentFolds::contentEnd(size_t line) const {
    BufferLines* l = lines();
    if (!l) return 0;
    const size_t a = l->lineStart(line);
    size_t e = l->lineEnd(line);
    if (e > a) {
        const gunichar last = charAt(e - 1);
        if (last == '\n') {
            e--;
            if (e > a && charAt(e - 1) == '\r') e--;
        } else if (last == '\r' || last == 0x2029) {
            e--;
        }
    }
    return e;
}

Fold::Span DocumentFolds::spanOf(const Fold::Range& r) const {
    BufferLines* l = lines();
    const size_t n = l ? l->lineCount() : 0;
    if (n == 0 || r.start >= n) return {0, 0};
    return Fold::spanOf(contentEnd(r.start), contentEnd(std::min(r.end, n - 1)));
}

void DocumentFolds::tagRange(const Fold::Span& f, size_t* start, size_t* end) const {
    *start = f.start + breakLength(f.start);
    *end = f.end + breakLength(f.end);
}

bool DocumentFolds::applies() const {
    return doc_->kind() == Document::Kind::Text && !doc_->path().empty() && !doc_->loading() &&
           !doc_->unread() && !doc_->largeFileFeaturesOff();
}

const Fold::Span* DocumentFolds::foldStartingOnLine(size_t line) const {
    BufferLines* l = lines();
    if (folds_.empty() || !l || line >= l->lineCount()) return nullptr;
    const size_t at = contentEnd(line);
    const std::vector<Fold::Span>& all = folds_.all();   // by start
    auto it = std::lower_bound(all.begin(), all.end(), at,
                               [](const Fold::Span& f, size_t p) { return f.start < p; });
    return it != all.end() && it->start == at ? &*it : nullptr;
}

bool DocumentFolds::startsRegion(size_t line) const { return Fold::startingAt(regions(), line) >= 0; }

bool DocumentFolds::startsClosedFold(size_t line) const { return foldStartingOnLine(line) != nullptr; }

void DocumentFolds::toggle(size_t line) { toggleLine(line); }

size_t DocumentFolds::lastHiddenLine(size_t line) const {
    BufferLines* l = lines();
    if (outer_.empty() || !l || line >= l->lineCount()) return line;
    const size_t at = contentEnd(line);
    auto it = std::lower_bound(outer_.begin(), outer_.end(), at,
                               [](const Fold::Span& f, size_t p) { return f.start < p; });
    if (it == outer_.end() || it->start != at) return line;
    return std::max(line, l->lineOf(it->end));
}

size_t DocumentFolds::hiddenLines() const {
    BufferLines* l = lines();
    if (!l) return 0;
    size_t n = 0;
    for (const Fold::Span& f : outer_) n += l->lineOf(f.end) - l->lineOf(f.start);
    return n;
}

std::vector<Fold::Range> DocumentFolds::closedLines() const {
    std::vector<Fold::Range> out;
    BufferLines* l = lines();
    if (!l) return out;
    out.reserve(outer_.size());
    for (const Fold::Span& f : outer_) out.push_back({l->lineOf(f.start), l->lineOf(f.end)});
    return out;
}

std::vector<size_t> DocumentFolds::caretOffsets() const {
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(buffer_, &a, &b);
    std::vector<size_t> out{offsetOf(&a)};
    if (BufferLines* l = lines(); l && l->extraCarets) {
        std::vector<size_t> more;
        l->extraCarets(more);
        out.insert(out.end(), more.begin(), more.end());
    }
    return out;
}

std::vector<size_t> DocumentFolds::caretLines() const {
    std::vector<size_t> out;
    BufferLines* l = lines();
    if (!l) return out;
    for (size_t o : caretOffsets()) out.push_back(l->lineOf(o));
    return out;
}

// ---------------------------------------------------------------- regions

// 0.3 s after the last edit. One timer, never one an edit: an undo of a
// Replace All puts back 40,000 edits, each through here.
void DocumentFolds::scheduleCompute() {
    lastEdit_ = g_get_monotonic_time();
    if (timer_) return;
    timer_ = g_timeout_add(300, &DocumentFolds::onComputeTimer, this);
}

gboolean DocumentFolds::onComputeTimer(gpointer p) {
    DocumentFolds* self = static_cast<DocumentFolds*>(p);
    self->timer_ = 0;
    const gint64 since = (g_get_monotonic_time() - self->lastEdit_) / 1000;
    if (since < 300) self->timer_ = g_timeout_add((guint)(300 - since), &DocumentFolds::onComputeTimer, self);
    else self->computeNow();
    return G_SOURCE_REMOVE;
}

// A copy of the text to a worker thread, Fold::compute there, the result
// back on the main thread if the text is still the one it was for. One at a
// time: an edit meanwhile asks for another once this one is back.
void DocumentFolds::computeNow() {
    if (!applies()) {
        haveCore_ = haveServer_ = false;
        server_.clear();
        if (!core_.empty()) {
            core_.clear();
            if (BufferLines* l = lines()) l->changed();
        }
        return;
    }
    if (computing_) {
        again_ = true;
        return;
    }
    computing_ = true;
    computes_++;
    struct Job {
        std::weak_ptr<int> alive;
        DocumentFolds* self;
        uint64_t gen;
        std::string text, name;
        std::vector<Fold::Range> ranges;
    };
    auto* job = new Job{alive_, this, generation_, doc_->text(), doc_->path(), {}};
    std::thread([job] {
        job->ranges = Fold::compute(utf16::fromUtf8(job->text), job->name);
        std::string().swap(job->text);
        g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
            Job* j = static_cast<Job*>(p);
            if (!j->alive.expired()) j->self->computed(j->gen, std::move(j->ranges), j->name);
            return G_SOURCE_REMOVE;
        }, job, [](gpointer p) { delete static_cast<Job*>(p); });
    }).detach();
}

void DocumentFolds::computed(uint64_t gen, std::vector<Fold::Range> ranges, const std::string& name) {
    computing_ = false;
    const bool current = gen == generation_ && name == doc_->path() && applies();
    if (current) {
        // The gutter draws again only for regions that differ: a draw of it
        // outside an edit, with a 5 MB line on screen, left GTK drawing the
        // view every frame for seconds (large_file).
        // Nor while the server's regions are the ones on show.
        const bool differ = ranges != core_ && !serverRegionsCurrent();
        core_ = std::move(ranges);
        coreGen_ = gen;
        haveCore_ = true;
        if (differ)
            if (BufferLines* l = lines()) l->changed();
    }
    if (again_ || !current) {
        again_ = false;
        if (!timer_ && applies()) scheduleCompute();
    }
}

const std::vector<Fold::Range>& DocumentFolds::freshRegions() {
    if (!applies()) {
        core_.clear();
        haveCore_ = false;
        return core_;
    }
    if (serverRegionsCurrent()) return server_;
    if (!coreCurrent()) {
        core_ = Fold::compute(utf16::fromUtf8(doc_->text()), doc_->path());
        coreGen_ = generation_;
        haveCore_ = true;
        computes_++;
    }
    return core_;
}

// The server's answer, as the Mac's: whole lines, nested properly, and only
// for the text it was asked about; the gutter draws again when they differ
// from the regions it shows.
bool DocumentFolds::setServerRanges(std::vector<Fold::Range> ranges, uint64_t generation) {
    if (generation != generation_ || !applies()) return false;
    std::vector<Fold::Range> rs =
        Fold::sanitize(std::move(ranges), (size_t)gtk_text_buffer_get_line_count(buffer_));
    const std::vector<Fold::Range> before = regions();
    server_ = std::move(rs);
    serverGen_ = generation;
    haveServer_ = !server_.empty();
    if (regions() != before)
        if (BufferLines* l = lines()) l->changed();
    return true;
}

void DocumentFolds::sourceChanged() {
    generation_++;
    haveCore_ = haveServer_ = false;
    core_.clear();
    server_.clear();
    if (!applies() && !folds_.empty()) unfoldAll();
    scheduleCompute();
    if (BufferLines* l = lines()) l->changed();
}

void DocumentFolds::forget() {
    if (!folds_.empty()) unfoldAll();
}

// ---------------------------------------------------------------- the tag

void DocumentFolds::applyTag(size_t a, size_t b) {
    if (a >= b) return;
    GtkTextIter ia, ib;
    gtk_text_buffer_get_iter_at_offset(buffer_, &ia, (int)a);
    gtk_text_buffer_get_iter_at_offset(buffer_, &ib, (int)b);
    const bool was = applyingTag_;
    applyingTag_ = true;
    gtk_text_buffer_apply_tag(buffer_, tag_, &ia, &ib);
    applyingTag_ = was;
}

void DocumentFolds::retag(size_t a, size_t b) {
    b = std::min(b, charCount());
    if (a >= b) return;
    GtkTextIter ia, ib;
    gtk_text_buffer_get_iter_at_offset(buffer_, &ia, (int)a);
    gtk_text_buffer_get_iter_at_offset(buffer_, &ib, (int)b);
    // Taken off only where it is: a removal over text without it still has
    // GTK lay those lines out again.
    GtkTextIter t = ia;
    if (gtk_text_iter_has_tag(&t, tag_) ||
        (gtk_text_iter_forward_to_tag_toggle(&t, tag_) && gtk_text_iter_compare(&t, &ib) < 0))
        gtk_text_buffer_remove_tag(buffer_, tag_, &ia, &ib);
    // The outermost folds whose tag meets [a, b): sorted and apart, so their
    // ends are in order too, and a tag ends at most two past its fold.
    auto it = std::lower_bound(outer_.begin(), outer_.end(), a,
                               [](const Fold::Span& f, size_t at) { return f.end + 2 <= at; });
    for (; it != outer_.end() && it->start < b; ++it) {
        size_t ts = 0, te = 0;
        tagRange(*it, &ts, &te);
        applyTag(std::max(ts, a), std::min(te, b));
    }
}

bool DocumentFolds::hidden(size_t offset) const {
    auto it = std::lower_bound(outer_.begin(), outer_.end(), offset,
                               [](const Fold::Span& f, size_t at) { return f.start < at; });
    if (it == outer_.begin()) return false;
    --it;   // the last one starting before it
    size_t ts = 0, te = 0;
    tagRange(*it, &ts, &te);
    return offset > it->start && offset < te;
}

// ---------------------------------------------------------------- closing and opening

void DocumentFolds::note(const std::string& message) {
    if (onNote) onNote(message);
}

void DocumentFolds::foldsChanged() {
    outer_ = folds_.outermost();
    if (BufferLines* l = lines()) l->changed();
    if (onFoldsChanged) onFoldsChanged();
}

bool DocumentFolds::closeWithinLimit(const std::vector<Fold::Range>& rs,
                                     const std::vector<size_t>& candidates, bool single) {
    if (candidates.empty()) return false;
    const std::vector<Fold::Range> closed = closedLines();
    const Fold::BudgetPick p = Fold::pickWithinBudget(rs, candidates, closed, kMaxHiddenLines);
    if (p.stayedOpen) {
        const std::string limit = withCommas(kMaxHiddenLines);
        if (single) {
            std::vector<Fold::Range> with = closed;
            for (size_t i : candidates)
                if (i < rs.size()) with.push_back(rs[i]);
            note("Not folded: this tab's folds would hide " + withCommas(Fold::hiddenLines(with)) +
                 " lines, and " + limit + " is the most they may hide, since GTK draws hidden "
                 "lines too");
        } else if (p.close.empty()) {
            note("Nothing folded: each region would take this tab's folds past " + limit +
                 " hidden lines");
        } else {
            note("Folded " + withCommas(p.close.size()) +
                 (p.close.size() == 1 ? " region; " : " regions; ") + withCommas(p.stayedOpen) +
                 " stayed open, since this tab's folds may hide " + limit + " lines at most");
        }
    }
    std::vector<Fold::Span> spans;
    spans.reserve(p.close.size());
    for (size_t i : p.close) {
        const Fold::Span s = spanOf(rs[i]);
        if (s.end > s.start) spans.push_back(s);
    }
    if (spans.empty()) return false;
    const std::vector<Fold::Span> before = outer_;
    // One merge (Fold All of a long file went in one region at a time on the
    // Mac once, 6 s for 50,000), then the tag over each new outermost fold.
    if (folds_.addAll(spans) == 0) return false;
    outer_ = folds_.outermost();
    for (const Fold::Span& f : outer_) {
        if (std::binary_search(before.begin(), before.end(), f, [](const Fold::Span& x, const Fold::Span& y) {
                return x.start != y.start ? x.start < y.start : x.end < y.end;
            }))
            continue;
        size_t ts = 0, te = 0;
        tagRange(f, &ts, &te);
        applyTag(ts, te);
    }
    moveCaretsOutOfFolds();
    foldsChanged();
    return true;
}

bool DocumentFolds::openFold(const Fold::Span& fold) {
    const Fold::Span f = fold;   // may point into folds_
    size_t ts = 0, te = 0;
    tagRange(f, &ts, &te);
    if (!folds_.remove(f.start)) return false;
    outer_ = folds_.outermost();
    retag(f.start, te);   // any fold it held is outermost now, and keeps its tag
    return true;
}

// A caret or selection end left in what was just hidden goes to the start
// of its fold (the end of the fold's first line), as on the Mac.
void DocumentFolds::moveCaretsOutOfFolds() {
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_mark(buffer_, &ins, gtk_text_buffer_get_insert(buffer_));
    gtk_text_buffer_get_iter_at_mark(buffer_, &bound, gtk_text_buffer_get_selection_bound(buffer_));
    auto out = [&](size_t p) {
        if (!hidden(p)) return p;
        auto it = std::lower_bound(outer_.begin(), outer_.end(), p,
                                   [](const Fold::Span& f, size_t at) { return f.start < at; });
        return (--it)->start;
    };
    const size_t a = offsetOf(&ins), b = offsetOf(&bound);
    const size_t na = out(a), nb = out(b);
    if (na == a && nb == b) return;
    gtk_text_buffer_get_iter_at_offset(buffer_, &ins, (int)na);
    gtk_text_buffer_get_iter_at_offset(buffer_, &bound, (int)nb);
    gtk_text_buffer_select_range(buffer_, &ins, &bound);
}

// Every fold hiding a caret or a selection end opens, the inner ones too, so
// what was gone to is seen (the Mac's revealRanges).
void DocumentFolds::revealSelection() {
    if (folds_.empty()) return;
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_mark(buffer_, &ins, gtk_text_buffer_get_insert(buffer_));
    gtk_text_buffer_get_iter_at_mark(buffer_, &bound, gtk_text_buffer_get_selection_bound(buffer_));
    // A caret at the end of a fold's last line is where the Mac shows it
    // after the placeholder, and where GTK's undo of Return below a fold
    // puts it back: here that is the end of the folded line, and nothing
    // opens.
    const size_t caret = offsetOf(&ins);
    if (caret == offsetOf(&bound) && hidden(caret)) {
        auto it = std::lower_bound(outer_.begin(), outer_.end(), caret,
                                   [](const Fold::Span& f, size_t at) { return f.start < at; });
        --it;   // hidden: some fold starts before it
        if (it->end == caret) {
            GtkTextIter to;
            gtk_text_buffer_get_iter_at_offset(buffer_, &to, (int)it->start);
            gtk_text_buffer_place_cursor(buffer_, &to);
            return;
        }
    }
    std::vector<Fold::Span> open;
    for (size_t p : {offsetOf(&ins), offsetOf(&bound)}) {
        if (!hidden(p)) continue;
        for (const Fold::Span& f : folds_.all()) {
            if (f.start >= p) break;
            size_t ts = 0, te = 0;
            tagRange(f, &ts, &te);
            if (p < te) open.push_back(f);
        }
    }
    if (open.empty()) return;
    bool did = false;
    for (const Fold::Span& f : open) did |= openFold(f);
    if (did) foldsChanged();
}

bool DocumentFolds::foldAtCarets() {
    if (!applies()) return false;
    const std::vector<Fold::Range> rs = freshRegions();
    std::vector<size_t> pick;
    for (size_t line : caretLines())
        for (size_t i : Fold::containing(rs, line)) {
            if (foldStartingOnLine(rs[i].start)) continue;   // folded: the next one out
            const Fold::Span s = spanOf(rs[i]);
            if (s.end <= s.start) continue;
            pick.push_back(i);
            break;
        }
    return closeWithinLimit(rs, pick, pick.size() == 1);
}

bool DocumentFolds::unfoldAtCarets() {
    if (folds_.empty()) return false;
    BufferLines* l = lines();
    if (!l) return false;
    GtkTextIter a, b;
    std::vector<std::pair<size_t, size_t>> ranges;
    if (gtk_text_buffer_get_selection_bounds(buffer_, &a, &b)) ranges.push_back({offsetOf(&a), offsetOf(&b)});
    else ranges.push_back({offsetOf(&a), offsetOf(&a)});
    std::vector<size_t> more = caretOffsets();
    for (size_t i = 1; i < more.size(); i++) ranges.push_back({more[i], more[i]});
    bool did = false;
    for (const auto& [from, to] : ranges) {
        std::vector<Fold::Span> in;
        if (to > from) {
            // Every fold that starts in the selection.
            for (const Fold::Span& f : folds_.all())
                if (f.start >= from && f.start <= to) in.push_back(f);
        } else if (const Fold::Span* f = foldStartingOnLine(l->lineOf(from))) {
            in.push_back(*f);
        }
        for (const Fold::Span& f : in) did |= openFold(f);
    }
    if (did) foldsChanged();
    return did;
}

bool DocumentFolds::toggleAtCarets() {
    BufferLines* l = lines();
    if (!l) return false;
    const std::vector<size_t> carets = caretOffsets();
    return foldStartingOnLine(l->lineOf(carets.front())) ? unfoldAtCarets() : foldAtCarets();
}

bool DocumentFolds::foldRecursivelyAtCarets() {
    if (!applies()) return false;
    const std::vector<Fold::Range> rs = freshRegions();
    std::vector<size_t> pick;
    for (size_t line : caretLines()) {
        const std::vector<size_t> c = Fold::containing(rs, line);
        if (c.empty()) continue;
        const Fold::Range top = rs[c.front()];   // the innermost, and all inside it
        for (size_t i = 0; i < rs.size(); i++)
            if (rs[i].start >= top.start && rs[i].end <= top.end) pick.push_back(i);
    }
    return closeWithinLimit(rs, pick, false);
}

bool DocumentFolds::unfoldRecursivelyAtCarets() {
    if (folds_.empty()) return false;
    BufferLines* l = lines();
    if (!l) return false;
    const std::vector<Fold::Range> rs = applies() ? freshRegions() : std::vector<Fold::Range>();
    bool did = false;
    for (size_t line : caretLines()) {
        const std::vector<size_t> c = Fold::containing(rs, line);
        size_t a, b;
        if (!c.empty()) {
            const Fold::Range top = rs[c.back()];   // the outermost
            a = l->lineStart(top.start);
            b = l->lineEnd(top.end);
        } else {
            a = l->lineStart(line);
            b = l->lineEnd(line);
        }
        std::vector<Fold::Span> in;
        for (const Fold::Span& f : folds_.all())
            if (f.start >= a && f.start <= b) in.push_back(f);
        for (const Fold::Span& f : in) did |= openFold(f);
    }
    if (did) foldsChanged();
    return did;
}

bool DocumentFolds::foldAll() {
    if (!applies()) return false;
    const std::vector<Fold::Range> rs = freshRegions();
    std::vector<size_t> all(rs.size());
    for (size_t i = 0; i < all.size(); i++) all[i] = i;
    return closeWithinLimit(rs, all, false);
}

// One removal of the tag over everything folded, one invalidation.
bool DocumentFolds::unfoldAll() {
    if (folds_.empty()) return false;
    size_t ts = 0, te = 0, ignore = 0;
    tagRange(outer_.back(), &ignore, &te);
    ts = outer_.front().start;
    folds_.clear();
    outer_.clear();
    te = std::min(te, charCount());
    if (te > ts) {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)ts);
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)te);
        gtk_text_buffer_remove_tag(buffer_, tag_, &a, &b);
    }
    foldsChanged();
    return true;
}

bool DocumentFolds::foldLevel(int level) {
    if (!applies() || level < 1) return false;
    const std::vector<Fold::Range> rs = freshRegions();
    const std::vector<int> d = Fold::depths(rs);
    const std::vector<size_t> carets = caretLines();
    std::vector<size_t> pick;
    for (size_t i = 0; i < rs.size(); i++) {
        if (d[i] != level) continue;
        bool holds = false;
        for (size_t line : carets) holds |= line >= rs[i].start && line <= rs[i].end;
        if (!holds) pick.push_back(i);
    }
    return closeWithinLimit(rs, pick, false);
}

bool DocumentFolds::foldAllComments() {
    if (!applies()) return false;
    const std::vector<Fold::Range> rs = freshRegions();
    std::vector<size_t> pick;
    for (size_t i = 0; i < rs.size(); i++)
        if (rs[i].kind == Fold::Kind::Comment) pick.push_back(i);
    return closeWithinLimit(rs, pick, false);
}

bool DocumentFolds::toggleLine(size_t line) {
    if (const Fold::Span* f = foldStartingOnLine(line)) {
        const bool did = openFold(*f);
        if (did) foldsChanged();
        return did;
    }
    if (!applies()) return false;
    const std::vector<Fold::Range> rs = freshRegions();
    const long i = Fold::startingAt(rs, line);
    return i >= 0 && closeWithinLimit(rs, {(size_t)i}, true);
}

bool DocumentFolds::openFoldEndingBefore(size_t caret) {
    for (const Fold::Span& f : outer_) {
        size_t ts = 0, te = 0;
        tagRange(f, &ts, &te);
        if (te != caret || caret <= f.end) continue;
        const bool did = openFold(f);
        if (did) foldsChanged();
        return did;
    }
    return false;
}

bool DocumentFolds::openFoldStartingAt(size_t caret) {
    auto it = std::lower_bound(outer_.begin(), outer_.end(), caret,
                               [](const Fold::Span& f, size_t at) { return f.start < at; });
    if (it == outer_.end() || it->start != caret) return false;
    const bool did = openFold(*it);
    if (did) foldsChanged();
    return did;
}

bool DocumentFolds::moveCaretBelowForReturn(size_t caret) {
    auto it = std::lower_bound(outer_.begin(), outer_.end(), caret,
                               [](const Fold::Span& f, size_t at) { return f.start < at; });
    if (it == outer_.end() || it->start != caret) return false;
    // To the end of the fold's last line, where Fold::Collapsed takes a line
    // break as a new line below the fold. No reveal for this one caret move.
    returning_ = true;
    returnFrom_ = caret;
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(buffer_, &at, (int)it->end);
    gtk_text_buffer_place_cursor(buffer_, &at);
    // The key may put nothing in after all (an input method took it): the
    // caret goes back before anything draws.
    if (!returnIdle_)
        returnIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE, [](gpointer p) -> gboolean {
            DocumentFolds* self = static_cast<DocumentFolds*>(p);
            self->returnIdle_ = 0;
            if (!self->returning_) return G_SOURCE_REMOVE;
            self->returning_ = false;
            GtkTextIter c;
            gtk_text_buffer_get_iter_at_mark(self->buffer_, &c, gtk_text_buffer_get_insert(self->buffer_));
            if (self->hidden(offsetOf(&c))) {
                GtkTextIter back;
                gtk_text_buffer_get_iter_at_offset(self->buffer_, &back, (int)self->returnFrom_);
                gtk_text_buffer_place_cursor(self->buffer_, &back);
            }
            return G_SOURCE_REMOVE;
        }, this, nullptr);
    return true;
}

// ---------------------------------------------------------------- following edits

void DocumentFolds::onInsertBefore(GtkTextBuffer*, GtkTextIter* at, char* text, int len, gpointer p) {
    DocumentFolds* self = static_cast<DocumentFolds*>(p);
    self->generation_++;
    self->editing_ = true;
    self->opened_.clear();
    self->editPos_ = offsetOf(at);
    self->editNewLen_ = 0;
    if (self->folds_.empty() || !text) return;
    const std::u16string u = unitsOf(text, len < 0 ? strlen(text) : (size_t)len);
    self->editNewLen_ = u.size();
    self->folds_.edit(self->editPos_, 0, u.data(), u.size(), unitOf(gtk_text_iter_get_char(at)),
                      &self->opened_);
    self->outer_ = self->folds_.outermost();
}

void DocumentFolds::onDeleteBefore(GtkTextBuffer*, GtkTextIter* from, GtkTextIter* to, gpointer p) {
    DocumentFolds* self = static_cast<DocumentFolds*>(p);
    GtkTextIter a = *from, b = *to;
    gtk_text_iter_order(&a, &b);
    self->generation_++;
    self->editing_ = true;
    self->opened_.clear();
    self->editPos_ = offsetOf(&a);
    self->editNewLen_ = 0;
    const size_t end = offsetOf(&b);
    if (self->folds_.empty() || end == self->editPos_) return;
    self->folds_.edit(self->editPos_, end - self->editPos_, nullptr, 0,
                      unitOf(gtk_text_iter_get_char(&b)), &self->opened_);
    self->outer_ = self->folds_.outermost();
}

// The buffer has the edit: the tag over what the edit opened comes off, and
// around the edit it is made right again (text typed inside a tagged range
// is tagged, and a line break put in at a fold's end must end it). A tag
// changed here invalidates iterators, so the one the signal passes is set
// again from its offset for the handlers after this one and for the caller.
void DocumentFolds::onInsertAfter(GtkTextBuffer* b, GtkTextIter* at, char*, int, gpointer p) {
    DocumentFolds* self = static_cast<DocumentFolds*>(p);
    self->editing_ = false;
    self->returning_ = false;   // Return's line break is in
    if (!self->opened_.empty() || !self->folds_.empty()) {
        // `at` is the end of the insertion.
        const int keep = gtk_text_iter_get_offset(at);
        const size_t end = (size_t)keep;
        const size_t start = end >= self->editNewLen_ ? end - self->editNewLen_ : 0;
        for (const Fold::Span& o : self->opened_) self->retag(o.start, o.end + 2);
        self->retag(start >= 2 ? start - 2 : 0, end + 2);
        if (!self->opened_.empty()) self->foldsChanged();
        self->opened_.clear();
        gtk_text_buffer_get_iter_at_offset(b, at, keep);
    }
    self->scheduleCompute();
}

void DocumentFolds::onDeleteAfter(GtkTextBuffer* b, GtkTextIter* from, GtkTextIter* to, gpointer p) {
    DocumentFolds* self = static_cast<DocumentFolds*>(p);
    self->editing_ = false;
    if (!self->opened_.empty() || !self->folds_.empty()) {
        const int keep = gtk_text_iter_get_offset(from);
        const size_t pos = (size_t)keep;
        for (const Fold::Span& o : self->opened_) self->retag(o.start, o.end + 2);
        self->retag(pos >= 2 ? pos - 2 : 0, pos + 2);
        if (!self->opened_.empty()) self->foldsChanged();
        self->opened_.clear();
        gtk_text_buffer_get_iter_at_offset(b, from, keep);
        gtk_text_buffer_get_iter_at_offset(b, to, keep);
    }
    self->scheduleCompute();
}

// The fold tag is this object's alone: a paste from a GtkTextView brings the
// copied range's tags, which would hide the pasted text.
void DocumentFolds::onApplyTag(GtkTextBuffer* b, GtkTextTag* tag, GtkTextIter*, GtkTextIter*,
                               gpointer p) {
    DocumentFolds* self = static_cast<DocumentFolds*>(p);
    if (tag == self->tag_ && !self->applyingTag_) g_signal_stop_emission_by_name(b, "apply-tag");
}

// A caret or selection end set in hidden text (Find, Go to Line, a
// definition, an undo putting the caret back) opens what hides it, at the
// next idle at high priority: before GTK lays out or draws, or carries out a
// scroll to the caret it was asked for, but never inside the signal. A tag
// changed there would invalidate every iterator, and
// gtk_text_buffer_select_range emits mark-set for the insert mark and then
// for the selection bound with an iterator of its own it holds meanwhile.
void DocumentFolds::onMarkSet(GtkTextBuffer* b, GtkTextIter* at, GtkTextMark* mark, gpointer p) {
    DocumentFolds* self = static_cast<DocumentFolds*>(p);
    if (self->folds_.empty() || self->returning_ || self->revealIdle_) return;
    if (mark != gtk_text_buffer_get_insert(b) && mark != gtk_text_buffer_get_selection_bound(b))
        return;
    if (!self->hidden(offsetOf(at))) return;
    self->revealIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE, [](gpointer q) -> gboolean {
        DocumentFolds* me = static_cast<DocumentFolds*>(q);
        me->revealIdle_ = 0;
        if (!me->returning_) me->revealSelection();
        return G_SOURCE_REMOVE;
    }, self, nullptr);
}

// ================================================================ FoldUi

namespace {

// The commands' actions, in the registry's order (Keys.cpp names them).
struct FoldAction {
    const char* name;
    bool (*run)(DocumentFolds*);
};
const FoldAction kFoldActions[] = {
    {"fold", [](DocumentFolds* f) { return f->foldAtCarets(); }},
    {"unfold", [](DocumentFolds* f) { return f->unfoldAtCarets(); }},
    {"togglefold", [](DocumentFolds* f) { return f->toggleAtCarets(); }},
    {"foldrecursively", [](DocumentFolds* f) { return f->foldRecursivelyAtCarets(); }},
    {"unfoldrecursively", [](DocumentFolds* f) { return f->unfoldRecursivelyAtCarets(); }},
    {"foldall", [](DocumentFolds* f) { return f->foldAll(); }},
    {"unfoldall", [](DocumentFolds* f) { return f->unfoldAll(); }},
    {"foldcomments", [](DocumentFolds* f) { return f->foldAllComments(); }},
    {"foldlevel1", [](DocumentFolds* f) { return f->foldLevel(1); }},
    {"foldlevel2", [](DocumentFolds* f) { return f->foldLevel(2); }},
    {"foldlevel3", [](DocumentFolds* f) { return f->foldLevel(3); }},
    {"foldlevel4", [](DocumentFolds* f) { return f->foldLevel(4); }},
    {"foldlevel5", [](DocumentFolds* f) { return f->foldLevel(5); }},
    {"foldlevel6", [](DocumentFolds* f) { return f->foldLevel(6); }},
    {"foldlevel7", [](DocumentFolds* f) { return f->foldLevel(7); }},
};

}  // namespace

FoldUi::FoldUi(Editor* editor) : editor_(editor), view_(editor->textView()) {
    pass_ = editor_->codeView()->addPass(CodeView::Layer::AboveText, 20,
        [this](GtkSnapshot* s, const GdkRectangle& visible) { drawPlaceholders(s, visible); });
    // A click on a placeholder opens its fold: in the capture phase, so the
    // text view's own selection gesture never sees it.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click));
    g_signal_connect_swapped(view_, "notify::buffer", G_CALLBACK(+[](gpointer self) {
        static_cast<FoldUi*>(self)->watchBuffer();
    }), this);
    g_signal_connect_swapped(view_, "notify::editable", G_CALLBACK(+[](gpointer self) {
        static_cast<FoldUi*>(self)->updateActions();
    }), this);
    watchBuffer();
}

FoldUi::~FoldUi() {
    if (DocumentFolds* f = DocumentFolds::of(buffer_)) {
        f->onNote = nullptr;
        f->onFoldsChanged = nullptr;
    }
    if (buffer_) g_object_unref(buffer_);
    buffer_ = nullptr;
    for (GSimpleAction* a : actions_) {
        g_signal_handlers_disconnect_by_data(a, this);
        g_object_unref(a);
    }
    g_signal_handlers_disconnect_by_data(view_, this);
    GListModel* ctrls = gtk_widget_observe_controllers(view_);
    for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
        GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
        g_signal_handlers_disconnect_by_data(c, this);
        g_object_unref(c);
    }
    g_object_unref(ctrls);
    if (pass_ >= 0) editor_->codeView()->removePass(pass_);
}

// The buffer on show's folds report to this editor; the one it leaves
// stops. The buffer is held while watched, so its folds are found or gone
// (DocumentFolds::of), never a freed object.
void FoldUi::watchBuffer() {
    GtkTextBuffer* now = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
    if (now != buffer_) {
        if (DocumentFolds* old = DocumentFolds::of(buffer_)) {
            old->onNote = nullptr;
            old->onFoldsChanged = nullptr;
        }
        if (buffer_) g_object_unref(buffer_);
        buffer_ = now;
        if (buffer_) g_object_ref(buffer_);
        if (DocumentFolds* f = DocumentFolds::of(buffer_)) {
            f->onNote = [this](const std::string& m) {
                if (onNote) onNote(m);
            };
            f->onFoldsChanged = [this] { gtk_widget_queue_draw(view_); };
        }
    }
    updateActions();
}

DocumentFolds* FoldUi::folds() const {
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    if (!gtk_text_view_get_editable(tv)) return nullptr;
    return DocumentFolds::of(gtk_text_view_get_buffer(tv));
}

void FoldUi::updateActions() {
    const bool on = folds() != nullptr;
    for (GSimpleAction* a : actions_) g_simple_action_set_enabled(a, on);
}

void FoldUi::addActions(GActionMap* map) {
    for (size_t i = 0; i < G_N_ELEMENTS(kFoldActions); i++) {
        GSimpleAction* a = g_simple_action_new(kFoldActions[i].name, nullptr);
        g_object_set_data(G_OBJECT(a), "minicode-fold-command", GSIZE_TO_POINTER(i));
        g_signal_connect(a, "activate", G_CALLBACK(+[](GSimpleAction* act, GVariant*, gpointer self) {
            const size_t which =
                GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(act), "minicode-fold-command"));
            static_cast<FoldUi*>(self)->run(kFoldActions[which].run);
        }), this);
        g_action_map_add_action(map, G_ACTION(a));
        actions_.push_back(a);   // the map holds its own reference too
    }
    updateActions();
}

// Only while the editor has the keyboard, like Ctrl+/: the key never folds a
// file nobody is looking at. A command that did nothing rings the bell,
// unless it said why in the status bar.
bool FoldUi::run(const std::function<bool(DocumentFolds*)>& command) {
    DocumentFolds* f = folds();
    if (!f || !gtk_widget_has_focus(view_)) {
        gtk_widget_error_bell(view_);
        return false;
    }
    bool noted = false;
    auto keep = f->onNote;
    f->onNote = [&](const std::string& m) {
        noted = true;
        if (onNote) onNote(m);
    };
    const bool did = command(f);
    f->onNote = keep;
    if (!did && !noted) gtk_widget_error_bell(view_);
    return did;
}

bool FoldUi::keyPressed(guint key, GdkModifierType mods) {
    DocumentFolds* f = folds();
    if (!f || !f->hasClosedFolds() || editor_->composing()) return false;
    if (mods & (GDK_ALT_MASK | GDK_SUPER_MASK)) return false;
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
    if (gtk_text_buffer_get_has_selection(b)) return false;
    if (BufferLines* l = BufferLines::of(b); l && l->extraCarets) {
        std::vector<size_t> more;
        l->extraCarets(more);
        if (!more.empty()) return false;   // several carets: the keys as they are
    }
    GtkTextIter c;
    gtk_text_buffer_get_iter_at_mark(b, &c, gtk_text_buffer_get_insert(b));
    const size_t at = offsetOf(&c);
    switch (key) {
        // Backspace just after a fold or Delete just before it opens the fold
        // rather than deleting text nobody can see.
        case GDK_KEY_BackSpace:
            return f->openFoldEndingBefore(at);
        case GDK_KEY_Delete:
        case GDK_KEY_KP_Delete:
            return f->openFoldStartingAt(at);
        // Return at the end of a folded line: the new line goes below the
        // fold, and the editor's own Return (AutoIndent, a Markdown list)
        // makes it there.
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_ISO_Enter:
            if (!(mods & GDK_CONTROL_MASK)) f->moveCaretBelowForReturn(at);
            return false;
        default:
            return false;
    }
}

// A placeholder after each folded line on screen, as on the Mac: a rounded
// box in editor.bracket-match holding three dots.
void FoldUi::drawPlaceholders(GtkSnapshot* s, const GdkRectangle& visible) {
    drawn_.clear();
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    GtkTextBuffer* b = gtk_text_view_get_buffer(tv);
    DocumentFolds* f = DocumentFolds::of(b);
    if (!f || f->outermost().empty() || !gtk_text_view_get_editable(tv)) return;
    GtkTextIter top, bottom;
    int ignore = 0;
    gtk_text_view_get_line_at_y(tv, &top, visible.y, &ignore);
    gtk_text_view_get_line_at_y(tv, &bottom, visible.y + visible.height, &ignore);
    if (!gtk_text_iter_ends_line(&bottom)) gtk_text_iter_forward_to_line_end(&bottom);
    const size_t from = offsetOf(&top), to = offsetOf(&bottom);
    const std::vector<Fold::Span>& outer = f->outermost();
    auto it = std::lower_bound(outer.begin(), outer.end(), from,
                               [](const Fold::Span& x, size_t at) { return x.start < at; });
    if (it == outer.end() || it->start > to) return;
    const GdkRGBA c = toGdk(editor_->documentSettings().bracketMatchColor());
    PangoLayout* zeros = gtk_widget_create_pango_layout(view_, "000");
    int zw = 0, zh = 0;
    pango_layout_get_pixel_size(zeros, &zw, &zh);
    g_object_unref(zeros);
    const int width = zw + 4;
    for (; it != outer.end() && it->start <= to; ++it) {
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(b, &at, (int)it->start);
        int ly = 0, lh = 0;
        gtk_text_view_get_line_yrange(tv, &at, &ly, &lh);
        if (lh <= 0) continue;
        GdkRectangle r;
        gtk_text_view_get_iter_location(tv, &at, &r);
        const int h = std::max(6, std::min(lh, zh + 2) - 2);
        const GdkRectangle box{r.x + 4, ly + (lh - h) / 2, width - 4, h};
        const graphene_rect_t g =
            GRAPHENE_RECT_INIT((float)box.x + 0.5f, (float)box.y + 0.5f, (float)box.width - 1,
                               (float)box.height - 1);
        GskRoundedRect round;
        gsk_rounded_rect_init_from_rect(&round, &g, 4);
        GdkRGBA fill = c, edge = c;
        fill.alpha = c.alpha * 0.18f;
        edge.alpha = c.alpha * 0.7f;
        gtk_snapshot_push_rounded_clip(s, &round);
        gtk_snapshot_append_color(s, &fill, &g);
        gtk_snapshot_pop(s);
        const float widths[4] = {1, 1, 1, 1};
        const GdkRGBA edges[4] = {edge, edge, edge, edge};
        gtk_snapshot_append_border(s, &round, widths, edges);
        // Three dots, drawn rather than typed: the font's "⋯" is too small.
        const float dot = std::max(1.5f, (float)h * 0.16f);
        const float cx = (float)box.x + (float)box.width / 2, cy = (float)box.y + (float)box.height / 2;
        for (int k = -1; k <= 1; k++) {
            const graphene_rect_t d = GRAPHENE_RECT_INIT(cx + (float)k * dot * 2.6f - dot / 2,
                                                         cy - dot / 2, dot, dot);
            GskRoundedRect dr;
            gsk_rounded_rect_init_from_rect(&dr, &d, dot / 2);
            gtk_snapshot_push_rounded_clip(s, &dr);
            gtk_snapshot_append_color(s, &c, &d);
            gtk_snapshot_pop(s);
        }
        drawn_.push_back({box, it->start});
    }
}

void FoldUi::onPressed(GtkGestureClick* g, int n, double x, double y, gpointer p) {
    FoldUi* self = static_cast<FoldUi*>(p);
    if (n != 1 || self->drawn_.empty()) return;
    DocumentFolds* f = self->folds();
    if (!f) return;
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(self->view_), GTK_TEXT_WINDOW_WIDGET,
                                          (int)x, (int)y, &bx, &by);
    for (const Placeholder& ph : self->drawn_) {
        const GdkRectangle& r = ph.rect;
        if (bx < r.x - 1 || bx > r.x + r.width + 1 || by < r.y - 1 || by > r.y + r.height + 1)
            continue;
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        const size_t start = ph.start;
        GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->view_));
        f->openFoldStartingAt(start);
        GtkTextIter at;
        gtk_text_buffer_get_iter_at_offset(b, &at, (int)start);
        gtk_text_buffer_place_cursor(b, &at);
        gtk_widget_grab_focus(self->view_);
        return;
    }
}
