// ChangeReviewPage.cpp — see ChangeReviewPage.h.
#include "ChangeReviewPage.h"

#include <algorithm>
#include <cstdio>

#include "GitPanel.h"
#include "GitRun.h"
#include "GitTrash.h"
#include "MergeConflicts.h"
#include "QuickDiff.h"
#include "SyntaxHighlighter.h"

using ChangeReview::ConflictPart;
using ChangeReview::FileDiff;
using ChangeReview::Op;
using ChangeReview::PageRow;
using ChangeReview::RowKind;
using ChangeReview::Section;

namespace {

std::string nameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The git runner for the core's reading and writing: on the git thread, an
// argument vector and never a shell; hash-object takes its text on stdin
// even when that is empty.
ChangeReview::GitOut runner(const std::string& dir, const std::vector<std::string>& args,
                            const std::string& input) {
    const bool wantsInput =
        !input.empty() || std::find(args.begin(), args.end(), "hash-object") != args.end();
    GitResult r = runGit(dir, args, wantsInput ? &input : nullptr);
    ChangeReview::GitOut o;
    o.ok = r.ok();
    o.status = r.status;
    o.out = r.out;
    o.err = r.ok() ? std::string() : gitFailure(r);
    return o;
}

std::string utf8(const std::u16string& s) { return QuickDiff::toUtf8(s); }

std::string rgbaCss(unsigned rgb, double alpha) {
    char b[64];
    snprintf(b, sizeof b, "rgba(%u,%u,%u,%.2f)", (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
             alpha);
    return b;
}

struct Delivery {
    std::shared_ptr<bool> alive;
    ChangeReviewPage* page;
    std::function<void(ChangeReviewPage*)> fn;
};

}  // namespace

// One read: every file and its diff, and the page laid out from them.
struct ChangeReviewPage::Data {
    ChangeReview::Read read;
    ChangeReview::Page page;
    size_t fingerprint = 0;
    // A worktree's page: what it is compared with, and the main checkout.
    std::string baseRef, baseLabel, mainPath;
};

const char* ChangeReviewPage::titleOf(Act a) {
    switch (a) {
    case Act::Stage: return "Stage";
    case Act::Unstage: return "Unstage";
    case Act::Discard: return "Discard";
    case Act::Side: return "Side by Side";
    case Act::Open: return "Open";
    case Act::DiscardFile: return "Discard File";
    case Act::StageFile: return "Stage File";
    case Act::UnstageFile: return "Unstage File";
    case Act::KeepCurrent: return "Keep Current";
    case Act::KeepIncoming: return "Keep Incoming";
    case Act::KeepBoth: return "Keep Both";
    case Act::MarkResolved: return "Mark Resolved";
    }
    return "";
}

ChangeReviewPage::ChangeReviewPage(GitPanel* panel, const std::string& root)
    : panel_(panel), root_s_(root) {
    panelAlive_ = panel->aliveFlag();
    build();
    panel_->setReviewOpen(true);
    panel_->onReviewRefresh = [this] { reload(); };
    reload();
}

ChangeReviewPage::ChangeReviewPage(GitPanel* panel, const std::string& worktree,
                                   const std::string& label, const std::string& branch,
                                   const std::string& mainRef)
    : panel_(panel), root_s_(worktree), worktree_(worktree), label_(label), branch_(branch),
      mainRef_(mainRef) {
    panelAlive_ = panel->aliveFlag();
    build();
    // A worktree's page belongs to the tree's Worktrees node: the panel's
    // reads are not its own, so it neither asks for them nor follows them.
    reload();
}

std::string ChangeReviewPage::title() const {
    if (worktree_.empty()) return "Uncommitted Changes";
    return (label_.empty() ? nameOf(worktree_) : label_) + " vs " + (mainRef_.empty() ? "main" : mainRef_);
}

ChangeReviewPage::~ChangeReviewPage() {
    *alive_ = false;
    if (worktree_.empty() && panelAlive_ && *panelAlive_) {
        panel_->onReviewRefresh = nullptr;
        panel_->setReviewOpen(false);
    }
    g_object_unref(root_);
}

void ChangeReviewPage::build() {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(root_);
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(bar, 12);
    gtk_widget_set_margin_end(bar, 12);
    gtk_widget_set_margin_top(bar, 8);
    gtk_widget_set_margin_bottom(bar, 6);
    title_ = gtk_label_new(title().c_str());
    gtk_widget_add_css_class(title_, "title-4");
    summary_ = gtk_label_new("");
    gtk_widget_add_css_class(summary_, "dim-label");
    messageLabel_ = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(messageLabel_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(messageLabel_, TRUE);
    gtk_label_set_xalign(GTK_LABEL(messageLabel_), 0);
    undo_ = gtk_button_new_with_label("Undo Discard");
    gtk_widget_set_visible(undo_, FALSE);
    g_signal_connect_swapped(undo_, "clicked", G_CALLBACK(+[](ChangeReviewPage* self) {
        self->undoDiscard();
    }), this);
    gtk_box_append(GTK_BOX(bar), title_);
    gtk_box_append(GTK_BOX(bar), summary_);
    if (worktree_.empty()) {
        gtk_box_append(GTK_BOX(bar), messageLabel_);
        gtk_box_append(GTK_BOX(bar), undo_);
        gtk_box_append(GTK_BOX(root_), bar);
    } else {
        // Apply to Main... at the bar's right; what it did on a line of its
        // own under the bar (a worktree's long title leaves no room beside
        // it), with Show in Source Control.
        GtkWidget* gap = gtk_label_new("");
        gtk_widget_set_hexpand(gap, TRUE);
        gtk_box_append(GTK_BOX(bar), gap);
        apply_ = gtk_button_new_with_label("Apply to Main…");
        gtk_widget_set_visible(apply_, FALSE);
        gtk_widget_set_tooltip_text(apply_, "Bring these changes into the main checkout as uncommitted changes");
        g_signal_connect_swapped(apply_, "clicked", G_CALLBACK(+[](ChangeReviewPage* self) {
            self->applyToMain();
        }), this);
        gtk_box_append(GTK_BOX(bar), apply_);
        gtk_box_append(GTK_BOX(root_), bar);
        noteRow_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(noteRow_, 12);
        gtk_widget_set_margin_end(noteRow_, 12);
        gtk_widget_set_margin_bottom(noteRow_, 6);
        gtk_widget_set_visible(noteRow_, FALSE);
        gtk_box_append(GTK_BOX(noteRow_), messageLabel_);
        source_ = gtk_button_new_with_label("Show in Source Control");
        gtk_widget_set_visible(source_, FALSE);
        gtk_widget_set_tooltip_text(source_, "The main checkout's uncommitted changes, with Source Control beside them");
        g_signal_connect_swapped(source_, "clicked", G_CALLBACK(+[](ChangeReviewPage* self) {
            self->pressSourceButton();
        }), this);
        gtk_box_append(GTK_BOX(noteRow_), source_);
        gtk_box_append(GTK_BOX(root_), noteRow_);
    }

    buffer_ = gtk_text_buffer_new(nullptr);
    view_ = gtk_text_view_new_with_buffer(buffer_);
    g_object_unref(buffer_);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view_), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view_), GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view_), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view_), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view_), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view_), 8);
    gtk_widget_add_css_class(view_, "minicode-editor");
    scroll_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroll_, "minicode-scroller");
    gtk_widget_add_css_class(scroll_, "minicode-editor-scroller");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), view_);
    gtk_widget_set_hexpand(scroll_, TRUE);
    gtk_widget_set_vexpand(scroll_, TRUE);
    gtk_box_append(GTK_BOX(root_), scroll_);

    // The tags: a tint for each kind of row, the changed words, the syntax.
    auto tag = [&](const char* name, const char* prop, const std::string& value) {
        GtkTextTag* t = gtk_text_buffer_create_tag(buffer_, name, nullptr);
        if (prop) g_object_set(t, prop, value.c_str(), nullptr);
        return t;
    };
    tag("removed", "paragraph-background", rgbaCss(0xF14C4C, 0.16));
    tag("added", "paragraph-background", rgbaCss(0x73C991, 0.16));
    tag("file", "paragraph-background", rgbaCss(0x808080, 0.18));
    tag("gap", "paragraph-background", rgbaCss(0x808080, 0.08));
    tag("conflict", "paragraph-background", rgbaCss(0xE2C08D, 0.12));
    // A conflict block's parts (ChangeReview::ConflictPart), colored from
    // editor.merge-current and editor.merge-incoming in applySettings: the
    // markers' lines at two and a half times the alpha, the ancestor gray.
    for (const char* name : {"conflict-current-marker", "conflict-current", "conflict-base-marker",
                             "conflict-base", "conflict-incoming", "conflict-incoming-marker"})
        tag(name, nullptr, "");
    tag("wordremoved", "background", rgbaCss(0xF14C4C, 0.35));
    tag("wordadded", "background", rgbaCss(0x73C991, 0.35));
    GtkTextTag* bold = tag("bold", nullptr, "");
    g_object_set(bold, "weight", PANGO_WEIGHT_BOLD, nullptr);
    GtkTextTag* dim = tag("dim", "foreground", "#9D9D9D");
    (void)dim;
    for (int i = 0; i < kTokenStyleCount; i++) tag(("syn" + std::to_string(i)).c_str(), nullptr, "");

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "released", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click));
}

void ChangeReviewPage::applySettings() {
    {
        const Settings fallback;
        auto css = [](const Rgba& c, double times) {
            const double a = times == 1 ? c.a : std::min(1.0, std::max(0.25, c.a * times));
            char b[64];
            snprintf(b, sizeof b, "rgba(%d,%d,%d,%.3f)", (int)c.r, (int)c.g, (int)c.b, a);
            return std::string(b);
        };
        const Rgba cur = mergeColor ? mergeColor(false) : fallback.mergeCurrentColor();
        const Rgba inc = mergeColor ? mergeColor(true) : fallback.mergeIncomingColor();
        Rgba base = cur;
        base.r = base.g = base.b = 0x60;
        base.a = 0x29 / 255.0;
        const std::pair<const char*, std::string> parts[] = {
            {"conflict-current-marker", css(cur, 2.5)}, {"conflict-current", css(cur, 1)},
            {"conflict-base-marker", css(base, 2.5)},   {"conflict-base", css(base, 1)},
            {"conflict-incoming", css(inc, 1)},         {"conflict-incoming-marker", css(inc, 2.5)}};
        GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
        for (const auto& [name, value] : parts)
            if (GtkTextTag* t = gtk_text_tag_table_lookup(table, name))
                g_object_set(t, "paragraph-background", value.c_str(), nullptr);
    }
    for (int i = 0; i < kTokenStyleCount; i++) {
        GtkTextTag* t = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer_),
                                                  ("syn" + std::to_string(i)).c_str());
        if (!t) continue;
        if (!syntaxColor) continue;
        char b[16];
        snprintf(b, sizeof b, "#%06X", syntaxColor(i).rgb());
        g_object_set(t, "foreground", b, nullptr);
    }
}

// ------------------------------------------------------------------ reading
void ChangeReviewPage::onMain(std::shared_ptr<bool> alive, ChangeReviewPage* page,
                              std::function<void(ChangeReviewPage*)> fn) {
    auto* d = new Delivery{std::move(alive), page, std::move(fn)};
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        auto* d = static_cast<Delivery*>(p);
        if (*d->alive) d->fn(d->page);
        delete d;
        return G_SOURCE_REMOVE;
    }, d, nullptr);
}

void ChangeReviewPage::reload() {
    if (loading_) {
        again_ = true;
        return;
    }
    loading_ = true;
    const std::string root = root_s_;
    const ChangeReview::Opened opened = opened_;
    const std::string worktree = worktree_, mainRef = mainRef_;
    panel_->runOnGitThread([this, root, opened, worktree, mainRef, alive = alive_] {
        auto d = std::make_shared<Data>();
        if (!worktree.empty()) {
            ChangeReview::WorktreeRead w = ChangeReview::readWorktree(worktree, mainRef, runner);
            d->read = std::move(w.read);
            d->baseRef = w.baseRef;
            d->baseLabel = w.baseLabel;
            d->mainPath = w.mainPath;
        } else {
            d->read = ChangeReview::readUncommitted(root, runner);
        }
        d->page = ChangeReview::layout(d->read.diffs, opened);
        d->fingerprint = ChangeReview::fingerprint(d->read.diffs);
        if (!*alive) return;
        onMain(alive, this, [d](ChangeReviewPage* p) {
            p->loading_ = false;
            p->reads_++;
            p->show(d);
            if (p->again_) {
                p->again_ = false;
                p->reload();
            }
        });
    });
}

void ChangeReviewPage::show(std::shared_ptr<Data> d) {
    // The note about the last action stays while the changes are what the
    // first read after it showed, and goes when they are not.
    if (!message_.empty()) {
        if (!messageSettled_) {
            messageSettled_ = true;
            messagePrint_ = d->fingerprint;
        } else if (messagePrint_ != d->fingerprint) {
            message_.clear();
        }
    }
    const bool same = data_ && data_->read.error == d->read.error &&
                      data_->fingerprint == d->fingerprint &&
                      data_->read.diffs.size() == d->read.diffs.size();
    if (same && data_->page.text == d->page.text) {
        // Nothing moved: keep the widgets (and the reader's place) as they
        // are, with the extras of this read for the next action.
        data_ = d;
        gtk_label_set_text(GTK_LABEL(messageLabel_), message_.c_str());
        updateSummary();
        return;
    }
    // Where the eye is, to put it back after the new layout.
    ChangeReview::Anchor anchor;
    if (data_ && !data_->page.rows.empty()) {
        const long top = topRow();
        if (top >= 0) anchor = ChangeReview::anchorAt(data_->page, data_->read.diffs, (size_t)top);
    }
    const double was = gtk_adjustment_get_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll_)));
    const bool first = !data_;
    data_ = d;
    fill();
    updateSummary();
    if (!first) {
        if (anchor.valid)
            scrollToRow(ChangeReview::rowFor(d->page, d->read.diffs, anchor));
        else
            gtk_adjustment_set_value(
                gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll_)), was);
    }
}

long ChangeReviewPage::topRow() const {
    if (rowChar_.empty()) return -1;
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll_));
    GtkTextIter it;
    int lineTop = 0;
    gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(view_), &it, (int)gtk_adjustment_get_value(adj),
                                &lineTop);
    return (long)rowAtOffset(gtk_text_iter_get_offset(&it));
}

void ChangeReviewPage::scrollToRow(size_t row) {
    if (row >= rowChar_.size()) return;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buffer_, &it, rowChar_[row]);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(view_), &it, 0, TRUE, 0, 0);
}

size_t ChangeReviewPage::rowAtOffset(int offset) const {
    auto it = std::upper_bound(rowChar_.begin(), rowChar_.end(), offset);
    return it == rowChar_.begin() ? 0 : (size_t)(it - rowChar_.begin() - 1);
}

// The page's text in the buffer, each row tinted by its kind, words and
// syntax colors over it, and the buttons at the ends of their rows.
void ChangeReviewPage::fill() {
    const ChangeReview::Page& p = data_->page;
    buttons_.clear();
    tints_.clear();
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(buffer_, &s, &e);
    // The caret to the start first: a button leaving as its anchor is
    // deleted asks the buffer for its selection, and the marks still sat in
    // the text that was going (a crash in gtk_text_buffer_get_selection_bounds).
    gtk_text_buffer_place_cursor(buffer_, &s);
    gtk_text_buffer_get_bounds(buffer_, &s, &e);
    gtk_text_buffer_delete(buffer_, &s, &e);   // the anchors go with it
    std::string error = data_->read.error;
    if (!error.empty() || p.rows.empty()) {
        rowChar_.clear();
        gtk_text_buffer_set_text(buffer_, !error.empty() ? error.c_str()
                                          : worktree_.empty() ? "No changes."
                                                              : "No changes against the main branch.", -1);
        gtk_text_buffer_get_bounds(buffer_, &s, &e);
        gtk_text_buffer_apply_tag_by_name(buffer_, "dim", &s, &e);
        return;
    }
    // UTF-16 offsets to characters.
    const std::u16string& t = p.text;
    std::vector<int> ch(t.size() + 1, 0);
    {
        int n = 0;
        for (size_t i = 0; i < t.size(); i++) {
            ch[i] = n;
            const bool high = t[i] >= 0xD800 && t[i] < 0xDC00 && i + 1 < t.size() &&
                              t[i + 1] >= 0xDC00 && t[i + 1] < 0xE000;
            if (high) {
                ch[i + 1] = n;
                i++;
            }
            n++;
        }
        ch[t.size()] = n;
    }
    std::string text = utf8(t);
    gtk_text_buffer_set_text(buffer_, text.c_str(), (int)text.size());
    std::vector<int> base(p.rows.size());
    for (size_t i = 0; i < p.rows.size(); i++) base[i] = ch[std::min(p.starts[i], t.size())];
    auto iterAt = [&](int off) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buffer_, &it, off);
        return it;
    };
    tints_.assign(p.rows.size(), "");
    for (size_t i = 0; i < p.rows.size(); i++) {
        const PageRow& r = p.rows[i];
        const char* tag = nullptr;
        switch (r.kind) {
        case RowKind::Removed: tag = "removed"; break;
        case RowKind::Added: tag = "added"; break;
        case RowKind::File: tag = "file"; break;
        case RowKind::Gap: tag = "gap"; break;
        case RowKind::Conflict:
            switch (r.part) {
            case ConflictPart::CurrentMarker: tag = "conflict-current-marker"; break;
            case ConflictPart::Current: tag = "conflict-current"; break;
            case ConflictPart::BaseMarker: tag = "conflict-base-marker"; break;
            case ConflictPart::Base: tag = "conflict-base"; break;
            case ConflictPart::Incoming: tag = "conflict-incoming"; break;
            case ConflictPart::IncomingMarker: tag = "conflict-incoming-marker"; break;
            case ConflictPart::Separator: break;   // untinted, as on the Mac
            default: tag = "conflict"; break;
            }
            break;
        default: break;
        }
        const int end = i + 1 < p.rows.size() ? base[i + 1] : ch[t.size()];
        if (tag) {
            GtkTextIter a = iterAt(base[i]), b = iterAt(end);
            gtk_text_buffer_apply_tag_by_name(buffer_, tag, &a, &b);
            tints_[i] = tag;
        }
        if (r.kind == RowKind::File || r.kind == RowKind::Section) {
            GtkTextIter a = iterAt(base[i]), b = iterAt(end);
            gtk_text_buffer_apply_tag_by_name(buffer_, "bold", &a, &b);
        } else if (r.kind == RowKind::Gap || r.kind == RowKind::Note) {
            GtkTextIter a = iterAt(base[i]), b = iterAt(end);
            gtk_text_buffer_apply_tag_by_name(buffer_, "dim", &a, &b);
        }
    }
    auto span = [&](const SideBySide::Span& sp, const std::string& name) {
        if (sp.start >= t.size() || sp.length == 0) return;
        const size_t last = std::min(t.size(), sp.start + sp.length);
        GtkTextIter a = iterAt(ch[sp.start]), b = iterAt(ch[last]);
        gtk_text_buffer_apply_tag_by_name(buffer_, name.c_str(), &a, &b);
    };
    for (const SideBySide::Span& sp : p.syntax)
        span(sp, "syn" + std::to_string(std::clamp(sp.style, 0, kTokenStyleCount - 1)));
    for (const SideBySide::Span& sp : p.words) span(sp, sp.style == 0 ? "wordremoved" : "wordadded");
    applySettings();

    // The buttons at the ends of their rows, last row first so the earlier
    // offsets hold; then each row's start counts the anchors before it.
    std::vector<std::vector<Act>> acts(p.rows.size());
    for (size_t i = 0; i < p.rows.size(); i++) acts[i] = actsFor(i);
    rowChar_ = base;
    for (size_t i = p.rows.size(); i-- > 0;) {
        if (acts[i].empty()) continue;
        const int endOfText = (i + 1 < p.rows.size() ? base[i + 1] - 1 : ch[t.size()]);
        GtkTextIter at = iterAt(endOfText);
        GtkTextChildAnchor* anchor = gtk_text_buffer_create_child_anchor(buffer_, &at);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_margin_start(box, 14);
        for (Act a : acts[i]) {
            GtkWidget* b = gtk_button_new_with_label(titleOf(a));
            gtk_widget_add_css_class(b, "flat");
            gtk_widget_add_css_class(b, "minicode-review-button");
            // A conflict's choices say what they keep, as on the Mac.
            if (a == Act::KeepCurrent || a == Act::KeepIncoming || a == Act::KeepBoth) {
                const PageRow& r = p.rows[i];
                const FileDiff& fd = data_->read.diffs[r.file];
                if (r.block >= 0 && (size_t)r.block < fd.conflicts.size()) {
                    using MergeConflicts::Keep;
                    const Keep k = a == Act::KeepCurrent    ? Keep::Current
                                   : a == Act::KeepIncoming ? Keep::Incoming
                                                            : Keep::Both;
                    gtk_widget_set_tooltip_text(
                        b, MergeConflicts::tooltip(k, fd.conflicts[(size_t)r.block]).c_str());
                }
            } else if (a == Act::MarkResolved) {
                gtk_widget_set_tooltip_text(
                    b, "Stage the file, which tells git its conflicts are resolved");
            }
            struct Press {
                ChangeReviewPage* page;
                std::shared_ptr<bool> alive;
                Act act;
                size_t row;
            };
            auto* pr = new Press{this, alive_, a, i};
            g_signal_connect_data(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
                auto* p = static_cast<Press*>(d);
                if (*p->alive) p->page->act(p->act, p->row);
            }), pr, +[](gpointer d, GClosure*) { delete static_cast<Press*>(d); }, (GConnectFlags)0);
            gtk_box_append(GTK_BOX(box), b);
            buttons_.push_back({a, i, titleOf(a), b});
        }
        gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(view_), box, anchor);
    }
    int anchorsBefore = 0;
    for (size_t i = 0; i < p.rows.size(); i++) {
        rowChar_[i] = base[i] + anchorsBefore;
        if (!acts[i].empty()) anchorsBefore++;
    }
}

void ChangeReviewPage::updateSummary() {
    size_t files = 0, added = 0, removed = 0;
    if (data_) {
        files = ChangeReview::fileCount(data_->read.diffs);
        for (const FileDiff& d : data_->read.diffs) {
            added += d.added;
            removed += d.removed;
        }
    }
    std::string counts;
    if (files)
        counts = std::to_string(files) + (files == 1 ? " file" : " files") + ", +" +
                 std::to_string(added) + " -" + std::to_string(removed);
    if (!worktree_.empty()) {
        std::string parts;
        auto add = [&](const std::string& t) {
            if (t.empty()) return;
            parts += (parts.empty() ? "" : "  ·  ") + t;
        };
        if (data_ && !data_->baseLabel.empty()) add("against " + data_->baseLabel);
        add(branch_);
        add(counts);
        gtk_label_set_text(GTK_LABEL(summary_), parts.c_str());
        // Once applied, the button says so until the worktree's changes are
        // other than those brought in (the agent worked on).
        if (applied_ && data_ && data_->fingerprint != appliedPrint_) applied_ = false;
        gtk_widget_set_visible(apply_, data_ && !data_->read.diffs.empty() && !data_->mainPath.empty());
        gtk_button_set_label(GTK_BUTTON(apply_), applyTitle().c_str());
        gtk_widget_set_sensitive(apply_, applyEnabled());
        gtk_widget_set_tooltip_text(apply_, applied_
            ? "These changes are in the main checkout now, uncommitted. The button comes back "
              "when the worktree's changes differ from what was brought in."
            : "Bring these changes into the main checkout as uncommitted changes");
        gtk_widget_set_visible(source_, sourceButtonShown());
        gtk_widget_set_visible(noteRow_, !message_.empty() || sourceButtonShown());
        gtk_widget_set_visible(undo_, FALSE);
        return;
    }
    gtk_label_set_text(GTK_LABEL(summary_), counts.c_str());
    const std::string undo = panel_->undoDiscardTitle();
    gtk_widget_set_visible(undo_, !undo.empty());
    gtk_widget_set_tooltip_text(undo_, undo.empty() ? nullptr : undo.c_str());
}

// ------------------------------------------------------------------ buttons
// Which buttons a row has: the file header's, or a block's first row's.
std::vector<ChangeReviewPage::Act> ChangeReviewPage::actsFor(size_t row) const {
    std::vector<Act> out;
    if (!data_ || row >= data_->page.rows.size()) return out;
    const PageRow& pr = data_->page.rows[row];
    if (pr.file >= data_->read.diffs.size()) return out;
    const FileDiff& fd = data_->read.diffs[pr.file];
    const ChangeReview::File& f = fd.file;
    if (pr.kind == RowKind::File) {
        if (f.conflict && f.section == Section::Changes &&
            (ChangeReview::canMarkResolved(fd) || !fd.conflicts.empty())) {
            // A conflict read as text: its blocks have the choices, and once
            // none is left, Mark Resolved stages it.
            if (!fd.conflicts.empty()) out.push_back(Act::Side);
            out.push_back(Act::Open);
            if (ChangeReview::canMarkResolved(fd)) out.push_back(Act::MarkResolved);
            return out;
        }
        if (f.conflict) {
            if (f.section == Section::Changes && f.newExists) out.push_back(Act::Open);
            return out;
        }
        const bool shows = !f.binary && !f.submodule && !f.symlink;
        if (shows) out.push_back(Act::Side);
        if (f.section == Section::Changes || (f.section == Section::Staged && f.newExists) ||
            (f.section == Section::Plain && f.newExists))
            out.push_back(Act::Open);
        if (f.section == Section::Changes) {
            if (!f.submodule) out.push_back(Act::DiscardFile);
            out.push_back(Act::StageFile);
        } else if (f.section == Section::Staged) {
            out.push_back(Act::UnstageFile);
        }
    } else if (pr.kind == RowKind::Conflict) {
        if (pr.blockStart && f.section == Section::Changes) {
            out.push_back(Act::KeepCurrent);
            out.push_back(Act::KeepIncoming);
            out.push_back(Act::KeepBoth);
        }
    } else if (pr.blockStart &&
               ChangeReview::wholeOnlyReason(f).empty()) {
        if (f.section == Section::Changes) {
            out.push_back(Act::Stage);
            out.push_back(Act::Discard);
        } else if (f.section == Section::Staged) {
            out.push_back(Act::Unstage);
        }
    }
    return out;
}

void ChangeReviewPage::onPressed(GtkGestureClick*, int, double x, double y, gpointer selfp) {
    ChangeReviewPage* self = static_cast<ChangeReviewPage*>(selfp);
    if (!self->data_) return;
    // A drag that selected text is no click.
    if (gtk_text_buffer_get_has_selection(self->buffer_)) return;
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(self->view_), GTK_TEXT_WINDOW_WIDGET,
                                          (int)x, (int)y, &bx, &by);
    GtkTextIter it;
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(self->view_), &it, bx, by);
    self->openGap(self->rowAtOffset(gtk_text_iter_get_offset(&it)));
}

bool ChangeReviewPage::openGap(size_t row) {
    if (!data_ || row >= data_->page.rows.size()) return false;
    const PageRow& pr = data_->page.rows[row];
    if (pr.kind != RowKind::Gap || pr.file >= data_->read.diffs.size()) return false;
    opened_[data_->read.diffs[pr.file].file.key()].insert(pr.gap);
    // Laid out again from what was read; nothing is asked of git.
    auto d = std::make_shared<Data>(*data_);
    d->page = ChangeReview::layout(d->read.diffs, opened_);
    show(d);
    return true;
}

void ChangeReviewPage::act(Act a, size_t row) {
    if (!data_ || row >= data_->page.rows.size()) return;
    const PageRow& pr = data_->page.rows[row];
    if (pr.file >= data_->read.diffs.size()) return;
    const size_t fi = pr.file;
    switch (a) {
    case Act::Stage: runOp(Op::Stage, fi, pr.block); break;
    case Act::Unstage: runOp(Op::Unstage, fi, pr.block); break;
    case Act::Discard: runOp(Op::Discard, fi, pr.block); break;
    case Act::Side: sideBySide(fi); break;
    case Act::Open: openFile(fi); break;
    case Act::DiscardFile:
        // The panel's Discard Changes: it asks, trashes and restores, and
        // its refresh brings the page up to date.
        panel_->discardPath(data_->read.top + "/" + data_->read.diffs[fi].file.path);
        break;
    case Act::StageFile:
    case Act::UnstageFile: stageFile(fi); break;
    case Act::KeepCurrent: keepConflict(MergeConflicts::Keep::Current, fi, pr.block); break;
    case Act::KeepIncoming: keepConflict(MergeConflicts::Keep::Incoming, fi, pr.block); break;
    case Act::KeepBoth: keepConflict(MergeConflicts::Keep::Both, fi, pr.block); break;
    case Act::MarkResolved: markResolved(fi); break;
    }
}

bool ChangeReviewPage::press(const std::string& title, size_t row) {
    for (const Button& b : buttons_)
        if (b.row == row && b.title == title) {
            act(b.act, b.row);
            return true;
        }
    return false;
}

GtkWidget* ChangeReviewPage::buttonWidget(const std::string& title, size_t row) const {
    for (const Button& b : buttons_)
        if (b.row == row && b.title == title) return b.widget;
    return nullptr;
}

// ------------------------------------------------------------------ actions
void ChangeReviewPage::say(const std::string& text, bool ok) {
    message_ = text;
    messageOk_ = ok;
    messageSettled_ = false;
    gtk_label_set_text(GTK_LABEL(messageLabel_), text.c_str());
    gtk_widget_set_tooltip_text(messageLabel_, text.c_str());
    if (ok) gtk_widget_remove_css_class(messageLabel_, "error");
    else gtk_widget_add_css_class(messageLabel_, "error");
    panel_->note(text, ok);
    updateSummary();
}

void ChangeReviewPage::finish(const std::string& text, bool ok) {
    say(text, ok);
    if (!ok) gtk_widget_error_bell(root_);
    panel_->refresh();
    reload();
}

// ------------------------------------------------------------ Apply to Main
std::string ChangeReviewPage::applyTitle() const { return applied_ ? "Applied to Main" : "Apply to Main…"; }
bool ChangeReviewPage::applyEnabled() const { return !applied_ && !applying_; }
bool ChangeReviewPage::sourceButtonShown() const { return applied_ && onShowSourceControl != nullptr; }

void ChangeReviewPage::pressSourceButton() {
    if (!onShowSourceControl || appliedMain_.empty()) {
        gtk_widget_error_bell(root_);
        return;
    }
    onShowSourceControl(appliedMain_);
}

std::vector<std::string> ChangeReviewPage::fileLines() const {
    std::vector<std::string> out;
    if (data_)
        for (const FileDiff& d : data_->read.diffs)
            out.push_back(std::string(1, d.file.letter) + " " + d.file.path);
    return out;
}

// Reads main's status, then asks (naming the files) or says why not.
void ChangeReviewPage::applyToMain() {
    auto d = data_;
    if (worktree_.empty() || !d || d->mainPath.empty() || d->read.diffs.empty() || applying_) {
        gtk_widget_error_bell(root_);
        return;
    }
    const std::string mainName = mainRef_.empty() ? "main" : mainRef_, wtName = nameOf(worktree_);
    panel_->runOnGitThread([this, d, mainName, wtName, alive = alive_] {
        ChangeReview::WorktreeRead w;
        w.read = d->read;
        w.baseRef = d->baseRef;
        w.baseLabel = d->baseLabel;
        w.mainPath = d->mainPath;
        const GitUi::ApplyPlan plan = ChangeReview::planApplyToMain(w, mainName, wtName, runner);
        if (!*alive) return;
        onMain(alive, this, [d, plan, mainName](ChangeReviewPage* p) {
            if (!plan.refusal.empty()) {
                p->finish(plan.refusal, false);
                return;
            }
            p->panel_->askQuestion(plan, [p, alive = p->alive_, d, mainName] {
                if (*alive) p->runApply(d, mainName);
            });
        });
    });
}

// The worktree's patch against the merge base, and one for each untracked
// file, applied to main: plainly first (all or nothing), else three-way,
// whose cleanly applied files are then unstaged; never a commit.
void ChangeReviewPage::runApply(std::shared_ptr<Data> d, const std::string& mainName) {
    applying_ = true;
    updateSummary();
    panel_->runOnGitThread([this, d, mainName, alive = alive_] {
        ChangeReview::WorktreeRead w;
        w.read = d->read;
        w.baseRef = d->baseRef;
        w.baseLabel = d->baseLabel;
        w.mainPath = d->mainPath;
        const ChangeReview::ApplyResult r = ChangeReview::applyToMain(w, mainName, runner);
        if (!*alive) return;
        onMain(alive, this, [d, r](ChangeReviewPage* p) {
            p->applying_ = false;
            if (r.ok) {
                p->applied_ = true;
                p->appliedPrint_ = d->fingerprint;
                p->appliedMain_ = d->mainPath;
            }
            p->finish(r.text, r.ok);
        });
    });
}

void ChangeReviewPage::runOp(Op op, size_t fi, long block) {
    auto d = data_;
    if (!d || fi >= d->read.diffs.size() || block < 0) return;
    const FileDiff& fd = d->read.diffs[fi];
    const GitUi::ActionPlan plan = ChangeReview::plan(fd, op, false);
    if (!plan.refusal.empty()) {
        finish(plan.refusal, false);
        return;
    }
    const std::u16string result = ChangeReview::blockResult(fd, (size_t)block, op);
    auto go = [this, op, result, fi, d, plan, alive = alive_] {
        if (!*alive) return;
        if (op == Op::Discard) writeDiscard(result, fi, d, plan);
        else writeIndex(result, fi, d, plan);
    };
    if (plan.question.empty()) {
        go();
        return;
    }
    // A tab holding unsaved text for the file: its buffer is not what the
    // view shows, so nothing is written over it.
    const std::string path = d->read.top + "/" + fd.file.path;
    if (hasUnsavedText && hasUnsavedText(path)) {
        finish(nameOf(path) + " has unsaved changes in a tab: save or revert them first, so the "
                              "discard does not cross them.",
               false);
        return;
    }
    panel_->askQuestion(plan, go);
}

// The index's new text, once the entry is checked to be the one the view
// was drawn from.
void ChangeReviewPage::writeIndex(std::u16string text, size_t fi, std::shared_ptr<Data> d,
                                  const GitUi::ActionPlan& plan) {
    const std::string done = plan.doneText;
    panel_->runOnGitThread([this, text, fi, d, done, alive = alive_] {
        const std::string failure = ChangeReview::stageText(d->read, fi, text, runner);
        if (!*alive) return;
        onMain(alive, this, [failure, done](ChangeReviewPage* p) {
            p->finish(failure.empty() ? done : failure, failure.empty());
        });
    });
}

// The work tree's new text: the file as it is goes to the Trash first, then
// the new text is written over it, once the file and the index are checked
// to be what the view was drawn from.
void ChangeReviewPage::writeDiscard(std::u16string text, size_t fi, std::shared_ptr<Data> d,
                                    const GitUi::ActionPlan& plan) {
    const std::string done = plan.doneText;
    panel_->runOnGitThread([this, text, fi, d, done, alive = alive_] {
        const std::string rel = d->read.diffs[fi].file.path;
        const std::string path = d->read.top + "/" + rel;
        std::string copy, trashed;
        const std::string failure = ChangeReview::discardText(
            d->read, fi, text, runner,
            [&](std::string* where, std::string* why) {
                std::string err;
                if (!GitTrash::trashCopy(path, where, &err)) {
                    *why = nameOf(rel) + " could not be copied to the Trash (" + err +
                           "), so nothing was discarded.";
                    return false;
                }
                copy = *where;
                return true;
            },
            &trashed);
        if (!failure.empty() && !copy.empty()) GitTrash::remove(copy);
        if (!*alive) return;
        onMain(alive, this, [failure, done, path, rel, trashed](ChangeReviewPage* p) {
            if (failure.empty() && !trashed.empty()) p->panel_->rememberDiscard(path, rel, trashed);
            p->finish(failure.empty() ? done : failure, failure.empty());
        });
    });
}

void ChangeReviewPage::stageFile(size_t fi, const std::string& done) {
    auto d = data_;
    if (!d || fi >= d->read.diffs.size()) return;
    const ChangeReview::Extra x = d->read.extras[fi];
    const std::vector<std::string> args = GitUi::stageArgs(x.row, d->read.initial);
    const std::string top = d->read.top, name = nameOf(x.row.path);
    panel_->runOnGitThread([this, args, top, name, done, staged = x.row.staged, alive = alive_] {
        GitResult r = runGit(top, args);
        if (!*alive) return;
        const std::string said =
            done.empty() ? std::string(staged ? "Unstaged " : "Staged ") + name + "." : done;
        const std::string text =
            r.ok() ? said : (gitFailure(r).empty() ? std::string("git failed.") : gitFailure(r));
        const bool ok = r.ok();
        onMain(alive, this, [text, ok](ChangeReviewPage* p) { p->finish(text, ok); });
    });
}

void ChangeReviewPage::openFile(size_t fi) {
    if (!data_ || fi >= data_->read.diffs.size() || !onOpenFile) return;
    const FileDiff& d = data_->read.diffs[fi];
    int line = -1;
    for (const SideBySide::Row& r : d.rows)
        if (r.kind != SideBySide::Kind::Same && r.right >= 0) {
            line = r.right;
            break;
        }
    onOpenFile(data_->read.top + "/" + d.file.path, line < 0 ? 0 : line + 1);
}

void ChangeReviewPage::sideBySide(size_t fi) {
    if (!data_ || fi >= data_->read.diffs.size() || !onShowComparison) return;
    const ChangeReview::File& f = data_->read.diffs[fi].file;
    const bool staged = f.section == Section::Staged;
    CompareSpec c;
    c.title = nameOf(f.path) + " (side by side)";
    const std::string oldPath = f.origPath.empty() ? f.path : f.origPath;
    if (f.section == Section::Plain) {
        c.leftLabel = data_->baseLabel + ": " + oldPath;
        c.rightLabel = (branch_.empty() ? std::string("worktree") : branch_) + ": " + f.path;
    } else {
        c.leftLabel = (staged ? "HEAD: " : "Staged or HEAD: ") + oldPath;
        c.rightLabel = (staged ? "Staged: " : "") + f.path;
    }
    const FileDiff& fd = data_->read.diffs[fi];
    const bool sides = f.conflict && !fd.conflicts.empty();
    if (sides) {
        // A conflict's two sides: every block as the current side keeps it
        // against every block as the incoming side does.
        const MergeConflicts::Block& b0 = fd.conflicts[0];
        c.title = nameOf(f.path) + " (current and incoming)";
        c.leftLabel = "Current" + (b0.currentLabel.empty() ? "" : " (" + b0.currentLabel + ")") +
                      ": " + f.path;
        c.rightLabel = "Incoming" +
                       (b0.incomingLabel.empty() ? "" : " (" + b0.incomingLabel + ")") + ": " +
                       f.path;
        c.leftText = utf8(MergeConflicts::resolvedAll(f.newText, fd.conflicts,
                                                      MergeConflicts::Keep::Current));
        c.rightText = utf8(MergeConflicts::resolvedAll(f.newText, fd.conflicts,
                                                       MergeConflicts::Keep::Incoming));
    } else {
        c.leftText = utf8(f.oldText);
        c.rightText = utf8(f.newText);
    }
    c.fileName = f.path;
    // (Neither side of a conflict's comparison is the file on disk.)
    if ((f.section == Section::Changes && !sides) || (f.section == Section::Plain && f.newExists))
        c.path = data_->read.top + "/" + f.path;
    onShowComparison(c);
}

// Mark Resolved: a conflicted file with no blocks left staged as it is,
// which is what tells git its conflict is resolved (the panel's question
// about conflict marks is not needed: none are left).
void ChangeReviewPage::markResolved(size_t fi) {
    if (!data_ || fi >= data_->read.diffs.size() ||
        !ChangeReview::canMarkResolved(data_->read.diffs[fi])) {
        gtk_widget_error_bell(root_);
        return;
    }
    stageFile(fi, "Marked " + nameOf(data_->read.diffs[fi].file.path) + " resolved.");
}

// Keep Current, Keep Incoming or Keep Both on a conflict: the work tree's
// file with that block replaced, written through the atomic writer once it
// is checked to be as read (a tab open on it follows the disk as one undo
// step). A tab with unsaved text for it is left alone.
void ChangeReviewPage::keepConflict(MergeConflicts::Keep keep, size_t fi, long block) {
    auto d = data_;
    if (!d || fi >= d->read.diffs.size()) return;
    const FileDiff& fd = d->read.diffs[fi];
    if (block < 0 || (size_t)block >= fd.conflicts.size()) {
        gtk_widget_error_bell(root_);
        return;
    }
    const ChangeReview::Extra x = d->read.extras[fi];
    const std::string rel = x.row.path;
    const std::string path = d->read.top + "/" + rel;
    const std::string name = nameOf(path);
    if (hasUnsavedText && hasUnsavedText(path)) {
        finish(name + " has unsaved changes in a tab: resolve the conflict there, or save the tab "
                      "first.",
               false);
        return;
    }
    const std::u16string text = ChangeReview::conflictResult(fd, (size_t)block, keep);
    const std::string bytes = FolderSearch::encodeFileText(utf8(text), x.workEnc, x.workBom);
    const size_t left = fd.conflicts.size() - 1;
    const std::string done = left == 0
                                 ? "No conflicts left in " + name + ". Mark Resolved stages it."
                                 : MergeConflicts::resolvedNote(name, left);
    const std::string read = x.workBytes;
    panel_->runOnGitThread([this, path, rel, name, bytes, read, done, alive = alive_] {
        std::string failure, now, error;
        FolderSearch::FileStamp stamp;
        if (FolderSearch::readFileStable(path, ChangeReview::kMaxSideBytes, &now, &stamp) !=
                FolderSearch::ReadStatus::Ok ||
            now != read)
            failure = ChangeReview::staleText(rel, false);
        else if (!FolderSearch::writeFileAtomically(path, bytes, &error))
            failure = name + " could not be written (" + error + ").";
        if (!*alive) return;
        onMain(alive, this, [failure, done](ChangeReviewPage* p) {
            p->finish(failure.empty() ? done : failure, failure.empty());
        });
    });
}


void ChangeReviewPage::undoDiscard() {
    panel_->undoDiscard();
}

// ---------------------------------------------------------- for the checks
bool ChangeReviewPage::shows(const std::string& title) const {
    return pageText().find(title) != std::string::npos;
}

std::string ChangeReviewPage::pageText() const {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(buffer_, &s, &e);
    // Without the buttons' anchors (U+FFFC).
    char* t = gtk_text_buffer_get_text(buffer_, &s, &e, FALSE);
    std::string out = t;
    g_free(t);
    return out;
}

std::string ChangeReviewPage::headerDetail() const { return gtk_label_get_text(GTK_LABEL(summary_)); }
std::string ChangeReviewPage::messageText() const { return gtk_label_get_text(GTK_LABEL(messageLabel_)); }
size_t ChangeReviewPage::blockCount() const { return data_ ? data_->page.blockRows.size() : 0; }
size_t ChangeReviewPage::fileCount() const {
    return data_ ? ChangeReview::fileCount(data_->read.diffs) : 0;
}
size_t ChangeReviewPage::rowCount() const { return data_ ? data_->page.rows.size() : 0; }

std::vector<std::string> ChangeReviewPage::buttonsOfRow(size_t row) const {
    std::vector<std::string> out;
    for (const Button& b : buttons_)
        if (b.row == row) out.push_back(b.title);
    return out;
}

long ChangeReviewPage::rowOfBlock(size_t block) const {
    return data_ && block < data_->page.blockRows.size() ? (long)data_->page.blockRows[block] : -1;
}

long ChangeReviewPage::rowOfFile(const std::string& path, bool staged) const {
    if (!data_) return -1;
    for (size_t i = 0; i < data_->page.rows.size(); i++) {
        const PageRow& r = data_->page.rows[i];
        if (r.kind != RowKind::File || r.file >= data_->read.diffs.size()) continue;
        const ChangeReview::File& f = data_->read.diffs[r.file].file;
        if (f.path == path && (f.section == Section::Staged) == staged) return (long)i;
    }
    return -1;
}

std::string ChangeReviewPage::rowText(size_t row) const {
    if (!data_ || row >= data_->page.rows.size() || row >= rowChar_.size()) return "";
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buffer_, &a, rowChar_[row]);
    b = a;
    if (!gtk_text_iter_ends_line(&b)) gtk_text_iter_forward_to_line_end(&b);
    char* t = gtk_text_buffer_get_text(buffer_, &a, &b, FALSE);
    std::string out = t;
    g_free(t);
    return out;
}

std::string ChangeReviewPage::rowTint(size_t row) const {
    return row < tints_.size() ? tints_[row] : std::string();
}

int ChangeReviewPage::rowBlock(size_t row) const {
    return data_ && row < data_->page.rows.size() ? data_->page.rows[row].block : -1;
}
