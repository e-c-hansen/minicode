// DocumentView.cpp — see DocumentView.h. The highlighting, the disk and the save
// were the single-buffer Editor's until tabs (PARITY.md 1.1); they are the
// same code, now per document.
#include "DocumentView.h"
#include "CsvTable.h"
#include "FileSniff.h"
#include "JsonOutline.h"

#include "BackupStore.h"
#include "BracketColors.h"
#include "BufferLines.h"
#include "Folds.h"
#include "LargeFileView.h"
#include "MultiEdit.h"
#include "SemanticColors.h"
#include "WrapIndentTags.h"
#include "SessionFiles.h"
#include "SessionLink.h"
#include "MarkdownTasks.h"
#include "StartupLog.h"
#include "Utf8Offsets.h"
#ifdef MUTIBLE_ENABLE_PDF
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

// The largest file the editor will open as text (LargeFileView.h): 256 MB.
// Android stops at 4 MB; past a size like this a GtkTextBuffer (plus the
// highlighter's mirror and the saved copy) is not an editor any more, and
// every window shares this process. Files over 4 MB are read on a worker
// thread (Document::beginLoading), so none of this holds the main loop.
constexpr off_t kMaxTextBytes = (off_t)LargeFileView::kMaxTextBytes;

// The buffer's document (Document::of), and how long a name given for the
// next step waits for it: a paste's text comes from the clipboard a moment
// after the key, and a name nothing took must not land on later typing.
const char kDocumentKey[] = "minicode-document";
constexpr gint64 kPendingNameUs = 2 * G_USEC_PER_SEC;

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

Document::Read Document::readText(const std::string& path, std::string& out,
                                  FileEncoding* encoding) {
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
    // The core's rules, the Mac's (MCReadDiskText): UTF-16 with its byte
    // order mark decoded exactly, bytes that are not UTF-8 and hold no NUL
    // read as Windows-1252 (or, mostly UTF-8, as UTF-8 with the stray bytes
    // kept: Utf8Raw), a UTF-8 mark kept apart. UTF-16 that does not
    // decode (a lone surrogate, an odd byte) is not text.
    FileEncoding found;
    if (!FolderSearch::decodeFileText(&out, &found.encoding, &found.bom)) return Read::NotText;
    // gtk_text_buffer_set_text requires valid UTF-8; handing it a binary file
    // spews GTK criticals and leaves the buffer cut at the first bad byte.
    // decodeFileText leaves a file that is neither (a NUL in bytes that are
    // not UTF-8) as it is, and a NUL is refused here too.
    if (!g_utf8_validate(out.data(), (gssize)out.size(), nullptr)) return Read::NotText;
    if (encoding) *encoding = found;
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

Document::Document(DocumentHost* host, DocumentTags* tags, std::shared_ptr<SessionLink::Link> link)
    : Document(host, tags, std::move(link), 0) {}

Document::Document(DocumentHost* host, DocumentTags* tags, std::shared_ptr<SessionLink::Link> link,
                   uint64_t heldId)
    : host_(host), tags_(tags), link_(std::move(link)) {
    // Its id on the session's host, from the client's range (a second
    // renderer on the host never makes the same one), or the held one's.
    id_ = heldId ? heldId : client().newId();
    buffer_ = gtk_text_buffer_new(tags_->table);
    // GTK's undo stays off: every edit goes to the session's host (undo()).
    gtk_text_buffer_set_enable_undo(buffer_, FALSE);
    g_object_set_data(G_OBJECT(buffer_), kDocumentKey, this);
    // Where its lines start, for the gutter; the buffer keeps it. Then its
    // folds (Folds.h), whose handlers come first of all after an edit: they
    // fix the fold tag before the host's own (an electric dedent) edit again.
    BufferLines::attach(buffer_);
    folds_ = std::make_unique<DocumentFolds>(this, buffer_);
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
    // The caret moved by anything but an edit: typing starts a new step.
    g_signal_connect(buffer_, "mark-set", G_CALLBACK(onMarkSet), this);
    // Its long lines shown a part at a time (LargeFileView.h); the buffer
    // keeps them.
    LongLines::attach(buffer_);
    brackets_ = std::make_unique<BracketColors>(buffer_);
    semantic_ = std::make_unique<SemanticColors>(buffer_);
    // Wrapped lines keep their indentation (WrapIndentTags.h): on once it
    // holds source text (wrapIndentSettingChanged, from startHighlighting).
    wrap_ = std::make_unique<WrapIndentTags>(buffer_, [this](size_t& first, size_t& end) {
        visibleLines(first, end);
    });
    // An empty text until it holds something: the session's host has it
    // from the start, so typing into a new file is followed too. A held
    // one's text comes back as one insertion (attachHeld waits for it).
    client().open(id_, "", "", this);
    enrol();
}

void Document::enrol() { link_->listen(id_, this); }
void Document::unenrol() { link_->unlisten(id_, this); }

void Document::attachHeld(const std::string& path, bool hostNamed) {
    clearSource();
    kind_ = Kind::Text;
    path_ = path;
    // The host has it by this path already, or (an untitled buffer) by none
    // until it is saved here.
    hostPath_ = hostNamed ? path : std::string();
    ext_ = extensionOf(path);
    markdown_ = ext_ == "md" || ext_ == "markdown";
#ifdef MUTIBLE_ENABLE_PDF
    latex_ = ext_ == "tex" || ext_ == "ltx" || ext_ == "latex";
#endif
    // The host's text, as one insertion from the host (Client::open's
    // answer), which records nothing.
    ++edits_;
    client().settle();
    const std::string text = this->text();
    sniffCsv(text);
    std::string disk;
    if (readText(path, disk, &encoding_) == Read::Ok) {
        rememberDisk(disk);
        // The buffer counts unsaved against the file as it is now, on the
        // host too (stage 7: the host's count and the tab's dot agree).
        tellHostSaved(&disk);
    }
    noteLargeFile(text);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
    startHighlighting();
    watch();
    refreshTaskCount();
    checkDirty();
}

void Document::detachFromHost() {
    Shared::Client& c = client();
    if (!c.isOpen(id_) || link_->lost()) return;
    closeOutside();   // the step this turn opened goes first
    c.detach(id_);
    detached_ = true;
}

void Document::rehost(std::shared_ptr<SessionLink::Link> link) {
    if (!link || link == link_) return;
    const bool wasOpen = client().isOpen(id_);
    if (wasOpen) client().close(id_);   // nothing reaches a lost host; the entry goes
    if (outsideIdle_) g_source_remove(outsideIdle_);
    outsideIdle_ = 0;
    outsideOpen_ = false;
    unenrol();
    link_ = std::move(link);
    id_ = client().newId();
    enrol();
    hostPath_.clear();
    if (kind_ != Kind::Text) return;
    // The file's text as last read or saved is the host's saved text, and
    // the buffer's difference from it an edit outside the history, so the
    // host counts it unsaved (its lifetime keeps an unsaved file).
    const std::string now = text();
    client().open(id_, path_, now, this);
    hostPath_ = path_;
    hostSavedFromDisk(now);
    if (stepOpen_) client().openStep(id_, {"Typing", false}, selection());
}

// The host's saved text made the file's (as last read or saved) when the
// buffer holds `now`, which differs from it (a buffer opened anew on a host,
// or a backup restored): the host's copy is set to the file's text and the
// difference goes as an edit outside the history, so the host counts it
// unsaved, and its lifetime keeps an unsaved file (a headless host after
// the app quits). The mirror already holds `now`.
void Document::hostSavedFromDisk(const std::string& now) {
    if (kind_ != Kind::Text || !client().isOpen(id_)) return;
    // A file not on disk (never saved, or gone) has nothing saved: all of the
    // buffer is unsaved, so the host keeps it (and backs it up once no
    // window has it) rather than taking it for saved.
    const std::string saved = diskKnown_ ? saved_ : std::string();
    if (saved == now) return;
    client().reset(id_, saved);
    std::size_t p = 0, q = 0;
    commonEnds(saved, now, p, q);
    const std::size_t from = g_utf8_strlen(saved.data(), (gssize)p);
    const std::size_t del = g_utf8_strlen(saved.data() + p, (gssize)(saved.size() - p - q));
    client().recordOutside(id_, Ot::Op{from, del, Model::utf16FromUtf8(now.substr(p, now.size() - p - q))});
}

void Document::tellHostPath() {
    if (kind_ != Kind::Text || path_ == hostPath_ || !client().isOpen(id_)) return;
    hostPath_ = path_;
    client().setPath(id_, path_);
}

Document::~Document() {
    // Saved, or let go when asked: its unsaved text is not kept any more. A
    // crash or a SIGKILL never gets here, and leaves the backup.
    if (backup_.written) BackupStore::shared().remove(backup_.id);
    unenrol();
    if (writeIdle_) g_source_remove(writeIdle_);
    if (dirtyIdle_) g_source_remove(dirtyIdle_);
    writeIdle_ = dirtyIdle_ = 0;
    if (!detached_ && client().isOpen(id_)) client().close(id_);
    folds_.reset();
    wrap_.reset();   // while the buffer is there to leave
    cancelLoading();
    stopWatching();
    if (taskTimer_) g_source_remove(taskTimer_);
    taskTimer_ = 0;
    cancelDeferred();
#ifdef MUTIBLE_ENABLE_PDF
    delete latexPreview_;
#endif
    latexPreview_ = nullptr;
    if (outsideIdle_) g_source_remove(outsideIdle_);
    outsideIdle_ = 0;
    g_signal_handlers_disconnect_by_data(buffer_, this);
    g_object_set_data(G_OBJECT(buffer_), kDocumentKey, nullptr);
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
    cancelLoading();
    largeFile_ = largeOff_ = largeOn_ = false;
    csv_ = 0;
    showsTable_ = false;
    html_ = showsPage_ = false;
    json_ = notebook_ = false;
    showsOutline_ = true;
    stopWatching();
    stopHighlighting();
    if (taskTimer_) g_source_remove(taskTimer_);
    taskTimer_ = 0;
    tasksDone_ = tasksTotal_ = 0;
    diskKnown_ = false;
    encoding_ = FileEncoding();
    std::string().swap(saved_);
    savedChars_ = 0;
    dirty_ = false;
    dirtyCheckPending_ = false;
    diskStamp_ = DiskStamp();
    diskQuestion_ = false;
    unread_ = false;
    markdown_ = latex_ = false;
    showsPreview_ = false;
    page_ = Markdown::Page();
}

// Set whole: never undone into (the history starts over), and while it
// runs no handler counts it as an edit.
void Document::fill(const std::string& text) {
    ++edits_;
    filling_ = true;
    gtk_text_buffer_set_text(buffer_, text.c_str(), (int)text.size());
    filling_ = false;
    resetHistory(text);
}

void Document::loadText(const std::string& path, const std::string& content,
                        const FileEncoding& encoding) {
    clearSource();
    kind_ = Kind::Text;
    path_ = path;
    encoding_ = encoding;
    ext_ = extensionOf(path);
    markdown_ = ext_ == "md" || ext_ == "markdown";
#ifdef MUTIBLE_ENABLE_PDF
    latex_ = ext_ == "tex" || ext_ == "ltx" || ext_ == "latex";
#endif
    sniffCsv(content);
    rememberDisk(content);
    noteLargeFile(content);
    fill(content);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
    startHighlighting();
    watch();
    refreshTaskCount();
    StartupLog::once("first-text", "editor: first file in its buffer and highlighted (%s, %zu bytes)",
                     ext_.empty() ? "no extension" : ext_.c_str(), content.size());
}

// A CSV or TSV by FileSniff's rules shows as a table, unless it is past the
// table's limit.
void Document::sniffCsv(const std::string& text) {
    const FileSniff::Kind k = FileSniff::classify(path_, text.data(),
                                                  std::min(text.size(), FileSniff::kHeadBytes));
    csv_ = k == FileSniff::Kind::Csv ? 1 : k == FileSniff::Kind::Tsv ? 2 : 0;
    showsTable_ = csv_ != 0 && !text.empty() && text.size() <= CsvTable::kMaxTableBytes;
    if (!showsTable_) csv_ = 0;
    json_ = k == FileSniff::Kind::Json && text.size() <= kMaxOutlineBytes;
    notebook_ = k == FileSniff::Kind::Notebook;
#ifdef MUTIBLE_ENABLE_BROWSER
    html_ = k == FileSniff::Kind::Html;
    showsPage_ = html_ && !text.empty();   // an empty file opens in its source
#endif
}

bool Document::jsonc() const { return JsonOutline::allowsComments(path_); }

char Document::csvDelimiter(const std::string& text) const {
    return csv_ == 2 ? '\t' : Csv::detectDelimiter(text);
}

void Document::setMessage(const std::string& path, const std::string& message) {
    clearSource();
    kind_ = Kind::Message;
    path_ = path;
    ext_.clear();
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
    fill("");
}

void Document::setDatabase(const std::string& path) {
    clearSource();
    kind_ = Kind::Database;
    path_ = path;
    ext_ = extensionOf(path);
    fill("");
}

void Document::setDiff(const std::string& title, const std::string& bytes, bool commit) {
    if (kind_ != Kind::Diff) {
        clearSource();
        fill("");
    }
    kind_ = Kind::Diff;
    path_.clear();
    ext_.clear();
    diffTitle_ = title;
    diffBytes_ = bytes;
    diffCommit_ = commit;
    compare_.reset();
    editorPage_.reset();
}

void Document::setEditorPage(const std::string& title, std::shared_ptr<EditorPage> page) {
    setDiff(title, "", false);
    editorPage_ = std::move(page);
}

void Document::setCompare(const std::string& title, std::shared_ptr<const CompareData> data) {
    setDiff(title, "", false);
    compare_ = std::move(data);
}

void Document::setUnread(const std::string& path) {
    clearSource();
    kind_ = Kind::Message;
    path_ = path;
    ext_ = extensionOf(path);
    fill("");
    unread_ = true;
}

// The Mac's takeBackup and showRestoredText in one: the backup's text is the
// buffer, set whole, so there is nothing to undo, and the disk's text is
// what it is compared with.
void Document::restoreText(const std::string& path, const std::string& text,
                           const std::string* disk, const DiskStamp& base, bool changed) {
    clearSource();
    kind_ = Kind::Text;
    path_ = path;
    ext_ = extensionOf(path);
    markdown_ = ext_ == "md" || ext_ == "markdown";
#ifdef MUTIBLE_ENABLE_PDF
    latex_ = ext_ == "tex" || ext_ == "ltx" || ext_ == "latex";
#endif
    sniffCsv(text);
    if (disk) rememberDisk(*disk);
    // Gone, or changed since: what the backup was based on stays the stamp,
    // so the next backup still says so.
    if (!disk || changed) diskStamp_ = base;
    noteLargeFile(text);
    fill(text);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_place_cursor(buffer_, &start);
    startHighlighting();
    watch();
    refreshTaskCount();
    checkDirty();
    hostSavedFromDisk(text);   // the host counts the backup's text unsaved too
}

void Document::askAboutDiskChange() {
    if (kind_ == Kind::Text && dirty()) diskQuestion_ = true;
}

bool Document::goneFromDisk() const {
    struct stat st;
    return diskStamp_.exists && !path_.empty() && stat(path_.c_str(), &st) != 0;
}

bool Document::changedOnDisk() const {
    if (kind_ != Kind::Text || path_.empty()) return false;
    struct stat st;
    const bool exists = stat(path_.c_str(), &st) == 0;
    // Never read here (gone when it came back from a backup): a file there
    // now is someone else's.
    if (!diskKnown_) return exists;
    if (!exists) return false;   // gone: goneFromDisk says so
    const double mtime = (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
    if (diskStamp_.exists && mtime == diskStamp_.mtime && (uint64_t)st.st_ino == diskStamp_.inode &&
        (int64_t)st.st_size == diskStamp_.size)
        return false;
    std::string content;
    if (readText(path_, content) != Read::Ok) return true;
    return content != saved_;
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
    nameStep("Preview Edit");
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
void Document::rememberDisk(const std::string& content, const FolderSearch::FileStamp* written) {
    diskKnown_ = true;
    saved_ = content;
    savedChars_ = g_utf8_strlen(content.data(), (gssize)content.size());
    diskQuestion_ = false;
    diskStamp_ = DiskStamp();
    struct stat st;
    if (written && written->exists) {
        // The stamp the write left, so a writer after it is a change.
        diskStamp_.exists = true;
        diskStamp_.mtime = (double)written->mtimeSec + written->mtimeNsec / 1e9;
        diskStamp_.inode = written->inode;
        diskStamp_.size = (int64_t)written->size;
    } else if (!path_.empty() && stat(path_.c_str(), &st) == 0) {
        diskStamp_.exists = true;
        diskStamp_.mtime = (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
        diskStamp_.inode = (uint64_t)st.st_ino;
        diskStamp_.size = (int64_t)st.st_size;
    }
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
    ++self->edits_;
    if (self->inAction_) self->dirtyCheckPending_ = true;
    else self->checkDirty();
    if (self->markdown_) self->scheduleTaskCount();
}

// ---------------------------------------------------------------- undo
//
// The core's UndoHistory (src/UndoHistory.h) in place of GtkTextHistory,
// in characters as GtkTextIter counts. Every insert and delete of the
// source is recorded from the buffer's signals, before the buffer changes
// (the deleted text is read then). A user action is one step: typing when
// no one named it (it joins the step before while the caret stays where
// that left it, as NSTextView's typing does), else a step of its own. An
// undo is the history's plan put in through MultiEdit::apply, one user
// action that is not recorded, so the highlighter, the folds, the change
// bars and the language server follow it as they follow any edit.

Document* Document::of(GtkTextBuffer* buffer) {
    return buffer ? static_cast<Document*>(g_object_get_data(G_OBJECT(buffer), kDocumentKey))
                  : nullptr;
}

bool Document::recording() const {
    return kind_ == Kind::Text && !filling_ && !replaying_ && !load_;
}

Model::UndoHistory::Selection Document::selection() const {
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_mark(buffer_, &ins, gtk_text_buffer_get_insert(buffer_));
    gtk_text_buffer_get_iter_at_mark(buffer_, &bound, gtk_text_buffer_get_selection_bound(buffer_));
    return {Model::UndoHistory::Range{(std::size_t)gtk_text_iter_get_offset(&bound),
                                      (std::size_t)gtk_text_iter_get_offset(&ins)}};
}

// Edits outside a user action (code calling gtk_text_buffer_insert, a
// test's set_text) are one step a turn of the main loop, as the Mac's
// NSUndoManager groups by event.
void Document::recordEdit(std::size_t at, std::size_t removed, std::u16string inserted) {
    Shared::Client& c = client();
    if (!stepOpen_ && !outsideOpen_) {
        Model::UndoHistory::StepInfo info{"Edit", false};
        if (pendingAt_ && g_get_monotonic_time() - pendingAt_ < kPendingNameUs) info = pendingStep_;
        pendingAt_ = 0;
        c.openStep(id_, info, selection());
        outsideOpen_ = true;
        outsideIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE, [](gpointer p) -> gboolean {
            Document* d = static_cast<Document*>(p);
            d->outsideIdle_ = 0;
            d->closeOutside();
            return G_SOURCE_REMOVE;
        }, this, nullptr);
    }
    c.record(id_, Ot::Op{at, removed, std::move(inserted)});
}

void Document::closeOutside() {
    if (!outsideOpen_) return;
    outsideOpen_ = false;
    if (outsideIdle_) g_source_remove(outsideIdle_);
    outsideIdle_ = 0;
    client().closeStep(id_, selection());
}

// The text was set whole (a file read, a message): nothing to undo, and the
// host's copy starts again from it. A step open around it goes on, as a new
// step on the new text.
void Document::resetHistory(const std::string& text) {
    if (outsideIdle_) g_source_remove(outsideIdle_);
    outsideIdle_ = 0;
    outsideOpen_ = false;
    pendingAt_ = 0;
    Shared::Client& c = client();
    if (kind_ != Kind::Text) {
        if (c.isOpen(id_)) c.close(id_);
        hostPath_.clear();
        return;
    }
    if (c.isOpen(id_)) {
        c.reset(id_, text);
    } else {
        c.open(id_, path_, text, this);
        hostPath_ = path_;
        if (stepOpen_) c.openStep(id_, {"Typing", false}, selection());
    }
    tellHostPath();   // a reattaching window finds it by its path
}

void Document::breakUndoCoalescing() { client().breakCoalescing(id_); }

void Document::nameStep(const char* name, bool coalesce) {
    if (replaying_ || !name) return;
    if (stepOpen_) {
        if (!stepNamed_) client().renameStep(id_, {name, coalesce});
        stepNamed_ = true;
        return;
    }
    closeOutside();
    pendingStep_ = {name, coalesce};
    pendingAt_ = g_get_monotonic_time();
}

void Document::nameStep(GtkTextBuffer* buffer, const char* name, bool coalesce) {
    if (Document* d = of(buffer)) d->nameStep(name, coalesce);
}

bool Document::canUndo() {
    closeOutside();
    Shared::Client& c = client();
    return kind_ == Kind::Text && c.isOpen(id_) && c.undoInfo(id_).canUndo;
}

bool Document::canRedo() {
    closeOutside();
    Shared::Client& c = client();
    return kind_ == Kind::Text && c.isOpen(id_) && c.undoInfo(id_).canRedo;
}

std::string Document::undoName() {
    return canUndo() ? client().undoInfo(id_).undoName : std::string();
}

std::string Document::redoName() {
    return canRedo() ? client().undoInfo(id_).redoName : std::string();
}

Document::UndoResult Document::undo() { return takeStep(true); }
Document::UndoResult Document::redo() { return takeStep(false); }

// The host takes the step back (or puts it back) and sends the edit, which
// hostEdited puts in the buffer before this returns.
Document::UndoResult Document::takeStep(bool undoing) {
    if (kind_ != Kind::Text || loading() || stepOpen_) return UndoResult::Nothing;
    closeOutside();
    Shared::Client& c = client();
    if (!c.isOpen(id_)) return UndoResult::Nothing;
    const Shared::Outcome o = c.undo(id_, !undoing);
    if (o == Shared::Outcome::Nothing) return UndoResult::Nothing;
    if (o == Shared::Outcome::Refused) {
        g_printerr("mutible: %s in %s refused: the text it would change is not what it left "
                   "there, so the history was forgotten\n",
                   undoing ? "undo" : "redo", path_.empty() ? "a new file" : path_.c_str());
        return UndoResult::Refused;
    }
    return UndoResult::Done;
}

void Document::hostEdited(Shared::DocId, const Ot::Ops& ops, int flag, const Shared::Selection* select) {
    // Snippet sessions, a completion waiting for its import: they listen
    // for the buffer's own signals, as they did for GTK's undo.
    if (flag) g_signal_emit_by_name(buffer_, flag == 2 ? "redo" : "undo");
    replaying_ = true;
    // An undo's plan comes as replacements apart in the text (last first):
    // one MultiEdit, which the highlighter takes as many small edits. Any
    // other shape goes in an operation at a time.
    std::vector<Ot::Replacement> reps;
    bool done = false;
    if (Ot::asReplacements(ops, Model::Unit::CodePoint, &reps)) {
        std::vector<MultiEdit::Change> changes;
        changes.reserve(reps.size());
        for (const Ot::Replacement& r : reps)
            changes.push_back({r.start, r.length, Model::utf8FromUtf16(r.text)});
        done = changes.empty() || MultiEdit::apply(buffer_, changes, false);
    }
    if (!done) {
        gtk_text_buffer_begin_user_action(buffer_);
        for (const Ot::Op& o : ops) {
            GtkTextIter a, b;
            gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)o.pos);
            if (o.del) {
                gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)(o.pos + o.del));
                gtk_text_buffer_delete(buffer_, &a, &b);
            }
            if (!o.ins.empty()) {
                const std::string t = Model::utf8FromUtf16(o.ins);
                gtk_text_buffer_insert(buffer_, &a, t.data(), (int)t.size());
            }
        }
        gtk_text_buffer_end_user_action(buffer_);
    }
    if (select && !select->empty()) {
        const int chars = gtk_text_buffer_get_char_count(buffer_);
        const Model::UndoHistory::Range& r = (*select)[0];
        GtkTextIter ins, bound;
        gtk_text_buffer_get_iter_at_offset(buffer_, &ins, std::min((int)r.caret, chars));
        gtk_text_buffer_get_iter_at_offset(buffer_, &bound, std::min((int)r.anchor, chars));
        gtk_text_buffer_select_range(buffer_, &ins, &bound);   // the marks move: not the user moving the caret
    }
    replaying_ = false;
}

void Document::onMarkSet(GtkTextBuffer* buf, GtkTextIter*, GtkTextMark* mark, gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (mark != gtk_text_buffer_get_insert(buf) && mark != gtk_text_buffer_get_selection_bound(buf))
        return;
    if (self->stepOpen_ || !self->recording()) return;
    self->client().noteSelection(self->id_, self->selection());
}

// ---------------------------------------------------------------- saving

bool Document::save(std::string* error) {
    // Not the file's text: a picture, a PDF, a message such as "Cannot
    // display". Saving a message used to write it over the file, which for
    // a binary file destroyed it.
    if (!canSave()) return true;
    saveNote_.clear();
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
    struct stat st;
    const bool exists = stat(target.c_str(), &st) == 0;
    if (exists && access(target.c_str(), W_OK) != 0) {
        if (error) *error = std::string("The file is not writable: ") + strerror(errno);
        return false;
    }
    // Stage 7: on a host that saves for apps, the host writes it by the same
    // rules (SessionFiles.h's saveDocument: through a link, hard links and
    // other owners in place, the encoding kept), after everything the
    // window asked, so it is the file's one writer. A host lost meanwhile:
    // written here, as before.
    if (hostSavesFile()) {
        bool lost = false;
        if (saveThroughHost(body, error, &lost)) return true;
        if (!lost) return false;
        if (error) error->clear();
    }

    // In the file's encoding, as read. A character Windows-1252 cannot store
    // (an arrow, an emoji) has the file written as UTF-8, and said so: never
    // a '?' in its place, never a question on every save (the Mac's C3b).
    FileEncoding encoding = encoding_;
    std::string unheld;
    if (!FolderSearch::canEncode(body, encoding.encoding, &unheld)) encoding = FileEncoding();
    const std::string bytes = FolderSearch::encodeFileText(body, encoding.encoding, encoding.bom);
    int mode = 0666;
    bool inPlace = false;
    if (exists) {
        mode = st.st_mode & 07777;
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
        if (!writeInPlace(target, bytes, (off_t)st.st_size, why)) {
            if (error) *error = why;
            return false;
        }
    } else {
        GError* err = nullptr;
        if (!g_file_set_contents_full(target.c_str(), bytes.data(), (gssize)bytes.size(),
                                      G_FILE_SET_CONTENTS_CONSISTENT, mode, &err)) {
            if (error) *error = err ? err->message : "Unknown error";
            g_clear_error(&err);
            return false;
        }
    }
    if (encoding.encoding != encoding_.encoding) {
        const std::size_t slash = path_.find_last_of('/');
        saveNote_ = "Saved " + (slash == std::string::npos ? path_ : path_.substr(slash + 1)) +
                    " as UTF-8: " +
                    FolderSearch::encodingLabel(encoding_.encoding, encoding_.bom) +
                    " cannot store \u201C" + unheld + "\u201D";
    }
    encoding_ = encoding;
    rememberDisk(body);   // so this save is not taken for someone else's
    markDirty(false);
    tellHostSaved(nullptr);
    return true;
}

// ---------------------------------------------------------------- saving through the host
//
// Stage 7 (docs/notes/session-host.md, "GTK's tabs save through the host"),
// the Mac's writeTextThroughHost: and MCSessionDoc's saveOnHostEncoding:bom:.
// The window asks everything it asks first (keep-sorted, organize and format
// on save, the link and permission checks above); the host then writes the
// document's text, which is the buffer's (every edit goes before the
// DocSave), in the document's encoding over whatever is on disk, so the host
// is the file's one writer and a page may save it too.

bool Document::hostSavesFile() const {
    return kind_ == Kind::Text && !detached_ && !link_->lost() && client().isOpen(id_) &&
           client().hostSavesForApps();
}

// Waits for the answer, at most 10 s (in one process a round trip through the
// host's thread; a headless host's over its socket). The wait takes only the
// host's messages, never a turn of the main loop, but someone's edits it
// puts in the buffer meanwhile are why the text the file holds is the
// buffer's only when the host wrote the version the buffer is at.
bool Document::saveThroughHost(const std::string& body, std::string* error, bool* lostMeanwhile) {
    Shared::Client& c = client();
    closeOutside();   // what this turn recorded goes first
    // An untitled buffer the host held is given its file's path first: the
    // host writes where its document says. Put back if the write fails.
    const std::string hostPathBefore = hostPath_;
    tellHostPath();
    const uint64_t req =
        c.requestSave(id_, 0, true, FolderSearch::encodingName(encoding_.encoding, encoding_.bom));
    Shared::Client::SaveAnswer a;
    const bool answered = req && c.settle(10000) && c.saveAnswer(req, &a);
    if (!answered || !a.ok) {
        if (hostPath_ != hostPathBefore && !c.lost()) {
            hostPath_ = hostPathBefore;
            c.setPath(id_, hostPathBefore);
        }
        if (!answered && c.lost()) {   // the window writes it, as before stage 7
            *lostMeanwhile = true;
            return false;
        }
        const std::string why = answered ? a.json["why"].asString() : std::string();
        if (!answered)
            g_printerr("mutible: the session's host did not answer a save of %s\n", path_.c_str());
        if (error)
            *error = !answered    ? "The session's host did not answer."
                     : why.empty() ? "The session's host could not write it."
                                   : why;
        return false;
    }
    // The encoding written: a Windows-1252 file whose text holds what it
    // cannot store is UTF-8 now, and the host's note says so in save's words.
    FolderSearch::TextEncoding e = encoding_.encoding;
    bool bom = encoding_.bom;
    if (FolderSearch::parseEncodingName(a.json["encodingName"].asString(), &e, &bom)) {
        encoding_.encoding = e;
        encoding_.bom = bom;
    }
    saveNote_ = a.json["note"].asString();
    // The text the file holds: the buffer's, unless someone's edit came in
    // after the host wrote (then the file's, as written).
    std::string saved = body;
    if (c.version(id_) == a.version && c.unconfirmed(id_) == 0) {
        saved = text();
    } else {
        std::string disk;
        if (readText(path_, disk) == Read::Ok) saved = disk;
    }
    // The stamp the host wrote it with, so the write is not taken for a
    // change made outside. Never DocSaved after the host's own write: that
    // would take the host's stamp away.
    const FolderSearch::FileStamp written = Shared::stampFromJson(a.json["stamp"]);
    rememberDisk(saved, &written);
    checkDirty();
    return true;
}

// The host is told the file holds the buffer's text (null) or `text` (the
// file's, not the buffer's: DocSaved flag 1, only to a host that saves for
// apps), where the document's text as last read or saved moves without a
// save through the host: its own write, a clean tab following the disk, a
// change on disk asked about, a held tab read.
void Document::tellHostSaved(const std::string* text) {
    if (kind_ != Kind::Text || !client().isOpen(id_)) return;
    if (text) client().savedAs(id_, Model::utf16FromUtf8(*text));
    else client().saved(id_);   // fences the host's counts until it took this (Client::saved)
}

// The host wrote the file for another renderer (a page's save): taken as
// this document's own save at the top of the next turn, the latest heard.
void Document::hostWrote(std::uint64_t version, const Json& json) {
    Shared::Client& c = client();
    if (kind_ != Kind::Text || !c.isOpen(id_)) return;
    HostWrite w;
    // The text written, when the buffer's is the host's at that version.
    w.knowsText = c.unconfirmed(id_) == 0 && c.version(id_) == version;
    if (w.knowsText) w.text = text();
    w.path = json["path"].asString();
    FolderSearch::TextEncoding e = FolderSearch::TextEncoding::Utf8;
    bool bom = false;
    w.knowsEncoding = FolderSearch::parseEncodingName(json["encodingName"].asString(), &e, &bom);
    w.encoding.encoding = e;
    w.encoding.bom = bom;
    w.stamp = Shared::stampFromJson(json["stamp"]);
    hostWrite_ = std::move(w);
    if (writeIdle_) return;
    writeIdle_ = g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        Document* d = static_cast<Document*>(p);
        d->writeIdle_ = 0;
        d->takeHostWrite();
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

void Document::takeHostWrite() {
    HostWrite w = std::move(hostWrite_);
    hostWrite_ = HostWrite();
    if (kind_ != Kind::Text || loading() || unread_ || path_.empty()) return;
    if (!w.path.empty() && w.path != path_) return;   // renamed since
    // Written over again since: a change made outside, as the disk checks
    // see it.
    const FolderSearch::FileStamp now = FolderSearch::stampOf(path_);
    if (w.stamp.exists && now != w.stamp) return;
    std::string saved = w.text;
    FileEncoding encoding = w.knowsEncoding ? w.encoding : encoding_;
    if (!w.knowsText && readText(path_, saved, &encoding) != Read::Ok) return;   // the file's
    encoding_ = encoding;
    rememberDisk(saved, w.stamp.exists ? &w.stamp : &now);
    checkDirty();
    host_->documentWrittenByHost(this);
}

// The host's count of the document as unsaved flipped while the buffer was
// the host's text (nothing of this window's unconfirmed): the dot follows
// it, as a page's does, at the top of the next turn, the latest heard, and
// only if the text has not changed since. Never where the unsaved state is
// more than the text against the file (takeHostDirty).
void Document::hostDirty(bool dirty) {
    Shared::Client& c = client();
    // This document's own messages only: another tab's edits in flight say
    // nothing of this one's text (Client::unconfirmed(doc)). A count sent
    // before the host took the window's DocSaved never reaches here: the
    // client holds it behind the DocSaved's fence (Client::saved).
    if (kind_ != Kind::Text || !c.isOpen(id_) || c.unconfirmed(id_) != 0) return;
    hostDirty_ = dirty;
    dirtyEdits_ = edits_;
    if (dirtyIdle_) return;
    dirtyIdle_ = g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        Document* d = static_cast<Document*>(p);
        d->dirtyIdle_ = 0;
        d->takeHostDirty();
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

void Document::takeHostDirty() {
    if (edits_ != dirtyEdits_ || dirty_ == hostDirty_) return;
    // Not an untitled buffer (the host has no path for it), a file never
    // read or gone, one still being read or not read yet.
    if (kind_ != Kind::Text || loading() || unread_ || path_.empty() || hostPath_ != path_ ||
        !diskKnown_ || goneFromDisk())
        return;
    markDirty(hostDirty_);
}

std::string Document::takeSaveNote() {
    std::string n;
    n.swap(saveNote_);
    return n;
}

std::string Document::encodingLabel() const {
    if (kind_ != Kind::Text || path_.empty() || loading()) return "";
    if (encoding_.encoding == FolderSearch::TextEncoding::Utf8 && !encoding_.bom) return "";
    return FolderSearch::encodingLabel(encoding_.encoding, encoding_.bom);
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
#ifdef MUTIBLE_ENABLE_PDF
        latex_ = ext == "tex" || ext == "ltx" || ext == "latex";
#endif
        if (showsPreview_ && !markdown_ && !latex_ && !notebook_) showsPreview_ = false;
        if (html_ && ext != "html" && ext != "htm" && ext != "xhtml") html_ = showsPage_ = false;
    }
    // Keep comparing against the same disk contents, under the new name.
    const bool watched = monitor_ != nullptr;
    stopWatching();
    path_ = path;
    if (watched) watch();
    tellHostPath();   // Save As, a rename
    const bool regrammar = ext != ext_ || wasSettings != isSettingsFile();
    ext_ = ext;
    if (regrammar && kind_ == Kind::Text) startHighlighting();
    if (regrammar) folds_->sourceChanged();   // maybe another way of folding
    refreshTaskCount();   // x.txt renamed to x.md counts its tasks
    host_->documentStateChanged(this);
}

// ---------------------------------------------------------------- the disk
//
// The Mac checks when its window becomes key (EditorController.mm
// -checkExternalChange), comparing modification dates. Here the file is also
// watched, so a change made in the terminal panel under the editor shows up
// without leaving the window, and the check compares contents: an atomic save
// by mutible itself, or a tool that rewrites the file unchanged, is then no
// change at all, whatever the dates say. Every document watches its own file,
// so a clean tab in the background follows its file too, as on the Mac.

void Document::watch() {
    if (path_.empty() || monitor_) return;
    GFile* f = g_file_new_for_path(path_.c_str());
    // WATCH_MOVES, so a save that renames a temporary file over this one
    // (what mutible and most tools do) arrives as a rename, not a deletion.
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
    FileEncoding encoding;
    if (readText(path_, content, &encoding) != Read::Ok) return;
    // Back from a backup made from an older version of the file: asked about
    // once, as a change seen now would be (askAboutDiskChange).
    if (diskQuestion_ && !dirty()) diskQuestion_ = false;
    if (content == saved_ && !diskQuestion_) {
        // The same text written another way (a tool saved it as UTF-8):
        // a clean buffer is written as the file is now.
        if (!dirty() && (encoding.encoding != encoding_.encoding || encoding.bom != encoding_.bom)) {
            encoding_ = encoding;
            host_->documentStateChanged(this);   // the status bar's label
        }
        return;
    }
    if (!dirty()) {
        applyDiskText(content, encoding);
        return;
    }
    // Edits here and a change there: the user decides. Once asked, this
    // version on disk is not asked about again, whatever the answer.
    if (host_->documentChangedOnDisk(this)) {
        rememberDisk(content);
        checkDirty();   // the change may be the very edits made here
        // The host counts the buffer against the file as it is now too
        // (stage 7: its count and the dot agree).
        tellHostSaved(&content);
    }
}

bool Document::reloadFromDisk() {
    if (kind_ != Kind::Text || path_.empty()) return false;
    std::string content;
    FileEncoding encoding;
    if (readText(path_, content, &encoding) != Read::Ok) return false;
    applyDiskText(content, encoding);
    return true;
}

// The disk's text replaces the buffer's in place: only the bytes between the
// common start and end of the two are replaced, as one user action, so the
// highlighter and the language server see one edit, one undo step ("Reload
// from Disk") takes it back and keeps the steps before it,
// and the caret stays where it was (a caret inside the replaced part keeps
// its line and column). The editor keeps the scroll position of one on show.
void Document::applyDiskText(const std::string& content, const FileEncoding& encoding) {
    host_->documentWillReload(this);
    folds_->forget();   // as on the Mac, a reload forgets the folds
    encoding_ = encoding;   // the file as it is now is what a save writes
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

        // A step of its own that keeps the steps before it, as the Mac's
        // clean tab following the disk; Reload chosen in the question is
        // the same step here, so it too can be taken back.
        nameStep("Reload from Disk");
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
    // The session's host counts it saved too: the reload went to it as an
    // edit, which it would count unsaved against the file's old text.
    if (!dirty()) tellHostSaved(nullptr);
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
const size_t kBatchEdits  = 8;      // a MultiEdit with this many ranges goes as a batch

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
    later_.clear();
}

void Document::carryLater(size_t pos, size_t oldLen, size_t newLen) {
    if (later_.empty()) return;
    // The ranges are apart, so their ends ascend: the first ending past pos
    // is the first an edit there can touch.
    auto it = std::upper_bound(later_.begin(), later_.end(), pos,
                               [](size_t p, const std::pair<size_t, size_t>& r) { return p < r.second; });
    for (; it != later_.end(); ++it) carry(it->first, it->second, pos, oldLen, newLen);
}

void Document::stopHighlighting() {
    brackets_->reset(nullptr, mirror_, {});
    semantic_->clear();
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
    // A new text, kind or name, or large-file mode turned on or off: the
    // wrap indent's tags follow (WrapIndentTags.h).
    wrapIndentSettingChanged();
    // Highlight tags are only ever put on by a highlighter (onApplyTag keeps
    // a paste's off), so without one there are none to take off: taking
    // them off a whole large file on show cost about 25 ms a tag, on every
    // change to the settings.
    const bool hadColors = hl_ != nullptr;
    semantic_->clear();   // a server's colors come again with its next answer
    pending_ = false;
    bulk_ = false;
    cancelDeferred();
    hl_.reset();
    std::string().swap(mirror_);
    if (kind_ != Kind::Text || ext_.empty() || !SyntaxHighlighter::supports(ext_) ||
        largeFileFeaturesOff()) {
        // No grammar (any more), or a large file (LargeFileView.h): no colors.
        // Covers a rename to a plain .txt.
        if (!hadColors) return;
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buffer_, &a, &b);
        applyingTags_ = true;
        removeHighlightTags(&a, &b);
        applyingTags_ = false;
        brackets_->reset(nullptr, mirror_, {});
        return;
    }
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer_, &a, &b);
    char* text = gtk_text_buffer_get_text(buffer_, &a, &b, TRUE);
    mirror_ = text ? text : "";
    g_free(text);

    hl_ = std::make_unique<IncrementalHighlighter<char>>(ext_);
    // Bracket-pair colors start from every line's tokens (BracketColors.h).
    const bool brackets = wantsBracketColors();
    std::vector<Token> tokens;
    hl_->reset(StringSource<char>(mirror_), brackets ? &tokens : nullptr);
    brackets_->reset(brackets ? hl_.get() : nullptr, mirror_, tokens);
    pendStart_ = 0;
    pendEnd_ = mirror_.size();
    pending_ = true;
    flushHighlighting();
}

// Record an edit already applied to mirror_: bytes [pos, pos + oldLen) became
// newLen bytes. The pending and deferred ranges are carried across it, then the
// lines the highlighter re-lexed join the pending range.
void Document::noteEdit(size_t pos, size_t oldLen, size_t newLen) {
    // The tags are recomputed for the final text at flush time; the bracket
    // depths take what each re-lexed line does to them now.
    std::vector<Token> relexed;
    const size_t linesBefore = hl_->lineCount();
    auto r = hl_->edit(StringSource<char>(mirror_), pos, oldLen, newLen, relexed);
    brackets_->edited(*hl_, StringSource<char>(mirror_), r, linesBefore, relexed);
    if (deferred_) carry(defStart_, defEnd_, pos, oldLen, newLen);
    carryLater(pos, oldLen, newLen);
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
        paintBracketsOnScreen();
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
    paintBracketsOnScreen();
}

// ------------------------------------------------ bracket-pair colors
// (BracketColors.h; the depths follow noteEdit, the lines on screen are
// painted after each flush and, through the editor's BracketView, on every
// scroll.)

bool Document::wantsBracketColors() const {
    return host_->documentSettings().bracketPairColorization() && !isSettingsFile();
}

void Document::paintBracketsOnScreen() {
    if (hl_ && !bulk_ && shownIn_ && brackets_->on())
        brackets_->paintVisible(shownIn_, *hl_, mirror_);
}

// ------------------------------------------------ semantic tokens
// (SemanticColors.h: the session hands them over, each character edit ends
// them, the lexer's retag of a line takes them off it, and the lines on
// screen are painted on arrival, on every scroll through the editor's
// BracketView, and after an idle retag.)

bool Document::takeSemanticSpans(std::vector<Lsp::SemanticSpan>&& spans, size_t textBytes) {
    if (!hl_ || bulk_ || kind_ != Kind::Text || largeFileFeaturesOff() ||
        textBytes != mirror_.size() || hl_->length() != mirror_.size() ||
        !host_->documentSettings().semanticHighlighting())
        return false;
    semantic_->set(std::move(spans));
    paintSemanticOnScreen();
    return true;
}

void Document::paintSemanticOnScreen() {
    if (hl_ && !bulk_ && shownIn_ && semantic_->current())
        semantic_->paintVisible(shownIn_, *hl_, mirror_);
}

void Document::semanticSettingChanged() {
    if (!host_->documentSettings().semanticHighlighting()) semantic_->clear();
}

void Document::wrapIndentSettingChanged() {
    if (!wrap_) return;
    wrap_->setOn(kind_ == Kind::Text && !largeFileFeaturesOff() &&
                 host_->documentSettings().wrappingIndent() != WrapIndent::Mode::None);
}

void Document::bracketSettingChanged() {
    const bool want = hl_ && !bulk_ && wantsBracketColors();
    if (want == brackets_->on()) return;
    std::vector<Token> tokens;
    if (want) hl_->lineTokens(StringSource<char>(mirror_), 0, hl_->lineCount(), tokens);
    brackets_->reset(want ? hl_.get() : nullptr, mirror_, tokens);
    paintBracketsOnScreen();
}

gboolean Document::onIdleRetag(gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (!self->hl_ || (!self->deferred_ && self->later_.empty()) || self->bulk_) {
        self->idleId_ = 0;   // resyncAfterBulk schedules it again if needed
        return G_SOURCE_REMOVE;
    }
    const gint64 t0 = g_get_monotonic_time();
    IncrementalHighlighter<char>& hl = *self->hl_;
    while (g_get_monotonic_time() - t0 < kSliceMicros) {
        const size_t len = hl.length();
        if (self->deferred_) {
            const size_t s = std::min(self->defStart_, len), e = std::min(self->defEnd_, len);
            if (e <= s) {
                self->deferred_ = false;
                continue;
            }
            const size_t first = hl.lineOf(s);
            const size_t end = std::min(first + kChunkLines, hl.lineOf(e - 1) + 1);
            self->retagSpan(first, end);
            self->defStart_ = end < hl.lineCount() ? hl.lineStart(end) : len;
            continue;
        }
        if (self->later_.empty()) break;
        auto& r = self->later_.front();
        const size_t s = std::min(r.first, len), e = std::min(r.second, len);
        if (e > s) {
            const size_t first = hl.lineOf(s);
            const size_t end = std::min(first + kChunkLines, hl.lineOf(e - 1) + 1);
            self->retagSpan(first, end);
            r.first = end < hl.lineCount() ? hl.lineStart(end) : len;
        }
        if (e <= s || r.first >= std::min(r.second, len))
            self->later_.erase(self->later_.begin());
    }
    // A retag took a server's colors off the lines it reached; those on
    // screen get them back while the tokens are current.
    self->paintSemanticOnScreen();
    if (self->deferred_ || !self->later_.empty()) return G_SOURCE_CONTINUE;
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

    // Byte -> character conversion over just these lines, one forward pass,
    // and only when there are tokens to place: a line too long to lex
    // (kMaxTokenizationLineLength) has none, and its megabytes were copied
    // and counted on every keystroke in it. GTK's own lines give the end.
    const bool copy = !tokens.empty() || !match;
    const std::string span = copy ? mirror_.substr(b0, b1 - b0) : std::string();
    if (!match)
        gtk_text_buffer_get_iter_at_offset(buffer_, &b,
                                           (int)(c0 + Utf8OffsetCursor(span).totalChars()));
    else if (endLine < hl_->lineCount())
        gtk_text_buffer_get_iter_at_line(buffer_, &b, (int)endLine);
    else
        gtk_text_buffer_get_end_iter(buffer_, &b);
    Utf8OffsetCursor cursor(span);

    applyingTags_ = true;
    removeHighlightTags(&a, &b);
    semantic_->lexed(&a, &b, firstLine, endLine);   // the lexer's colors alone, until painted
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

// A MultiEdit of many ranges (several carets typing, Replace All; D5).
// Read back whole at its end, as resyncAfterBulk does, the edit ran from its
// first range to its last: with carets spread through a file every line
// between was re-lexed and retagged, about a second of CPU for a keystroke
// at 1,000 carets in 100,000 lines (linux/PARITY.md, D7), and copying the
// buffer out alone cost 55 ms of it. Instead its changes are taken in bytes
// at begin-user-action, while the buffer still holds the text before them;
// at its end they make the new mirror in one pass and go to the
// highlighter together (EditBatch.h), each re-lexing its own lines, and only
// those lines are retagged, the ones on screen at once. Anything else
// editing the buffer inside the same action (the count of edits differs)
// falls back to the resync.
void Document::startBatch() {
    batching_ = false;
    const std::vector<MultiEdit::Change>* changes = MultiEdit::applyingChanges();
    if (MultiEdit::applyingTo() != buffer_ || !changes || changes->size() < kBatchEdits ||
        !hl_ || bulk_ || kind_ != Kind::Text || filling_ || !linesMatch())
        return;
    batch_.clear();
    batch_.reserve(changes->size());
    batchExpected_ = 0;
    for (const MultiEdit::Change& c : *changes) {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)c.start);
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)(c.start + c.length));
        const size_t pa = byteOffsetOf(&a), pb = c.length ? byteOffsetOf(&b) : pa;
        batch_.push_back({pa, pb > pa ? pb - pa : 0, c.text});
        batchExpected_ += (c.length > 0 ? 1 : 0) + (c.text.empty() ? 0 : 1);
    }
    if (!EditBatch::valid(batch_, mirror_.size())) return;
    batching_ = true;
    batchEdits_ = 0;
    bulk_ = true;   // the mirror is behind the buffer until the batch ends
}

void Document::finishBatch() {
    batching_ = false;
    if (!hl_ || batchEdits_ != batchExpected_) {
        resyncAfterBulk();
        return;
    }
    bulk_ = false;
    // Byte ranges kept across edits (pending, deferred, later) follow the
    // changes as carry() follows one edit: moved by what the changes before
    // them add, stretched over a change they touch.
    std::vector<long long> added(batch_.size() + 1, 0);   // by the changes before k
    for (size_t k = 0; k < batch_.size(); k++)
        added[k + 1] = added[k] + (long long)batch_[k].text.size() - (long long)batch_[k].oldLen;
    auto map = [&](size_t x, bool end) {
        // The first change ending after x (for an end, at or after it).
        size_t lo = 0, hi = batch_.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            const size_t e = batch_[mid].pos + batch_[mid].oldLen;
            if (end ? e < x : e <= x) lo = mid + 1;
            else hi = mid;
        }
        if (lo < batch_.size() && batch_[lo].pos < x)   // inside it: its edge
            return (size_t)((long long)(end ? batch_[lo].pos + batch_[lo].oldLen : batch_[lo].pos) +
                            added[lo] + (end ? (long long)batch_[lo].text.size() -
                                                   (long long)batch_[lo].oldLen
                                             : 0));
        return (size_t)((long long)x + added[lo]);
    };
    if (deferred_) {
        defStart_ = map(defStart_, false);
        defEnd_ = map(defEnd_, true);
    }
    if (pending_) {
        pendStart_ = map(pendStart_, false);
        pendEnd_ = map(pendEnd_, true);
    }
    for (auto& r : later_) {
        r.first = map(r.first, false);
        r.second = map(r.second, true);
    }
    std::vector<std::pair<size_t, size_t>> lines;   // re-lexed: [first, end), ascending, apart
    std::string after = EditBatch::apply(
        *hl_, mirror_, batch_,
        [&](const TextSource<char>& now, const IncrementalHighlighter<char>::Range& r,
            size_t linesBefore, const std::vector<Token>& relexed) {
            brackets_->edited(*hl_, now, r, linesBefore, relexed);
            lines.push_back({r.firstLine, r.endLine});
        });
    mirror_ = std::move(after);
    // The lines on screen now; the rest in idle time, each line costing a
    // retag of its own (about 80 us: every highlight tag taken off it), so
    // 1,000 carets spread through a file left 85 ms in the keystroke.
    size_t vf = 0, ve = hl_->lineCount();
    if (shownIn_) visibleLines(vf, ve);
    std::vector<std::pair<size_t, size_t>> rest;
    auto byteOf = [&](size_t line) {
        return line < hl_->lineCount() ? hl_->lineStart(line) : hl_->length();
    };
    for (const auto& l : lines) {
        const size_t a = std::max(l.first, vf), b = std::min(l.second, ve);
        if (a < b) retagSpan(a, b);
        if (l.first < vf)
            rest.push_back({byteOf(l.first), byteOf(std::min(l.second, vf))});
        if (l.second > ve && l.second > l.first)
            rest.push_back({byteOf(std::max(l.first, ve)), byteOf(l.second)});
    }
    if (!rest.empty()) {
        // Into the list, which stays ascending and apart.
        std::vector<std::pair<size_t, size_t>> merged;
        merged.reserve(later_.size() + rest.size());
        std::merge(later_.begin(), later_.end(), rest.begin(), rest.end(), std::back_inserter(merged));
        later_.clear();
        for (const auto& r : merged) {
            if (r.second <= r.first) continue;
            if (!later_.empty() && r.first <= later_.back().second)
                later_.back().second = std::max(later_.back().second, r.second);
            else
                later_.push_back(r);
        }
    }
    if (pending_) flushHighlighting();
    else paintBracketsOnScreen();
    if ((deferred_ || !later_.empty()) && !idleId_)
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
        char* raw = gtk_text_buffer_get_text(buffer_, &ls, &le, TRUE);
        std::u16string line = utf16::fromUtf8(raw ? raw : "");
        g_free(raw);
        ColorSpan span;
        if (!Settings::findColor(line, span)) {
            // An opacity is a link too, opening a slider (Editor::pickOpacity);
            // an underline in the accent says it can be clicked without
            // looking like a swatch, as the Mac's dotted one does.
            ValueSpan os;
            double value = 0;
            if (!Settings::findOpacity(line, os, value)) continue;
            GtkTextTag* tag = gtk_text_tag_table_lookup(table, kOpacityTag);
            if (!tag) {
                const GdkRGBA accent{0x4E / 255.0f, 0xA1 / 255.0f, 0xF7 / 255.0f, 1};
                tag = gtk_text_buffer_create_tag(buffer_, kOpacityTag, "underline",
                                                 PANGO_UNDERLINE_SINGLE, "underline-rgba",
                                                 &accent, NULL);
                tags_->highlight.push_back(tag);
            }
            GtkTextIter a = ls, b = ls;
            gtk_text_iter_forward_chars(&a, (int)utf16::toCharOffset(line, os.start));
            gtk_text_iter_forward_chars(&b, (int)utf16::toCharOffset(line, os.start + os.length));
            gtk_text_buffer_apply_tag(buffer_, tag, &a, &b);
            continue;
        }

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
    if (len > 0 && self->recording())
        self->recordEdit((std::size_t)gtk_text_iter_get_offset(loc), 0,
                         Model::utf16FromUtf8(std::string(text, (std::size_t)len)));
    // A server's tokens are for the text before any character edit.
    if (len > 0) self->semantic_->edited();
    if (self->batching_) {
        if (len > 0) self->batchEdits_++;
        return;
    }
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
    if (!gtk_text_iter_equal(start, end) && self->recording()) {
        const int a = std::min(gtk_text_iter_get_offset(start), gtk_text_iter_get_offset(end));
        const int b = std::max(gtk_text_iter_get_offset(start), gtk_text_iter_get_offset(end));
        self->recordEdit((std::size_t)a, (std::size_t)(b - a), std::u16string());
    }
    if (!gtk_text_iter_equal(start, end)) self->semantic_->edited();
    if (self->batching_) {
        if (!gtk_text_iter_equal(start, end)) self->batchEdits_++;
        return;
    }
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
    // One step for the action: named (nameStep, a MultiEdit) or typing.
    self->closeOutside();
    if (self->recording()) {
        Model::UndoHistory::StepInfo info{"Typing", true};
        self->stepNamed_ = self->pendingAt_ && g_get_monotonic_time() - self->pendingAt_ < kPendingNameUs;
        if (self->stepNamed_) info = self->pendingStep_;
        self->pendingAt_ = 0;
        self->client().openStep(self->id_, info, self->selection());
        self->stepOpen_ = true;
    }
    self->inAction_ = true;
    self->actionEdits_ = 0;
    self->startBatch();
}

void Document::onEndUserAction(GtkTextBuffer*, gpointer selfp) {
    Document* self = static_cast<Document*>(selfp);
    if (self->stepOpen_) {
        self->stepOpen_ = false;
        self->client().closeStep(self->id_, self->selection());
    }
    self->inAction_ = false;
    if (self->batching_) self->finishBatch();
    else if (self->bulk_) self->resyncAfterBulk();
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
