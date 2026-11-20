// SessionWebGit.h — pure C++ (POSIX), part of the portable core. The
// session's web page's Source Control (R3's fourth slice, SessionWeb.h):
// what the window's Source Control lists (the staged files, then the
// changed ones, each with its status letter) and the review page of those
// changes (ChangeReview's layout: each file's blocks with their unchanged
// lines folded, inline, or side by side), read by the server with the
// core's ChangeReview::readUncommitted, the reading the review page of the
// GTK port runs, and sent to a page as data it draws with textContent.
// Since R3's last slice, also the window's open commit pages and worktree
// pages (ReviewPage, readReviewPage), and the pictures of a binary image
// file, before and after, as the window's review shows them (pictureSize;
// the server serves their bytes from the read at pictureTarget).
//
// Read only, always: git runs with the core's fixed arguments (status,
// ls-files, ls-tree, cat-file --batch, rev-parse), never one a page says;
// a page's messages only pick from what was read (a file's key, the gaps
// it opened, inline or side by side). Nothing stages, unstages, discards,
// commits or applies.
//
// Held to the session's folder, as every read of the page's is: a file
// outside the folder (the folder below the repository's top level) is left
// out, so is a dotfile unless the tree shows them, and a work tree file
// that is a symbolic link, or resolves outside the folder, is shown as a
// link without its text.
//
// Reads run on a thread of their own (GitReads), one at a time, so the
// server's loop never waits for git; a read that shows the same as the last
// is not sent again.
//
// Tested in tests/session_web_tests.inc (`sessionweb:git`) against a real
// git in a scratch repository.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ChangeReviewRead.h"
#include "Json.h"

namespace Shared {
namespace Web {

// git `args` in `dir` (git -C dir ...), `input` on its stdin: the first
// `git` on PATH, spawned with no shell, in a process group of its own, with
// GIT_OPTIONAL_LOCKS=0 (a status never rewrites the index), no terminal
// prompt and no editor; stopped (SIGTERM, then SIGKILL 2 s later) past
// `timeoutMs` or past 256 MB of output. What a headless host runs; the Mac
// gives its own runner (Options::git).
ChangeReview::GitOut runGit(const std::string& dir, const std::vector<std::string>& args,
                            const std::string& input, int timeoutMs = 30000);

// Holds a read to the session's folder (see above): `folder` resolved or
// not, `hidden` the tree's Show Hidden Files.
void confineReview(ChangeReview::Read& r, const std::string& folder, bool hidden);

// The most rows the whole review sends at once; a file past it is sent as
// its header only, with `more`, and the page asks for it alone.
constexpr std::size_t kReviewRowBudget = 20000;
// A row's text past this many UTF-16 units is cut (and its colors with it).
constexpr std::size_t kReviewRowChars = 2000;
// The most gaps a page may say it opened, per file and in all.
constexpr std::size_t kMaxOpenedGaps = 4096;

// The list: {t: "git", version, top, branch, initial, files: [{key,
// section: "staged" | "changes", letter, path, orig, added, removed}],
// count (files, a file in both lists once), error (no repository, git
// failed)}.
Json gitStatusJson(const ChangeReview::Read& r, std::uint64_t version);

// ---- pictures (the window's image diffs, #217)
// A picture's size in pixels from its header alone, never decoding a
// pixel: PNG, GIF, JPEG, WebP, BMP and ICO, the kinds every browser shows
// (fileType's "image" less SVG, which is text, and AVIF). 0 by 0 for
// anything else, so a TIFF or HEIC file keeps the binary file's note.
ChangeReview::ImageSide pictureSize(const std::string& bytes);
// How the page's reads see pictures: pictureSize.
ChangeReview::ImageReading pageImages();
// Where a review's picture is served: `base` (Access::base), then
// "picture/PAGE/VERSION/FILE/old" or ".../new", PAGE 0 the review of
// uncommitted changes, else the number the server gave a window's review
// page, VERSION the read's, FILE the file's index in it. Numbers only:
// nothing in the address is a path or a name.
std::string pictureTarget(const std::string& base, int page, std::uint64_t version, std::size_t file,
                          bool newSide);
// The parts of what follows "picture/"; false unless it is exactly that.
bool pictureParts(const std::string& rest, int* page, std::uint64_t* version, std::size_t* file, bool* newSide);
// A file's picture address, for reviewJson (null: the pictures are named
// in a note instead).
using PictureUrl = std::function<std::string(std::size_t file, bool newSide)>;

// One file of the review: {key, section, letter, path, orig, header,
// added, removed, canSide, side, note | rows, images}. A file shown as
// pictures (File::image) has `images`, [{side: "old" | "new", label,
// width, height, bytes, url}], the old first, and no rows of them; its
// label is the window's ("Before: 4 × 3 pixels, 75 bytes"). Inline rows, as the review
// page lays them out (ChangeReview::layout, three unchanged lines kept
// beside each block): ["c", oldLine, newLine, text, syntax] an unchanged
// line, ["r", oldLine, 0, text, syntax, words] removed, ["a", 0, newLine,
// text, syntax, words] added, ["x", 0, line, text, syntax, part] a line of
// a conflict block, ["g", id, hidden, text] folded lines. Side by side
// (`side`, when `canSide`): ["s", kind ("c" same, "m" changed, "r", "a"),
// oldLine, newLine, oldText, newText, oldSyntax, newSyntax, oldWords,
// newWords] and the same gaps. Lines count from 1 (0: none on that side);
// spans are [start, length, style] in UTF-16 units, a syntax style a
// TokenStyle, a word's style 0 (removed) or 1 (added). `opened` holds the
// gaps shown whole, by id (the new line their hidden lines start on).
// `file` is its index in the read, for `picture`.
Json reviewFileJson(const ChangeReview::FileDiff& d, const std::set<int>& opened, bool side,
                    const PictureUrl& picture = nullptr, std::size_t file = 0);

// The whole review: {t: "review", version, files: [reviewFileJson, or the
// header alone with `more` past kReviewRowBudget], error}. `opened` and
// `side` by file key.
Json reviewJson(const ChangeReview::Read& r, std::uint64_t version, const ChangeReview::Opened& opened,
                const std::set<std::string>& side, std::size_t budget = kReviewRowBudget,
                const PictureUrl& picture = nullptr);

// ---- the window's review pages (R3): a commit's page and a worktree's
// compared with main, as the window has them open. The window reports
// them beside its tabs (`reviews`); the page picks one by its id, and the
// server reads it with git as the window would, from what it checked:
// a commit by hex digits only, in the session's folder's repository; a
// worktree only when that repository's `worktree list` names it, compared
// with the branch the list says the main checkout is on (never a name the
// report gives). Read only: no Apply to Main, nothing kept or rejected.
struct ReviewPage {
    std::string kind;     // "commit" or "worktree"
    std::string hash;     // a commit's name: hex digits
    std::string path;     // a worktree's folder, absolute
    std::string title;    // the window's tab's title
    bool onShow = false;  // the tab on show in its group
    std::string id() const;   // "commit:HASH" or "worktree:PATH"
};
constexpr std::size_t kMaxReviewPages = 32;
// 4 to 64 hex digits: what a commit's page may name.
bool commitName(const std::string& s);
// The window's report: [{kind, hash | path, title, onShow}], the pages
// that do not hold up left out, at most kMaxReviewPages.
Json reviewsReportJson(const std::vector<ReviewPage>& pages);
// The report checked again (it is a renderer's): kinds, commit names and
// absolute paths with no control characters, titles cut to 300 bytes and
// made valid UTF-8, each id once, at most kMaxReviewPages.
std::vector<ReviewPage> reviewPagesFrom(const Json& report);
// What a page is told: [{id, kind, title, onShow}].
Json reviewsViewJson(const std::vector<ReviewPage>& pages);

// A window's review page as read: the review, and its header as the
// window's page words it (a commit's subject, its byline and the counts,
// the rest of its message; a worktree's "x compared with main", what it is
// compared against, its branch and the counts).
struct ReviewRead {
    ChangeReview::Read read;
    std::string title, detail, body;
};
// Reads `p` for a page of the session at `folder` (`hidden`: the tree's
// Show Hidden Files), held to what it may show: a commit's files under the
// folder (no dotfiles unless hidden), a worktree's under its own folder,
// with its work tree's links shown as links (confineReview). `now` dates
// a commit's byline.
ReviewRead readReviewPage(const std::string& folder, bool hidden, const ReviewPage& p,
                          const ChangeReview::GitRunner& run, long long now);

// A page's `opened` ({key: [id, ...]}) and `side` ([key, ...]), capped.
ChangeReview::Opened openedFrom(const Json& j);
std::set<std::string> sideFrom(const Json& j);

// Reads of the folder's uncommitted changes on a thread of their own (any
// thread may call). `wake` is called (on the reading thread) when a read
// finishes; it is never called once this object is gone, and a read under
// way then finishes on its own and is dropped, so destroying this never
// waits for git.
class GitReads {
public:
    // The folder's uncommitted changes (readUncommitted, its pictures
    // sized by pageImages, held by confineReview).
    GitReads(std::string folder, bool hidden, ChangeReview::GitRunner run, std::function<void()> wake);
    // Whatever `reader` reads (a window's review page, readReviewPage),
    // on the same terms.
    GitReads(std::function<ReviewRead()> reader, std::function<void()> wake);
    ~GitReads();
    GitReads(const GitReads&) = delete;
    GitReads& operator=(const GitReads&) = delete;

    // A read now, or once the one under way ends (asks while one runs are
    // one more read, not one each).
    void ask();
    struct Latest {
        std::shared_ptr<const ChangeReview::Read> read;   // null before the first ends
        std::shared_ptr<const ReviewRead> page;           // the same read, with its header
        std::uint64_t version = 0;    // counts reads that showed something new
        std::uint64_t finished = 0;   // counts every read that ended
        bool reading = false;
    };
    Latest latest() const;

private:
    struct State;
    std::shared_ptr<State> s_;
};

}  // namespace Web
}  // namespace Shared
