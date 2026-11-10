// Document.cpp — see Document.h. The highlighting, the disk and the save
// were the single-buffer Editor's until tabs (PARITY.md 1.1); they are the
// same code, now per document.
#include "Document.h"

#include "MarkdownTasks.h"
#include "Utf8Offsets.h"
#ifdef MINICODE_ENABLE_PDF
#include "Latex.h"
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// The largest file the editor will open as text, 16 MB. Android stops at
// 4 MB; a desktop has room for more, but a GtkTextBuffer holding hundreds of
// megabytes (plus the highlighter's mirror and the saved copy) is not an
// editor any more, and reading one whole on the main thread freezes every
// window, since they all share this process.
constexpr off_t kMaxTextBytes = 16 * 1024 * 1024;

// Writes `text` into the existing file at `path`, keeping its inode (so hard
// links, owner, ACLs and extended attributes stay). Space is reserved before
// anything is overwritten, so a full disk fails with the old contents
// intact; only a crash midway can leave the file half written, which is
// why ordinary files take the atomic rename instead.
bool writeInPlace(const std::string& path, const std::string& text, off_t oldSize,
                  std::string& why) {
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) { why = strerror(errno); return false; }
    const off_t size = (off_t)text.size();
    if (size > oldSize) {
        int r = posix_fallocate(fd, 0, size);
        if (r != 0 && r != EOPNOTSUPP && r != EINVAL) {
            why = std::string("Not enough space to save: ") + strerror(r);
            close(fd);
            return false;
        }
    }
    size_t done = 0;
    while (done < text.size()) {
        ssize_t n = pwrite(fd, text.data() + done, text.size() - done, (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { why = strerror(errno); close(fd); return false; }
        done += (size_t)n;
    }
    if (ftruncate(fd, size) != 0 || fsync(fd) != 0) {
        why = strerror(errno);
        close(fd);
        return false;
    }
    if (close(fd) != 0) { why = strerror(errno); return false; }
    return true;
}

// The common start and end of two texts, in bytes, backed off to character
// starts: `p` bytes from the front and `q` from the back are the same.
void commonEnds(const std::string& a, const std::string& b, std::size_t& p, std::size_t& q) {
    const std::size_t n = std::min(a.size(), b.size());
    p = 0;
    while (p < n && a[p] == b[p]) ++p;
    while (p > 0 && p < a.size() && ((unsigned char)a[p] & 0xC0) == 0x80) --p;
    q = 0;
    while (q < n - p && a[a.size() - 1 - q] == b[b.size() - 1 - q]) ++q;
    while (q > 0 && ((unsigned char)a[a.size() - q] & 0xC0) == 0x80) --q;
}

}  // namespace

// ---------------------------------------------------------------- reading

Document::Read Document::readText(const std::string& path, std::string& out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return Read::Missing;
    if (!S_ISREG(st.st_mode)) return Read::NotRegular;
    if (st.st_size > kMaxTextBytes) return Read::TooLarge;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return Read::Failed;
    out.clear();
    out.reserve((size_t)st.st_size);
    char buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return Read::Failed; }
        if (n == 0) break;
        out.append(buf, (size_t)n);
        if ((off_t)out.size() > kMaxTextBytes) { close(fd); return Read::TooLarge; }
    }
    close(fd);
    // gtk_text_buffer_set_text requires valid UTF-8; handing it a binary file
    // spews GTK criticals and leaves the buffer cut at the first bad byte.
    if (!g_utf8_validate(out.data(), (gssize)out.size(), nullptr)) return Read::NotText;
    return Read::Ok;
}

std::string Document::extensionOf(const std::string& path) {
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return e;
}

// ---------------------------------------------------------------- life

Document::Document(DocumentHost* host, DocumentTags* tags) : host_(host), tags_(tags) {
    static uint64_t next = 0;
    id_ = ++next;
    buffer_ = gtk_text_buffer_new(tags_->table);
    gtk_text_buffer_set_enable_undo(buffer_, FALSE);
    // Highlighting follows every edit. The plain handlers run before the
    // buffer changes (both signals are RUN_LAST), while the iterators still
    // describe the old text; the _after ones run once it has changed.
    g_signal_connect(buffer_, "insert-text", G_CALLBACK(onInsertText), this);
    g_signal_connect(buffer_, "delete-range", G_CALLBACK(onDeleteRange), this);
    g_signal_connect_after(buffer_, "insert-text", G_CALLBACK(onInsertTextAfter), this);
    g_signal_connect_after(buffer_, "delete-range", G_CALLBACK(onDeleteRangeAfter), this);
    g_signal_connect(buffer_, "apply-tag", G_CALLBACK(onApplyTag), this);
    g_signal_connect(buffer_, "begin-user-action", G_CALLBACK(onBeginUserAction), this);
    g_signal_connect(buffer_, "end-user-action", G_CALLBACK(onEndUserAction), this);
    g_signal_connect(buffer_, "changed", G_CALLBACK(onChanged), this);
}

Document::~Document() {
    stopWatching();
    if (taskTimer_) g_source_remove(taskTimer_);
    taskTimer_ = 0;
    cancelDeferred();
#ifdef MINICODE_ENABLE_PDF
    delete latexPreview_;
#endif
    latexPreview_ = nullptr;
    g_signal_handlers_disconnect_by_data(buffer_, this);
    g_object_unref(buffer_);
    if (pageBuffer_) g_object_unref(pageBuffer_);
}

GtkTextBuffer* Document::page() {
    if (!pageBuffer_) {
        pageBuffer_ = gtk_text_buffer_new(tags_->table);
        gtk_text_buffer_set_enable_undo(pageBuffer_, FALSE);
    }
    return pageBuffer_;
}

// The buffer holds something new, set whole: not an edit, never undone into,
// never "unsaved".
void Document::clearSource() {
    stopWatching();
    stopHighlighting();
    if (taskTimer_) g_source_remove(taskTimer_);
    taskTimer_ = 0;
    tasksDone_ = tasksTotal_ = 0;
    diskKnown_ = false;
    std::string().swap(saved_);
    savedChars_ = 0;
    dirty_ = false;
    dirtyCheckPending_ = false;
    markdown_ = latex_ = false;
    showsPreview_ = false;
    page_ = Markdown::Page();
}

// gtk_text_buffer_set_text is irreversible (never undone into), and while it
// runs no handler counts it as an edit.
void Document::fill(const std::string& text) {
    filling_ = true;
    gtk_text_buffer_set_text(buffer_, text.c_str(), (int)text.size());
    filling_ = false;
}

void Document::loadText(const std::string& path, const std::string& content) {
    clearSource();
    kind_ = Kind::Text;
    path_ = path;
    ext_ = extensionOf(path);
    markdown_ = ext_ == "md" || ext_ == "markdown";
#ifdef MINICODE_ENABLE_PDF
    latex_ = ext_ == "tex" || ext_ == "ltx" || ext_ == "latex";
#endif
    rememberDisk(content);
    gtk_text_buffer_set_enable_undo(buffer_, TRUE);
    fill(content);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
    startHighlighting();
    watch();
    refreshTaskCount();
}

void Document::setMessage(const std::string& path, const std::string& message) {
    clearSource();
    kind_ = Kind::Message;
    path_ = path;
    ext_.clear();
    gtk_text_buffer_set_enable_undo(buffer_, FALSE);
    fill(message);
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    gtk_text_buffer_apply_tag_by_name(buffer_, "plainmsg", &a, &b);
}

void Document::setMedia(const std::string& path) {
    clearSource();
    kind_ = Kind::Media;
    path_ = path;
    ext_ = extensionOf(path);
    gtk_text_buffer_set_enable_undo(buffer_, FALSE);
    fill("");
}

void Document::setDiff(const std::string& title, const std::string& bytes, bool commit) {
    if (kind_ != Kind::Diff) {
        clearSource();
        gtk_text_buffer_set_enable_undo(buffer_, FALSE);
        fill("");
    }
    kind_ = Kind::Diff;
    path_.clear();
    ext_.clear();
    diffTitle_ = title;
    diffBytes_ = bytes;
    diffCommit_ = commit;
}

std::string Document::text() const {
    if (hl_ && !bulk_) return mirror_;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* raw = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    std::string s = raw ? raw : "";
    g_free(raw);
    return s;
}

void Document::spliceText(const std::string& to) {
    if (kind_ != Kind::Text) return;
    const std::string old = text();
    std::size_t p = 0, q = 0;
    commonEnds(old, to, p, q);
    if (p + q >= old.size() && p + q >= to.size()) return;   // the same text
    const long from = g_utf8_strlen(old.data(), (gssize)p);
    const long end = g_utf8_strlen(old.data(), (gssize)(old.size() - q));
    gtk_text_buffer_begin_user_action(buffer_);
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)from);
    gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)end);
    gtk_text_buffer_delete(buffer_, &a, &b);
    gtk_text_buffer_insert(buffer_, &a, to.data() + p, (int)(to.size() - p - q));
    gtk_text_buffer_end_user_action(buffer_);
}

// ---------------------------------------------------------------- unsaved

// Dirty means it differs from the disk, as on the Mac (noteEdited): the text
// as last read or saved is kept, and each edit compares with it, character
// counts first (GTK keeps the buffer's), so most keystrokes cost nothing;
// equal counts compare the bytes, a few milliseconds at the 16 MB limit.
// Inside a user action (a paste, typing over a selection, a reload) the
// comparison waits for its end.
void Document::rememberDisk(const std::string& content) {
    diskKnown_ = true;
    saved_ = content;
    savedChars_ = g_utf8_strlen(content.data(), (gssize)content.size());
}

bool Document::matchesSaved() {
    if (!diskKnown_) return false;
    if ((long)gtk_text_buffer_get_char_count(buffer_) != savedChars_) return false;
    if (hl_ && !bulk_) return mirror_ == saved_;
    return text() == saved_;
}

void Document::checkDirty() {
    dirtyCheckPending_ = false;
    if (kind_ != Kind::Text) return;
    markDirty(!matchesSaved());
}

void Document::markDirty(bool d) {
    if (dirty_ == d) return;
    dirty_ = d;
    host_->documentStateChanged(this);
}

void Document::onChanged(GtkTextBuffer*, gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (self->kind_ != Kind::Text || self->filling_) return;
    if (self->inAction_) self->dirtyCheckPending_ = true;
    else self->checkDirty();
    if (self->markdown_) self->scheduleTaskCount();
}

// ---------------------------------------------------------------- saving

bool Document::save(std::string* error) {
    // Not the file's text: a picture, a PDF, a message such as "Cannot
    // display". Saving a message used to write it over the file, which for
    // a binary file destroyed it.
    if (!canSave()) return true;
    const std::string body = text();

    // Write through a symlink to the file it names (a dotfile repo links its
    // files into place), and keep the file's permissions. The write itself is
    // atomic, a temporary file renamed over the old one, so a full disk or a
    // crash midway leaves the previous contents intact, as the macOS build's
    // writeToFile:atomically: does. A read-only file is refused rather than
    // quietly replaced by that rename.
    // A link to nothing is refused: saving would replace the link itself
    // with a regular file, and whatever it was meant to point at would never
    // get the edits.
    struct stat lst;
    char* resolved = realpath(path_.c_str(), nullptr);
    if (!resolved && lstat(path_.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode)) {
        if (error) *error = "The file is a link to something that does not exist.";
        return false;
    }
    const std::string target = resolved ? resolved : path_;
    free(resolved);
    int mode = 0666;
    bool inPlace = false;
    struct stat st;
    if (stat(target.c_str(), &st) == 0) {
        mode = st.st_mode & 07777;
        if (access(target.c_str(), W_OK) != 0) {
            if (error) *error = std::string("The file is not writable: ") + strerror(errno);
            return false;
        }
        // The atomic rename puts a new file in the old one's place. That new
        // file belongs to us and has no other names, so it would cut a hard
        // link, take a group-writable file away from its owner, and fail in
        // a folder we may not write to. Those files are written in place.
        char* dir = g_path_get_dirname(target.c_str());
        const bool dirWritable = access(dir, W_OK) == 0;
        g_free(dir);
        inPlace = st.st_nlink > 1 || st.st_uid != geteuid() || !dirWritable;
    }
    if (inPlace) {
        std::string why;
        if (!writeInPlace(target, body, (off_t)st.st_size, why)) {
            if (error) *error = why;
            return false;
        }
    } else {
        GError* err = nullptr;
        if (!g_file_set_contents_full(target.c_str(), body.data(), (gssize)body.size(),
                                      G_FILE_SET_CONTENTS_CONSISTENT, mode, &err)) {
            if (error) *error = err ? err->message : "Unknown error";
            g_clear_error(&err);
            return false;
        }
    }
    rememberDisk(body);   // so this save is not taken for someone else's
    markDirty(false);
    return true;
}

// ---------------------------------------------------------------- rename

bool Document::isSettingsFile() const {
    const std::string& settings = host_->settingsPath();
    if (path_.empty() || settings.empty()) return false;
    if (path_ == settings) return true;
    char a[PATH_MAX], b[PATH_MAX];
    return realpath(path_.c_str(), a) && realpath(settings.c_str(), b) && std::string(a) == b;
}

void Document::setPath(const std::string& path) {
    const bool wasSettings = isSettingsFile();
    const std::string ext = extensionOf(path);
    if (kind_ == Kind::Text) {
        // The name decides the mode, as when a file is opened: x.txt renamed
        // to x.md may show its page, and a preview whose file stops being
        // Markdown or LaTeX goes back to the source.
        markdown_ = ext == "md" || ext == "markdown";
#ifdef MINICODE_ENABLE_PDF
        latex_ = ext == "tex" || ext == "ltx" || ext == "latex";
#endif
        if (showsPreview_ && !markdown_ && !latex_) showsPreview_ = false;
    }
    // Keep comparing against the same disk contents, under the new name.
    const bool watched = monitor_ != nullptr;
    stopWatching();
    path_ = path;
    if (watched) watch();
    const bool regrammar = ext != ext_ || wasSettings != isSettingsFile();
    ext_ = ext;
    if (regrammar && kind_ == Kind::Text) startHighlighting();
    refreshTaskCount();   // x.txt renamed to x.md counts its tasks
    host_->documentStateChanged(this);
}

// ---------------------------------------------------------------- the disk
//
// The Mac checks when its window becomes key (EditorController.mm
// -checkExternalChange), comparing modification dates. Here the file is also
// watched, so a change made in the terminal panel under the editor shows up
// without leaving the window, and the check compares contents: an atomic save
// by MiniCode itself, or a tool that rewrites the file unchanged, is then no
// change at all, whatever the dates say. Every document watches its own file,
// so a clean tab in the background follows its file too, as on the Mac.

void Document::watch() {
    if (path_.empty() || monitor_) return;
    GFile* f = g_file_new_for_path(path_.c_str());
    // WATCH_MOVES, so a save that renames a temporary file over this one
    // (what MiniCode and most tools do) arrives as a rename, not a deletion.
    monitor_ = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(f);
    if (monitor_) g_signal_connect(monitor_, "changed", G_CALLBACK(onFileChanged), this);
}

void Document::stopWatching() {
    if (checkTimer_) g_source_remove(checkTimer_);
    checkTimer_ = 0;
    if (monitor_) {
        g_signal_handlers_disconnect_by_data(monitor_, this);
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
        monitor_ = nullptr;
    }
}

// A burst of events (write, close, rename) becomes one check, a moment after
// the last of them, when the writer is done.
void Document::onFileChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev,
                             gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    switch (ev) {
        case G_FILE_MONITOR_EVENT_CHANGED:
        case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_RENAMED:
        case G_FILE_MONITOR_EVENT_MOVED_IN:
            break;
        default:
            return;   // attribute changes, and deletion (the buffer stays, as on the Mac)
    }
    if (self->checkTimer_) g_source_remove(self->checkTimer_);
    self->checkTimer_ = g_timeout_add(200, [](gpointer p) -> gboolean {
        Document* d = static_cast<Document*>(p);
        d->checkTimer_ = 0;
        d->checkExternalChange();
        return G_SOURCE_REMOVE;
    }, self);
}

void Document::checkExternalChange() {
    if (kind_ != Kind::Text || path_.empty() || !diskKnown_) return;
    // Gone, unreadable, replaced by a pipe, grown past what the editor opens
    // or no longer text: what is in the editor stays, as on the Mac.
    std::string content;
    if (readText(path_, content) != Read::Ok) return;
    if (content == saved_) return;
    if (!dirty()) {
        applyDiskText(content);
        return;
    }
    // Edits here and a change there: the user decides. Once asked, this
    // version on disk is not asked about again, whatever the answer.
    if (host_->documentChangedOnDisk(this)) {
        rememberDisk(content);
        checkDirty();   // the change may be the very edits made here
    }
}

bool Document::reloadFromDisk() {
    if (kind_ != Kind::Text || path_.empty()) return false;
    std::string content;
    if (readText(path_, content) != Read::Ok) return false;
    applyDiskText(content);
    return true;
}

// The disk's text replaces the buffer's in place: only the bytes between the
// common start and end of the two are replaced, as one user action, so the
// highlighter and the language server see one edit, undo can take it back,
// and the caret stays where it was (a caret inside the replaced part keeps
// its line and column). The editor keeps the scroll position of one on show.
void Document::applyDiskText(const std::string& content) {
    host_->documentWillReload(this);
    rememberDisk(content);
    const std::string old = text();
    std::size_t p = 0, q = 0;
    commonEnds(old, content, p, q);
    if (p + q < old.size() || p + q < content.size()) {
        const long from = g_utf8_strlen(old.data(), (gssize)p);
        const long to = g_utf8_strlen(old.data(), (gssize)(old.size() - q));
        GtkTextIter cur;
        gtk_text_buffer_get_iter_at_mark(buffer_, &cur, gtk_text_buffer_get_insert(buffer_));
        const long curOff = gtk_text_iter_get_offset(&cur);
        const int curLine = gtk_text_iter_get_line(&cur);
        const int curCol = gtk_text_iter_get_line_offset(&cur);

        gtk_text_buffer_begin_user_action(buffer_);
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)from);
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)to);
        gtk_text_buffer_delete(buffer_, &a, &b);
        gtk_text_buffer_insert(buffer_, &a, content.data() + p, (int)(content.size() - p - q));
        gtk_text_buffer_end_user_action(buffer_);

        if (curOff > from && curOff < to) {
            GtkTextIter back;
            const int lines = gtk_text_buffer_get_line_count(buffer_);
            gtk_text_buffer_get_iter_at_line(buffer_, &back, std::min(curLine, lines - 1));
            const int len = gtk_text_iter_get_chars_in_line(&back);
            const bool last = gtk_text_iter_get_line(&back) == lines - 1;
            gtk_text_iter_set_line_offset(&back,
                std::min(curCol, std::max(0, last ? len : len - 1)));
            gtk_text_buffer_place_cursor(buffer_, &back);
        }
    }
    checkDirty();
    refreshTaskCount();
    host_->documentReloaded(this);
}

// ---------------------------------------------------------------- tasks
//
// Task lists ("- [ ] x"), over the shared MarkdownTasks core: the count in
// the window title.

void Document::refreshTaskCount() {
    if (taskTimer_) g_source_remove(taskTimer_);
    taskTimer_ = 0;
    int done = 0, total = 0;
    if (isMarkdown() && !path_.empty()) {
        const MarkdownTasks::Counts c = MarkdownTasks::count(text());
        done = c.done;
        total = c.total;
    }
    if (done == tasksDone_ && total == tasksTotal_) return;
    tasksDone_ = done;
    tasksTotal_ = total;
    host_->documentStateChanged(this);
}

void Document::scheduleTaskCount() {
    if (taskTimer_) g_source_remove(taskTimer_);
    taskTimer_ = g_timeout_add(300, [](gpointer p) -> gboolean {
        Document* self = static_cast<Document*>(p);
        self->taskTimer_ = 0;
        self->refreshTaskCount();
        return G_SOURCE_REMOVE;
    }, this);
}

// ---------------------------------------------------------------- highlighting

// The same scheme as the macOS build (textStorage:willProcessEditing: and
// flushHighlighting in EditorController.mm), in UTF-8 bytes rather than UTF-16:
//
// - startHighlighting() lexes the whole buffer once: on file load, when the
//   grammar changes, and when the settings change.
// - onInsertText / onDeleteRange run BEFORE the buffer changes, while the
//   iterators still point into the old text. They turn the edit into bytes,
//   apply it to mirror_ and to the IncrementalHighlighter (which re-lexes the
//   edited lines and any below whose start state changed), and widen the
//   pending byte range.
// - Once the buffer has changed (the _after handlers, or end-user-action when
//   the edit is part of one), flushHighlighting() retags the pending lines:
//   highlight tags off over exactly those lines, then the tokens on.
//
// Retagging is what costs: GTK takes a few microseconds per tag applied, so
// retagging every line of a 50,000-line file takes over a second. A small
// range is retagged at once; a large one (typing "/*" near the top, opening a
// big file) gets the lines on screen at once and the rest in idle time slices
// of a few milliseconds, the way GtkSourceView does it. The lexer's own state
// is always complete; only the tags lag. A document not on show has no lines
// on screen: the lines around its caret go first.
//
// Only the tags in tags_->highlight (one per token style, plus the settings
// file's swatches) are ever removed, so find matches, squiggles or any other
// tag survive.
//
// GtkTextIter speaks characters, and a byte index within its line. When GTK's
// lines are the highlighter's lines, byte offset = the highlighter's line start
// + the iterator's line index, with no scanning. They differ only in a file
// holding a lone '\r' or U+2029, which GTK also ends a line at; then offsets
// are found by counting characters through mirror_ instead.

namespace {
const size_t kSyncLines   = 1000;   // retag at once up to this many lines
const size_t kChunkLines  = 250;    // lines per step of the idle retag
const gint64 kSliceMicros = 5000;   // idle retag budget per main loop turn
const int    kBulkEdits   = 64;     // edits in one user action before resyncing whole

size_t byteOfChar(const std::string& s, long chars) {
    size_t b = 0;
    for (long c = 0; b < s.size(); ++b) {
        if (((unsigned char)s[b] & 0xC0) == 0x80) continue;   // continuation byte
        if (c++ == chars) break;
    }
    return b;
}

// Carry a byte range [s, e) across an edit that replaced [pos, pos + oldLen)
// with newLen bytes: shifted if it lay below, stretched if the edit touched it.
void carry(size_t& s, size_t& e, size_t pos, size_t oldLen, size_t newLen) {
    const size_t editEnd = pos + oldLen;
    if (s >= editEnd) {
        s = s + newLen - oldLen;
        e = e + newLen - oldLen;
    } else if (e > pos) {
        s = std::min(s, pos);
        e = std::max(e, editEnd) + newLen - oldLen;
    }
}
}  // namespace

bool Document::linesMatch() const {
    return hl_ && (size_t)gtk_text_buffer_get_line_count(buffer_) == hl_->lineCount();
}

size_t Document::byteOffsetOf(const GtkTextIter* it) const {
    if (linesMatch())
        return hl_->lineStart((size_t)gtk_text_iter_get_line(it)) +
               (size_t)gtk_text_iter_get_line_index(it);
    return byteOfChar(mirror_, gtk_text_iter_get_offset(it));
}

bool Document::isHighlightTag(GtkTextTag* tag) const {
    const std::vector<GtkTextTag*>& tags = tags_->highlight;
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

void Document::removeHighlightTags(const GtkTextIter* a, const GtkTextIter* b) {
    for (GtkTextTag* t : tags_->highlight) gtk_text_buffer_remove_tag(buffer_, t, a, b);
}

void Document::cancelDeferred() {
    if (idleId_) g_source_remove(idleId_);
    idleId_ = 0;
    deferred_ = false;
}

void Document::stopHighlighting() {
    hl_.reset();
    std::string().swap(mirror_);
    pending_ = false;
    bulk_ = false;
    cancelDeferred();
}

void Document::restartHighlighting() {
    if (kind_ == Kind::Text) startHighlighting();
}

void Document::startHighlighting() {
    pending_ = false;
    bulk_ = false;
    cancelDeferred();
    hl_.reset();
    std::string().swap(mirror_);
    if (kind_ != Kind::Text || ext_.empty() || !SyntaxHighlighter::supports(ext_)) {
        // No grammar (any more): no colors. Covers a rename to a plain .txt.
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buffer_, &a, &b);
        applyingTags_ = true;
        removeHighlightTags(&a, &b);
        applyingTags_ = false;
        return;
    }
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* text = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    mirror_ = text ? text : "";
    g_free(text);

    hl_ = std::make_unique<IncrementalHighlighter<char>>(ext_);
    hl_->reset(StringSource<char>(mirror_), nullptr);
    pendStart_ = 0;
    pendEnd_ = mirror_.size();
    pending_ = true;
    flushHighlighting();
}

// Record an edit already applied to mirror_: bytes [pos, pos + oldLen) became
// newLen bytes. The pending and deferred ranges are carried across it, then the
// lines the highlighter re-lexed join the pending range.
void Document::noteEdit(size_t pos, size_t oldLen, size_t newLen) {
    std::vector<Token> unused;   // recomputed for the final text at flush time
    auto r = hl_->edit(StringSource<char>(mirror_), pos, oldLen, newLen, unused);
    if (deferred_) carry(defStart_, defEnd_, pos, oldLen, newLen);
    if (pending_) {
        carry(pendStart_, pendEnd_, pos, oldLen, newLen);
        pendStart_ = std::min(pendStart_, r.start);
        pendEnd_ = std::max(pendEnd_, r.end);
    } else {
        pendStart_ = r.start;
        pendEnd_ = r.end;
        pending_ = true;
    }
}

// The highlighter's lines on screen, with some margin. Before the view has a
// size, or while another document is on show, the lines around the cursor.
void Document::visibleLines(size_t& first, size_t& end) const {
    const size_t margin = 60;
    GdkRectangle vis{0, 0, 0, 0};
    if (shownIn_) gtk_text_view_get_visible_rect(shownIn_, &vis);
    GtkTextIter top, bottom;
    if (vis.height > 0) {
        gtk_text_view_get_line_at_y(shownIn_, &top, vis.y, nullptr);
        gtk_text_view_get_line_at_y(shownIn_, &bottom, vis.y + vis.height, nullptr);
    } else {
        gtk_text_buffer_get_iter_at_mark(buffer_, &top, gtk_text_buffer_get_insert(buffer_));
        bottom = top;
    }
    const size_t t = (size_t)gtk_text_iter_get_line(&top);
    const size_t b = (size_t)gtk_text_iter_get_line(&bottom);
    first = t > margin ? t - margin : 0;
    end = b + margin + 1;
}

void Document::flushHighlighting() {
    if (!hl_ || !pending_ || bulk_) return;
    pending_ = false;
    const size_t len = hl_->length();
    const size_t s = std::min(pendStart_, len), e = std::min(pendEnd_, len);
    const size_t first = hl_->lineOf(s);
    const size_t end = e > s ? hl_->lineOf(e - 1) + 1 : first + 1;
    if (end - first <= kSyncLines) {
        retagSpan(first, end);
        return;
    }
    // Too many lines to retag in one go: the ones on screen now, all of them
    // later. (The visible ones are retagged twice; that is one chunk's work.)
    if (linesMatch()) {
        size_t vf, ve;
        visibleLines(vf, ve);
        vf = std::max(vf, first);
        ve = std::min(ve, end);
        if (vf < ve) retagSpan(vf, ve);
    } else {
        retagSpan(first, first + kSyncLines);
    }
    const size_t b0 = hl_->lineStart(first);
    const size_t b1 = end < hl_->lineCount() ? hl_->lineStart(end) : len;
    if (deferred_) {
        defStart_ = std::min(defStart_, b0);
        defEnd_ = std::max(defEnd_, b1);
    } else {
        defStart_ = b0;
        defEnd_ = b1;
        deferred_ = true;
    }
    // Below redraw and input, so typing and scrolling stay ahead of it.
    if (!idleId_) idleId_ = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, onIdleRetag, this, nullptr);
}

gboolean Document::onIdleRetag(gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (!self->hl_ || !self->deferred_ || self->bulk_) {
        self->idleId_ = 0;   // resyncAfterBulk schedules it again if needed
        return G_SOURCE_REMOVE;
    }
    const gint64 t0 = g_get_monotonic_time();
    IncrementalHighlighter<char>& hl = *self->hl_;
    while (g_get_monotonic_time() - t0 < kSliceMicros) {
        const size_t len = hl.length();
        const size_t s = std::min(self->defStart_, len), e = std::min(self->defEnd_, len);
        if (e <= s) {
            self->deferred_ = false;
            break;
        }
        const size_t first = hl.lineOf(s);
        const size_t end = std::min(first + kChunkLines, hl.lineOf(e - 1) + 1);
        self->retagSpan(first, end);
        self->defStart_ = end < hl.lineCount() ? hl.lineStart(end) : len;
    }
    if (self->deferred_) return G_SOURCE_CONTINUE;
    self->idleId_ = 0;
    return G_SOURCE_REMOVE;
}

void Document::retagSpan(size_t firstLine, size_t endLine) {
    endLine = std::min(endLine, hl_->lineCount());
    std::vector<Token> tokens;
    hl_->lineTokens(StringSource<char>(mirror_), firstLine, endLine, tokens);
    retagLines(firstLine, endLine, tokens);
}

// Lines [firstLine, endLine) of the highlighter: highlight tags off over
// exactly those lines, then `tokens` (byte offsets, ascending) on. Adjacent
// tokens of one style are applied together, since a comment spanning lines
// comes as one piece per line.
void Document::retagLines(size_t firstLine, size_t endLine, const std::vector<Token>& tokens) {
    if (!hl_ || firstLine >= endLine) return;
    const size_t b0 = hl_->lineStart(firstLine);
    const size_t b1 = endLine < hl_->lineCount() ? hl_->lineStart(endLine) : hl_->length();
    const bool match = linesMatch();
    GtkTextIter a, b;
    if (match) {
        gtk_text_buffer_get_iter_at_line(buffer_, &a, (int)firstLine);
    } else {
        Utf8OffsetCursor whole(mirror_);
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)whole.charOffset((long)b0));
    }
    const long c0 = gtk_text_iter_get_offset(&a);

    // Byte -> character conversion over just these lines, one forward pass.
    const std::string span = mirror_.substr(b0, b1 - b0);
    gtk_text_buffer_get_iter_at_offset(buffer_, &b,
                                       (int)(c0 + Utf8OffsetCursor(span).totalChars()));
    Utf8OffsetCursor cursor(span);

    applyingTags_ = true;
    removeHighlightTags(&a, &b);
    GtkTextIter ts = a, te = a;
    for (size_t i = 0; i < tokens.size();) {
        const size_t start = tokens[i].start;
        size_t end = start + tokens[i].length;
        const TokenStyle st = tokens[i].style;
        size_t j = i + 1;
        while (j < tokens.size() && tokens[j].style == st && tokens[j].start == end) {
            end = tokens[j].start + tokens[j].length;
            ++j;
        }
        i = j;
        if (start < b0 || end > b1 || end <= start) continue;
        gtk_text_iter_set_offset(&ts, (int)(c0 + cursor.charOffset((long)(start - b0))));
        gtk_text_iter_set_offset(&te, (int)(c0 + cursor.charOffset((long)(end - b0))));
        gtk_text_buffer_apply_tag(buffer_, tags_->highlight[(int)st], &ts, &te);
    }
    if (isSettingsFile()) {
        if (match) decorateColors((int)firstLine, (int)endLine);
        else       decorateColors(0, INT_MAX);
    }
    applyingTags_ = false;
}

// A user action with many edits in it (a paste from another GtkTextView is
// one insert per run of tags, thousands for a large one) stops being tracked
// edit by edit, since each costs a copy of the mirror. When the action ends,
// the whole of it is found as one edit: the common prefix and suffix of the
// old and new text bound the bytes it changed.
void Document::resyncAfterBulk() {
    bulk_ = false;
    if (!hl_) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* raw = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    std::string now = raw ? raw : "";
    g_free(raw);
    const size_t n = std::min(now.size(), mirror_.size());
    size_t p = 0;
    while (p < n && now[p] == mirror_[p]) ++p;
    size_t q = 0;
    while (q < n - p && now[now.size() - 1 - q] == mirror_[mirror_.size() - 1 - q]) ++q;
    const size_t oldLen = mirror_.size() - p - q, newLen = now.size() - p - q;
    mirror_ = std::move(now);
    if (oldLen || newLen) noteEdit(p, oldLen, newLen);
    flushHighlighting();
    if (deferred_ && !idleId_)
        idleId_ = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, onIdleRetag, this, nullptr);
}

// Every color value in the settings file becomes a swatch of itself: the value
// drawn on its own color, in black or white text, whichever reads.
// Lines [firstLine, endLine), clamped to the buffer.
void Document::decorateColors(int firstLine, int endLine) {
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    const Settings& settings = host_->documentSettings();
    int lines = std::min(endLine, gtk_text_buffer_get_line_count(buffer_));
    for (int ln = std::max(firstLine, 0); ln < lines; ++ln) {
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(buffer_, &ls, ln);
        le = ls;
        if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
        char* raw = gtk_text_buffer_get_text(buffer_, &ls, &le, FALSE);
        std::u16string line = utf16::fromUtf8(raw ? raw : "");
        g_free(raw);
        ColorSpan span;
        if (!Settings::findColor(line, span)) continue;

        Rgba text = settings.contrastText(span.color);
        std::string name = "swatch-" + Settings::formatColor(span.color) +
                           (text.rgb() == 0 ? "-k" : "-w");
        GtkTextTag* tag = gtk_text_tag_table_lookup(table, name.c_str());
        if (!tag) {
            GdkRGBA bg{span.color.r / 255.0f, span.color.g / 255.0f, span.color.b / 255.0f,
                       (float)span.color.a};
            GdkRGBA fg{text.r / 255.0f, text.g / 255.0f, text.b / 255.0f, (float)text.a};
            tag = gtk_text_buffer_create_tag(buffer_, name.c_str(), "background-rgba", &bg,
                                             "foreground-rgba", &fg, NULL);
            tags_->highlight.push_back(tag);
        }
        GtkTextIter a = ls, b = ls;
        gtk_text_iter_forward_chars(&a, (int)utf16::toCharOffset(line, span.start));
        gtk_text_iter_forward_chars(&b, (int)utf16::toCharOffset(line, span.start + span.length));
        gtk_text_buffer_apply_tag(buffer_, tag, &a, &b);
    }
}

// ------------------------------------------------ buffer signals

void Document::onInsertText(GtkTextBuffer*, GtkTextIter* loc, char* text, int len,
                            gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (!self->hl_ || len <= 0 || self->bulk_) return;
    if (self->inAction_ && ++self->actionEdits_ > kBulkEdits) {
        self->bulk_ = true;
        return;
    }
    const size_t pos = std::min(self->byteOffsetOf(loc), self->mirror_.size());
    self->mirror_.insert(pos, text, (size_t)len);
    self->noteEdit(pos, 0, (size_t)len);
}

void Document::onDeleteRange(GtkTextBuffer*, GtkTextIter* start, GtkTextIter* end,
                             gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (!self->hl_ || self->bulk_) return;
    if (self->inAction_ && ++self->actionEdits_ > kBulkEdits) {
        self->bulk_ = true;
        return;
    }
    size_t a = self->byteOffsetOf(start), b = self->byteOffsetOf(end);
    if (b < a) std::swap(a, b);
    a = std::min(a, self->mirror_.size());
    b = std::min(b, self->mirror_.size());
    if (a == b) return;
    self->mirror_.erase(a, b - a);
    self->noteEdit(a, b - a, 0);
}

// Inside a user action the retag waits for its end, so a delete-then-insert
// (typing over a selection, a paste) retags once. The host hears about each
// edit here, once the buffer has changed (never for a fill).
void Document::onInsertTextAfter(GtkTextBuffer*, GtkTextIter* loc, char* text, int len,
                                 gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (!self->inAction_) self->flushHighlighting();
    if (self->kind_ == Kind::Text && !self->filling_)
        self->host_->documentEdited(self, loc, text, len);
}

void Document::onDeleteRangeAfter(GtkTextBuffer*, GtkTextIter* a, GtkTextIter*, gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (!self->inAction_) self->flushHighlighting();
    if (self->kind_ == Kind::Text && !self->filling_)
        self->host_->documentEdited(self, a, nullptr, 0);
}

// GtkTextBuffer emits these for the outermost begin/end pair only.
void Document::onBeginUserAction(GtkTextBuffer*, gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    self->inAction_ = true;
    self->actionEdits_ = 0;
}

void Document::onEndUserAction(GtkTextBuffer*, gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    self->inAction_ = false;
    if (self->bulk_) self->resyncAfterBulk();
    else self->flushHighlighting();
    if (self->dirtyCheckPending_) self->checkDirty();
}

// Highlight tags belong to the highlighter alone. Pasting from a GtkTextView
// inserts the copied range with its tags (gtk_text_buffer_insert_range), which
// would lay the source's colors over the lines just retagged; stopping the
// emission keeps them off.
void Document::onApplyTag(GtkTextBuffer* buf, GtkTextTag* tag, GtkTextIter*, GtkTextIter*,
                          gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (self->kind_ == Kind::Text && !self->applyingTags_ && self->isHighlightTag(tag))
        g_signal_stop_emission_by_name(buf, "apply-tag");
}
