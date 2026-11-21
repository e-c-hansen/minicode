// Latex.cpp — see Latex.h.
#ifdef MUTIBLE_ENABLE_PDF

#include "EnvLock.h"
#include "Latex.h"

#include "LatexClick.h"
#include "MarkdownEdit.h"
#include "PageWords.h"
#include "Question.h"
#include "PdfView.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <poppler.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>

#include "DocumentView.h"
#include "LatexScratch.h"

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

std::string baseNameOf(const std::string& p) {
    char* b = g_path_get_basename(p.c_str());
    std::string out = b ? b : p;
    g_free(b);
    return out;
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
// after mutible quits (or crashes) would otherwise carry on alone.
void dieWithParent(gpointer) { prctl(PR_SET_PDEATHSIG, SIGTERM); }

GSubprocessLauncher* newLauncher(GSubprocessFlags flags) {
    GSubprocessLauncher* l;
    {
        EnvLock::EnvRead env;   // the launcher copies the environment (EnvLock.h)
        l = g_subprocess_launcher_new(flags);
    }
    g_subprocess_launcher_set_child_setup(l, dieWithParent, nullptr, nullptr);
    return l;
}

std::string fileText(const std::string& path) {
    char* data = nullptr;
    gsize len = 0;
    std::string out;
    if (g_file_get_contents(path.c_str(), &data, &len, nullptr)) out.assign(data, len);
    g_free(data);
    return out;
}

std::string sha256Hex(const std::string& s) {
    char* h = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                          reinterpret_cast<const guchar*>(s.data()), s.size());
    std::string out = h ? h : "";
    g_free(h);
    return out;
}

// When process `pid` started, in whole seconds after boot (field 22 of
// /proc/<pid>/stat, in ticks), or 0 when there is no such process. Against
// pid reuse: a record or folder names the pid and this.
long long processStart(long long pid) {
    const std::string stat = fileText("/proc/" + std::to_string(pid) + "/stat");
    const auto close = stat.rfind(')');
    if (close == std::string::npos) return 0;
    // After ") " field 3 (state) starts; starttime is the 20th token from there.
    const char* p = stat.c_str() + close + 1;
    for (int field = 3; field < 22; ++field) {
        while (*p == ' ') ++p;
        while (*p && *p != ' ') ++p;
    }
    while (*p == ' ') ++p;
    const long hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    return (long long)(std::strtoull(p, nullptr, 10) / (unsigned long long)hz) + 1;   // never 0
}

void removeTree(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            const std::string full = dir + "/" + n;
            GStatBuf st;
            if (g_lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) removeTree(full);
            else g_remove(full.c_str());
        }
        closedir(d);
    }
    g_rmdir(dir.c_str());
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
    std::string src, scratch, pdf, synctex, log;
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
    warnButton_ = gtk_button_new_with_label("");
    gtk_widget_add_css_class(warnButton_, "flat");
    gtk_widget_set_visible(warnButton_, FALSE);
    g_signal_connect_swapped(warnButton_, "clicked", G_CALLBACK(+[](gpointer self) {
        static_cast<LatexPreview*>(self)->showWarnings();
    }), this);
    gtk_box_append(GTK_BOX(bar), status_);
    gtk_box_append(GTK_BOX(bar), warnButton_);
    gtk_box_append(GTK_BOX(bar), button_);
    warnPopover_ = gtk_popover_new();
    gtk_widget_set_parent(warnPopover_, warnButton_);
    warnBox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* warnScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(warnScroll), warnBox_);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(warnScroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(warnScroll), 320);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(warnScroll), 420);
    gtk_popover_set_child(GTK_POPOVER(warnPopover_), warnScroll);
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
    // The failure page: the problems as rows over the log.
    logPage_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    problemsBox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(problemsBox_, "minicode-latex-problems");
    gtk_widget_set_visible(problemsBox_, FALSE);
    gtk_widget_set_vexpand(logScroll_, TRUE);
    gtk_box_append(GTK_BOX(logPage_), problemsBox_);
    gtk_box_append(GTK_BOX(logPage_), logScroll_);
    gtk_stack_add_named(GTK_STACK(stack_), logPage_, "log");
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
    if (forwardTimer_) g_source_remove(forwardTimer_);
    if (changedId_) g_signal_handler_disconnect(buffer_, changedId_);
    if (shutdownId_)
        if (GApplication* app = g_application_get_default())
            g_signal_handler_disconnect(app, shutdownId_);
    if (pdfBytes_) g_bytes_unref(pdfBytes_);
    gtk_widget_unparent(popover_);
    gtk_widget_unparent(warnPopover_);
    clearMarks();
    delete pdf_;
    removeOutDir();
    g_object_unref(root_);
}

void LatexPreview::onShutdown(GApplication*, gpointer selfp) {
    static_cast<LatexPreview*>(selfp)->stopRun();
}

// ---------------------------------------------------------------- tectonic

std::string LatexPreview::managedTectonicPath() {
    char* p = g_build_filename(g_get_user_data_dir(), "mutible", "bin", "tectonic", nullptr);
    std::string out = p;
    g_free(p);
    return out;
}

std::string LatexPreview::tectonicPath() {
    EnvLock::EnvRead lock;
    if (const char* env = g_getenv("MUTIBLE_TECTONIC"))
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
    // What Save writes: hidden text too (a fold, a long line's rest).
    char* t = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

// `.<file name>.<preview>.mutible.tex`: the whole name, extension included,
// so notes.tex and notes.ltx in one folder do not share a copy, and this
// preview's number, so two windows on one file do not either. Each run's
// removal of its copy would otherwise delete the other's input mid-typeset.
std::string LatexPreview::scratchPath() const {
    char* b = g_path_get_basename(path_.c_str());
    const std::string name = b ? b : "";
    g_free(b);
    const std::string tag = "." + std::to_string(instance_) + ".mutible.tex";
    if (path_.empty() || name.empty() || name == "." || name == "/")
        return outDir() + "/untitled" + tag;
    return dirName(path_) + "/." + name + tag;
}

namespace {
// The folder tectonic's output goes under: $XDG_RUNTIME_DIR/mutible-latex,
// which only the user can reach, or, with no runtime folder, one in /tmp that
// must be the user's own. A /tmp path anyone could have made first is used
// only if it is a real folder (not a link), owned by this user and closed to
// everyone else; otherwise a fresh private one from g_dir_make_tmp.
std::string outputBase() {
    const char* runtime = g_getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime && g_file_test(runtime, G_FILE_TEST_IS_DIR))
        return std::string(runtime) + "/mutible-latex";
    static std::string chosen;
    if (!chosen.empty()) return chosen;
    const std::string dir = std::string(g_get_tmp_dir()) + "/mutible-latex-" + g_get_user_name();
    g_mkdir(dir.c_str(), 0700);   // fails harmlessly if it is there already
    GStatBuf st;
    if (g_lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
        (st.st_mode & 077) == 0) {
        chosen = dir;
    } else {
        char* tmp = g_dir_make_tmp("mutible-latex-XXXXXX", nullptr);
        chosen = tmp ? tmp : dir;
        g_free(tmp);
    }
    return chosen;
}
}  // namespace

// <outputBase>/<a hash of the document's path>, so two documents with the
// same name in different folders never share output.
// (LatexScratch::folderName, as on the Mac): one folder for each preview, so
// two windows on one file never share a PDF, and a folder whose process has
// gone is found by the first typeset of a launch.
namespace {
LatexScratch::Owner thisOwner(unsigned view) {
    static const long long start = processStart(getpid());
    LatexScratch::Owner o;
    o.pid = getpid();
    o.start = start;
    o.view = view;
    return o;
}
}  // namespace

std::string LatexPreview::outDir() const {
    const std::string base = outputBase();
    const std::string dir = base + "/" + LatexScratch::folderName(
        sha256Hex(path_).substr(0, 16), thisOwner(instance_));
    g_mkdir_with_parents(dir.c_str(), 0700);
    return dir;
}

void LatexPreview::removeOutDir() {
    if (path_.empty() && instance_ == 0) return;
    removeTree(outputBase() + "/" + LatexScratch::folderName(
        sha256Hex(path_).substr(0, 16), thisOwner(instance_)));
}

// The first typeset of a launch: folders of previews whose process has gone
// (a crash) are removed, and with them the hidden sibling their record names,
// but only while it is still exactly the bytes recorded. Folders named by
// the hash alone (older builds) and live processes' are left.
namespace {
void sweepOldFolders(const std::string& base) {
    DIR* d = opendir(base.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) names.push_back(e->d_name);
    closedir(d);
    for (const std::string& n : names) {
        LatexScratch::Owner o;
        if (!LatexScratch::parseFolderName(n, &o)) continue;
        if (!LatexScratch::ownerGone(o, (double)processStart(o.pid))) continue;
        const std::string dir = base + "/" + n;
        LatexScratch::Record r;
        if (LatexScratch::parse(fileText(dir + "/sibling.record"), &r)) {
            const bool alive = processStart(r.pid) > 0 &&
                               std::abs((double)processStart(r.pid) - r.start) <= 1.0;
            const std::string now = fileText(r.sibling);
            GStatBuf st;
            const bool exists = g_stat(r.sibling.c_str(), &st) == 0;
            if (LatexScratch::decide(r, alive, exists, now.size(), sha256Hex(now)) ==
                    LatexScratch::Action::RemoveSibling &&
                LatexScratch::isPreviewCopyName(r.sibling))
                g_remove(r.sibling.c_str());
        }
        removeTree(dir);
    }
}
bool gSwept = false;   // this launch's first typeset has swept
}  // namespace

void LatexPreview::sweepAgainForTest() { gSwept = false; }

// Written before the sibling, so a crash after leaves the means to remove it.
void LatexPreview::writeRecord(const std::string& src) {
    if (!gSwept) {
        gSwept = true;
        sweepOldFolders(outputBase());
    }
    LatexScratch::Record r;
    r.sibling = scratchPath();
    r.pid = getpid();
    r.start = (double)thisOwner(instance_).start;
    r.size = src.size();
    r.sha256 = sha256Hex(src);
    const std::string text = LatexScratch::format(r);
    g_file_set_contents((outDir() + "/sibling.record").c_str(), text.data(), (gssize)text.size(),
                        nullptr);
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
    if ((!pdf_->hasDocument() || bufferText() != pdfSrc_ || missing_) && !typesettingBuffer())
        compileNow();
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
    clearMarks();
    problems_.clear();
    problemFiles_.clear();
    gtk_widget_set_visible(warnButton_, FALSE);
    removeOutDir();
    if (pdfBytes_) g_bytes_unref(pdfBytes_);
    pdfBytes_ = nullptr;
    pdfSrc_.clear();
    sync_ = SyncTexIndex();
    syncScratch_.clear();
    doc_ = LatexDoc();
    inputDocs_.clear();
    editFile_.clear();
    path_.clear();
    failure_.clear();
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "pdf");
    setStatus("", false);
    answerWaiters("The document was closed before it was typeset.");
}

void LatexPreview::onBufferChanged(GtkTextBuffer*, gpointer selfp) {
    LatexPreview* self = static_cast<LatexPreview*>(selfp);
    self->clearMarks();   // the lines were the typeset text's
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
    runSrc_.clear();
    // Taken away now, not when the killed run reports back: by then a new run
    // for the same document may have written it again.
    removeScratch();
}

// Remove the running typeset's hidden sibling. Through the descriptor of the
// folder it was written in, which still names that folder after a rename or
// a move to the trash, so the copy goes wherever the folder went; by path
// only if the folder could not be opened.
void LatexPreview::removeScratch() {
    if (!runRecord_.empty()) g_remove(runRecord_.c_str());
    runRecord_.clear();
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
        failure_ = "tectonic is not installed. The LaTeX preview offers to download it.";
        tellTypeset(nullptr, failure_);
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
    const std::string stem = baseStem(job->scratch);   // ".notes.mutible"
    job->pdf = out + "/" + stem + ".pdf";
    job->synctex = out + "/" + stem + ".synctex.gz";

    writeRecord(job->src);
    runRecord_ = out + "/sibling.record";
    job->log = out + "/" + stem + ".log";

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
    proc_ = g_subprocess_launcher_spawn(l, &err, tool.c_str(), "--untrusted", "--synctex", "--keep-logs", "--chatter",
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
    runSrc_ = job->src;
    runScratch_ = job->scratch;
    runDirFd_ = open(dirName(job->scratch).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char* name = g_path_get_basename(job->scratch.c_str());
    runScratchName_ = name ? name : "";
    g_free(name);
    setStatus("Typesetting…", true);
    tellTypeset(nullptr, "");
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
    runSrc_.clear();

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

    // TeX's transcript: read for its warnings and removed, whatever came of
    // the run (--keep-logs leaves it beside the PDF).
    const std::string transcript = fileText(job->log);
    g_remove(job->log.c_str());
    setProblems(log, transcript, job->src, job->scratch, ok);

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
                failure_.clear();
                showDocument();
                tellTypeset(pdfBytes_, "");
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
    refreshWarnings(problem.empty());

    if (queued_) compileNow();
    else answerWaiters(problem.empty() ? "" : "The document did not typeset. The preview "
                                              "shows tectonic's log.");
}

void LatexPreview::showDocument() {
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "pdf");
    setButton("Recompile", true);
    showPagesStatus();
}

// The page count and what to do, or which files the typeset read have
// unsaved changes: it read them from disk, so the page shows them only once
// they are saved (the Mac's showPagesStatus).
void LatexPreview::showPagesStatus() {
    const int n = pdf_->pageCount();
    std::string text = n == 1 ? "1 page" : std::to_string(n) + " pages";
    std::vector<std::string> unsaved;
    if (inputs_.unsaved)
        for (const std::string& f : inputFiles())
            if (inputs_.unsaved(f)) unsaved.push_back(baseNameOf(f));
    std::string tail = "double-click text to edit it";
    if (unsaved.size() == 1)
        tail = unsaved[0] + " has unsaved changes: save to see them in the PDF";
    else if (unsaved.size() > 1)
        tail = std::to_string(unsaved.size()) +
               " files it reads have unsaved changes: save to see them in the PDF";
    setStatus(text + " · " + tail, false);
}

// ---------------------------------------------------------------- problems

void LatexPreview::setProblems(const std::string& output, const std::string& transcript,
                               const std::string& src, const std::string& sibling, bool) {
    clearMarks();
    problems_ = LatexLog::collect(output, transcript);
    problemFiles_.clear();
    auto exists = [](const std::string& f) { return g_file_test(f.c_str(), G_FILE_TEST_IS_REGULAR); };
    for (const LatexLog::Problem& pr : problems_)
        problemFiles_.push_back(LatexLog::sourcePath(pr.file, path_, sibling, exists));
    if (bufferText() == src) markLines(src);
    typesetProblems_ = true;
}

void LatexPreview::markLines(const std::string&) {
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    static const struct { const char* name; const char* color; } kinds[2] = {
        {"latex-warning", "#CCA700"}, {"latex-error", "#F14C4C"}};
    GtkTextTag* tags[2];
    for (int i = 0; i < 2; ++i) {
        tags[i] = gtk_text_tag_table_lookup(table, kinds[i].name);
        if (!tags[i]) {
            GdkRGBA c;
            gdk_rgba_parse(&c, kinds[i].color);
            tags[i] = gtk_text_buffer_create_tag(buffer_, kinds[i].name, "underline",
                                                 PANGO_UNDERLINE_ERROR, "underline-rgba", &c, NULL);
        }
    }
    const int lines = gtk_text_buffer_get_line_count(buffer_);
    for (size_t i = 0; i < problems_.size(); ++i) {
        const LatexLog::Problem& pr = problems_[i];
        if (pr.line < 1 || pr.line > lines || problemFiles_[i] != path_) continue;
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_line(buffer_, &a, pr.line - 1);
        b = a;
        gtk_text_iter_forward_to_line_end(&b);
        if (gtk_text_iter_equal(&a, &b) && !gtk_text_iter_forward_char(&b)) continue;
        gtk_text_buffer_apply_tag(buffer_, tags[pr.severity == LatexLog::Severity::Error ? 1 : 0],
                                  &a, &b);
        marked_ = true;
    }
}

void LatexPreview::clearMarks() {
    if (!marked_) return;
    marked_ = false;
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    for (const char* n : {"latex-error", "latex-warning"})
        if (GtkTextTag* t = gtk_text_tag_table_lookup(table, n))
            gtk_text_buffer_remove_tag(buffer_, t, &a, &b);
}

void LatexPreview::fillRows(GtkWidget* box, bool warnings) {
    while (GtkWidget* c = gtk_widget_get_first_child(box)) gtk_box_remove(GTK_BOX(box), c);
    int shown = 0;
    for (size_t i = 0; i < problems_.size(); ++i) {
        const LatexLog::Problem& pr = problems_[i];
        const bool isWarning = pr.severity == LatexLog::Severity::Warning;
        if (warnings && !isWarning) continue;
        char* base = problemFiles_[i].empty() ? nullptr : g_path_get_basename(problemFiles_[i].c_str());
        std::string where = base ? base : "";
        g_free(base);
        if (pr.line > 0) where += (where.empty() ? "line " : ":") + std::to_string(pr.line);
        std::string text = std::string(isWarning ? "warning  " : "error  ") +
                           (where.empty() ? "" : where + "  ") + pr.message;
        GtkWidget* row = gtk_button_new();
        GtkWidget* label = gtk_label_new(text.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_button_set_child(GTK_BUTTON(row), label);
        gtk_widget_add_css_class(row, "flat");
        gtk_widget_set_tooltip_text(row, pr.message.c_str());
        const bool linked = pr.line > 0 && !problemFiles_[i].empty();
        gtk_widget_set_sensitive(row, linked);
        g_object_set_data(G_OBJECT(row), "mc-problem", GINT_TO_POINTER((int)i + 1));
        g_signal_connect(row, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer selfp) {
            LatexPreview* self = static_cast<LatexPreview*>(selfp);
            const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "mc-problem")) - 1;
            gtk_popover_popdown(GTK_POPOVER(self->warnPopover_));
            if (i >= 0 && i < (int)self->problems_.size() && self->openProblem_)
                self->openProblem_(self->problemFiles_[i], self->problems_[i].line);
        }), this);
        gtk_box_append(GTK_BOX(box), row);
        ++shown;
    }
    gtk_widget_set_visible(box, shown > 0 || box == warnBox_);
}

bool LatexPreview::pressRow(GtkWidget* box, int index) {
    GtkWidget* c = gtk_widget_get_first_child(box);
    for (int i = 0; c && i < index; ++i) c = gtk_widget_get_next_sibling(c);
    if (!c || !gtk_widget_get_sensitive(c)) return false;
    g_signal_emit_by_name(c, "clicked");
    return true;
}

bool LatexPreview::openProblemRow(int index) { return pressRow(problemsBox_, index); }
bool LatexPreview::openWarningRow(int index) { return pressRow(warnBox_, index); }

int LatexPreview::warningCount() const {
    int n = 0;
    for (const LatexLog::Problem& p : problems_) n += p.severity == LatexLog::Severity::Warning;
    return n;
}
bool LatexPreview::warningsShown() const { return gtk_widget_get_visible(warnButton_); }
std::string LatexPreview::warningsLabel() const { return gtk_button_get_label(GTK_BUTTON(warnButton_)); }
void LatexPreview::showWarnings() {
    if (warningsShown()) gtk_popover_popup(GTK_POPOVER(warnPopover_));
}

// After a typeset that worked: its warnings behind the bar's button.
void LatexPreview::refreshWarnings(bool good) {
    const int n = good ? warningCount() : 0;
    if (n > 0) {
        fillRows(warnBox_, true);
        const std::string t = std::to_string(n) + (n == 1 ? " warning" : " warnings");
        gtk_button_set_label(GTK_BUTTON(warnButton_), t.c_str());
    }
    gtk_widget_set_visible(warnButton_, n > 0);
    if (n == 0) gtk_popover_popdown(GTK_POPOVER(warnPopover_));
    typesetProblems_ = false;
}

void LatexPreview::showFailure(const std::string& log) {
    failure_ = log.empty() ? "The document did not typeset." : log;
    tellTypeset(nullptr, failure_);
    if (!typesetProblems_) {   // not a typeset's: no problems to list
        problems_.clear();
        problemFiles_.clear();
        clearMarks();
    }
    fillRows(problemsBox_, false);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(logView_)),
                             log.c_str(), (int)log.size());
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "log");
    setStatus("Did not typeset — see the log", false);
    setButton("Recompile", true);
}

void LatexPreview::showTectonicMissing() {
    missing_ = true;
    problems_.clear();
    problemFiles_.clear();
    clearMarks();
    gtk_widget_set_visible(warnButton_, FALSE);
    fillRows(problemsBox_, false);
    std::string text =
        "The LaTeX preview needs tectonic, a single-binary TeX engine.\n\n"
        "mutible uses whichever tectonic is already on your machine (on your PATH, "
        "or named by MUTIBLE_TECTONIC)";
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
    return gtk_stack_get_visible_child(GTK_STACK(stack_)) == logPage_;
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
    // Skip the typing pause, and queue behind a run of older text. A run of
    // this very text answers when it ends: queuing another typeset the same
    // text twice (Show Line in PDF pressed while the preview's own typeset
    // ran, linux/tests/gui/LatexForwardSuite.cpp).
    if (!typesettingBuffer()) compileNow();
}

bool LatexPreview::typesettingBuffer() const {
    return proc_ && runSrc_ == bufferText();
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

static std::string realPathOf(const std::string& p) {
    char* r = realpath(p.c_str(), nullptr);
    std::string out = r ? r : p;
    free(r);
    return out;
}

bool LatexPreview::readsFile(const std::string& file) const {
    if (!sync_.valid() || path_.empty()) return false;
    if (realPathOf(file) == realPathOf(path_)) return true;
    return sync_.tagForFile(file, dirName(path_), realPathOf) != 0;
}

void LatexPreview::forwardSearch(const std::string& file, int line) {
    if (path_.empty() || line < 1) return;
    std::shared_ptr<LatexPreview*> alive = alive_;
    pdfForBuffer([alive, file, line](GBytes* pdf, const std::string& error) {
        LatexPreview* self = *alive;
        if (!self) return;
        if (!pdf) { self->setStatus(error, false); return; }
        self->doForward(file, line);
    });
}

void LatexPreview::doForward(const std::string& file, int line) {
    forwardFound_ = 0;
    int tag = 0;
    if (file.empty() || realPathOf(file) == realPathOf(path_)) tag = sync_.tagForPath(syncScratch_);
    else tag = sync_.tagForFile(file, dirName(path_), realPathOf);
    if (!tag) { setStatus("This file is not part of the typeset document", false); return; }
    int found = 0;
    const std::vector<SyncTexBox> boxes = sync_.boxesForLine(tag, line, &found);
    if (boxes.empty() || found == 0) {
        setStatus("Line " + std::to_string(line) + " put nothing on the page", false);
        return;
    }
    forwardFound_ = found;
    std::vector<PdfView::Highlight> hl;
    for (const SyncTexBox& b : boxes) hl.push_back({b.page - 1, b.x, b.y, b.width, b.height});
    pdf_->setHighlights(hl);
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "pdf");
    PdfView::Anchor a;
    a.page = hl[0].page;
    a.y = std::max(0.0, hl[0].y - 80);
    pdf_->scrollTo(a);
    if (forwardTimer_) g_source_remove(forwardTimer_);
    forwardTimer_ = g_timeout_add(1500, [](gpointer p) -> gboolean {
        LatexPreview* self = static_cast<LatexPreview*>(p);
        self->forwardTimer_ = 0;
        self->pdf_->setHighlights({});
        return G_SOURCE_REMOVE;
    }, this);
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
                // The problem can name a path: Question wraps inside it.
                Question::tell(parent, "The PDF was not exported", problem);
            });
        }, ask);
    g_object_unref(dlg);
}

// ---------------------------------------------------------------- downloading

// The download itself, blocking: tectonic into the managed folder, or why not
// in *problemOut (empty on success). Runs on a worker thread.
static void fetchTectonicBlocking(std::string* problemOutP) {
    std::string& problem = *problemOutP;
        const std::string bin = dirName(LatexPreview::managedTectonicPath());
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
        char* curl = nullptr;
        char* wget = nullptr;
        {
            EnvLock::EnvRead env;
            curl = g_find_program_in_path("curl");
            if (!curl) wget = g_find_program_in_path("wget");
        }
        if (curl) {
            fetch = {curl, "-fsSL", "--proto", "=https", "--retry", "2", "-o", archive, url};
            g_free(curl);
        } else if (wget) {
            fetch = {wget, "-q", "--https-only", "-O", archive, url};
            g_free(wget);
        } else {
            problem = "Neither curl nor wget is installed, and one is needed to download.";
            return;
        }
        if (!run(fetch, &problem)) {
            problem = "The download failed.\n\n" + problem;
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
            problem = "The download did not match the checksum published for tectonic " +
                         std::string(kTectonicVersion) + ", so it was thrown away.";
            return;
        }

        g_mkdir_with_parents(unpack.c_str(), 0755);
        const std::string unpacked = unpack + "/tectonic";
        const std::string dest = LatexPreview::managedTectonicPath();
        bool ok = run({"tar", "-xzf", archive, "-C", unpack}, &problem);
        if (ok && !g_file_test(unpacked.c_str(), G_FILE_TEST_IS_REGULAR)) {
            problem = "The download did not contain tectonic.";
            ok = false;
        }
        if (ok) {
            g_chmod(unpacked.c_str(), 0755);
            if (g_rename(unpacked.c_str(), dest.c_str()) != 0) {
                problem = "Could not move tectonic into " + bin + ".";
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
        if (ok && !isExecutable(dest)) problem = "The download did not contain tectonic.";
}

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
        fetchTectonicBlocking(&d->problem);
    });
    g_object_unref(task);
}

void LatexPreview::finishDownload(const std::string& problem) {
    downloading_ = false;
    if (!problem.empty()) {
        showFailure("Could not install tectonic.\n\n" + problem +
                    "\n\nYou can also install it yourself and put it on your PATH, or name "
                    "it with MUTIBLE_TECTONIC.");
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

std::string LatexPreview::inputFileForTag(int tag) const {
    if (tag == 0 || !sync_.valid() || path_.empty()) return "";
    const std::string written = sync_.pathForTag(tag);
    if (written.empty()) return "";
    const std::string dir = dirName(path_);
    std::string p = written[0] == '/' ? written : dir + "/" + written;
    if (!g_file_test(p.c_str(), G_FILE_TEST_IS_REGULAR)) return "";
    const std::string real = realPathOf(p);
    if (real == realPathOf(path_) || real == realPathOf(syncScratch_)) return "";
    // tectonic writes the kernel's spelling of the folder; the tab and the
    // tree have the window's.
    const std::string realDir = realPathOf(dir) + "/";
    if (real.compare(0, realDir.size(), realDir) == 0) return dir + "/" + real.substr(realDir.size());
    return p;
}

std::vector<std::string> LatexPreview::inputFiles() const {
    std::vector<std::string> files;
    if (!sync_.valid()) return files;
    for (const auto& kv : sync_.inputs()) {
        const std::string f = inputFileForTag(kv.first);
        if (!f.empty() && std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f);
    }
    return files;
}

// The \input file's source, parsed, from its tab's buffer or the disk; null
// when it cannot be read. Parsed again only when its text changed.
const LatexDoc* LatexPreview::docOfInputFile(const std::string& file, std::string* source) {
    std::string src;
    bool have = inputs_.text && inputs_.text(file, &src);
    if (!have) {
        gchar* data = nullptr;
        gsize n = 0;
        if (!g_file_get_contents(file.c_str(), &data, &n, nullptr)) return nullptr;
        src.assign(data, n);
        g_free(data);
        if (!g_utf8_validate(src.data(), (gssize)src.size(), nullptr)) return nullptr;
    }
    auto& slot = inputDocs_[file];
    if (slot.first != src || slot.second.spans.empty()) {
        slot.first = src;
        slot.second = LatexDoc::parseInput(src);
    }
    if (source) *source = src;
    return &slot.second;
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
    const int mainTag = sync_.tagForPath(syncScratch_);
    // The document's own text, or an \input file's: whichever the boxes
    // under the point name first that can place the click (the Mac's
    // MCLatexSpanAtPointInFiles).
    std::vector<SyncTexHit> hits;
    const LatexSpan* span = nullptr;
    std::string file, fileSrc;
    bool first = true;
    for (int tag : sync_.tagsAtPoint(page + 1, x, y, mainTag)) {
        const LatexDoc* doc = nullptr;
        std::string f, src;
        if (tag == mainTag) {
            doc = &doc_;
        } else {
            f = inputFileForTag(tag);
            if (f.empty()) continue;
            doc = docOfInputFile(f, &src);
        }
        if (!doc) continue;
        std::vector<SyncTexHit> h;
        const LatexSpan* s = latexSpanAtPoint(*doc, sync_, tag, text, page + 1, x, y, &h);
        if (first || s) hits = h;
        first = false;
        if (s) {
            span = s;
            file = f;
            fileSrc = src;
            break;
        }
    }
    if (!span) {
        setStatus("That is not text mutible can trace back to the source", false);
        return;
    }
    editFile_ = file;
    editInputSrc_ = fileSrc;
    const std::string& src = file.empty() ? pdfSrc_ : editInputSrc_;
    const LatexDoc& spanDoc = file.empty() ? doc_ : inputDocs_[file].second;

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
    editSrc_ = src;
    editValid_ = true;
    addingItem_ = false;
    editItem_ = span->itemIndex;
    // An \input file's span is in that file's text, and its lists.
    const bool inList = span->listIndex >= 0 && (size_t)span->listIndex < spanDoc.lists.size();
    if (inList) editList_ = spanDoc.lists[(size_t)span->listIndex];
    editAnchor_ = rect;
    const std::size_t a = std::min(span->start, src.size());
    std::string title = "Editing " + describeSpan(*span);
    if (!file.empty()) title += " in " + baseNameOf(file);
    showPopover(title, src.substr(a, std::min(span->end, src.size()) - a), inList, rect);
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

std::string LatexPreview::editTitle() const { return gtk_label_get_text(GTK_LABEL(popLabel_)); }

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

// The box's edit made on `now`, the text as it is when it is applied: its
// span (adding an entry, its list) found again there from `base`, the text
// the box opened on (MarkdownEdit::rebaseSpan). 1 with `*out` made, 0 when
// there is nothing to change, -1 when the span is not in `now` once,
// exactly as the box showed it.
static int latexEditOnto(const std::string& base, const std::string& now, LatexSpan span,
                         LatexList list, bool adding, int itemIndex, const std::string& text,
                         LatexDoc::Edit* out) {
    if (adding) {
        if (text.empty()) return 0;
        const auto r = MarkdownEdit::rebaseSpan(base, list.start, list.end, now);
        if (r.first == std::string::npos) return -1;
        const std::size_t from = list.start;
        for (std::size_t& at : list.itemBodyStart) at = at - from + r.first;
        for (std::size_t& at : list.itemBodyEnd) at = at - from + r.first;
        list.end = r.second;
        list.start = r.first;
        *out = LatexDoc::addItem(now, list, itemIndex, text);
        return 1;
    }
    const auto r = MarkdownEdit::rebaseSpan(base, span.start, span.end, now);
    if (r.first == std::string::npos) return -1;
    span.start = r.first;
    span.end = r.second;
    if (now.compare(span.start, span.end - span.start, text) == 0) return 0;
    *out = LatexDoc::replaceSpan(now, span, text);
    return 1;
}

// An edit that could not go in: its text on the clipboard and the bar
// saying why, never dropped unseen (the Mac's keepLostEdit:file:, its words).
void LatexPreview::keepLostEdit(const std::string& text, const std::string& file) {
    if (!text.empty())
        gdk_clipboard_set_text(gtk_widget_get_clipboard(popText_), text.c_str());
    const std::string name = file.empty() ? std::string("The file") : baseNameOf(file);
    setStatus(name + " changed while the box was open, and so did the text you edited, so "
                     "your edit was not applied; your text is on the clipboard",
              false);
}

void LatexPreview::commitEdit() {
    if (!editValid_) return;
    const std::string text = editText();
    const bool adding = addingItem_;
    editValid_ = false;
    gtk_popover_popdown(GTK_POPOVER(popover_));
    if (!editFile_.empty()) {
        commitInputEdit(text, adding);
        return;
    }

    // The span's bytes index editSrc_, the text the box opened on. The edit
    // is made on the buffer as it is now (an agent may have rewritten some
    // other part), the span found again in it; refused only when the span
    // itself changed.
    const std::string current = bufferText();
    LatexDoc::Edit edit;
    const int made = latexEditOnto(editSrc_, current, editSpan_, editList_, adding, editItem_,
                                   text, &edit);
    if (made < 0) {
        // Only a draft that differs from what the box opened with is lost.
        const std::string& was = editSrc_;
        const bool changed = editSpan_.end > was.size() || editSpan_.start > editSpan_.end ||
                             was.compare(editSpan_.start, editSpan_.end - editSpan_.start, text) != 0;
        if (adding || changed) keepLostEdit(text, path_);
        return;
    }
    if (made == 0) return;
    spliceBuffer(current, edit.source);
    setStatus(adding ? "Added a list entry" : "Typesetting…", true);
    compileNow();
}

// Turn `from` into `to` in the buffer by replacing only the bytes that differ,
// as one user action: one step for Ctrl+Z, and an ordinary edit for the
// highlighter and the unsaved-changes mark.
void LatexPreview::spliceInto(GtkTextBuffer* buffer, const std::string& from, const std::string& to) {
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
    gtk_text_buffer_get_iter_at_offset(buffer, &ia, (int)a);
    gtk_text_buffer_get_iter_at_offset(buffer, &ib, (int)b);
    gtk_text_buffer_begin_user_action(buffer);
    Document::nameStep(buffer, "Edit in Preview");
    gtk_text_buffer_delete(buffer, &ia, &ib);
    const std::string ins = to.substr(p, toEnd - p);
    if (!ins.empty()) gtk_text_buffer_insert(buffer, &ia, ins.c_str(), (int)ins.size());
    gtk_text_buffer_end_user_action(buffer);
}

void LatexPreview::spliceBuffer(const std::string& from, const std::string& to) {
    spliceInto(buffer_, from, to);
}

// The popover's edit of an \\input file: a splice of that file's text, put
// into its tab by the host (made behind the preview when there was none),
// unsaved, one undo step there. Refused when the file's text changed while
// the box was open.
void LatexPreview::commitInputEdit(const std::string& text, bool adding) {
    const std::string file = editFile_;
    const std::string name = baseNameOf(file);
    std::string now;
    const bool read = inputs_.text && inputs_.text(file, &now);
    if (!read) {
        gchar* data = nullptr;
        gsize n = 0;
        if (g_file_get_contents(file.c_str(), &data, &n, nullptr)) {
            now.assign(data, n);
            g_free(data);
        } else {
            now = "\x01";   // unreadable: never the text the box opened on
        }
    }
    LatexDoc::Edit edit;
    const int made = latexEditOnto(editInputSrc_, now, editSpan_, editList_, adding, editItem_,
                                   text, &edit);
    if (made < 0) {
        // Only a draft that differs from what the box opened with is lost.
        const std::string& was = editInputSrc_;
        const bool changed = editSpan_.end > was.size() || editSpan_.start > editSpan_.end ||
                             was.compare(editSpan_.start, editSpan_.end - editSpan_.start, text) != 0;
        if (adding || changed) keepLostEdit(text, file);
        return;
    }
    if (made == 0) return;
    if (!inputs_.edit || !inputs_.edit(file, now, edit.source)) {
        setStatus(name + " could not be changed", false);
        return;
    }
    showPagesStatus();
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
    Document* d = Document::of(self->buffer_);
    if (!d || d->undo() != Document::UndoResult::Done) return TRUE;
    self->setStatus("Undid an edit", true);
    self->compileNow();
    return TRUE;
}

gboolean LatexPreview::onRedo(GtkWidget*, GVariant*, gpointer selfp) {
    LatexPreview* self = static_cast<LatexPreview*>(selfp);
    Document* d = Document::of(self->buffer_);
    if (!d || d->redo() != Document::UndoResult::Done) return TRUE;
    self->setStatus("Redid an edit", true);
    self->compileNow();
    return TRUE;
}

#endif  // MUTIBLE_ENABLE_PDF

// The same download for a caller with no preview (the Markdown preview's math
// line): `done` runs on the main loop with the problem, "" when it worked.
void LatexPreview::downloadTectonicThen(std::function<void(const std::string& problem)> done) {
    if (!kTectonicArch) { done("There is no official tectonic build for this processor."); return; }
    struct Ctx { std::string problem; std::function<void(const std::string&)> done; };
    GTask* task = g_task_new(nullptr, nullptr,
        [](GObject*, GAsyncResult* res, gpointer) {
            auto* c = static_cast<Ctx*>(g_task_get_task_data(G_TASK(res)));
            c->done(c->problem);
        }, nullptr);
    g_task_set_task_data(task, new Ctx{"", std::move(done)},
                         [](gpointer p) { delete static_cast<Ctx*>(p); });
    g_task_run_in_thread(task, [](GTask*, gpointer, gpointer data, GCancellable*) {
        fetchTectonicBlocking(&static_cast<Ctx*>(data)->problem);
    });
    g_object_unref(task);
}
