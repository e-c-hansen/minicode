// MarkdownSide.cpp — see MarkdownSide.h.
#include "MarkdownSide.h"
#include "Mermaid.h"

#include <time.h>
#include <algorithm>
#include <cmath>

namespace {
double threadCpuMs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void shiftPage(Markdown::Page& p, int by) {
    for (Markdown::Span& s : p.spans) { s.start += by; s.end += by; }
    for (auto& a : p.anchors) a.second += by;
    for (Markdown::PendingPicture& pp : p.pending) { pp.start += by; pp.end += by; }
}
}  // namespace

struct MarkdownSide::Job {
    MarkdownSide* self = nullptr;
    std::weak_ptr<int> alive;
    uint64_t generation = 0;
    std::string text;
    std::vector<MarkdownPage::Piece> old;
    std::vector<bool> keep;
    bool whole = false;
    double prepMs = 0;
    // The worker's answer.
    std::vector<MdRun> runs;
    std::vector<MarkdownPage::Piece> pieces;
    std::vector<long> match;
    double workerMs = 0;
};

MarkdownSide::MarkdownSide(GtkTextTagTable* tags) {
    buffer_ = gtk_text_buffer_new(tags);
    gtk_text_buffer_set_enable_undo(buffer_, FALSE);
    view_ = gtk_text_view_new_with_buffer(buffer_);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view_), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view_), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view_), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view_), 6);
    gtk_widget_add_css_class(view_, "minicode-editor");
    gtk_widget_add_css_class(view_, "minicode-prose");
    Markdown::installTooltips(GTK_TEXT_VIEW(view_));
    // A diagram arrived (or failed): the pieces waiting for one are built again.
    std::weak_ptr<int> alive = alive_;
    mermaidListener_ = Mermaid::subscribe([this, alive] {
        if (alive.expired()) return;
        for (const Info& i : infos_)
            if (i.page.waiting) { schedule(0); return; }
    });
    scroller_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller_, "minicode-scroller");
    gtk_widget_add_css_class(scroller_, "minicode-editor-scroller");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), view_);
    gtk_widget_set_size_request(scroller_, 200, -1);
    gtk_widget_set_hexpand(scroller_, TRUE);
    gtk_widget_set_vexpand(scroller_, TRUE);
    g_object_ref_sink(scroller_);
    paneAdj_ = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    g_signal_connect_swapped(paneAdj_, "value-changed", G_CALLBACK(+[](gpointer self) {
        static_cast<MarkdownSide*>(self)->paneMoved();
    }), this);
    g_signal_connect_swapped(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)),
                             "notify::page-size", G_CALLBACK(+[](gpointer self) {
        static_cast<MarkdownSide*>(self)->fit();
    }), this);
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "released", G_CALLBACK(+[](GtkGestureClick*, int, double x, double y,
                                                     gpointer self) {
        static_cast<MarkdownSide*>(self)->clicked(x, y);
    }), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click));
    stale_ = true;
}

MarkdownSide::~MarkdownSide() {
    Mermaid::unsubscribe(mermaidListener_);
    alive_.reset();   // a running job finds nobody
    stopSettle();
    if (srcAdj_) g_signal_handlers_disconnect_by_data(srcAdj_, this);
    if (timer_) g_source_remove(timer_);
    if (arrivedTimer_) g_source_remove(arrivedTimer_);
    if (job_) job_->self = nullptr;
    dropWidgets(0, infos_.size());
    g_object_unref(scroller_);
    g_object_unref(buffer_);
}

bool MarkdownSide::setContext(const std::string& folder, bool webImages, bool mermaid, bool dark) {
    if (folder == folder_ && webImages == webImages_ && mermaid == mermaid_ && dark == dark_ && hooks_)
        return false;
    folder_ = folder;
    webImages_ = webImages;
    mermaid_ = mermaid;
    dark_ = dark;
    stale_ = true;
    return true;
}

void MarkdownSide::schedule(int delayMs) {
    if (timer_) g_source_remove(timer_);
    timer_ = g_timeout_add(delayMs, [](gpointer p) -> gboolean {
        MarkdownSide* self = static_cast<MarkdownSide*>(p);
        self->timer_ = 0;
        self->start();
        return G_SOURCE_REMOVE;
    }, this);
}

void MarkdownSide::clear() {
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    again_ = false;
    ++generation_;   // an answer on the way is dropped
    arrived_.clear();
    if (arrivedTimer_) g_source_remove(arrivedTimer_);
    arrivedTimer_ = 0;
    dropWidgets(0, infos_.size());
    infos_.clear();
    gtk_text_buffer_set_text(buffer_, "", 0);
    stats_.pieces = 0;
    stats_.builtTotal = 0;
    stale_ = false;
    marks_.clear();
    marksDirty_ = true;
    pageLeads_ = false;
    stopSettle();
}

int MarkdownSide::embedCount() const {
    int n = 0;
    for (const Info& i : infos_) n += (int)i.page.embeds.size();
    return n;
}

std::vector<GtkWidget*> MarkdownSide::embedWidgets() const {
    std::vector<GtkWidget*> w;
    for (const Info& i : infos_)
        for (const Markdown::Embed& e : i.page.embeds) w.push_back(e.widget);
    return w;
}

int MarkdownSide::pendingPictures() const {
    int n = 0;
    for (const Info& i : infos_) n += (int)i.page.pending.size();
    return n;
}

std::string MarkdownSide::pageText() const {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* raw = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    std::string t = raw;
    g_free(raw);
    return t;
}

// ------------------------------------------------------------ the render

void MarkdownSide::start() {
    if (!getText) return;
    if (job_) { again_ = true; return; }
    const double t0 = threadCpuMs();
    auto* job = new Job();
    job->self = this;
    job->alive = alive_;
    job->generation = ++generation_;
    job->text = getText();
    job->whole = stale_;
    if (!job->whole) {
        for (const Info& i : infos_) {
            job->old.push_back(i.piece);
            job->keep.push_back(i.page.pending.empty() && i.page.waiting == 0);
        }
    }
    job->prepMs = threadCpuMs() - t0;
    job_ = job;
    GTask* task = g_task_new(nullptr, nullptr, [](GObject*, GAsyncResult* res, gpointer p) {
        Job* j = static_cast<Job*>(p);
        (void)res;
        std::shared_ptr<int> alive = j->alive.lock();
        MarkdownSide* self = alive ? j->self : nullptr;
        if (self) {
            self->job_ = nullptr;
            if (j->generation == self->generation_) self->apply(j);
            if (self->again_) { self->again_ = false; self->start(); }
        }
        delete j;
    }, job);
    g_task_set_task_data(task, job, nullptr);
    g_task_run_in_thread(task, [](GTask* t, gpointer, gpointer data, GCancellable*) {
        Job* j = static_cast<Job*>(data);
        const double c0 = threadCpuMs();
        j->runs = MarkdownParser::parse(j->text);
        j->pieces = MarkdownPage::split(j->runs);
        j->match = MarkdownPage::match(j->old, j->keep, j->pieces);
        j->workerMs = threadCpuMs() - c0;
        g_task_return_boolean(t, TRUE);
    });
    g_object_unref(task);
}

void MarkdownSide::dropWidgets(size_t from, size_t to) {
    for (size_t k = from; k < to && k < infos_.size(); k++)
        for (const Markdown::Embed& e : infos_[k].page.embeds)
            if (e.widget && gtk_widget_get_parent(e.widget) == view_)
                gtk_text_view_remove(GTK_TEXT_VIEW(view_), e.widget);
}

void MarkdownSide::apply(Job* job) {
    const double c0 = threadCpuMs();
    if (stale_ || !hooks_) {   // a new context: new hooks, every piece built
        hooks_ = std::make_shared<Markdown::Hooks>();
        hooks_->folder = folder_;
        hooks_->webImages = webImages_ && WebImages::available();
        hooks_->mermaid = mermaid_ && Mermaid::available();
        hooks_->mermaidDark = dark_;
        std::weak_ptr<int> alive = alive_;
        hooks_->link = [this, alive](const std::string& url) {
            if (!alive.expired() && onLink) onLink(url);
        };
    }
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    std::vector<Info> next;
    next.reserve(job->pieces.size());
    size_t oc = 0;   // the first old piece not yet kept or dropped
    int pos = 0;     // where the next piece goes
    int built = 0, kept = 0;
    auto dropUpTo = [&](size_t upto) {
        if (oc >= upto) return;
        int len = 0;
        for (size_t k = oc; k < upto; k++) len += infos_[k].page.length;
        dropWidgets(oc, upto);
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, pos);
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, pos + len);
        gtk_text_buffer_delete(buffer_, &a, &b);
        oc = upto;
    };
    for (size_t i = 0; i < job->pieces.size(); i++) {
        const MarkdownPage::Piece& np = job->pieces[i];
        const long m = job->whole ? -1 : job->match[i];
        if (m >= 0 && (size_t)m < infos_.size() && (size_t)m >= oc) {
            dropUpTo((size_t)m);
            Info in = std::move(infos_[(size_t)m]);
            const int delta = np.base - in.piece.base;
            if (delta)
                for (Markdown::Span& s : in.page.spans)
                    if (s.line >= 0) s.line += delta;
            in.piece = np;
            pos += in.page.length;
            oc = (size_t)m + 1;
            next.push_back(std::move(in));
            kept++;
            continue;
        }
        Info in;
        in.piece = np;
        in.page = Markdown::renderRuns(tv, buffer_, job->runs, np.begin, np.end, pos, hooks_);
        pos += in.page.length;
        next.push_back(std::move(in));
        built++;
    }
    dropUpTo(infos_.size());
    // Pieces moved into `next` leave empty shells behind; the dropped ones'
    // widgets are gone already.
    infos_ = std::move(next);
    stale_ = false;
    fitWidth_ = 0;
    fit();
    for (const Info& in : infos_)
        if (!in.page.pending.empty()) fetchPictures(in);
    stats_.renders++;
    stats_.pieces = (int)infos_.size();
    stats_.built = built;
    stats_.kept = kept;
    stats_.builtTotal += built;
    stats_.mainCpuMs = job->prepMs + (threadCpuMs() - c0);
    stats_.workerCpuMs = job->workerMs;
    marksDirty_ = true;
    afterRender();
    stats_.mainCpuMs = job->prepMs + (threadCpuMs() - c0);
}

void MarkdownSide::fit() {
    if (infos_.empty()) return;
    const double page = gtk_adjustment_get_page_size(
        gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)));
    const int width = (page > 0 ? (int)page : 400) - 8 - 8 - 16;
    if (width == fitWidth_) return;
    fitWidth_ = width;
    for (Info& in : infos_)
        if (!in.page.embeds.empty()) Markdown::fit(in.page, width);
}

// ------------------------------------------------------------ web pictures

void MarkdownSide::fetchPictures(const Info& info) {
    std::weak_ptr<int> alive = alive_;
    std::vector<std::string> asked;
    for (const Markdown::PendingPicture& p : info.page.pending) {
        if (std::find(asked.begin(), asked.end(), p.url) != asked.end()) continue;
        asked.push_back(p.url);
        const std::string url = p.url;
        WebImages::fetch(url, [this, alive, url](WebImages::PicturePtr pic) {
            if (alive.expired()) return;
            arrived_.emplace_back(url, std::move(pic));
            if (!arrivedTimer_)
                arrivedTimer_ = g_timeout_add(100, [](gpointer s) -> gboolean {
                    MarkdownSide* self = static_cast<MarkdownSide*>(s);
                    self->arrivedTimer_ = 0;
                    self->placeArrived();
                    return G_SOURCE_REMOVE;
                }, this);
        });
    }
}

void MarkdownSide::placeArrived() {
    auto batch = std::move(arrived_);
    arrived_.clear();
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    bool any = false;
    for (const auto& a : batch) {
        int start = 0;
        for (Info& in : infos_) {
            if (!in.page.pending.empty()) {
                Markdown::Page tmp = in.page;
                shiftPage(tmp, start);
                const int before = gtk_text_buffer_get_char_count(buffer_);
                const int n = Markdown::placeWebPicture(tv, buffer_, tmp, a.first, a.second);
                if (n) {
                    in.page.length += gtk_text_buffer_get_char_count(buffer_) - before;
                    shiftPage(tmp, -start);
                    tmp.length = in.page.length;
                    in.page = std::move(tmp);
                    any = true;
                }
            }
            start += in.page.length;
        }
    }
    if (any) { fitWidth_ = 0; fit(); }
}

// ------------------------------------------------------------ links

void MarkdownSide::clicked(double x, double y) {
    if (!onLink) return;
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    if (gtk_text_buffer_get_has_selection(buffer_)) return;
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(tv, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    GtkTextIter it;
    if (!gtk_text_view_get_iter_at_location(tv, &it, bx, by)) return;
    const int at = gtk_text_iter_get_offset(&it);
    int start = 0;
    for (const Info& in : infos_) {
        if (at < start + in.page.length) {
            const Markdown::Span* s = in.page.spanAt(at - start);
            if (s && !s->url.empty()) onLink(s->url);
            return;
        }
        start += in.page.length;
    }
}

// ------------------------------------------------------------ scroll sync
//
// The map is PreviewScroll's (core, previewscroll:*): a mark per source
// line's first run, interpolated from one mark's top to the next one's both
// ways. The marks here are (line, character) only; a mark's top is asked of
// the page's layout when a question reaches it, and a question needs just the
// two marks around its place (PreviewScroll::around), so the page is not laid
// out beyond them. GTK lays a text view out lazily (a line has no height
// until it is laid out, and big buffers are laid out in idle slices), so a
// target is held: re-applied while the adjustment's upper bound still moves,
// and given up on when the user moves anything. Our own scrolls are held
// (holding_), and a value-changed that leaves a side where we last saw it,
// or came with its upper bound moving (GTK keeping a line still while the
// layout above it changes), is no move of the user's.

void MarkdownSide::setSource(GtkTextView* source) {
    if (source_ == source) return;
    if (srcAdj_) g_signal_handlers_disconnect_by_data(srcAdj_, this);
    source_ = source;
    srcAdj_ = source ? gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(source)) : nullptr;
    if (srcAdj_)
        g_signal_connect_swapped(srcAdj_, "value-changed", G_CALLBACK(+[](gpointer self) {
            static_cast<MarkdownSide*>(self)->sourceMoved();
        }), this);
}

bool MarkdownSide::syncActive() {
    return source_ && !infos_.empty() && gtk_widget_get_mapped(scroller_) &&
           gtk_widget_get_mapped(GTK_WIDGET(source_));
}

void MarkdownSide::rebuildMarks() {
    marks_.clear();
    int start = 0, last = -1;
    for (const Info& in : infos_) {
        for (const Markdown::Span& s : in.page.spans)
            if (s.line > last) {
                marks_.push_back({s.line, -1, (long)start + s.start});
                last = s.line;
            }
        start += in.page.length;
    }
    marksDirty_ = false;
}

double MarkdownSide::topOfAt(long at) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buffer_, &it, (int)at);
    GdkRectangle r;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view_), &it, &r);
    return r.y;
}

// The page around mark i (-1: before the first), tops asked for those two.
PreviewScroll::Page MarkdownSide::pageAround(int i) {
    if (marksDirty_) rebuildMarks();
    std::vector<PreviewScroll::Mark> two;
    const int n = (int)marks_.size();
    int lines = source_ ? gtk_text_buffer_get_line_count(gtk_text_view_get_buffer(source_)) : 0;
    double bottom = 0;
    if (n == 0) return PreviewScroll::around(two, i, 0, lines);
    const int k = std::max(0, std::min(i, n - 1));
    two.push_back(marks_[(size_t)k]);
    two.back().top = topOfAt(two.back().at);
    if (i >= 0 && k + 1 < n) {
        two.push_back(marks_[(size_t)k + 1]);
        two.back().top = topOfAt(two.back().at);
    } else if (i >= 0) {
        GtkTextIter e;
        gtk_text_buffer_get_end_iter(buffer_, &e);
        GdkRectangle r;
        gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view_), &e, &r);
        bottom = r.y + r.height;
    }
    return PreviewScroll::around(two, i < 0 ? -1 : 0, bottom, lines);
}

double MarkdownSide::paneYForLine(double line) {
    if (marksDirty_) rebuildMarks();
    PreviewScroll::Page whole;
    whole.marks = marks_;   // lines only: markAt looks at no top
    const int i = PreviewScroll::markAt(whole, line);
    return PreviewScroll::yForLine(pageAround(i), line);
}

double MarkdownSide::paneTopLine() {
    if (marksDirty_) rebuildMarks();
    if (marks_.empty()) return 0;
    GtkTextView* tv = GTK_TEXT_VIEW(view_);
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv, &vis);
    GtkTextIter it;
    if (!gtk_text_view_get_iter_at_location(tv, &it, 0, vis.y))
        gtk_text_view_get_iter_at_location(tv, &it, 8, vis.y);
    const long off = gtk_text_iter_get_offset(&it);
    auto ub = std::upper_bound(marks_.begin(), marks_.end(), off,
                               [](long o, const PreviewScroll::Mark& m) { return o < m.at; });
    const int i = (int)(ub - marks_.begin()) - 1;
    return PreviewScroll::lineForY(pageAround(i), vis.y);
}

double MarkdownSide::sourceYForLine(double line) {
    GtkTextBuffer* b = gtk_text_view_get_buffer(source_);
    const int count = gtk_text_buffer_get_line_count(b);
    const int n = std::max(0, std::min((int)std::floor(line), count - 1));
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(b, &it, n);
    int y = 0, h = 0;
    gtk_text_view_get_line_yrange(source_, &it, &y, &h);
    return y + std::max(0.0, std::min(1.0, line - n)) * h;
}

double MarkdownSide::sourceTopLine() {
    if (!source_) return 0;
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(source_, &vis);
    GtkTextIter it;
    int top = 0;
    gtk_text_view_get_line_at_y(source_, &it, vis.y, &top);
    int y = 0, h = 0;
    gtk_text_view_get_line_yrange(source_, &it, &y, &h);
    const double frac = h > 0 ? std::max(0.0, std::min(1.0, (vis.y - y) / (double)h)) : 0;
    return gtk_text_iter_get_line(&it) + frac;
}

void MarkdownSide::apply(const Target& t) {
    if (!syncActive()) return;
    holding_ = true;
    if (t.toPane) {
        const double size = gtk_adjustment_get_page_size(paneAdj_);
        gtk_adjustment_set_value(paneAdj_, paneYForLine(t.line) - t.into * size);
    } else {
        gtk_adjustment_set_value(srcAdj_, sourceYForLine(t.line));
    }
    holding_ = false;
    lastPaneV_ = gtk_adjustment_get_value(paneAdj_);
    lastPaneUp_ = gtk_adjustment_get_upper(paneAdj_);
    lastSrcV_ = gtk_adjustment_get_value(srcAdj_);
    lastSrcUp_ = gtk_adjustment_get_upper(srcAdj_);
}

void MarkdownSide::follow(const Target& t) {
    target_ = t;
    apply(t);
    startSettle();
}

void MarkdownSide::startSettle() {
    GtkAdjustment* a = target_.toPane ? paneAdj_ : srcAdj_;
    settleUpper_ = gtk_adjustment_get_upper(a);
    settleStable_ = 0;
    settleTicks_ = 0;
    if (settleTimer_) return;
    settleTimer_ = g_timeout_add(80, [](gpointer p) -> gboolean {
        MarkdownSide* self = static_cast<MarkdownSide*>(p);
        GtkAdjustment* a = self->target_.toPane ? self->paneAdj_ : self->srcAdj_;
        const double up = gtk_adjustment_get_upper(a);
        if (!self->syncActive() || ++self->settleTicks_ > 250) { self->settleTimer_ = 0; return G_SOURCE_REMOVE; }
        if (up != self->settleUpper_) {
            self->settleUpper_ = up;
            self->settleStable_ = 0;
            self->apply(self->target_);
        } else if (++self->settleStable_ >= 4) {
            self->settleTimer_ = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }, this);
}

void MarkdownSide::stopSettle() {
    if (settleTimer_) g_source_remove(settleTimer_);
    settleTimer_ = 0;
}

void MarkdownSide::sourceMoved() {
    if (!srcAdj_) return;
    const double v = gtk_adjustment_get_value(srcAdj_), up = gtk_adjustment_get_upper(srcAdj_);
    const bool ours = holding_ || std::fabs(v - lastSrcV_) < 0.5 || up != lastSrcUp_;
    lastSrcV_ = v;
    lastSrcUp_ = up;
    if (ours || !syncActive()) return;
    pageLeads_ = false;
    follow({true, sourceTopLine(), 0});
}

void MarkdownSide::paneMoved() {
    const double v = gtk_adjustment_get_value(paneAdj_), up = gtk_adjustment_get_upper(paneAdj_);
    const bool ours = holding_ || std::fabs(v - lastPaneV_) < 0.5 || up != lastPaneUp_;
    lastPaneV_ = v;
    lastPaneUp_ = up;
    if (ours || !syncActive()) return;
    pageLeads_ = true;
    follow({false, paneTopLine(), 0});
}

// The caret's block in the page, brought into view when it is not (a third
// of the way down, as the Mac's marker does).
void MarkdownSide::sourceCaret(int line) {
    if (!syncActive() || line < 0) return;
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(view_), &vis);
    const double y = paneYForLine(line);
    if (y >= vis.y && y < vis.y + vis.height * 0.9) return;
    pageLeads_ = false;
    follow({true, (double)line, 0.3});
}

void MarkdownSide::afterRender() {
    if (!syncActive() || pageLeads_) return;
    follow({true, sourceTopLine(), 0});
}
