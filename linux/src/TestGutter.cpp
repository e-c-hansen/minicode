// TestGutter.cpp — see TestGutter.h. The Mac's mac/src/TestGutter.mm, step
// for step where GTK allows.
#include "TestGutter.h"

#include "CodeView.h"
#include "DocumentView.h"
#include "Editor.h"
#include "Gutter.h"

#include <algorithm>
#include <cctype>

// A command typed for some tests, waiting for the command it typed to end.
struct TestGutter::Run {
    int id = 0;
    std::string path;
    std::string line;                 // the test command, without a cd before it
    std::vector<std::string> labels;
    bool typed = false;               // where is set
    Typed where;
};

namespace {

// From the worker back to the main loop, if the gutter still lives.
void deliver(std::shared_ptr<bool> alive, std::function<void()> fn) {
    struct Delivery {
        std::shared_ptr<bool> alive;
        std::function<void()> fn;
    };
    auto* d = new Delivery{std::move(alive), std::move(fn)};
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        auto* d = static_cast<Delivery*>(p);
        if (*d->alive) d->fn();
        delete d;
        return G_SOURCE_REMOVE;
    }, d, nullptr);
}

void runJob(gpointer data, gpointer) {
    auto* job = static_cast<std::function<void()>*>(data);
    (*job)();
    delete job;
}

std::string tipFor(const std::string& label, TestState state) {
    switch (state) {
    case TestState::Idle: return "Run " + label;
    case TestState::Running: return label + " is running";
    case TestState::Passed: return label + " passed. Click to run it again.";
    case TestState::Failed: return label + " failed. Click to run it again.";
    }
    return label;
}

}  // namespace

// ==================================================================== life
TestGutter::TestGutter(Editor* editor) : editor_(editor), view_(editor->textView()) {
    alive_ = std::make_shared<bool>(true);
    pool_ = g_thread_pool_new(runJob, nullptr, 1, FALSE, nullptr);
    g_signal_connect_swapped(view_, "notify::buffer", G_CALLBACK(+[](TestGutter* self) {
        self->attachBuffer();
    }), this);
    // The view's editable flag says whether it shows the source (a preview
    // or a message is read-only).
    g_signal_connect_swapped(view_, "notify::editable", G_CALLBACK(+[](TestGutter* self) {
        self->refreshSoon();
    }), this);
    if (CodeView* cv = editor_->codeView())
        if (Gutter* g = cv->gutter()) g->onTest = [this](int index) { runTestAtIndex(index); };
    attachBuffer();
}

TestGutter::~TestGutter() {
    *alive_ = false;
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    detachBuffer();
    g_signal_handlers_disconnect_by_data(view_, this);
    if (CodeView* cv = editor_->codeView())
        if (Gutter* g = cv->gutter()) g->onTest = nullptr;
    if (action_) {
        g_signal_handlers_disconnect_by_data(action_, this);
        g_object_unref(action_);
    }
    // A look already running finishes on the pool's thread; what it
    // delivers finds alive_ false and is dropped.
    g_thread_pool_free(pool_, FALSE, FALSE);
}

void TestGutter::post(std::function<void()> job) {
    g_thread_pool_push(pool_, new std::function<void()>(std::move(job)), nullptr);
}

void TestGutter::note(const std::string& text) {
    if (onNote) onNote(text);
}

void TestGutter::installActions(GActionMap* window) {
    if (action_) return;
    action_ = g_simple_action_new("runtestatcursor", nullptr);
    g_signal_connect_swapped(action_, "activate", G_CALLBACK(+[](TestGutter* self, GVariant*) {
        self->runTestAtCaret();
    }), this);
    g_action_map_add_action(window, G_ACTION(action_));
    updateAction();
}

void TestGutter::updateAction() {
    if (action_) g_simple_action_set_enabled(action_, canRunTestAtCaret());
}

// ================================================================ watching
GtkTextBuffer* TestGutter::viewBuffer() const {
    return gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
}

void TestGutter::attachBuffer() {
    GtkTextBuffer* b = viewBuffer();
    if (b == buffer_) return;
    detachBuffer();
    generation_++;
    if (b) {
        buffer_ = GTK_TEXT_BUFFER(g_object_ref(b));
        g_signal_connect_swapped(b, "changed", G_CALLBACK(+[](TestGutter* self) {
            self->edited();
        }), this);
    }
    updateAction();
    // Another tab in the view: its marks (kept with its buffer) are looked
    // at again at once.
    scheduleAfter(50);
}

void TestGutter::detachBuffer() {
    if (!buffer_) return;
    g_signal_handlers_disconnect_by_data(buffer_, this);
    g_object_unref(buffer_);
    buffer_ = nullptr;
}

// The file on show changed: what its tests' last runs said no longer holds,
// and runs still going will say nothing.
void TestGutter::edited() {
    generation_++;
    lastEdit_ = g_get_monotonic_time();
    const std::string path = pathToLook();
    const bool same = !path.empty() && path == shownPath_ && viewBuffer() == shownBuffer_;
    if (same) {
        results_.erase(path);
        runs_.erase(std::remove_if(runs_.begin(), runs_.end(),
                                   [&](const std::unique_ptr<Run>& r) { return r->path == path; }),
                    runs_.end());
        pauseWanted_ = true;
    }
    scheduleAfter(same ? 300 : 50);
}

void TestGutter::refreshSoon() { scheduleAfter(50); }

void TestGutter::scheduleAfter(int ms) {
    if (timer_) return;
    timer_ = g_timeout_add((guint)ms, [](gpointer p) -> gboolean {
        TestGutter* self = static_cast<TestGutter*>(p);
        self->timer_ = 0;
        // Still typing: wait for the pause (one timer, not one a key).
        const gint64 since = (g_get_monotonic_time() - self->lastEdit_) / 1000;
        if (self->pauseWanted_ && since < 300) {
            self->scheduleAfter((int)(300 - since) + 10);
            return G_SOURCE_REMOVE;
        }
        self->pauseWanted_ = false;
        self->look();
        return G_SOURCE_REMOVE;
    }, this);
}

// The file on show while its editable source is, "" otherwise (a preview,
// a diff, a message, a large file with its features off, one loading).
std::string TestGutter::pathToLook() const {
    Document* d = editor_->active();
    if (!d || !editor_->isSource() || editor_->isDiff() || d->path().empty() || d->loading() ||
        d->largeFileFeaturesOff() || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)))
        return std::string();
    return d->path();
}

// The kind of test file `path` is, None when its language's tests are off.
Runnables::Kind TestGutter::kindOf(const std::string& path) const {
    if (path.empty()) return Runnables::Kind::None;
    const Runnables::Kind kind = Runnables::kindFor(path);
    if (kind == Runnables::Kind::None) return kind;
    std::string v = templateFor ? templateFor(Runnables::languageOf(kind)) : std::string();
    for (char& c : v) c = (char)std::tolower((unsigned char)c);
    if (v == "off" || v == "none" || v == "false") return Runnables::Kind::None;
    return kind;
}

std::string TestGutter::bufferText(GtkTextBuffer* b) const {
    if (!b) return std::string();
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(b, &a, &z);
    char* t = gtk_text_buffer_get_text(b, &a, &z, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

// On the main loop: what to read; on the worker: the tests; back on the
// main loop, the marks, if nothing was typed meanwhile.
void TestGutter::look() {
    const unsigned gen = ++generation_;
    const std::string path = pathToLook();
    const Runnables::Kind kind = kindOf(path);
    GtkTextBuffer* buffer = viewBuffer();
    BufferLines* lines = BufferLines::of(buffer);
    if (kind == Runnables::Kind::None || !lines) {
        shownPath_ = path;
        shownBuffer_ = buffer;
        kind_ = Runnables::Kind::None;
        tests_.clear();
        if (lines) lines->setTests(false, {});
        looks_++;
        updateAction();
        return;
    }
    std::shared_ptr<bool> alive = alive_;
    auto text = std::make_shared<std::string>(bufferText(buffer));
    post([this, alive, text, kind, gen, path, buffer] {
        auto found = std::make_shared<std::vector<Runnables::Test>>(Runnables::find(*text, kind));
        deliver(alive, [this, found, kind, gen, path, buffer] {
            if (gen != generation_) return;   // typed or switched meanwhile: the next look
            if (path != pathToLook() || buffer != viewBuffer()) return;
            shownPath_ = path;
            shownBuffer_ = buffer;
            kind_ = kind;
            tests_ = *found;
            pushMarks();
            looks_++;
            updateAction();
        });
    });
}

std::string TestGutter::labelOf(const Runnables::Test& t) const { return t.label(kind_); }

void TestGutter::pushMarks() {
    if (shownBuffer_ != viewBuffer()) return;   // another tab: its look pushes them
    BufferLines* lines = BufferLines::of(shownBuffer_);
    if (!lines) return;
    auto said = results_.find(shownPath_);
    std::vector<GutterTest> marks;
    for (const Runnables::Test& t : tests_) {
        const std::string label = labelOf(t);
        GutterTest m;
        m.line = t.line;
        if (said != results_.end()) {
            auto it = said->second.find(label);
            if (it != said->second.end()) m.state = it->second;
        }
        m.tip = tipFor(label, m.state);
        marks.push_back(m);
    }
    lines->setTests(true, std::move(marks));
}

// ================================================================ running
bool TestGutter::runTestAtIndex(int index) {
    if (index < 0 || (size_t)index >= tests_.size()) {
        gtk_widget_error_bell(view_);
        return false;
    }
    return start({tests_[(size_t)index]}, false);
}

bool TestGutter::runTestAtCaret() {
    BufferLines* lines = BufferLines::of(viewBuffer());
    if (!canRunTestAtCaret() || !lines) {
        gtk_widget_error_bell(view_);
        note("No tests here to run");
        return false;
    }
    GtkTextIter caret;
    GtkTextBuffer* b = viewBuffer();
    gtk_text_buffer_get_iter_at_mark(b, &caret, gtk_text_buffer_get_insert(b));
    const int at = Runnables::testAt(tests_, lines->lineOf((size_t)gtk_text_iter_get_offset(&caret)));
    if (at < 0) return start(tests_, true);
    return start({tests_[(size_t)at]}, false);
}

bool TestGutter::canRunTestAtCaret() const {
    const std::string path = pathToLook();
    return kind_ != Runnables::Kind::None && !path.empty() && path == shownPath_ &&
           viewBuffer() == shownBuffer_ && !tests_.empty();
}

bool TestGutter::start(const std::vector<Runnables::Test>& tests, bool wholeFile) {
    const std::string path = pathToLook();
    if (path.empty() || path != shownPath_ || kind_ == Runnables::Kind::None) {
        gtk_widget_error_bell(view_);
        note("Open a test file to run its tests");
        return false;
    }
    Runnables::Request r;
    r.file = path;
    r.root = root ? root() : std::string();
    if (kind_ == Runnables::Kind::Js) r.text = bufferText(viewBuffer());
    if (kind_ == Runnables::Kind::Pytest && python) r.python = python(path);
    r.tmpl = templateFor ? templateFor(Runnables::languageOf(kind_)) : std::string();
    Runnables::Command c;
    std::string problem;
    if (!Runnables::command(r, kind_, tests, wholeFile, &c, &problem)) {
        gtk_widget_error_bell(view_);
        note(problem);
        return false;
    }
    if (!run) {
        gtk_widget_error_bell(view_);
        return false;
    }
    auto owned = std::make_unique<Run>();
    Run* rn = owned.get();
    rn->id = ++nextRun_;
    rn->path = path;
    rn->line = c.line;
    for (const Runnables::Test& t : tests) rn->labels.push_back(labelOf(t));
    runs_.push_back(std::move(owned));
    // Running, before the line goes: `typed` may come at once.
    std::map<std::string, TestState>& said = results_[path];
    for (const std::string& label : rn->labels) said[label] = TestState::Running;
    pushMarks();
    const int id = rn->id;
    std::shared_ptr<bool> alive = alive_;
    const bool going = run(c.line, c.dir, [this, alive, id](const Typed& where, const std::string& why) {
        if (!*alive) return;
        auto it = std::find_if(runs_.begin(), runs_.end(),
                               [id](const std::unique_ptr<Run>& r) { return r->id == id; });
        if (it == runs_.end()) return;   // the file was edited meanwhile
        if (!why.empty()) {
            note("Not run: " + why);
            forget(id);
            return;
        }
        (*it)->typed = true;
        (*it)->where = where;
    });
    if (!going) {   // the save failed and said why
        forget(id);
        return false;
    }
    lastCommand_ = c.line;
    return true;
}

void TestGutter::forget(int runId) {
    auto it = std::find_if(runs_.begin(), runs_.end(),
                           [runId](const std::unique_ptr<Run>& r) { return r->id == runId; });
    if (it == runs_.end()) return;
    auto said = results_.find((*it)->path);
    if (said != results_.end())
        for (const std::string& label : (*it)->labels) {
            auto s = said->second.find(label);
            if (s != said->second.end() && s->second == TestState::Running) said->second.erase(s);
        }
    const bool shown = (*it)->path == shownPath_;
    runs_.erase(it);
    if (shown) pushMarks();
}

// A command ended in some terminal: if it was the one a run of ours typed,
// its tests passed (exit status 0) or failed.
void TestGutter::commandFinished(const Finished& f) {
    std::vector<int> stale;
    bool changed = false;
    for (size_t i = 0; i < runs_.size();) {
        Run& r = *runs_[i];
        if (!r.typed || r.where.terminal != f.terminal) {
            i++;
            continue;
        }
        if (r.where.shell != f.shell) {   // the shell was restarted: nothing will say
            stale.push_back(r.id);
            i++;
            continue;
        }
        // The command typed was that shell's started + 1.
        const long long mine = r.where.started + 1;
        if (f.done < mine) {   // one before ours ended
            i++;
            continue;
        }
        // Ours ended: its status is the one reported only when it was the
        // last to end, and its line, when known, must be the run's.
        TestState state = TestState::Idle;
        if (f.done == mine && f.status >= 0 && (f.line.empty() || f.line.find(r.line) != std::string::npos))
            state = f.status == 0 ? TestState::Passed : TestState::Failed;
        auto said = results_.find(r.path);
        if (said != results_.end())
            for (const std::string& label : r.labels) {
                if (state == TestState::Idle) said->second.erase(label);
                else said->second[label] = state;
            }
        if (r.path == shownPath_) changed = true;
        runs_.erase(runs_.begin() + (long)i);
    }
    for (int id : stale) forget(id);
    if (changed) pushMarks();
}

void TestGutter::terminalsChanged() {
    if (!terminalOpen) return;
    std::vector<int> gone;
    for (const auto& r : runs_)
        if (r->typed && !terminalOpen(r->where.terminal)) gone.push_back(r->id);
    for (int id : gone) forget(id);
}

// ================================================================ checks
std::vector<std::string> TestGutter::testLabels() const {
    std::vector<std::string> out;
    for (const Runnables::Test& t : tests_) out.push_back(labelOf(t));
    return out;
}

std::vector<TestState> TestGutter::testStates() const {
    std::vector<TestState> out;
    if (BufferLines* l = BufferLines::of(viewBuffer()))
        for (const GutterTest& t : l->tests()) out.push_back(t.state);
    return out;
}
