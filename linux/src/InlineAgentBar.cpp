// InlineAgentBar.cpp — see InlineAgentBar.h.
#include "InlineAgentBar.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include "Document.h"
#include "Editor.h"
#include "FormatRun.h"
#include "Lsp.h"
#include "LspClient.h"
#include "MultiEdit.h"

namespace {

std::set<InlineAgentBar*>& alive() {
    static std::set<InlineAgentBar*> s;
    return s;
}

void installCss() {
    static bool done = false;
    if (done) return;
    done = true;
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        p, ".inline-agent { background: @theme_bg_color; border: 1px solid #4EA1F7;"
           " border-radius: 6px; padding: 6px 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.35); }"
           ".inline-agent .inline-agent-text { font-family: monospace; }"
           ".inline-agent .inline-agent-hint { color: #9CA3AF; font-size: 85%; }"
           ".inline-agent .inline-agent-error { color: #F14C4C; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

size_t bytesBefore(const std::u16string& u, size_t units) {
    return Lsp::toUtf8(u.substr(0, std::min(units, u.size()))).size();
}
std::string trimmed(std::string s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

void childSetup(gpointer) {
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, nullptr);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
}

}  // namespace

struct InlineAgentBar::Run {
    std::mutex m;
    std::string preview, activity;
    bool changed = false;
    std::atomic<bool> cancel{false};
};

InlineAgentBar::InlineAgentBar(Editor* editor) : ed_(editor) { alive().insert(this); }

InlineAgentBar::~InlineAgentBar() {
    alive().erase(this);
    generation_++;
    cancelRun();
    if (pollTimer_) g_source_remove(pollTimer_);
    pollTimer_ = 0;
    clearTint();
    if (adjustment_) {
        if (scrollHandler_) g_signal_handler_disconnect(adjustment_, scrollHandler_);
        g_object_unref(adjustment_);
    }
    adjustment_ = nullptr;
    box_ = nullptr;   // hangs on the editor's overlay, which goes with the editor
}

void InlineAgentBar::build() {
    if (box_) return;
    installCss();
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    g_object_ref_sink(box_);
    gtk_widget_add_css_class(box_, "inline-agent");
    gtk_widget_set_halign(box_, GTK_ALIGN_START);
    gtk_widget_set_valign(box_, GTK_ALIGN_START);
    gtk_widget_set_margin_start(box_, 56);
    gtk_widget_set_visible(box_, FALSE);
    entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_), "Ask Claude to change this file");
    gtk_widget_set_size_request(entry_, 420, -1);
    g_signal_connect_swapped(entry_, "activate", G_CALLBACK(+[](InlineAgentBar* s) { s->submit(); }),
                             this);
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onKeyPressed), this);
    gtk_widget_add_controller(entry_, keys);
    label_ = gtk_label_new("");
    gtk_widget_add_css_class(label_, "inline-agent-text");
    gtk_label_set_xalign(GTK_LABEL(label_), 0.0);
    gtk_label_set_wrap(GTK_LABEL(label_), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label_), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label_), 90);
    gtk_label_set_selectable(GTK_LABEL(label_), FALSE);
    gtk_widget_set_visible(label_, FALSE);
    hint_ = gtk_label_new("");
    gtk_widget_add_css_class(hint_, "inline-agent-hint");
    gtk_label_set_xalign(GTK_LABEL(hint_), 0.0);
    gtk_box_append(GTK_BOX(box_), entry_);
    gtk_box_append(GTK_BOX(box_), label_);
    gtk_box_append(GTK_BOX(box_), hint_);
    ed_->addOverlay(box_);
}

static gboolean keyEscape(guint key, GdkModifierType mods) {
    return key == GDK_KEY_Escape && !(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK));
}

void InlineAgentBar::onKeyPressed(GtkEventControllerKey*, guint key, guint, GdkModifierType mods,
                                  gpointer self) {
    InlineAgentBar* s = static_cast<InlineAgentBar*>(self);
    if (!keyEscape(key, mods)) return;
    if (s->mode_ == Mode::Working) {
        // Stops the run; a second Esc closes.
        s->generation_++;
        s->cancelRun();
        s->setMode(Mode::Message);
        s->say("Stopped.", false);
        return;
    }
    if (s->mode_ == Mode::Pending) s->drop();
    s->close();
}

void InlineAgentBar::focusText() {
    if (GtkWidget* v = ed_->textView()) gtk_widget_grab_focus(v);
}

bool InlineAgentBar::open() {
    if (!ed_->active() || !ed_->isSource()) return false;
    if (mode_ != Mode::Closed && doc_ != ed_->active()) close();
    build();
    if (mode_ == Mode::Closed) {
        doc_ = ed_->active();
        setMode(Mode::Input);
        gtk_editable_set_text(GTK_EDITABLE(entry_), "");
        gtk_widget_set_visible(label_, FALSE);
        message_.clear();
        gtk_widget_set_visible(box_, TRUE);
        GtkTextView* view = GTK_TEXT_VIEW(ed_->textView());
        adjustment_ = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
        if (adjustment_) {
            g_object_ref(adjustment_);
            scrollHandler_ = g_signal_connect_swapped(
                adjustment_, "value-changed", G_CALLBACK(+[](InlineAgentBar* s) { s->reposition(); }),
                this);
        }
        reposition();
    }
    gtk_widget_grab_focus(entry_);
    return true;
}

// Under the caret's line: only the caret's own line is asked for, so the
// rest of the file is not laid out.
void InlineAgentBar::reposition() {
    if (!box_ || mode_ == Mode::Closed) return;
    GtkTextView* view = GTK_TEXT_VIEW(ed_->textView());
    GtkTextBuffer* b = gtk_text_view_get_buffer(view);
    GtkTextIter caret;
    gtk_text_buffer_get_iter_at_mark(b, &caret, gtk_text_buffer_get_insert(b));
    GdkRectangle r;
    gtk_text_view_get_iter_location(view, &caret, &r);
    int x, y;
    gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &x, &y);
    int minH = 0, natH = 0;
    gtk_widget_measure(box_, GTK_ORIENTATION_VERTICAL, -1, &minH, &natH, nullptr, nullptr);
    int top = y + r.height + 4;
    const int viewH = gtk_widget_get_height(GTK_WIDGET(view));
    if (viewH > natH) top = std::max(2, std::min(top, viewH - natH - 2));
    else top = std::max(2, top);
    gtk_widget_set_margin_top(box_, top);
}

void InlineAgentBar::setMode(Mode m) {
    mode_ = m;
    const char* hint = "";
    switch (m) {
        case Mode::Input: hint = "Return asks, Esc closes"; break;
        case Mode::Working: hint = "Esc stops"; break;
        case Mode::Pending: hint = "Return keeps the change, Esc drops it, or ask for more"; break;
        case Mode::Message: hint = "Return asks again, Esc closes"; break;
        case Mode::Closed: break;
    }
    if (hint_) gtk_label_set_text(GTK_LABEL(hint_), hint);
    reposition();
}

void InlineAgentBar::say(const std::string& text, bool error) {
    message_ = text;
    messageError_ = error;
    diffText_.clear();
    gtk_label_set_text(GTK_LABEL(label_), text.c_str());
    if (error) gtk_widget_add_css_class(label_, "inline-agent-error");
    else gtk_widget_remove_css_class(label_, "inline-agent-error");
    gtk_widget_set_visible(label_, !text.empty());
    reposition();
}

int InlineAgentBar::tintedLines() const {
    if (!tintBuffer_ || !tintTag_) return 0;
    int n = 0;
    const int lines = gtk_text_buffer_get_line_count(tintBuffer_);
    for (int i = 0; i < lines; i++) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(tintBuffer_, &it, i);
        if (gtk_text_iter_has_tag(&it, tintTag_)) n++;
    }
    return n;
}

void InlineAgentBar::clearTint() {
    if (tintBuffer_) {
        if (tintTag_) {
            GtkTextIter a, b;
            gtk_text_buffer_get_bounds(tintBuffer_, &a, &b);
            gtk_text_buffer_remove_tag(tintBuffer_, tintTag_, &a, &b);
        }
        g_object_unref(tintBuffer_);
    }
    tintBuffer_ = nullptr;
    tintTag_ = nullptr;
}

// The lines the pending edit replaces, tinted red in the buffer (a tag: never
// text, never a step in the undo history).
void InlineAgentBar::tint(const std::u16string&, const std::vector<InlineAgent::DiffLine>& diff) {
    clearTint();
    GtkTextBuffer* b = ed_->sourceBuffer();
    if (!b) return;
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(b);
    GtkTextTag* tag = gtk_text_tag_table_lookup(table, "inline-agent-replaced");
    if (!tag)
        tag = gtk_text_buffer_create_tag(b, "inline-agent-replaced", "background", "rgba(241,76,76,0.28)",
                                         "paragraph-background", "rgba(241,76,76,0.28)", nullptr);
    tintBuffer_ = GTK_TEXT_BUFFER(g_object_ref(b));
    tintTag_ = tag;
    for (const InlineAgent::DiffLine& d : diff) {
        if (d.kind != InlineAgent::DiffLine::Removed || d.oldLine < 1) continue;
        GtkTextIter a, e;
        gtk_text_buffer_get_iter_at_line(b, &a, d.oldLine - 1);
        e = a;
        if (!gtk_text_iter_ends_line(&e)) gtk_text_iter_forward_to_line_end(&e);
        gtk_text_buffer_apply_tag(b, tag, &a, &e);
    }
}

void InlineAgentBar::close() {
    if (mode_ == Mode::Closed) return;
    const bool waiting = mode_ == Mode::Pending && !rounds_.empty();
    generation_++;
    cancelRun();
    clearTint();
    rounds_.clear();
    requests_.clear();
    if (adjustment_) {
        if (scrollHandler_) g_signal_handler_disconnect(adjustment_, scrollHandler_);
        g_object_unref(adjustment_);
    }
    adjustment_ = nullptr;
    scrollHandler_ = 0;
    const bool hadFocus = box_ && gtk_widget_get_visible(box_) && gtk_widget_is_ancestor(
                              gtk_window_get_focus(GTK_WINDOW(gtk_widget_get_root(box_))), box_);
    mode_ = Mode::Closed;
    if (box_) gtk_widget_set_visible(box_, FALSE);
    doc_ = nullptr;
    if (waiting && note) note("The agent's edit was not made: its bar closed before Keep");
    if (hadFocus) focusText();
}

void InlineAgentBar::drop() {
    clearTint();
    rounds_.clear();
    requests_.clear();
    diffText_.clear();
}

void InlineAgentBar::cancelRun() {
    if (run_) run_->cancel = true;
    run_.reset();
    if (pollTimer_) g_source_remove(pollTimer_);
    pollTimer_ = 0;
}

// ---------------------------------------------------------------- asking

void InlineAgentBar::submit() {
    if (mode_ == Mode::Working || mode_ == Mode::Closed) return;
    const std::string request = trimmed(gtk_editable_get_text(GTK_EDITABLE(entry_)));
    if (request.empty()) {
        if (mode_ == Mode::Pending) keep();
        return;
    }
    if (doc_ != ed_->active()) return;
    InlineAgent::EditRequest r;
    r.request = request;
    r.earlier = requests_;
    const std::string& path = ed_->currentPath();
    const std::string root = folder ? folder() : std::string();
    r.path = !root.empty() && path.compare(0, root.size() + 1, root + "/") == 0
                 ? path.substr(root.size() + 1)
                 : (path.empty() ? "untitled" : g_path_get_basename(path.c_str()));
    std::u16string now;
    size_t a = 0, b = 0;
    if (!ed_->sourceText(now, a, b)) return;
    askedBase_ = now;
    askedBaseA_ = a;
    askedBaseB_ = b;
    if (!rounds_.empty()) {   // builds on the edit waiting
        askedText_ = pendingText_;
        askedA_ = pendA_;
        askedB_ = pendB_;
    } else {
        askedText_ = Lsp::toUtf8(now);
        askedA_ = bytesBefore(now, a);
        askedB_ = bytesBefore(now, b);
    }
    r.text = askedText_;
    r.selStart = askedA_;
    r.selEnd = askedB_;
    asking_ = request;
    gtk_editable_set_text(GTK_EDITABLE(entry_), "");
    setMode(Mode::Working);
    say("Working…", false);
    start(InlineAgent::editPrompt(r));
}

void InlineAgentBar::start(const std::string& prompt) {
    const std::string cmd = command ? command() : std::string("claude");
    const char* sh = g_getenv("SHELL");
    const std::string shell = sh && *sh ? sh : "/bin/sh";
    // A login shell, so the PATH the user's profile sets finds claude; exec,
    // so the task is Claude itself and stopping it stops Claude.
    const std::string line = "exec " + cmd + " " + InlineAgent::kPrintFlags;
    lastPrompt_ = prompt;
    lastCommand_ = line;
    std::string cwd = folder ? folder() : std::string();
    if (cwd.empty() || !g_file_test(cwd.c_str(), G_FILE_TEST_IS_DIR)) cwd = g_get_home_dir();
    std::string path;
    for (const std::string& d : LspSearchDirs()) path += d + ":";
    path += g_getenv("PATH") ? g_getenv("PATH") : "/usr/bin:/bin";
    gchar** env = g_environ_setenv(g_get_environ(), "NO_COLOR", "1", TRUE);
    env = g_environ_setenv(env, "PATH", path.c_str(), TRUE);
    std::vector<std::string> environ;
    for (gchar** e = env; e && *e; e++) environ.push_back(*e);
    g_strfreev(env);

    auto run = std::make_shared<Run>();
    run_ = run;
    const unsigned gen = ++generation_;
    InlineAgentBar* self = this;
    pollTimer_ = g_timeout_add(80, +[](gpointer p) -> gboolean {
        static_cast<InlineAgentBar*>(p)->pollPreview();
        return G_SOURCE_CONTINUE;
    }, this);

    std::thread([run, shell, line, cwd, environ, prompt, self, gen]() {
        struct Done {
            InlineAgentBar* bar;
            unsigned gen;
            std::string reply, error;
            bool notFound;
        };
        auto post = [&](std::string reply, std::string error, bool notFound) {
            g_idle_add(+[](gpointer p) -> gboolean {
                std::unique_ptr<Done> d(static_cast<Done*>(p));
                if (alive().count(d->bar) && d->bar->generation_ == d->gen)
                    d->bar->ended(d->reply, d->error, d->notFound);
                return G_SOURCE_REMOVE;
            }, new Done{self, gen, std::move(reply), std::move(error), notFound});
        };
        std::vector<const char*> av = {shell.c_str(), "-l", "-c", line.c_str(), nullptr};
        std::vector<const char*> ev;
        for (const std::string& e : environ) ev.push_back(e.c_str());
        ev.push_back(nullptr);
        GPid pid = 0;
        int inFd = -1, outFd = -1, errFd = -1;
        GError* gerr = nullptr;
        if (!g_spawn_async_with_pipes_and_fds(cwd.c_str(), av.data(), ev.data(),
                                              G_SPAWN_DO_NOT_REAP_CHILD, childSetup, nullptr, -1, -1,
                                              -1, nullptr, nullptr, 0, &pid, &inFd, &outFd, &errFd,
                                              &gerr)) {
            post("", std::string("Could not start the agent: ") + (gerr ? gerr->message : ""), false);
            g_clear_error(&gerr);
            return;
        }
        for (int fd : {inFd, outFd, errFd}) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        InlineAgent::Stream stream;
        std::string err;
        size_t written = 0;
        bool timedOut = false, outOpen = true, errOpen = true;
        const double deadline = nowSeconds() + 300;
        double lastPreview = 0;
        signal(SIGPIPE, SIG_IGN);
        while (outOpen || errOpen) {
            if (run->cancel) {
                kill(pid, SIGKILL);
                break;
            }
            if (nowSeconds() > deadline) {
                timedOut = true;
                kill(pid, SIGKILL);
                break;
            }
            struct pollfd fds[3];
            int n = 0;
            int iIn = -1, iOut = -1, iErr = -1;
            if (inFd >= 0) { iIn = n; fds[n++] = {inFd, POLLOUT, 0}; }
            if (outOpen) { iOut = n; fds[n++] = {outFd, POLLIN, 0}; }
            if (errOpen) { iErr = n; fds[n++] = {errFd, POLLIN, 0}; }
            poll(fds, (nfds_t)n, 50);
            if (iIn >= 0 && (fds[iIn].revents & (POLLOUT | POLLERR | POLLHUP))) {
                const ssize_t w = write(inFd, prompt.data() + written, prompt.size() - written);
                if (w > 0) written += (size_t)w;
                if (written >= prompt.size() || (w < 0 && errno != EAGAIN)) {
                    ::close(inFd);
                    inFd = -1;
                }
            }
            char buf[16384];
            if (iOut >= 0 && fds[iOut].revents) {
                const ssize_t r = read(outFd, buf, sizeof buf);
                if (r > 0) {
                    const bool changed = stream.feed(std::string(buf, (size_t)r));
                    const double t = nowSeconds();
                    if (changed && t - lastPreview >= 0.08) {
                        lastPreview = t;
                        std::lock_guard<std::mutex> lock(run->m);
                        run->preview = InlineAgent::preview(stream.text());
                        run->activity = stream.activity();
                        run->changed = true;
                    }
                } else if (r == 0 || errno != EAGAIN) {
                    outOpen = false;
                }
            }
            if (iErr >= 0 && fds[iErr].revents) {
                const ssize_t r = read(errFd, buf, sizeof buf);
                if (r > 0) {
                    if (err.size() < 65536) err.append(buf, (size_t)r);
                } else if (r == 0 || errno != EAGAIN) {
                    errOpen = false;
                }
            }
        }
        if (inFd >= 0) ::close(inFd);
        ::close(outFd);
        ::close(errFd);
        int status = 0;
        for (int i = 0; i < 200; i++) {   // a second, then SIGKILL
            if (waitpid(pid, &status, WNOHANG) == pid) break;
            if (i == 100) kill(pid, SIGKILL);
            g_usleep(10000);
            if (i == 199) waitpid(pid, &status, 0);
        }
        g_spawn_close_pid(pid);
        if (run->cancel) return;
        stream.finish();
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (timedOut) {
            post("", "Claude Code did not answer within five minutes, so it was stopped.", false);
        } else if (code == 127 || err.find("command not found") != std::string::npos) {
            post("", "", true);
        } else if (code != 0 || stream.failed()) {
            std::string reason = trimmed(err);
            if (stream.sawJson() && !stream.error().empty()) reason = stream.error();
            if (reason.empty() && !stream.sawJson()) reason = trimmed(stream.reply());
            size_t cut = reason.size(), lines = 0;
            while (cut > 0 && lines < 6) {
                const size_t p = reason.rfind('\n', cut - 1);
                if (p == std::string::npos) { cut = 0; break; }
                cut = p;
                lines++;
            }
            if (cut > 0) reason = reason.substr(cut + 1);
            post("", reason.empty() ? "Claude Code stopped with status " + std::to_string(code) + "."
                                    : reason, false);
        } else {
            post(stream.reply(), "", false);
        }
    }).detach();
}

void InlineAgentBar::pollPreview() {
    if (!run_ || mode_ != Mode::Working) return;
    std::string text, activity;
    {
        std::lock_guard<std::mutex> lock(run_->m);
        if (!run_->changed) return;
        run_->changed = false;
        text = run_->preview;
        activity = run_->activity;
    }
    previews_++;
    if (previewLog_.size() < 200) {
        previewLog_.push_back(text);
        if (!activity.empty()) previewLog_.push_back(activity);
    }
    say(text.empty() ? (activity.empty() ? "Working…" : activity) : text, false);
}

void InlineAgentBar::ended(const std::string& reply, const std::string& error, bool notFound) {
    run_.reset();
    if (pollTimer_) g_source_remove(pollTimer_);
    pollTimer_ = 0;
    const bool had = !rounds_.empty();
    if (notFound || !error.empty()) {
        setMode(had ? Mode::Pending : Mode::Message);
        const std::string cmd = command ? command() : std::string("claude");
        say(notFound ? "Claude Code (" + cmd + ") was not found. Install it from "
                       "claude.com/claude-code, or name the command in the settings file's "
                       "agent.command."
                     : error, true);
        if (!had) gtk_widget_grab_focus(entry_);
        return;
    }
    const InlineAgent::Reply parsed = InlineAgent::parseReply(reply);
    if (parsed.edits.empty()) {
        setMode(had ? Mode::Pending : Mode::Message);
        say(parsed.answer.empty() ? "Claude Code gave no answer." : parsed.answer, false);
        return;
    }
    const InlineAgent::Applied ap = InlineAgent::applyEdits(askedText_, parsed.edits, askedA_, askedB_);
    if (!ap.ok) {
        setMode(had ? Mode::Pending : Mode::Message);
        say("The edit could not be applied: " + ap.error, true);
        return;
    }
    if (!had) {
        base_ = askedBase_;
        baseA_ = askedBaseA_;
        baseB_ = askedBaseB_;
    }
    rounds_.push_back(parsed.edits);
    requests_.push_back(asking_);
    pendingText_ = ap.text;
    pendA_ = ap.selStart;
    pendB_ = ap.selEnd;
    pending_ = Lsp::toUtf16(ap.text);
    // The buffer may have changed while Claude thought: make the blocks again
    // on it, as Keep would, and show that.
    std::u16string now;
    size_t a = 0, b = 0;
    if (ed_->sourceText(now, a, b) && now != base_) {
        std::string text, error2;
        size_t sa = 0, sb = 0;
        const size_t movedA = InlineAgent::moveOffset(base_, now, baseA_);
        const size_t movedB = InlineAgent::moveOffset(base_, now, baseB_);
        if (makeRounds(Lsp::toUtf8(now), bytesBefore(now, movedA), bytesBefore(now, movedB), &text,
                       &sa, &sb, &error2)) {
            base_ = now;
            baseA_ = movedA;
            baseB_ = movedB;
            pendingText_ = text;
            pendA_ = sa;
            pendB_ = sb;
            pending_ = Lsp::toUtf16(text);
        } else {
            drop();
            setMode(Mode::Message);
            say("The edit no longer fits the file: " + error2, true);
            return;
        }
    }
    showPending(now);
}

bool InlineAgentBar::makeRounds(const std::string& from, size_t selStart, size_t selEnd,
                                std::string* text, size_t* selA, size_t* selB,
                                std::string* error) const {
    std::string cur = from;
    size_t a = selStart, b = selEnd;
    for (const std::vector<InlineAgent::Edit>& edits : rounds_) {
        const InlineAgent::Applied ap = InlineAgent::applyEdits(cur, edits, a, b);
        if (!ap.ok) {
            *error = ap.error;
            return false;
        }
        cur = ap.text;
        a = ap.selStart;
        b = ap.selEnd;
    }
    *text = cur;
    *selA = a;
    *selB = b;
    return true;
}

// The pending edit as a diff against the buffer as it is, the replaced lines
// tinted.
void InlineAgentBar::showPending(const std::u16string& now) {
    const std::vector<InlineAgent::DiffLine> diff = InlineAgent::pendingDiff(now, pending_);
    const size_t n = InlineAgent::changedLines(now, pending_);
    std::string plain, markup;
    size_t shown = 0;
    for (const InlineAgent::DiffLine& d : diff) {
        if (shown >= 18) {
            plain += "…\n";
            markup += "…\n";
            break;
        }
        shown++;
        const std::string t = Lsp::toUtf8(d.text);
        gchar* esc = g_markup_escape_text(t.c_str(), -1);
        switch (d.kind) {
            case InlineAgent::DiffLine::Removed:
                plain += "- " + t + "\n";
                markup += std::string("<span background=\"#5a1d1d\">- ") + esc + "</span>\n";
                break;
            case InlineAgent::DiffLine::Added:
                plain += "+ " + t + "\n";
                markup += std::string("<span background=\"#1d4a2a\">+ ") + esc + "</span>\n";
                break;
            case InlineAgent::DiffLine::Gap:
                plain += "  … " + std::to_string(d.more) + " lines\n";
                markup += "  … " + std::to_string(d.more) + " lines\n";
                break;
            default:
                plain += "  " + t + "\n";
                markup += std::string("  ") + esc + "\n";
        }
        g_free(esc);
    }
    if (!plain.empty()) { plain.pop_back(); markup.pop_back(); }
    const std::string head = n == 0 ? "The agent's change makes no difference, not made yet:"
                                    : "The agent's change, " + std::to_string(n) + " line" +
                                          (n == 1 ? "" : "s") + ", not made yet:";
    message_ = head;
    messageError_ = false;
    diffText_ = plain;
    gtk_label_set_markup(GTK_LABEL(label_),
                         (std::string(g_markup_escape_text(head.c_str(), -1)) + "\n" + markup).c_str());
    gtk_widget_remove_css_class(label_, "inline-agent-error");
    gtk_widget_set_visible(label_, TRUE);
    tint(now, diff);
    setMode(Mode::Pending);
    gtk_widget_grab_focus(entry_);
}

// ------------------------------------------------------------------ Keep

void InlineAgentBar::keep() {
    if (mode_ != Mode::Pending || rounds_.empty() || doc_ != ed_->active()) return;
    std::u16string now;
    size_t a = 0, b = 0;
    if (!ed_->sourceText(now, a, b)) return;
    const size_t movedA = InlineAgent::moveOffset(base_, now, baseA_);
    const size_t movedB = InlineAgent::moveOffset(base_, now, baseB_);
    std::string text, error;
    size_t sa = 0, sb = 0;
    if (!makeRounds(Lsp::toUtf8(now), bytesBefore(now, movedA), bytesBefore(now, movedB), &text, &sa,
                    &sb, &error)) {
        drop();
        setMode(Mode::Message);
        say("The edit no longer fits the file: " + error, true);
        return;
    }
    const std::u16string next = Lsp::toUtf16(text);
    if (!InlineAgent::sameChange(base_, pending_, now, next)) {
        // Not the change that was shown: show this one and wait for Return.
        base_ = now;
        baseA_ = movedA;
        baseB_ = movedB;
        pendingText_ = text;
        pendA_ = sa;
        pendB_ = sb;
        pending_ = next;
        showPending(now);
        return;
    }
    clearTint();
    if (!applyToBuffer(text)) {
        drop();
        setMode(Mode::Message);
        say("The file could not be changed here.", true);
        return;
    }
    rounds_.clear();
    requests_.clear();
    close();
}

// The new text in place of the buffer's as the line runs that changed, one
// user action (Formatting::apply's way): one Ctrl+Z takes it back.
bool InlineAgentBar::applyToBuffer(const std::string& after) {
    GtkTextBuffer* b = ed_->sourceBuffer();
    const std::string before = ed_->text();
    const FormatRun::Replacement r(before, after);
    if (r.empty()) return true;
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_mark(b, &ins, gtk_text_buffer_get_insert(b));
    gtk_text_buffer_get_iter_at_mark(b, &bound, gtk_text_buffer_get_selection_bound(b));
    const size_t insAt = (size_t)gtk_text_iter_get_offset(&ins);
    const size_t boundAt = (size_t)gtk_text_iter_get_offset(&bound);
    std::vector<MultiEdit::Change> changes;
    for (const FormatRun::Replacement::Change& c : r.changes())
        changes.push_back(MultiEdit::Change{c.start, c.length, c.text});
    if (!MultiEdit::apply(b, changes, false)) return false;
    const size_t newIns = r.map(insAt);
    const size_t newBound = insAt == boundAt ? newIns : r.map(boundAt);
    gtk_text_buffer_get_iter_at_offset(b, &ins, (int)newIns);
    gtk_text_buffer_get_iter_at_offset(b, &bound, (int)newBound);
    gtk_text_buffer_select_range(b, &ins, &bound);
    return true;
}
