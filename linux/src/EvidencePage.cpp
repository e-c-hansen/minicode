// EvidencePage.cpp — see EvidencePage.h.
#include "EvidencePage.h"

#include <algorithm>

#include "AgentView.h"

namespace {
const size_t kExpandBytes = 1 << 20;   // a whole output, shown up to 1 MB

std::string outputKey(const AgentEvidence::Command& c) {
    return c.toolUseId + "@" + std::to_string(c.output.offset) + "+" +
           std::to_string(c.output.length);
}

GtkTextTag* tagNamed(GtkTextBuffer* b, const char* name, const char* prop = nullptr,
                     const char* value = nullptr) {
    GtkTextTagTable* t = gtk_text_buffer_get_tag_table(b);
    GtkTextTag* tag = gtk_text_tag_table_lookup(t, name);
    if (tag) return tag;
    tag = gtk_text_buffer_create_tag(b, name, nullptr);
    if (prop) g_object_set(tag, prop, value, nullptr);
    return tag;
}

GtkWidget* heading(const char* text) {
    GtkWidget* l = gtk_label_new(nullptr);
    char* m = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    return l;
}

GtkWidget* textLabel() {
    GtkWidget* l = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_yalign(GTK_LABEL(l), 0);
    return l;
}

GtkWidget* boxed(GtkWidget* head, GtkWidget* label) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(box), head);
    GtkWidget* sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), label);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_size_request(sc, -1, 140);
    gtk_box_append(GTK_BOX(box), sc);
    return box;
}
}  // namespace

EvidencePage::EvidencePage(const std::string& transcript, const std::string& name)
    : transcript_(transcript), name_(name) {
    reader_ = std::make_unique<AgentEvidence::Reader>(transcript);
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    g_object_ref_sink(root_);
    gtk_widget_set_margin_start(root_, 12);
    gtk_widget_set_margin_end(root_, 12);
    gtk_widget_set_margin_top(root_, 10);
    gtk_widget_set_margin_bottom(root_, 10);
    title_ = gtk_label_new(name.c_str());
    gtk_widget_set_halign(title_, GTK_ALIGN_START);
    gtk_widget_add_css_class(title_, "title-3");
    info_ = gtk_label_new("");
    gtk_widget_set_halign(info_, GTK_ALIGN_START);
    gtk_widget_add_css_class(info_, "dim-label");
    gtk_box_append(GTK_BOX(root_), title_);
    gtk_box_append(GTK_BOX(root_), info_);
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_set_homogeneous(GTK_BOX(top), TRUE);
    claim_ = textLabel();
    test_ = textLabel();
    gtk_box_append(GTK_BOX(top), boxed(heading("What the agent said"), claim_));
    gtk_box_append(GTK_BOX(top), boxed(heading("The last test run"), test_));
    gtk_box_append(GTK_BOX(root_), top);
    gtk_box_append(GTK_BOX(root_), heading("Commands"));
    scroll_ = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll_, TRUE);
    commands_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(commands_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(commands_), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(commands_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(commands_), 8);
    gtk_widget_add_css_class(commands_, "monospace");
    buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(commands_));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), commands_);
    gtk_box_append(GTK_BOX(root_), scroll_);
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 1);
    g_signal_connect(click, "released", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(commands_, GTK_EVENT_CONTROLLER(click));
    render();
    timer_ = g_timeout_add_seconds(2, &EvidencePage::tick, this);
    update();
}

EvidencePage::~EvidencePage() {
    *alive_ = false;
    closed_ = true;
    if (timer_) g_source_remove(timer_);
    if (worker_.joinable()) worker_.join();
    g_object_unref(root_);
}

int EvidencePage::tick(void* p) {
    static_cast<EvidencePage*>(p)->update();
    return G_SOURCE_CONTINUE;
}

void EvidencePage::update() {
    if (closed_) return;
    if (updating_) { again_ = true; return; }
    updating_ = true;
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this] { work(); });
}

// On the worker: the commands, then each output's ends, published in two
// steps so the list shows at once and the outputs fill in.
void EvidencePage::work() {
    struct Pack { EvidencePage* page; std::shared_ptr<bool> alive; Shown s; };
    auto publish = [this](Shown s) {
        Pack* p = new Pack{this, alive_, std::move(s)};
        g_idle_add(
            [](gpointer d) -> gboolean {
                std::unique_ptr<Pack> pk(static_cast<Pack*>(d));
                if (*pk->alive) pk->page->show(std::move(pk->s));
                return G_SOURCE_REMOVE;
            },
            p);
    };
    std::unique_lock<std::mutex> lock(mutex_);
    const AgentTranscript::Fs fs = AgentTranscript::posixFs();
    const bool changed = reader_->update(fs) || !started_ || force_;
    force_ = false;
    started_ = true;
    const AgentEvidence::Evidence& ev = reader_->evidence();

    auto make = [&](bool withOutputs) {
        Shown sh;
        sh.final = ev.finalMessage + (ev.finalCut ? "…" : "");
        sh.cwd = ev.cwd;
        sh.lastTest = ev.lastTest;
        sh.outputs = withOutputs;
        size_t tests = 0;
        for (size_t i = 0; i < ev.commands.size(); i++) {
            const AgentEvidence::Command& c = ev.commands[i];
            Cmd m;
            m.desc = c.description;
            m.command = c.command;
            m.commandCut = c.commandCut;
            m.cwd = c.cwd.empty() ? ev.cwd : c.cwd;
            m.answered = c.answered;
            m.isError = c.isError;
            m.result = AgentView::resultWord(c);
            m.isTest = c.test != AgentEvidence::TestTool::None;
            if (m.isTest) tests++;
            m.testName = AgentEvidence::testToolName(c.test);
            m.summary = AgentView::summaryText(c.summary);
            m.summaryLine = c.summary.line;
            m.summaryFailed = c.summary.found && (c.summary.failed > 0 || c.summary.errors > 0);
            if (withOutputs && c.answered) {
                const std::string key = outputKey(c);
                auto it = outputs_.find(key);
                const bool live = c.inBackground;   // still being written: read it again
                if (it == outputs_.end() || live) {
                    Loaded lo;
                    if (it != outputs_.end()) lo = it->second;
                    if (!c.persistedPath.empty()) {
                        lo.out = AgentTranscript::readTextFile(fs, c.persistedPath, 20, 20);
                        lo.out.persistedPath = c.persistedPath;
                    } else if (!c.output.empty()) {
                        lo.out = AgentTranscript::readOutput(fs, transcript_, c.output, 20, 20);
                        if (!lo.out.persistedPath.empty()) {
                            std::string p = lo.out.persistedPath;
                            lo.out = AgentTranscript::readTextFile(fs, p, 20, 20);
                            lo.out.persistedPath = p;
                        }
                    }
                    it = outputs_.insert_or_assign(key, lo).first;
                }
                Loaded& lo = it->second;
                if (wantFull_[i] && !lo.haveFull) {
                    bool cut = false;
                    if (!lo.out.persistedPath.empty()) {
                        std::string data;
                        fs.read(lo.out.persistedPath, 0, kExpandBytes + 1, &data);
                        cut = data.size() > kExpandBytes;
                        if (cut) data.resize(kExpandBytes);
                        lo.full = data;
                    } else {
                        lo.full = AgentTranscript::readOutputText(fs, transcript_, c.output,
                                                                  kExpandBytes, &cut);
                    }
                    lo.full = AgentEvidence::stripAnsi(lo.full);
                    lo.haveFull = true;
                    lo.fullCut = cut;
                }
                m.loaded = true;
                m.head = lo.out.head;
                m.tail = lo.out.tail;
                m.omitted = lo.out.omitted;
                m.whole = lo.out.whole;
                if (wantFull_[i] && lo.haveFull) {
                    m.full = lo.full;
                    m.haveFull = true;
                    m.fullCut = lo.fullCut;
                }
            } else if (!c.answered) {
                m.loaded = true;
            }
            sh.cmds.push_back(std::move(m));
        }
        std::vector<std::string> parts;
        if (!ev.cwd.empty()) parts.push_back(ev.cwd);
        parts.push_back(std::to_string(ev.commands.size()) +
                        (ev.commands.size() == 1 ? " command, " : " commands, ") +
                        std::to_string(tests) + (tests == 1 ? " test run" : " test runs"));
        if (!ev.versions.empty()) {
            std::string v = "Claude Code ";
            for (size_t i = 0; i < ev.versions.size(); i++) v += (i ? ", " : "") + ev.versions[i];
            parts.push_back(v);
        }
        if (ev.badLines > 0)
            parts.push_back(std::to_string(ev.badLines) + " unreadable " +
                            (ev.badLines == 1 ? "line" : "lines") + " skipped");
        for (size_t i = 0; i < parts.size(); i++) sh.info += (i ? "  ·  " : "") + parts[i];
        return sh;
    };
    if (changed) {
        publish(make(false));
        publish(make(true));
    } else {
        Shown none;   // nothing to show; only ends the update
        none.outputs = true;
        none.lastTest = -2;
        publish(std::move(none));
    }
}

void EvidencePage::show(Shown s) {
    if (s.lastTest != -2) {
        cmds_ = std::move(s.cmds);
        final_ = std::move(s.final);
        cwd_ = std::move(s.cwd);
        info2_ = std::move(s.info);
        lastTest_ = s.lastTest;
        loaded_ = true;
        if (s.outputs) outputsLoaded_ = true;
        render();
    }
    if (s.outputs) {
        updating_ = false;
        if (again_) {
            again_ = false;
            update();
        }
    }
}

std::string EvidencePage::finalMessageText() const { return final_; }

std::string EvidencePage::lastTestText() const {
    const char* t = gtk_label_get_text(GTK_LABEL(test_));
    return t ? t : "";
}

std::string EvidencePage::commandsText() const {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* t = gtk_text_buffer_get_text(buffer_, &a, &b, FALSE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

void EvidencePage::renderLastTest() {
    std::string s;
    if (lastTest_ < 0 || lastTest_ >= (int)cmds_.size()) {
        s = "No test command was run.";
    } else {
        const Cmd& c = cmds_[(size_t)lastTest_];
        if (!c.summary.empty()) s += c.summary + "\n";
        s += !c.answered ? "still running"
             : c.isError ? "the command reported an error"
                         : "the command reported no error";
        if (c.summary.empty() && c.answered) s += "\nno test summary found in its output";
        s += "\n";
        if (!c.desc.empty()) s += c.desc + "\n";
        std::string cmd = c.command.size() > 300 ? c.command.substr(0, 300) + "…" : c.command;
        s += "$ " + cmd;
        if (!c.summaryLine.empty() && c.summaryLine != c.summary) s += "\n" + c.summaryLine;
    }
    gtk_label_set_text(GTK_LABEL(test_), s.c_str());
}

void EvidencePage::render() {
    gtk_label_set_text(GTK_LABEL(info_), info2_.c_str());
    gtk_label_set_text(GTK_LABEL(claim_),
                       final_.empty() ? "(The session has no reply with text yet.)" : final_.c_str());
    renderLastTest();

    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll_));
    const double place = gtk_adjustment_get_value(adj);
    gtk_text_buffer_set_text(buffer_, "", 0);
    g_object_set(tagNamed(buffer_, "ev-bold"), "weight", 700, nullptr);
    tagNamed(buffer_, "ev-muted", "foreground", "#9CA3AF");
    tagNamed(buffer_, "ev-red", "foreground", "#F14C4C");
    tagNamed(buffer_, "ev-green", "foreground", "#73C991");
    tagNamed(buffer_, "ev-mono", "family", "monospace");
    GtkTextTag* test = tagNamed(buffer_, "ev-test", "background", "#E2C08D");
    g_object_set(test, "foreground", "#1E1E1E", nullptr);
    GtkTextTag* indent = tagNamed(buffer_, "ev-indent");
    g_object_set(indent, "left-margin", 36, nullptr);
    auto add = [&](const std::string& text, std::initializer_list<const char*> tags) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer_, &end);
        const int start = gtk_text_iter_get_offset(&end);
        gtk_text_buffer_insert(buffer_, &end, text.c_str(), (int)text.size());
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, start);
        gtk_text_buffer_get_end_iter(buffer_, &b);
        for (const char* t : tags) gtk_text_buffer_apply_tag_by_name(buffer_, t, &a, &b);
    };
    auto link = [&](const std::string& text, const std::string& name, bool indented) {
        GtkTextTag* t = tagNamed(buffer_, name.c_str(), "foreground", "#4EA1F7");
        g_object_set(t, "underline", PANGO_UNDERLINE_SINGLE, nullptr);
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer_, &end);
        const int start = gtk_text_iter_get_offset(&end);
        gtk_text_buffer_insert(buffer_, &end, text.c_str(), (int)text.size());
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, start);
        gtk_text_buffer_get_end_iter(buffer_, &b);
        gtk_text_buffer_apply_tag(buffer_, t, &a, &b);
        if (indented) gtk_text_buffer_apply_tag_by_name(buffer_, "ev-indent", &a, &b);
    };
    if (!loaded_) add("Reading the transcript…", {"ev-muted"});
    else if (cmds_.empty()) add("This session ran no shell commands.", {"ev-muted"});
    for (size_t i = 0; i < cmds_.size(); i++) {
        const Cmd& c = cmds_[i];
        const std::string n = std::to_string(i);
        add("\n" + std::to_string(i + 1) + "  ", {"ev-muted"});
        add(c.desc.empty() ? "(no description)" : c.desc,
            c.desc.empty() ? std::initializer_list<const char*>{"ev-muted"}
                           : std::initializer_list<const char*>{"ev-bold"});
        if (c.isTest) {
            add("   ", {});
            add(" TEST " + c.testName + " ", {"ev-test", "ev-bold"});
        }
        add("   ", {});
        add(c.result, {!c.answered ? "ev-muted" : c.isError ? "ev-red" : "ev-green", "ev-bold"});
        add("   ", {});
        link("Open in Terminal", "ev:terminal:" + n, false);
        add("\n", {});
        add("$ " + c.command + (c.commandCut ? " …(cut)" : "") + "\n", {"ev-mono", "ev-muted", "ev-indent"});
        if (c.isTest && c.answered)
            add((c.summary.empty() ? "no test summary found in its output" : c.summary) + "\n",
                {"ev-bold", "ev-indent",
                 c.summary.empty() ? "ev-muted" : c.summaryFailed ? "ev-red" : "ev-green"});
        if (!c.answered) {
            add("(no result yet)\n", {"ev-muted", "ev-indent"});
        } else if (!c.loaded) {
            add("Reading the output…\n", {"ev-muted", "ev-indent"});
        } else if (c.haveFull) {
            add(c.full, {"ev-mono", "ev-indent"});
            if (c.full.empty() || c.full.back() != '\n') add("\n", {});
            if (c.fullCut) add("(cut at 1 MB)\n", {"ev-muted", "ev-indent"});
            link("Show less", "ev:less:" + n, true);
            add("\n", {});
        } else if (c.head.empty() && c.tail.empty()) {
            add("(no output)\n", {"ev-muted", "ev-indent"});
        } else {
            for (const std::string& l : c.head) add(l + "\n", {"ev-mono", "ev-indent"});
            if (!c.whole) {
                add("⋯ " + AgentView::moreLinesText(c.omitted) + "  ", {"ev-muted", "ev-indent"});
                link("Show all", "ev:expand:" + n, true);
                add("\n", {});
            }
            for (const std::string& l : c.tail) add(l + "\n", {"ev-mono", "ev-indent"});
        }
    }
    gtk_adjustment_set_value(adj, place);
}

void EvidencePage::expand(size_t index) {
    if (index >= cmds_.size()) return;
    {
        std::lock_guard<std::mutex> g(mutex_);
        wantFull_[index] = true;
        force_ = true;
    }
    update();
}

void EvidencePage::collapse(size_t index) {
    if (index >= cmds_.size()) return;
    {
        std::lock_guard<std::mutex> g(mutex_);
        wantFull_[index] = false;
    }
    cmds_[index].haveFull = false;
    cmds_[index].full.clear();
    render();
}

void EvidencePage::openInTerminal(size_t index) {
    if (index >= cmds_.size() || !onOpenInTerminal) return;
    const Cmd& c = cmds_[index];
    onOpenInTerminal(c.command, c.cwd.empty() ? cwd_ : c.cwd);
}

bool EvidencePage::linkAt(double x, double y, std::string* name) const {
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(commands_), GTK_TEXT_WINDOW_WIDGET,
                                          (int)x, (int)y, &bx, &by);
    GtkTextIter it;
    if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(commands_), &it, bx, by)) return false;
    GSList* tags = gtk_text_iter_get_tags(&it);
    bool found = false;
    for (GSList* l = tags; l; l = l->next) {
        gchar* n = nullptr;
        g_object_get(l->data, "name", &n, nullptr);
        if (n && g_str_has_prefix(n, "ev:")) {
            if (name) *name = n;
            found = true;
        }
        g_free(n);
    }
    g_slist_free(tags);
    return found;
}

void EvidencePage::onPressed(GtkGestureClick*, int, double x, double y, gpointer p) {
    EvidencePage* self = static_cast<EvidencePage*>(p);
    GtkTextIter a, b;
    if (gtk_text_buffer_get_selection_bounds(self->buffer_, &a, &b)) return;   // a selection drag
    std::string name;
    if (!self->linkAt(x, y, &name)) return;
    const size_t last = name.rfind(':');
    const size_t i = (size_t)std::atol(name.c_str() + last + 1);
    if (name.compare(0, 10, "ev:expand:") == 0) self->expand(i);
    else if (name.compare(0, 8, "ev:less:") == 0) self->collapse(i);
    else if (name.compare(0, 12, "ev:terminal:") == 0) self->openInTerminal(i);
}
