// Latex.cpp — see Latex.h.
#ifdef MINICODE_ENABLE_PDF

#include "Latex.h"

#include "LatexClick.h"
#include "PageWords.h"
#include "PdfView.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <poppler.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace {

// tectonic is pinned so the download is reproducible, at the Mac's version;
// any tectonic already on the machine is preferred over ours. The musl builds
// are static, so they run on any distribution without extra libraries (the
// glibc build wants libgraphite2). The checksums are the ones GitHub publishes
// for the release's assets.
constexpr const char* kTectonicVersion = "0.17.0";
#if defined(__x86_64__)
constexpr const char* kTectonicArch = "x86_64-unknown-linux-musl";
constexpr const char* kTectonicSha256 =
    "8533d07f9ccbd7a65824b9e0459041bca34af1eb33daba48f59215593753a3b7";
#elif defined(__aarch64__)
constexpr const char* kTectonicArch = "aarch64-unknown-linux-musl";
constexpr const char* kTectonicSha256 =
    "b10954a95404f3ab2328d2fa59a5ebab8e657f893fab096f98be8db7c0c979b8";
#else
constexpr const char* kTectonicArch = nullptr;   // no download offered
constexpr const char* kTectonicSha256 = nullptr;
#endif

constexpr guint kDebounceMs = 600;   // the Mac's 0.6 s

bool isExecutable(const std::string& p) {
    return !p.empty() && g_file_test(p.c_str(), G_FILE_TEST_IS_EXECUTABLE) &&
           !g_file_test(p.c_str(), G_FILE_TEST_IS_DIR);
}

std::string dirName(const std::string& p) {
    char* d = g_path_get_dirname(p.c_str());
    std::string out = d ? d : ".";
    g_free(d);
    return out;
}

std::string baseStem(const std::string& p) {
    char* b = g_path_get_basename(p.c_str());
    std::string out = b ? b : "";
    g_free(b);
    const auto dot = out.find_last_of('.');
    if (dot != std::string::npos && dot > 0) out.erase(dot);
    return out;
}

// A child of the app dies with it: a typeset or a download left running
// after MiniCode quits (or crashes) would otherwise carry on alone.
void dieWithParent(gpointer) { prctl(PR_SET_PDEATHSIG, SIGTERM); }

GSubprocessLauncher* newLauncher(GSubprocessFlags flags) {
    GSubprocessLauncher* l = g_subprocess_launcher_new(flags);
    g_subprocess_launcher_set_child_setup(l, dieWithParent, nullptr, nullptr);
    return l;
}

std::string describeSpan(const LatexSpan& span) {
    switch (span.kind) {
        case LatexSpanKind::Field: return "\\" + span.command;
        case LatexSpanKind::Math:  return "math";
        case LatexSpanKind::Text:  return span.itemIndex >= 0 ? "list item" : "text";
    }
    return "text";
}

// A byte offset moved back onto the start of a UTF-8 character.
std::size_t charStart(const std::string& s, std::size_t i) {
    while (i > 0 && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    return i;
}

}  // namespace

// One typeset in flight: what it was given, and where its output lands.
struct LatexPreview::Job {
    std::shared_ptr<LatexPreview*> alive;
    unsigned generation = 0;
    std::string src, scratch, pdf, synctex;
};

// ---------------------------------------------------------------- construction

LatexPreview::LatexPreview(GtkTextBuffer* buffer)
    : buffer_(buffer), alive_(std::make_shared<LatexPreview*>(this)) {
    static unsigned instances = 0;
    instance_ = ++instances;
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_vexpand(root_, TRUE);
    // Focusable, so Ctrl+Z reaches the shortcut below while the pages show.
    gtk_widget_set_focusable(root_, TRUE);
    g_object_ref_sink(root_);

    // The status line along the top, as the Mac's LatexView has it: a
    // spinner while typesetting, what happened, and Recompile / Download….
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(bar, "minicode-status");
    gtk_widget_add_css_class(bar, "minicode-latex-bar");
    spinner_ = gtk_spinner_new();
    gtk_widget_set_visible(spinner_, FALSE);
    status_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status_), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(status_, TRUE);
    button_ = gtk_button_new_with_label("Recompile");
    gtk_widget_add_css_class(button_, "flat");
    g_signal_connect(button_, "clicked", G_CALLBACK(onButton), this);
    gtk_box_append(GTK_BOX(bar), spinner_);
    gtk_box_append(GTK_BOX(bar), status_);
    gtk_box_append(GTK_BOX(bar), button_);
    gtk_box_append(GTK_BOX(root_), bar);

    pdf_ = new PdfView();
    pdf_->setClickCallback(onPdfClick, this);

    logView_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logView_), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(logView_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(logView_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(logView_), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(logView_), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(logView_), 12);
    gtk_widget_add_css_class(logView_, "minicode-editor");
    logScroll_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(logScroll_, "minicode-scroller");
    gtk_widget_add_css_class(logScroll_, "minicode-editor-scroller");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(logScroll_), logView_);

    stack_ = gtk_stack_new();
    gtk_widget_set_vexpand(stack_, TRUE);
    gtk_stack_add_named(GTK_STACK(stack_), pdf_->widget(), "pdf");
    gtk_stack_add_named(GTK_STACK(stack_), logScroll_, "log");
    gtk_box_append(GTK_BOX(root_), stack_);

    // Preview edits are buffer edits, so undo is the buffer's.
    GtkEventController* keys = gtk_shortcut_controller_new();
    auto add = [&](const char* trigger, GtkShortcutFunc fn) {
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
            gtk_shortcut_new(gtk_shortcut_trigger_parse_string(trigger),
                             gtk_callback_action_new(fn, this, nullptr)));
    };
    add("<Control>z", onUndo);
    add("<Control><Shift>z", onRedo);
    add("<Control>y", onRedo);
    gtk_widget_add_controller(root_, keys);

    // The edit popover, built once and refilled for each double-click.
    popover_ = gtk_popover_new();
    gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_TOP);
    gtk_widget_set_parent(popover_, root_);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(box, 380, -1);
    popLabel_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(popLabel_), 0.0);
    gtk_widget_add_css_class(popLabel_, "dim-label");
    popText_ = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(popText_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(popText_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(popText_), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(popText_), 4);
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), popText_);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 74);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 220);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroll), TRUE);
    // Return commits and Shift+Return types a newline, as on the Mac; Escape
    // closes the popover by itself.
    GtkEventController* editKeys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(editKeys, GTK_PHASE_CAPTURE);
    g_signal_connect(editKeys, "key-pressed", G_CALLBACK(onEditKey), this);
    gtk_widget_add_controller(popText_, editKeys);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    popAdd_ = gtk_button_new_with_label("Add item");
    gtk_widget_set_tooltip_text(popAdd_,
        "Add a new entry to this list, below the one you clicked");
    g_signal_connect_swapped(popAdd_, "clicked", G_CALLBACK(+[](gpointer self) {
        static_cast<LatexPreview*>(self)->startAddItem();
    }), this);
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget* cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(+[](gpointer self) {
        static_cast<LatexPreview*>(self)->cancelEdit();
    }), this);
    GtkWidget* save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect_swapped(save, "clicked", G_CALLBACK(+[](gpointer self) {
        static_cast<LatexPreview*>(self)->commitEdit();
    }), this);
    gtk_box_append(GTK_BOX(buttons), popAdd_);
    gtk_box_append(GTK_BOX(buttons), spacer);
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), save);

    gtk_box_append(GTK_BOX(box), popLabel_);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_popover_set_child(GTK_POPOVER(popover_), box);

    changedId_ = g_signal_connect(buffer_, "changed", G_CALLBACK(onBufferChanged), this);
    // On quit, stop tectonic and take the hidden sibling away with it.
    if (GApplication* app = g_application_get_default())
        shutdownId_ = g_signal_connect(app, "shutdown", G_CALLBACK(onShutdown), this);
}

LatexPreview::~LatexPreview() {
    *alive_ = nullptr;
    // An export still waiting for a typeset is dropped, not answered: its
    // window is going too, so there is nowhere to report to. Dropping it
    // frees the file it would have written.
    waiters_.clear();
    stopRun();
    if (debounce_) g_source_remove(debounce_);
    if (changedId_) g_signal_handler_disconnect(buffer_, changedId_);
    if (shutdownId_)
        if (GApplication* app = g_application_get_default())
            g_signal_handler_disconnect(app, shutdownId_);
    if (pdfBytes_) g_bytes_unref(pdfBytes_);
    gtk_widget_unparent(popover_);
    delete pdf_;
    g_object_unref(root_);
}

void LatexPreview::onShutdown(GApplication*, gpointer selfp) {
    static_cast<LatexPreview*>(selfp)->stopRun();
}

// ---------------------------------------------------------------- tectonic

std::string LatexPreview::managedTectonicPath() {
    char* p = g_build_filename(g_get_user_data_dir(), "minicode", "bin", "tectonic", nullptr);
    std::string out = p;
    g_free(p);
    return out;
}

std::string LatexPreview::tectonicPath() {
    if (const char* env = g_getenv("MINICODE_TECTONIC"))
        if (isExecutable(env)) return env;
    const std::string managed = managedTectonicPath();
    if (isExecutable(managed)) return managed;
    char* onPath = g_find_program_in_path("tectonic");
    std::string out = onPath ? onPath : "";
    g_free(onPath);
    return out;
}

// ---------------------------------------------------------------- the source

std::string LatexPreview::bufferText() const {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* t = gtk_text_buffer_get_text(buffer_, &a, &b, FALSE);   // what Save writes
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

// `.<file name>.<preview>.minicode.tex`: the whole name, extension included,
// so notes.tex and notes.ltx in one folder do not share a copy, and this
// preview's number, so two windows on one file do not either. Each run's
// removal of its copy would otherwise delete the other's input mid-typeset.
std::string LatexPreview::scratchPath() const {
    char* b = g_path_get_basename(path_.c_str());
    const std::string name = b ? b : "";
    g_free(b);
    const std::string tag = "." + std::to_string(instance_) + ".minicode.tex";
    if (path_.empty() || name.empty() || name == "." || name == "/")
        return outDir() + "/untitled" + tag;
    return dirName(path_) + "/." + name + tag;
}

namespace {
// The folder tectonic's output goes under: $XDG_RUNTIME_DIR/minicode-latex,
// which only the user can reach, or, with no runtime folder, one in /tmp that
// must be the user's own. A /tmp path anyone could have made first is used
// only if it is a real folder (not a link), owned by this user and closed to
// everyone else; otherwise a fresh private one from g_dir_make_tmp.
std::string outputBase() {
    const char* runtime = g_getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime && g_file_test(runtime, G_FILE_TEST_IS_DIR))
        return std::string(runtime) + "/minicode-latex";
    static std::string chosen;
    if (!chosen.empty()) return chosen;
    const std::string dir = std::string(g_get_tmp_dir()) + "/minicode-latex-" + g_get_user_name();
    g_mkdir(dir.c_str(), 0700);   // fails harmlessly if it is there already
    GStatBuf st;
    if (g_lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
        (st.st_mode & 077) == 0) {
        chosen = dir;
    } else {
        char* tmp = g_dir_make_tmp("minicode-latex-XXXXXX", nullptr);
        chosen = tmp ? tmp : dir;
        g_free(tmp);
    }
    return chosen;
}
}  // namespace

// <outputBase>/<a hash of the document's path>, so two documents with the
// same name in different folders never share output.
std::string LatexPreview::outDir() const {
    const std::string base = outputBase();
    char* hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path_.c_str(), -1);
    std::string dir = base + "/" + std::string(hash).substr(0, 12);
    g_free(hash);
    g_mkdir_with_parents(dir.c_str(), 0700);
    return dir;
}

void LatexPreview::setPath(const std::string& path) {
    if (path == path_) return;
    // A rename (the shell says so by calling this with the pages still up)
    // keeps them: the source is the same. The next typeset writes the hidden
    // sibling under the new name.
    if (!path_.empty() && !path.empty() && pdf_->hasDocument()) {
        stopRun();
        queued_ = false;
        path_ = path;
        if (active_ && bufferText() != pdfSrc_) compileNow();
        else answerWaiters("");
        return;
    }
    const bool wasActive = active_;
    close();
    path_ = path;
    active_ = wasActive;
    if (active_ && !path_.empty()) compileNow();
}

void LatexPreview::setActive(bool active) {
    active_ = active;
    if (!active) {
        if (debounce_) g_source_remove(debounce_);
        debounce_ = 0;
        gtk_popover_popdown(GTK_POPOVER(popover_));
        return;
    }
    if (!pdf_->hasDocument() || bufferText() != pdfSrc_ || missing_) compileNow();
    gtk_widget_grab_focus(root_);
}

void LatexPreview::close() {
    stopRun();
    if (debounce_) g_source_remove(debounce_);
    debounce_ = 0;
    queued_ = false;
    active_ = false;
    gtk_popover_popdown(GTK_POPOVER(popover_));
    editValid_ = false;
    pdf_->clear();
    if (pdfBytes_) g_bytes_unref(pdfBytes_);
    pdfBytes_ = nullptr;
    pdfSrc_.clear();
    sync_ = SyncTexIndex();
    syncScratch_.clear();
    doc_ = LatexDoc();
    path_.clear();
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "pdf");
    setStatus("", false);
    answerWaiters("The document was closed before it was typeset.");
}

void LatexPreview::onBufferChanged(GtkTextBuffer*, gpointer selfp) {
    LatexPreview* self = static_cast<LatexPreview*>(selfp);
    if (self->active_ && !self->path_.empty()) self->scheduleCompile();
}

void LatexPreview::scheduleCompile() {
    if (debounce_) g_source_remove(debounce_);
    debounce_ = g_timeout_add(kDebounceMs, [](gpointer p) -> gboolean {
        LatexPreview* self = static_cast<LatexPreview*>(p);
        self->debounce_ = 0;
        self->compileNow();
        return G_SOURCE_REMOVE;
    }, this);
}

// ---------------------------------------------------------------- typesetting

void LatexPreview::stopRun() {
    ++generation_;   // whatever is running now reports to nobody
    if (proc_) {
        g_subprocess_force_exit(proc_);
        g_object_unref(proc_);
        proc_ = nullptr;
    }
    // Taken away now, not when the killed run reports back: by then a new run
    // for the same document may have written it again.
    removeScratch();
}

// Remove the running typeset's hidden sibling. Through the descriptor of the
// folder it was written in, which still names that folder after a rename or
// a move to the trash, so the copy goes wherever the folder went; by path
// only if the folder could not be opened.
void LatexPreview::removeScratch() {
    if (runDirFd_ >= 0) {
        unlinkat(runDirFd_, runScratchName_.c_str(), 0);
        ::close(runDirFd_);
    } else if (!runScratch_.empty()) {
        g_remove(runScratch_.c_str());
    }
    runDirFd_ = -1;
    runScratchName_.clear();
    runScratch_.clear();
}

void LatexPreview::compileNow() {
    if (debounce_) g_source_remove(debounce_);
    debounce_ = 0;
    if (path_.empty()) return;
    const std::string tool = tectonicPath();
    if (tool.empty()) {
        showTectonicMissing();
        answerWaiters("tectonic is not installed. The LaTeX preview offers to download it.");
        return;
    }
    missing_ = false;
    if (proc_) { queued_ = true; return; }

    auto* job = new Job;
    job->alive = alive_;
    job->src = bufferText();
    job->scratch = scratchPath();
    const std::string out = outDir();
    const std::string stem = baseStem(job->scratch);   // ".notes.minicode"
    job->pdf = out + "/" + stem + ".pdf";
    job->synctex = out + "/" + stem + ".synctex.gz";

    GError* err = nullptr;
    if (!g_file_set_contents(job->scratch.c_str(), job->src.data(), (gssize)job->src.size(),
                             &err)) {
        showFailure(std::string("Cannot write the preview copy next to your file:\n") +
                    (err ? err->message : "unknown error"));
        g_clear_error(&err);
        delete job;
        answerWaiters("The preview copy could not be written next to the file, so the "
                      "document was not typeset.");
        return;
    }

    queued_ = false;
    job->generation = ++generation_;
    GSubprocessLauncher* l = newLauncher(static_cast<GSubprocessFlags>(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE));
    g_subprocess_launcher_set_cwd(l, dirName(path_).c_str());
    proc_ = g_subprocess_launcher_spawn(l, &err, tool.c_str(), "--synctex", "--chatter",
                                        "minimal", "--color", "never", "--outdir",
                                        out.c_str(), job->scratch.c_str(), nullptr);
    g_object_unref(l);
    if (!proc_) {
        g_remove(job->scratch.c_str());
        showFailure(std::string("tectonic did not run: ") + (err ? err->message : ""));
        g_clear_error(&err);
        delete job;
        answerWaiters("tectonic did not run.");
        return;
    }
    runScratch_ = job->scratch;
    runDirFd_ = open(dirName(job->scratch).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char* name = g_path_get_basename(job->scratch.c_str());
    runScratchName_ = name ? name : "";
    g_free(name);
    setStatus("Typesetting…", true);
    g_subprocess_communicate_async(proc_, nullptr, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            Job* job = static_cast<Job*>(data);
            GBytes* output = nullptr;
            GSubprocess* proc = G_SUBPROCESS(src);
            g_subprocess_communicate_finish(proc, res, &output, nullptr, nullptr);
            const bool ok = g_subprocess_get_if_exited(proc) &&
                            g_subprocess_get_exit_status(proc) == 0;
            LatexPreview* self = *job->alive;
            if (self && job->generation == self->generation_) self->finishCompile(job, output, ok);
            if (output) g_bytes_unref(output);
            delete job;
        }, job);
}

void LatexPreview::finishCompile(Job* job, GBytes* output, bool ok) {
    removeScratch();
    g_object_unref(proc_);
    proc_ = nullptr;

    std::string log;
    if (output) {
        gsize n = 0;
        const char* d = static_cast<const char*>(g_bytes_get_data(output, &n));
        log.assign(d ? d : "", n);
        if (!g_utf8_validate(log.data(), (gssize)log.size(), nullptr)) {
            char* fixed = g_utf8_make_valid(log.data(), (gssize)log.size());
            log = fixed;
            g_free(fixed);
        }
    }

    std::string problem;
    if (ok) {
        // The bytes are kept so an export is exactly what tectonic wrote.
        char* data = nullptr;
        gsize len = 0;
        if (g_file_get_contents(job->pdf.c_str(), &data, &len, nullptr)) {
            std::string loadErr;
            if (pdf_->load(job->pdf, true, &loadErr)) {
                if (pdfBytes_) g_bytes_unref(pdfBytes_);
                pdfBytes_ = g_bytes_new_take(data, len);
                data = nullptr;
                pdfSrc_ = job->src;
                sync_ = SyncTexIndex::parse(readGzipFile(job->synctex));
                syncScratch_ = job->scratch;
                doc_ = LatexDoc::parse(pdfSrc_);
                showDocument();
            } else {
                problem = "The PDF tectonic wrote could not be read: " + loadErr;
            }
            g_free(data);
        } else {
            problem = "tectonic finished but wrote no PDF.";
        }
    } else {
        problem = log.empty() ? "tectonic did not run." : log;
    }
    if (!problem.empty()) showFailure(problem);

    if (queued_) compileNow();
    else answerWaiters(problem.empty() ? "" : "The document did not typeset. The preview "
                                              "shows tectonic's log.");
}

void LatexPreview::showDocument() {
    const int n = pdf_->pageCount();
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "pdf");
    std::string text = n == 1 ? "1 page" : std::to_string(n) + " pages";
    setStatus(text + " · double-click text to edit it", false);
    setButton("Recompile", true);
}

void LatexPreview::showFailure(const std::string& log) {
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(logView_)),
                             log.c_str(), (int)log.size());
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "log");
    setStatus("Did not typeset — see the log", false);
    setButton("Recompile", true);
}

void LatexPreview::showTectonicMissing() {
    missing_ = true;
    std::string text =
        "The LaTeX preview needs tectonic, a single-binary TeX engine.\n\n"
        "MiniCode uses whichever tectonic is already on your machine (on your PATH, "
        "or named by MINICODE_TECTONIC)";
    if (kTectonicArch)
        text += ", or it can download the official build for you, about 10 MB, into " +
                dirName(managedTectonicPath()) + ".\n\n"
                "The first document you typeset also downloads the LaTeX packages it "
                "uses, roughly 40 MB, which tectonic caches for later.";
    else
        text += ". There is no official build to download for this processor; install "
                "tectonic from your distribution or with cargo.";
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(logView_)),
                             text.c_str(), (int)text.size());
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "log");
    setStatus("tectonic was not found", false);
    setButton(kTectonicArch ? "Download…" : "Recompile", kTectonicArch != nullptr);
}

void LatexPreview::setStatus(const std::string& text, bool busy) {
    gtk_label_set_text(GTK_LABEL(status_), text.c_str());
    gtk_widget_set_visible(spinner_, busy);
    if (busy) gtk_spinner_start(GTK_SPINNER(spinner_));
    else      gtk_spinner_stop(GTK_SPINNER(spinner_));
}

void LatexPreview::setButton(const char* label, bool sensitive) {
    gtk_button_set_label(GTK_BUTTON(button_), label);
    gtk_widget_set_sensitive(button_, sensitive);
}

std::string LatexPreview::status() const { return gtk_label_get_text(GTK_LABEL(status_)); }

bool LatexPreview::showingLog() const {
    return gtk_stack_get_visible_child(GTK_STACK(stack_)) == logScroll_;
}

std::string LatexPreview::logText() const {
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logView_));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

void LatexPreview::onButton(GtkButton*, gpointer selfp) {
    LatexPreview* self = static_cast<LatexPreview*>(selfp);
    if (!tectonicPath().empty()) self->compileNow();
    else if (kTectonicArch) self->downloadTectonic();
}

// ---------------------------------------------------------------- export

void LatexPreview::pdfForBuffer(PdfCb done) {
    if (pdfBytes_ && pdfSrc_ == bufferText() && !proc_) {
        done(pdfBytes_, "");
        return;
    }
    if (tectonicPath().empty()) {
        done(nullptr, "tectonic is not installed. The LaTeX preview offers to download it.");
        return;
    }
    waiters_.push_back(std::move(done));
    compileNow();   // skip the typing pause; queues if one is running
}

// Called when a typeset ends with nothing queued after it. The buffer may have
// moved on during the run, and then the waiters wait for the next one.
void LatexPreview::answerWaiters(const std::string& error) {
    if (waiters_.empty()) return;
    if (error.empty() && pdfSrc_ != bufferText() && !path_.empty()) { compileNow(); return; }
    std::vector<PdfCb> waiters;
    waiters.swap(waiters_);
    for (PdfCb& w : waiters) w(error.empty() ? pdfBytes_ : nullptr, error);
}

void LatexPreview::exportPdf(GtkWindow* parent) {
    if (path_.empty()) return;
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Export PDF");
    GFile* folder = g_file_new_for_path(dirName(path_).c_str());
    gtk_file_dialog_set_initial_folder(dlg, folder);
    g_object_unref(folder);
    gtk_file_dialog_set_initial_name(dlg, (baseStem(path_) + ".pdf").c_str());

    struct Ask { std::shared_ptr<LatexPreview*> alive; GtkWindow* parent; };
    auto* ask = new Ask{alive_, parent};
    gtk_file_dialog_save(dlg, parent, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            Ask* ask = static_cast<Ask*>(data);
            GFile* dest = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, nullptr);
            LatexPreview* self = *ask->alive;
            GtkWindow* parent = ask->parent;
            delete ask;
            if (!dest) return;   // cancelled
            if (!self) { g_object_unref(dest); return; }
            // Held by the waiter, so it is let go of even when the waiter is
            // dropped unanswered (the preview closing mid-typeset).
            std::shared_ptr<GFile> owned(dest, [](GFile* f) { g_object_unref(f); });
            self->pdfForBuffer([owned, parent](GBytes* pdf, const std::string& error) {
                GFile* dest = owned.get();
                std::string problem = error;
                if (pdf) {
                    gsize n = 0;
                    const void* d = g_bytes_get_data(pdf, &n);
                    GError* err = nullptr;
                    if (!g_file_replace_contents(dest, static_cast<const char*>(d), n,
                                                 nullptr, FALSE, G_FILE_CREATE_NONE,
                                                 nullptr, nullptr, &err)) {
                        char* name = g_file_get_basename(dest);
                        problem = std::string("Could not save “") + (name ? name : "") +
                                  "”: " + (err ? err->message : "unknown error");
                        g_free(name);
                        g_clear_error(&err);
                    }
                }
                if (problem.empty()) return;
                // An OK button of our own and choose(), as main.cpp's error
                // alerts do: a buttonless show() alert would not close.
                GtkAlertDialog* alert = gtk_alert_dialog_new("The PDF was not exported");
                gtk_alert_dialog_set_detail(alert, problem.c_str());
                const char* ok[] = {"OK", nullptr};
                gtk_alert_dialog_set_buttons(alert, ok);
                gtk_alert_dialog_set_modal(alert, TRUE);
                gtk_alert_dialog_choose(alert, parent, nullptr, nullptr, nullptr);
                g_object_unref(alert);
            });
        }, ask);
    g_object_unref(dlg);
}

// ---------------------------------------------------------------- downloading

struct LatexPreview::Download {
    std::shared_ptr<LatexPreview*> alive;
    std::string problem;
};

void LatexPreview::downloadTectonic() {
    if (downloading_ || !kTectonicArch) return;
    downloading_ = true;
    setStatus("Downloading tectonic…", true);
    setButton("Download…", false);

    GTask* task = g_task_new(nullptr, nullptr,
        [](GObject*, GAsyncResult* res, gpointer) {
            auto* d = static_cast<Download*>(g_task_get_task_data(G_TASK(res)));
            if (LatexPreview* self = *d->alive) self->finishDownload(d->problem);
        }, nullptr);
    auto* d = new Download{alive_, ""};
    g_task_set_task_data(task, d, [](gpointer p) { delete static_cast<Download*>(p); });
    // The whole download runs on a worker thread, blocking on each tool in turn.
    g_task_run_in_thread(task, [](GTask*, gpointer, gpointer data, GCancellable*) {
        auto* d = static_cast<Download*>(data);
        const std::string bin = dirName(managedTectonicPath());
        const std::string file = std::string("tectonic-") + kTectonicVersion + "-" +
                                 kTectonicArch + ".tar.gz";
        const std::string url =
            std::string("https://github.com/tectonic-typesetting/tectonic/releases/download/"
                        "tectonic%40") + kTectonicVersion + "/" + file;
        const std::string archive = bin + "/" + file + ".part";
        const std::string unpack = bin + "/.tectonic-unpack";
        g_mkdir_with_parents(bin.c_str(), 0755);

        // Runs a tool, and on failure says what went wrong in its own words.
        auto run = [](const std::vector<std::string>& argv, std::string* problem) {
            std::vector<const char*> args;
            for (const std::string& a : argv) args.push_back(a.c_str());
            args.push_back(nullptr);
            GSubprocessLauncher* l = newLauncher(static_cast<GSubprocessFlags>(
                G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
            GError* err = nullptr;
            GSubprocess* p = g_subprocess_launcher_spawnv(l, args.data(), &err);
            g_object_unref(l);
            if (!p) {
                *problem = err ? err->message : "could not start " + argv[0];
                g_clear_error(&err);
                return false;
            }
            GBytes* errOut = nullptr;
            g_subprocess_communicate(p, nullptr, nullptr, nullptr, &errOut, nullptr);
            const bool ok = g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0;
            if (!ok) {
                gsize n = 0;
                const char* e = errOut ? static_cast<const char*>(g_bytes_get_data(errOut, &n)) : nullptr;
                *problem = (e && n) ? std::string(e, n) : argv[0] + " failed.";
            }
            if (errOut) g_bytes_unref(errOut);
            g_object_unref(p);
            return ok;
        };

        // GLib has no HTTPS client of its own, so the download goes through
        // curl or wget, one of which every desktop distribution has.
        std::vector<std::string> fetch;
        if (char* curl = g_find_program_in_path("curl")) {
            fetch = {curl, "-fsSL", "--proto", "=https", "--retry", "2", "-o", archive, url};
            g_free(curl);
        } else if (char* wget = g_find_program_in_path("wget")) {
            fetch = {wget, "-q", "--https-only", "-O", archive, url};
            g_free(wget);
        } else {
            d->problem = "Neither curl nor wget is installed, and one is needed to download.";
            return;
        }
        if (!run(fetch, &d->problem)) {
            d->problem = "The download failed.\n\n" + d->problem;
            g_remove(archive.c_str());
            return;
        }

        // The archive must be the one the release published, byte for byte.
        char* bytes = nullptr;
        gsize len = 0;
        std::string sum;
        if (g_file_get_contents(archive.c_str(), &bytes, &len, nullptr)) {
            char* s = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                  reinterpret_cast<const guchar*>(bytes), len);
            sum = s ? s : "";
            g_free(s);
            g_free(bytes);
        }
        if (sum != kTectonicSha256) {
            g_remove(archive.c_str());
            d->problem = "The download did not match the checksum published for tectonic " +
                         std::string(kTectonicVersion) + ", so it was thrown away.";
            return;
        }

        g_mkdir_with_parents(unpack.c_str(), 0755);
        const std::string unpacked = unpack + "/tectonic";
        const std::string dest = managedTectonicPath();
        bool ok = run({"tar", "-xzf", archive, "-C", unpack}, &d->problem);
        if (ok && !g_file_test(unpacked.c_str(), G_FILE_TEST_IS_REGULAR)) {
            d->problem = "The download did not contain tectonic.";
            ok = false;
        }
        if (ok) {
            g_chmod(unpacked.c_str(), 0755);
            if (g_rename(unpacked.c_str(), dest.c_str()) != 0) {
                d->problem = "Could not move tectonic into " + bin + ".";
                ok = false;
            }
        }
        g_remove(archive.c_str());
        // Whatever else the archive held (nothing, today) goes with the folder.
        GDir* dir = g_dir_open(unpack.c_str(), 0, nullptr);
        if (dir) {
            while (const char* name = g_dir_read_name(dir))
                g_remove((unpack + "/" + name).c_str());
            g_dir_close(dir);
        }
        g_rmdir(unpack.c_str());
        if (ok && !isExecutable(dest)) d->problem = "The download did not contain tectonic.";
    });
    g_object_unref(task);
}

void LatexPreview::finishDownload(const std::string& problem) {
    downloading_ = false;
    if (!problem.empty()) {
        showFailure("Could not install tectonic.\n\n" + problem +
                    "\n\nYou can also install it yourself and put it on your PATH, or name "
                    "it with MINICODE_TECTONIC.");
        setButton("Download…", true);
        return;
    }
    compileNow();
}

// ---------------------------------------------------------------- click -> source

void LatexPreview::onPdfClick(void* selfp, int page, double x, double y, int nPress) {
    LatexPreview* self = static_cast<LatexPreview*>(selfp);
    gtk_widget_grab_focus(self->root_);
    if (nPress == 2) self->doubleClick(page, x, y);
}

void LatexPreview::doubleClick(int page, double x, double y) {
    if (!sync_.valid()) {
        setStatus("No SyncTeX data, so the preview cannot be edited", false);
        return;
    }
    // SyncTeX's lines belong to the source the PDF was typeset from; with the
    // buffer ahead of it, they could point at the wrong text.
    if (bufferText() != pdfSrc_ || proc_) {
        if (!proc_) compileNow();
        setStatus("The preview is catching up with your edits; try again in a moment", true);
        return;
    }
    PopplerPage* pp = poppler_document_get_page(pdf_->document(), page);
    if (!pp) return;
    const PageText text = pageTextOf(pp);
    g_object_unref(pp);
    const int tag = sync_.tagForPath(syncScratch_);
    std::vector<SyncTexHit> hits;
    const LatexSpan* span = latexSpanAtPoint(doc_, sync_, tag, text, page + 1, x, y, &hits);
    if (!span) {
        setStatus("That is not text MiniCode can trace back to the source", false);
        return;
    }

    // Anchor the popover on the box the click landed in.
    double ax = x - 2, ay = y - 2, aw = 4, ah = 4;
    if (!hits.empty()) {
        const SyncTexHit& h = hits.front();
        ax = h.x;
        ay = h.y;
        aw = std::max(h.width, 4.0);
        ah = std::max(h.height, 4.0);
        // A box far from the click (a paragraph's) would put the popover
        // somewhere odd; then the click point itself is the anchor.
        if (x < ax - 2 || x > ax + aw + 2 || y < ay - 2 || y > ay + ah + 2) {
            ax = x - 2; ay = y - 2; aw = ah = 4;
        }
    }
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    GdkRectangle rect{0, 0, 1, 1};
    if (pdf_->pagePointIn(root_, page, ax, ay, &x1, &y1) &&
        pdf_->pagePointIn(root_, page, ax + aw, ay + ah, &x2, &y2)) {
        rect.x = (int)x1;
        rect.y = (int)y1;
        rect.width = std::max(1, (int)(x2 - x1));
        rect.height = std::max(1, (int)(y2 - y1));
    }

    editSpan_ = *span;
    editSrc_ = pdfSrc_;
    editValid_ = true;
    addingItem_ = false;
    editItem_ = span->itemIndex;
    const bool inList = span->listIndex >= 0 && (size_t)span->listIndex < doc_.lists.size();
    if (inList) editList_ = doc_.lists[(size_t)span->listIndex];
    editAnchor_ = rect;
    showPopover("Editing " + describeSpan(*span),
                pdfSrc_.substr(span->start, span->end - span->start), inList, rect);
}

// ---------------------------------------------------------------- the popover

void LatexPreview::showPopover(const std::string& title, const std::string& text,
                               bool allowsItem, const GdkRectangle& anchor) {
    gtk_label_set_text(GTK_LABEL(popLabel_), title.c_str());
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(popText_));
    gtk_text_buffer_set_text(b, text.c_str(), (int)text.size());
    gtk_widget_set_visible(popAdd_, allowsItem);
    gtk_popover_set_pointing_to(GTK_POPOVER(popover_), &anchor);
    gtk_popover_popup(GTK_POPOVER(popover_));
    gtk_widget_grab_focus(popText_);
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    gtk_text_buffer_select_range(b, &s, &e);
}

bool LatexPreview::editing() const {
    return editValid_ && gtk_widget_get_visible(popover_);
}

std::string LatexPreview::editText() const {
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(popText_));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

void LatexPreview::setEditText(const std::string& text) {
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(popText_)),
                             text.c_str(), (int)text.size());
}

void LatexPreview::cancelEdit() {
    editValid_ = false;
    gtk_popover_popdown(GTK_POPOVER(popover_));
}

void LatexPreview::startAddItem() {
    if (!editValid_) return;
    addingItem_ = true;
    showPopover("New list entry", "", false, editAnchor_);
}

void LatexPreview::commitEdit() {
    if (!editValid_) return;
    const std::string text = editText();
    const bool adding = addingItem_;
    editValid_ = false;
    gtk_popover_popdown(GTK_POPOVER(popover_));

    // The span's bytes index editSrc_. If the buffer changed while the
    // popover was open, they may no longer be the same text: refuse.
    const std::string current = bufferText();
    if (current != editSrc_) {
        setStatus("The source changed while you were editing, so nothing was replaced",
                  false);
        return;
    }
    LatexDoc::Edit edit;
    if (adding) {
        if (text.empty()) return;
        edit = LatexDoc::addItem(current, editList_, editItem_, text);
    } else {
        if (text == current.substr(editSpan_.start, editSpan_.end - editSpan_.start)) return;
        edit = LatexDoc::replaceSpan(current, editSpan_, text);
    }
    spliceBuffer(current, edit.source);
    setStatus(adding ? "Added a list entry" : "Typesetting…", true);
    compileNow();
}

// Turn `from` into `to` in the buffer by replacing only the bytes that differ,
// as one user action: one step for Ctrl+Z, and an ordinary edit for the
// highlighter and the unsaved-changes mark.
void LatexPreview::spliceBuffer(const std::string& from, const std::string& to) {
    std::size_t p = 0;
    const std::size_t limit = std::min(from.size(), to.size());
    while (p < limit && from[p] == to[p]) ++p;
    std::size_t s = 0;
    while (s < limit - p && from[from.size() - 1 - s] == to[to.size() - 1 - s]) ++s;
    // Both ends on character boundaries. The bytes either side of the change
    // are the same in both strings, so checking one is checking both.
    p = charStart(from, p);
    while (s > 0 && ((unsigned char)from[from.size() - s] & 0xC0) == 0x80) --s;
    const std::size_t fromEnd = from.size() - s, toEnd = to.size() - s;

    const glong a = g_utf8_pointer_to_offset(from.data(), from.data() + p);
    const glong b = a + g_utf8_pointer_to_offset(from.data() + p, from.data() + fromEnd);
    GtkTextIter ia, ib;
    gtk_text_buffer_get_iter_at_offset(buffer_, &ia, (int)a);
    gtk_text_buffer_get_iter_at_offset(buffer_, &ib, (int)b);
    gtk_text_buffer_begin_user_action(buffer_);
    gtk_text_buffer_delete(buffer_, &ia, &ib);
    const std::string ins = to.substr(p, toEnd - p);
    if (!ins.empty()) gtk_text_buffer_insert(buffer_, &ia, ins.c_str(), (int)ins.size());
    gtk_text_buffer_end_user_action(buffer_);
}

gboolean LatexPreview::onEditKey(GtkEventControllerKey*, guint keyval, guint,
                                 GdkModifierType state, gpointer selfp) {
    if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter) return FALSE;
    if (state & GDK_SHIFT_MASK) return FALSE;   // a newline
    static_cast<LatexPreview*>(selfp)->commitEdit();
    return TRUE;
}

gboolean LatexPreview::onUndo(GtkWidget*, GVariant*, gpointer selfp) {
    LatexPreview* self = static_cast<LatexPreview*>(selfp);
    if (!gtk_text_buffer_get_can_undo(self->buffer_)) return TRUE;
    gtk_text_buffer_undo(self->buffer_);
    self->setStatus("Undid an edit", true);
    self->compileNow();
    return TRUE;
}

gboolean LatexPreview::onRedo(GtkWidget*, GVariant*, gpointer selfp) {
    LatexPreview* self = static_cast<LatexPreview*>(selfp);
    if (!gtk_text_buffer_get_can_redo(self->buffer_)) return TRUE;
    gtk_text_buffer_redo(self->buffer_);
    self->setStatus("Redid an edit", true);
    self->compileNow();
    return TRUE;
}

#endif  // MINICODE_ENABLE_PDF
