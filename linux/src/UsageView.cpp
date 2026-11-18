// UsageView.cpp — see UsageView.h.
#include "UsageView.h"

#include <ctime>
#include <utility>

#include "AgentModel.h"
#include "AgentTranscript.h"
#include "ClaudeStatusLine.h"
#include "ClaudeUsage.h"
#include "StatusLineOffer.h"

// ------------------------------------------------------------------ model
struct UsageModel::Impl {
    GThreadPool* pool = nullptr;   // one thread at most, so looks run in order
    // Only on the pool's thread.
    ClaudeUsage::Scanner scanner;
    ClaudeUsage::Tally tally;
    std::string projects;
    // Only on the main loop: the look out now's callbacks.
    std::vector<std::function<void()>> running;

    struct Job {
        UsageModel* model;
        std::string projects, log;
    };
    struct Done {
        UsageModel* model;
        std::vector<std::string> lines;
    };

    static void work(gpointer data, gpointer user) {
        Job* job = static_cast<Job*>(data);
        Impl* self = static_cast<Impl*>(user);
        if (job->projects != self->projects) {
            self->scanner = ClaudeUsage::Scanner();
            self->tally.clear();
            self->projects = job->projects;
        }
        const double now = (double)time(nullptr);
        self->scanner.scan(AgentTranscript::posixFs(), job->projects, now - ClaudeUsage::kHorizon, self->tally);
        const std::vector<ClaudeUsage::Sample> samples =
            ClaudeUsage::parseLog(ClaudeStatusLine::readLog(job->log));
        const std::vector<ClaudeUsage::Entry> entries = self->tally.sorted();
        const ClaudeUsage::Forecast f = ClaudeUsage::forecast(samples, entries, now);
        auto* done = new Done{job->model, ClaudeUsage::describe(f, now)};
        delete job;
        g_idle_add(
            +[](gpointer p) -> gboolean {
                Done* d = static_cast<Done*>(p);
                d->model->finished(std::move(d->lines));
                delete d;
                return G_SOURCE_REMOVE;
            },
            done);
    }
};

UsageModel& UsageModel::shared() {
    // Never freed: the pool's thread may be reading when the app quits.
    static UsageModel* model = new UsageModel();
    return *model;
}

UsageModel::UsageModel() : impl_(new Impl) {
    impl_->pool = g_thread_pool_new(&Impl::work, impl_, 1, FALSE, nullptr);
}

void UsageModel::refresh() {
    if (last_ && g_get_monotonic_time() - last_ < 5 * G_USEC_PER_SEC && !busy_) return;
    look(nullptr);
}

void UsageModel::refreshThen(std::function<void()> done) {
    if (!done) done = [] {};
    look(std::move(done));
}

void UsageModel::look(std::function<void()> done) {
    if (done) waiting_.push_back(std::move(done));
    if (busy_) {
        again_ = true;
        return;
    }
    busy_ = true;
    again_ = false;
    impl_->running = std::move(waiting_);
    waiting_.clear();
    // Where to look, read on the main loop (the environment can change).
    auto* job = new Impl::Job{this, AgentModel::defaultProjectsDir(),
                              ClaudeStatusLine::logPath(StatusLineOffer::usageDir())};
    g_thread_pool_push(impl_->pool, job, nullptr);
}

void UsageModel::finished(std::vector<std::string> lines) {
    lines_ = std::move(lines);
    loaded_ = true;
    last_ = g_get_monotonic_time();
    busy_ = false;
    std::vector<std::function<void()>> callbacks = std::move(impl_->running);
    impl_->running.clear();
    // A copy: a listener may go while they are told.
    const std::map<int, std::function<void()>> listeners = listeners_;
    for (const auto& kv : listeners)
        if (listeners_.count(kv.first)) kv.second();
    for (auto& cb : callbacks) cb();
    if (again_ || !waiting_.empty()) look(nullptr);
}

int UsageModel::addListener(std::function<void()> fn) {
    const int id = nextListener_++;
    listeners_[id] = std::move(fn);
    return id;
}

void UsageModel::removeListener(int id) { listeners_.erase(id); }

// ------------------------------------------------------------------ view
UsageView::UsageView() : self_(std::make_shared<UsageView*>(this)) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(root_, "minicode-usage");
    gtk_widget_set_margin_start(root_, 10);
    gtk_widget_set_margin_end(root_, 10);
    gtk_widget_set_margin_bottom(root_, 6);
    GtkWidget* heading = gtk_label_new("PLAN USAGE");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_add_css_class(heading, "minicode-procs-heading");
    gtk_box_append(GTK_BOX(root_), heading);
    text_ = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(text_), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(text_), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(text_), 0);
    gtk_label_set_selectable(GTK_LABEL(text_), TRUE);
    gtk_widget_set_focusable(text_, FALSE);   // selectable with the mouse, never a Tab stop
    gtk_widget_add_css_class(text_, "minicode-usage-text");
    gtk_box_append(GTK_BOX(root_), text_);
    action_ = gtk_button_new();
    actionLabel_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(actionLabel_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(actionLabel_), PANGO_ELLIPSIZE_END);
    gtk_button_set_child(GTK_BUTTON(action_), actionLabel_);
    gtk_widget_add_css_class(action_, "flat");
    gtk_widget_add_css_class(action_, "minicode-usage-action");
    gtk_widget_set_margin_top(action_, 2);
    // Through the shared handle: the button may outlive the view.
    g_signal_connect_data(
        action_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
            if (UsageView* v = **static_cast<std::shared_ptr<UsageView*>*>(p)) v->act();
        }),
        new std::shared_ptr<UsageView*>(self_),
        [](gpointer p, GClosure*) { delete static_cast<std::shared_ptr<UsageView*>*>(p); }, (GConnectFlags)0);
    gtk_box_append(GTK_BOX(root_), action_);
    installed_ = StatusLineOffer::installed();
    setActionTitle();
    listener_ = UsageModel::shared().addListener([this] { showLines(); });
    showLines();
}

UsageView::~UsageView() {
    *self_ = nullptr;
    UsageModel::shared().removeListener(listener_);
}

std::string UsageView::actionTitle() const { return gtk_label_get_text(GTK_LABEL(actionLabel_)); }
std::string UsageView::forecastText() const { return gtk_label_get_text(GTK_LABEL(text_)); }

void UsageView::setActionTitle() {
    gtk_label_set_text(GTK_LABEL(actionLabel_), installed_ ? "Stop logging plan usage…"
                                                           : "Log plan usage from Claude Code's status line…");
    gtk_widget_set_tooltip_text(
        action_, installed_
                     ? "Puts Claude Code's status line back as it was before mutible changed it, after asking."
                     : "Changes Claude Code's status line, after showing you how, so it notes the plan usage "
                       "Claude Code reports; your own status line keeps running.");
}

void UsageView::showLines() {
    std::vector<std::string> shown;
    std::string fit;
    for (const std::string& l : UsageModel::shared().lines()) {
        if (l.rfind("Fit: ", 0) == 0) fit = l;
        else shown.push_back(l);
    }
    std::string text;
    if (UsageModel::shared().loaded()) {
        for (size_t i = 0; i < shown.size(); i++) text += (i ? "\n" : "") + shown[i];
    } else {
        text = "Reading Claude Code's transcripts…";
    }
    fit_ = fit;
    gtk_widget_set_tooltip_text(text_, fit.empty() ? nullptr : fit.c_str());
    if (text != forecastText()) gtk_label_set_text(GTK_LABEL(text_), text.c_str());
}

void UsageView::refresh() {
    const bool installed = StatusLineOffer::installed();
    if (installed != installed_) {
        installed_ = installed;
        setActionTitle();
    }
    UsageModel::shared().refresh();
}

void UsageView::act() {
    GtkRoot* root = gtk_widget_get_root(root_);
    GtkWindow* parent = root && GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr;
    std::shared_ptr<UsageView*> self = self_;
    auto after = [self](bool installed) {
        UsageView* v = *self;
        if (!v) return;
        v->installed_ = installed;
        v->setActionTitle();
        UsageModel::shared().refreshThen(nullptr);
    };
    if (installed_) StatusLineOffer::uninstall(parent, after);
    else StatusLineOffer::install(parent, after);
}
