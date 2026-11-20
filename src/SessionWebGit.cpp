// SessionWebGit.cpp — the session's web page's Source Control (R3's fourth
// slice, SessionWebGit.h): git run with fixed arguments, the review of the
// folder's uncommitted changes held to the folder, and what a page is sent
// of it. Nothing here writes to the repository.
#include "SessionWebGit.h"

#include "QuickDiff.h"
#include "SqliteRead.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace Shared {
namespace Web {

namespace {

using Clock = std::chrono::steady_clock;

long long msLeft(Clock::time_point until) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
}

std::string realOf(const std::string& path) {
    char buf[PATH_MAX];
    if (!path.empty() && ::realpath(path.c_str(), buf)) return buf;
    return "";
}

// `path` is `dir` or under it (both resolved).
bool within(const std::string& dir, const std::string& path) {
    if (dir.empty() || path.empty()) return false;
    if (path == dir) return true;
    const std::string d = dir.back() == '/' ? dir : dir + "/";
    return path.compare(0, d.size(), d) == 0;
}

bool hasDotName(const std::string& rel) {
    std::size_t at = 0;
    while (at <= rel.size()) {
        const std::size_t slash = rel.find('/', at);
        const std::size_t end = slash == std::string::npos ? rel.size() : slash;
        if (end > at && rel[at] == '.') return true;
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    return false;
}

std::string sectionName(ChangeReview::Section s) {
    return s == ChangeReview::Section::Staged ? "staged" : s == ChangeReview::Section::Changes ? "changes" : "plain";
}

Json spanJson(const SideBySide::Span& s) {
    Json a = Json::array();
    a.push(Json((int64_t)s.start));
    a.push(Json((int64_t)s.length));
    a.push(Json(s.style));
    return a;
}

// A row's text, cut at kReviewRowChars (never inside a surrogate pair),
// with the spans that fall in it, as UTF-8 and JSON.
struct RowText {
    std::string text;
    Json syntax = Json::array(), words = Json::array();
};
RowText rowText(const std::u16string& text, const std::vector<SideBySide::Span>& syntax,
                const std::vector<SideBySide::Span>& words) {
    RowText r;
    std::size_t n = text.size();
    if (n > kReviewRowChars) {
        n = kReviewRowChars;
        if (n && text[n - 1] >= 0xD800 && text[n - 1] <= 0xDBFF) n--;
    }
    r.text = QuickDiff::toUtf8(text.substr(0, n));
    if (n < text.size()) r.text += "\xe2\x80\xa6";   // …
    auto add = [&](const std::vector<SideBySide::Span>& spans, Json& out) {
        for (const SideBySide::Span& s : spans) {
            if (s.start >= n || !s.length) continue;
            out.push(spanJson({s.start, std::min(s.length, n - s.start), s.style}));
        }
    };
    add(syntax, r.syntax);
    add(words, r.words);
    return r;
}

// The spans of `spans` (offsets into the page's text) inside [from, to),
// made relative to `from`. `spans` are in order of their start.
std::vector<SideBySide::Span> spansIn(const std::vector<SideBySide::Span>& spans, std::size_t from,
                                      std::size_t to) {
    std::vector<SideBySide::Span> out;
    auto it = std::lower_bound(spans.begin(), spans.end(), from,
                               [](const SideBySide::Span& s, std::size_t at) { return s.start + s.length <= at; });
    for (; it != spans.end() && it->start < to; ++it) {
        const std::size_t a = std::max(it->start, from), b = std::min(it->start + it->length, to);
        if (b > a) out.push_back({a - from, b - a, it->style});
    }
    return out;
}

// One line's syntax spans on one side of a file.
std::vector<SideBySide::Span> lineSyntax(const std::vector<SideBySide::Span>& spans,
                                         const std::vector<std::size_t>& starts, int line, std::size_t length) {
    if (spans.empty() || line < 0 || (std::size_t)line + 1 >= starts.size()) return {};
    const std::size_t from = starts[(std::size_t)line];
    return spansIn(spans, from, from + length);
}

Json fileHead(const ChangeReview::FileDiff& d) {
    const ChangeReview::File& f = d.file;
    Json j = Json::object();
    j.set("key", Json(f.key()));
    j.set("section", Json(sectionName(f.section)));
    j.set("letter", Json(std::string(1, f.letter)));
    j.set("path", Json(SqliteRead::validUtf8(f.path)));
    if (!f.origPath.empty()) j.set("orig", Json(SqliteRead::validUtf8(f.origPath)));
    j.set("header", Json(SqliteRead::validUtf8(QuickDiff::toUtf8(ChangeReview::fileHeaderText(d)))));
    j.set("added", Json((int64_t)d.added));
    j.set("removed", Json((int64_t)d.removed));
    return j;
}

// A file whose rows can be shown side by side: text, with rows, not a
// conflict (whose blocks have no other side to show).
bool canSide(const ChangeReview::FileDiff& d) {
    const ChangeReview::File& f = d.file;
    return !d.rows.empty() && !f.conflict && !f.binary && !f.tooLarge && !f.submodule && !f.symlink &&
           !f.image;
}

// The page says this, not the window's "Side by Side or Open shows it".
const char* const kTooLarge = "Too large to show on this page; the mutible window's review shows it.";

// The inline rows of file `fi` of page `p`.
Json inlineRows(const ChangeReview::Page& p, std::size_t fi, std::size_t* count) {
    Json rows = Json::array();
    for (std::size_t i = 0; i < p.rows.size(); i++) {
        const ChangeReview::PageRow& row = p.rows[i];
        if (row.file != fi) continue;
        if (row.kind == ChangeReview::RowKind::Section || row.kind == ChangeReview::RowKind::File) continue;
        const std::size_t from = p.starts[i];
        const std::size_t to = i + 1 < p.starts.size() ? p.starts[i + 1] - 1 : p.text.size();
        const std::u16string text = p.text.substr(from, to - from);
        Json r = Json::array();
        using K = ChangeReview::RowKind;
        if (row.kind == K::Gap) {
            r.push(Json("g"));
            r.push(Json(row.gap));
            r.push(Json((int64_t)row.hidden));
            r.push(Json(QuickDiff::toUtf8(text)));
        } else if (row.kind == K::Note || row.kind == K::Image) {
            r.push(Json("n"));
            r.push(Json(0));
            r.push(Json(0));
            std::string note = QuickDiff::toUtf8(text);
            if (note.compare(0, 18, "Too large to show ") == 0) note = kTooLarge;
            if (row.kind == K::Image) {
                // The pictures are drawn by the window; the page names them.
                bool first = true;
                for (std::size_t k = 0; k < i; k++)
                    if (p.rows[k].file == fi && p.rows[k].kind == K::Image) first = false;
                if (!first) continue;
                note = "A picture: shown in the mutible window's review.";
            }
            r.push(Json(note));
        } else {
            const RowText t = rowText(text, spansIn(p.syntax, from, to), spansIn(p.words, from, to));
            const char* kind = row.kind == K::Removed ? "r" : row.kind == K::Added ? "a"
                               : row.kind == K::Conflict ? "x" : "c";
            r.push(Json(kind));
            r.push(Json(row.kind == K::Added || row.kind == K::Conflict ? 0 : row.oldLine + 1));
            r.push(Json(row.kind == K::Removed ? 0 : row.newLine + 1));
            r.push(Json(t.text));
            r.push(t.syntax);
            if (row.kind == K::Removed || row.kind == K::Added) r.push(t.words);
            if (row.kind == K::Conflict) r.push(Json((int)row.part));
        }
        rows.push(std::move(r));
        (*count)++;
    }
    return rows;
}

// Side by side, folded as the inline layout folds (the same gap ids, so a
// gap opened in one shows opened in the other).
Json sideRows(const ChangeReview::FileDiff& d, const std::set<int>& opened, std::size_t* count,
              std::size_t context = 3) {
    Json rows = Json::array();
    const std::size_t nr = d.rows.size();
    std::vector<bool> near(nr, false);
    for (const SideBySide::Hunk& b : d.blocks) {
        const std::size_t lo = b.first > context ? b.first - context : 0;
        const std::size_t hi = std::min(nr, b.end + context);
        for (std::size_t r = lo; r < hi; r++) near[r] = true;
    }
    std::size_t wordBudget = 4u << 20;
    auto addRow = [&](const SideBySide::Row& row) {
        const std::u16string o = row.left >= 0 ? ChangeReview::oldLine(d, row.left) : std::u16string();
        const std::u16string n = row.right >= 0 ? ChangeReview::newLine(d, row.right) : std::u16string();
        SideBySide::WordChanges w;
        if (row.kind == SideBySide::Kind::Changed && wordBudget && o.size() <= 2000 && n.size() <= 2000) {
            wordBudget -= std::min(wordBudget, o.size() + n.size());
            w = SideBySide::wordChanges(o, n);
            for (SideBySide::Span& s : w.right) s.style = 1;
            for (SideBySide::Span& s : w.left) s.style = 0;
        }
        const RowText lt = rowText(o, lineSyntax(d.oldSyntax, d.oldStarts, row.left, o.size()), w.left);
        const RowText rt = rowText(n, lineSyntax(d.newSyntax, d.newStarts, row.right, n.size()), w.right);
        const char* kind = row.kind == SideBySide::Kind::Same      ? "c"
                           : row.kind == SideBySide::Kind::Changed ? "m"
                           : row.kind == SideBySide::Kind::Removed ? "r"
                                                                   : "a";
        Json r = Json::array();
        r.push(Json("s"));
        r.push(Json(kind));
        r.push(Json(row.left + 1));
        r.push(Json(row.right + 1));
        r.push(Json(lt.text));
        r.push(Json(rt.text));
        r.push(lt.syntax);
        r.push(rt.syntax);
        r.push(lt.words);
        r.push(rt.words);
        rows.push(std::move(r));
        (*count)++;
    };
    for (std::size_t r = 0; r < nr;) {
        if (d.rows[r].kind == SideBySide::Kind::Same && !near[r]) {
            std::size_t e = r;
            while (e < nr && d.rows[e].kind == SideBySide::Kind::Same && !near[e]) e++;
            const int id = d.rows[r].right;
            if (e - r >= 2 && !opened.count(id)) {
                Json g = Json::array();
                g.push(Json("g"));
                g.push(Json(id));
                g.push(Json((int64_t)(e - r)));
                g.push(Json(ChangeReview::gapText(e - r)));
                rows.push(std::move(g));
                (*count)++;
                r = e;
                continue;
            }
            for (; r < e; r++) addRow(d.rows[r]);
            continue;
        }
        addRow(d.rows[r]);
        r++;
    }
    if (d.lastNewlineChanged) {
        const bool newEnds = !d.file.newText.empty() && d.file.newText.back() == u'\n';
        Json n = Json::array();
        n.push(Json("n"));
        n.push(Json(0));
        n.push(Json(0));
        n.push(Json(newEnds ? "A newline was added at the end of the file."
                            : "The newline at the end of the file was taken away."));
        rows.push(std::move(n));
        (*count)++;
    }
    return rows;
}

Json fileJson(const ChangeReview::FileDiff& d, const ChangeReview::Page* page, std::size_t fi,
              const std::set<int>& opened, bool side, std::size_t* count) {
    Json j = fileHead(d);
    const bool can = canSide(d);
    j.set("canSide", Json(can));
    j.set("side", Json(side && can));
    if (side && can) {
        j.set("rows", sideRows(d, opened, count));
    } else if (page) {
        j.set("rows", inlineRows(*page, fi, count));
    } else {
        ChangeReview::Opened o;
        if (!opened.empty()) o[d.file.key()] = opened;
        const ChangeReview::Page p = ChangeReview::layout({d}, o);
        j.set("rows", inlineRows(p, 0, count));
    }
    return j;
}

}  // namespace

// ------------------------------------------------------------------ git

ChangeReview::GitOut runGit(const std::string& dir, const std::vector<std::string>& args,
                            const std::string& input, int timeoutMs) {
    ChangeReview::GitOut r;
    // stdin: the input in an unlinked scratch file (no pipe to fill, and no
    // SIGPIPE when git exits before reading it), else /dev/null.
    int in = -1;
    if (!input.empty()) {
        const char* tmp = std::getenv("TMPDIR");
        std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/mutible-git-XXXXXX";
        std::vector<char> name(path.begin(), path.end());
        name.push_back('\0');
        in = ::mkstemp(name.data());
        if (in >= 0) {
            ::unlink(name.data());
            std::size_t at = 0;
            while (at < input.size()) {
                const ssize_t n = ::write(in, input.data() + at, input.size() - at);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                at += (std::size_t)n;
            }
            ::lseek(in, 0, SEEK_SET);
        }
    } else {
        in = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    if (in < 0) {
        r.err = "git's input could not be made: " + std::string(std::strerror(errno));
        return r;
    }
    int out[2] = {-1, -1}, err[2] = {-1, -1};
    if (::pipe(out) != 0 || ::pipe(err) != 0) {
        r.err = std::strerror(errno);
        for (int fd : {in, out[0], out[1], err[0], err[1]})
            if (fd >= 0) ::close(fd);
        return r;
    }
    for (int fd : {in, out[0], out[1], err[0], err[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err[1], 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, def;
    sigemptyset(&none);
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setpgroup(&at, 0);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    // The caller's environment, less what would point git elsewhere or
    // make it wait for a person, plus ours.
    std::vector<std::string> env;
    for (char** e = environ; e && *e; e++) {
        const std::string kv = *e;
        const std::string k = kv.substr(0, kv.find('='));
        if (k == "GIT_DIR" || k == "GIT_WORK_TREE" || k == "GIT_INDEX_FILE" || k == "GIT_OPTIONAL_LOCKS" ||
            k == "GIT_TERMINAL_PROMPT" || k == "GIT_EDITOR" || k == "GIT_PAGER")
            continue;
        env.push_back(kv);
    }
    env.push_back("GIT_OPTIONAL_LOCKS=0");
    env.push_back("GIT_TERMINAL_PROMPT=0");
    env.push_back("GIT_EDITOR=true");
    env.push_back("GIT_PAGER=cat");
    std::vector<char*> envp;
    for (std::string& kv : env) envp.push_back(&kv[0]);
    envp.push_back(nullptr);
    std::vector<std::string> argvs = {"git", "-C", dir};
    argvs.insert(argvs.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : argvs) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int spawned = posix_spawnp(&pid, "git", &fa, &at, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    ::close(in);
    ::close(out[1]);
    ::close(err[1]);
    if (spawned != 0) {
        ::close(out[0]);
        ::close(err[0]);
        r.err = "git could not be run: " + std::string(std::strerror(spawned));
        return r;
    }
    const std::size_t kMaxOut = std::size_t(256) << 20;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    Clock::time_point killAt{};
    bool termed = false, killed = false;
    int fds[2] = {out[0], err[0]};
    std::string* sinks[2] = {&r.out, &r.err};
    char buf[65536];
    while (fds[0] >= 0 || fds[1] >= 0) {
        if (!termed && (msLeft(deadline) <= 0 || r.out.size() > kMaxOut)) {
            ::killpg(pid, SIGTERM);
            termed = true;
            killAt = Clock::now() + std::chrono::seconds(2);
        }
        if (termed && !killed && msLeft(killAt) <= 0) {
            ::killpg(pid, SIGKILL);
            killed = true;
        }
        if (killed && msLeft(killAt + std::chrono::seconds(1)) <= 0) break;   // something holds the pipes
        pollfd p[2];
        nfds_t n = 0;
        int which[2];
        for (int i = 0; i < 2; i++)
            if (fds[i] >= 0) {
                p[n] = pollfd{fds[i], POLLIN, 0};
                which[n++] = i;
            }
        const long long wait = termed ? 100 : std::max(1LL, std::min(1000LL, msLeft(deadline)));
        const int got = ::poll(p, n, (int)wait);
        if (got < 0 && errno == EINTR) continue;
        for (nfds_t k = 0; k < n; k++) {
            if (!p[k].revents) continue;
            const int i = which[k];
            const ssize_t m = ::read(fds[i], buf, sizeof buf);
            if (m < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (m <= 0) {
                ::close(fds[i]);
                fds[i] = -1;
            } else if (sinks[i]->size() <= kMaxOut) {
                sinks[i]->append(buf, (std::size_t)m);
            }
        }
    }
    for (int fd : fds)
        if (fd >= 0) ::close(fd);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (termed) {
        r.ok = false;
        r.err = r.out.size() > kMaxOut ? "git wrote more than this page reads."
                                       : "git did not answer within " + std::to_string(timeoutMs / 1000) +
                                             " seconds.";
        r.out.clear();
        return r;
    }
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    r.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return r;
}

// ------------------------------------------------------- held to the folder

void confineReview(ChangeReview::Read& r, const std::string& folder, bool hidden) {
    if (r.top.empty()) return;
    const std::string realTop = realOf(r.top), realFolder = realOf(folder);
    std::string prefix;   // the folder from the top level, "" for the top itself
    if (realTop.empty() || realFolder.empty() || !within(realTop, realFolder)) {
        // A folder git placed somewhere else: nothing can be shown as its.
        r.diffs.clear();
        r.extras.clear();
        r.error = "This folder is not in a git repository.";
        return;
    }
    if (realFolder.size() > realTop.size()) prefix = realFolder.substr(realTop.size() + (realTop == "/" ? 0 : 1));
    std::vector<ChangeReview::FileDiff> diffs;
    std::vector<ChangeReview::Extra> extras;
    for (std::size_t i = 0; i < r.diffs.size(); i++) {
        ChangeReview::FileDiff& d = r.diffs[i];
        const std::string& path = d.file.path;
        std::string inside = path;
        if (!prefix.empty()) {
            if (path.compare(0, prefix.size() + 1, prefix + "/") != 0) continue;
            inside = path.substr(prefix.size() + 1);
        }
        if (!hidden && hasDotName(inside)) continue;
        // The work tree's side was read from the disk: never through a link,
        // nor from a file that resolves outside the folder.
        const bool fromDisk = i < r.extras.size() && r.extras[i].workRead;
        if (fromDisk) {
            const std::string abs = r.top + "/" + path;
            struct stat st;
            const bool link = ::lstat(abs.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
            if (link || !within(realFolder, realOf(abs))) {
                ChangeReview::File f = d.file;
                f.symlink = true;
                f.conflict = false;
                f.oldText.clear();
                f.newText.clear();
                d = ChangeReview::diffFile(f, "");
                if (i < r.extras.size()) {
                    r.extras[i].workBytes.clear();
                    r.extras[i].workRead = false;
                }
            }
        }
        diffs.push_back(std::move(d));
        if (i < r.extras.size()) extras.push_back(std::move(r.extras[i]));
    }
    r.diffs = std::move(diffs);
    r.extras = std::move(extras);
}

// ------------------------------------------------------------- the page

Json gitStatusJson(const ChangeReview::Read& r, std::uint64_t version) {
    Json j = Json::object();
    j.set("t", Json("git"));
    j.set("version", Json((int64_t)version));
    j.set("top", Json(SqliteRead::validUtf8(r.top)));
    j.set("branch", Json(SqliteRead::validUtf8(r.branchText)));
    j.set("initial", Json(r.initial));
    if (!r.error.empty()) j.set("error", Json(SqliteRead::validUtf8(r.error)));
    Json files = Json::array();
    for (const ChangeReview::FileDiff& d : r.diffs) files.push(fileHead(d));
    j.set("files", std::move(files));
    j.set("count", Json((int64_t)ChangeReview::fileCount(r.diffs)));
    return j;
}

Json reviewFileJson(const ChangeReview::FileDiff& d, const std::set<int>& opened, bool side) {
    std::size_t count = 0;
    return fileJson(d, nullptr, 0, opened, side, &count);
}

Json reviewJson(const ChangeReview::Read& r, std::uint64_t version, const ChangeReview::Opened& opened,
                const std::set<std::string>& side, std::size_t budget) {
    Json j = Json::object();
    j.set("t", Json("review"));
    j.set("version", Json((int64_t)version));
    if (!r.error.empty()) j.set("error", Json(SqliteRead::validUtf8(r.error)));
    // One layout of every file, as the review page lays them out; side by
    // side files are laid out on their own.
    const ChangeReview::Page page = ChangeReview::layout(r.diffs, opened);
    Json files = Json::array();
    std::size_t count = 0;
    for (std::size_t fi = 0; fi < r.diffs.size(); fi++) {
        const ChangeReview::FileDiff& d = r.diffs[fi];
        if (count >= budget && fi > 0) {
            Json h = fileHead(d);
            h.set("canSide", Json(canSide(d)));
            h.set("side", Json(side.count(d.file.key()) > 0 && canSide(d)));
            h.set("more", Json(true));
            files.push(std::move(h));
            continue;
        }
        static const std::set<int> none;
        auto it = opened.find(d.file.key());
        files.push(fileJson(d, &page, fi, it == opened.end() ? none : it->second, side.count(d.file.key()) > 0,
                            &count));
    }
    j.set("files", std::move(files));
    return j;
}

ChangeReview::Opened openedFrom(const Json& j) {
    ChangeReview::Opened o;
    if (!j.isObject()) return o;
    std::size_t total = 0;
    for (const auto& kv : j.items()) {
        if (!kv.second.isArray()) continue;
        std::set<int>& ids = o[kv.first];
        for (const Json& id : kv.second.elements()) {
            if (total >= kMaxOpenedGaps) return o;
            if (!id.isNumber()) continue;
            const double v = id.asNumber();
            if (v < 0 || v > (double)INT_MAX) continue;
            ids.insert((int)v);
            total++;
        }
    }
    return o;
}

std::set<std::string> sideFrom(const Json& j) {
    std::set<std::string> s;
    if (!j.isArray()) return s;
    for (const Json& k : j.elements()) {
        if (s.size() >= kMaxOpenedGaps) break;
        if (k.isString()) s.insert(k.asString());
    }
    return s;
}

// ------------------------------------------------------------ the reads

struct GitReads::State {
    std::mutex mutex;
    std::string folder;
    bool hidden = false;
    ChangeReview::GitRunner run;
    std::function<void()> wake;   // cleared when the GitReads goes
    bool running = false, wanted = false, gone = false;
    std::shared_ptr<const ChangeReview::Read> read;
    std::uint64_t version = 0, finished = 0;
    std::size_t print = 0;
};

namespace {

std::size_t printOf(const ChangeReview::Read& r) {
    std::size_t h = ChangeReview::fingerprint(r.diffs);
    for (const std::string* s : {&r.top, &r.error, &r.branchText})
        h = h * 1000003u ^ std::hash<std::string>()(*s);
    return h * 31u + (r.initial ? 1u : 0u);
}

}  // namespace

GitReads::GitReads(std::string folder, bool hidden, ChangeReview::GitRunner run, std::function<void()> wake)
    : s_(std::make_shared<State>()) {
    s_->folder = std::move(folder);
    s_->hidden = hidden;
    s_->run = std::move(run);
    s_->wake = std::move(wake);
}

GitReads::~GitReads() {
    std::lock_guard<std::mutex> lock(s_->mutex);
    s_->gone = true;
    s_->wake = nullptr;
}

void GitReads::ask() {
    std::lock_guard<std::mutex> lock(s_->mutex);
    s_->wanted = true;
    if (s_->running) return;
    s_->running = true;
    std::shared_ptr<State> s = s_;
    std::thread([s] {
        for (;;) {
            std::string folder;
            bool hidden;
            ChangeReview::GitRunner run;
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                if (!s->wanted || s->gone) {
                    s->running = false;
                    return;
                }
                s->wanted = false;
                folder = s->folder;
                hidden = s->hidden;
                run = s->run;
            }
            auto read = std::make_shared<ChangeReview::Read>(ChangeReview::readUncommitted(folder, run));
            confineReview(*read, folder, hidden);
            const std::size_t print = printOf(*read);
            std::lock_guard<std::mutex> lock(s->mutex);
            if (s->gone) {
                s->running = false;
                return;
            }
            s->finished++;
            if (!s->read || print != s->print) {
                s->read = std::move(read);
                s->print = print;
                s->version++;
            }
            if (s->wake) s->wake();
        }
    }).detach();
}

GitReads::Latest GitReads::latest() const {
    std::lock_guard<std::mutex> lock(s_->mutex);
    Latest l;
    l.read = s_->read;
    l.version = s_->version;
    l.finished = s_->finished;
    l.reading = s_->running;
    return l;
}

}  // namespace Web
}  // namespace Shared
