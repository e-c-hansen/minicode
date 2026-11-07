// FolderSearch.h — project-wide text search, the pure C++17 core of "Find in
// Folder". No GUI, no third-party dependencies: std::filesystem to walk the
// tree and plain byte scanning to match. The GTK port runs it on a worker
// thread; Android can call it through JNI the same way.
//
// The semantics are the macOS search panel's (src/Search.mm), so the three
// front ends agree on what a search finds:
//
// - The query is trimmed of surrounding whitespace and needs at least two
//   characters; anything shorter is not searched at all.
// - Matching is case-insensitive by default. Folding is one-to-one per
//   character and covers ASCII, Latin-1, Latin Extended-A, Greek and
//   Cyrillic. NSString's full Unicode folding also equates canonically
//   equivalent sequences and "ß" with "ss"; this does not.
// - Dotfiles and dot folders are searched (.github, .env, .claude), as VS
//   Code searches them, except for the folders isSkippedDirectory names
//   (.git, .svn, .hg, node_modules, build, .build, DerivedData, dist, .venv,
//   venv, __pycache__ and the tools' cache folders) and .DS_Store. The scope
//   folder itself is always searched, even if its own name is on that list.
// - With useIgnoreFiles (the default, search.use-ignore-files), what the
//   folder's .gitignore and .ignore files ignore is left out too, as VS Code
//   and ripgrep leave it out: every such file from the window's folder
//   (ignoreTop) down, and .git/info/exclude at its top. See Ignore below.
//   (Until 2026-11-06 every dotfile was skipped and no ignore file read.)
// - Files over 1 MB are skipped, and so is any file that is not valid UTF-8,
//   which is how binaries fall out (the Mac drops them the same way, when
//   NSString refuses to decode them).
// - Search stops at 2000 matches.
// - The display text is the matched line with ANSI escape sequences and other
//   control characters removed, trimmed, and cut to 200 characters.
//
// Where it deliberately differs from Search.mm, it is safer, not different in
// what it finds in ordinary projects:
// - Symlinked folders are followed, as on the Mac, but each real folder is
//   visited once, so a symlink loop cannot recurse forever.
// - Only regular files are read (after following symlinks), so a named pipe
//   in the tree cannot block the search. The size limit applies to the file a
//   symlink points at, not to the link.
// - Lines end at \n, \r\n, \r and U+2029, the separators GtkTextBuffer also
//   uses, so a match's line number is the line the editor shows. (NSString also
//   splits on U+2028 and U+0085.)
// - A file's entries are visited in byte order of their names, files before
//   subfolders, so results come out in a stable order.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "TextFind.h"

struct FolderSearchMatch {
    std::string path;          // full path: the root joined with relativePath
    std::string relativePath;  // relative to the searched root, '/' separated
    int         line = 0;      // 1-based line number
    int         column = 0;    // 1-based column of the match, in characters
                               // (code points) of the raw line
    std::size_t byteColumn = 0;  // 0-based byte offset of the match in the raw
                                 // line; GtkTextBuffer's "line index"
    std::size_t byteLength = 0;  // bytes the match covers in the raw line
    std::string text;          // the line for display: cleaned, trimmed, capped
    // Matches on the line, counted as Replace in Folder counts them (a row
    // is a line, so "a = a" is one row and two matches). 1 for a TODO.
    std::size_t count = 1;
};

struct FolderSearchOptions {
    std::size_t   maxMatches   = 2000;
    std::uintmax_t maxFileSize = 1024 * 1024;   // bytes; larger files are skipped
    std::size_t   maxLineChars = 200;           // display text cap, in characters
    bool          caseSensitive = false;        // the Mac panel is insensitive
    // VS Code's other two options (TextFind.h has the rules). With either
    // set the line is matched by TextFind; a regex is not trimmed and needs
    // one character, not two.
    bool          wholeWord = false;
    bool          regex = false;
    bool          preserveCase = false;         // Replace in Folder's AB toggle
    // VS Code's "files to include" and "files to exclude": comma-separated
    // globs (see PathFilter). Empty includes everything, excludes nothing.
    std::string   include;
    std::string   exclude;
    // Leave out what .gitignore and .ignore files ignore (Ignore below).
    bool          useIgnoreFiles = true;
    // The folder whose ignore files apply, from its top: the window's
    // folder, when the scope searched is a folder inside it. "" (or a
    // folder the scope is not inside) means the scope itself.
    std::string   ignoreTop;
};

struct FolderSearchResult {
    std::vector<FolderSearchMatch> matches;
    std::size_t filesSearched = 0;   // text files actually scanned
    std::size_t filesMatched  = 0;   // files with at least one match
    std::size_t totalMatches  = 0;   // the matches' counts added up: what the status says
    bool truncated = false;          // stopped at maxMatches
    bool cancelled = false;          // the cancel flag was raised mid-search
    // A regex left to std::regex took too long on a line, and the search
    // stopped there (TextFind::Query::tooSlow).
    bool tooSlow = false;
    std::string error;               // the regex did not compile: why
};

// Replace in Folder's count before it asks: every match in every file the
// search would read (no limit on matches), by file.
// A file reached under two names (a symlink, a symlinked folder, a hard
// link) is counted, and replaced, once: under the first name the walk meets.
struct FolderReplaceFile {
    std::string path;
    std::string relativePath;
    std::size_t matches = 0;
    // The file itself is outside the folder searched, reached through a
    // symlink: Replace in Folder names it before writing there.
    bool outside = false;
};
struct FolderReplacePlan {
    std::vector<FolderReplaceFile> files;   // only files with matches, in search order
    std::size_t matches = 0;                // in all of them
    bool cancelled = false;
    std::string error;                      // the regex did not compile: why
};

namespace FolderSearch {

// The shortest query, in characters, that is searched.
constexpr std::size_t kMinQueryChars = 2;

// The query as it will be searched: surrounding whitespace trimmed.
std::string normalizeQuery(const std::string& query);

// True when normalizeQuery(query) has at least kMinQueryChars characters.
bool isSearchable(const std::string& query);

// Directory names never descended into: version control's own folders,
// dependencies, build output, virtual environments and the tools' caches.
bool isSkippedDirectory(const std::string& name);
// File names never listed or searched (.DS_Store).
bool isSkippedFile(const std::string& name);

// .gitignore and .ignore, as git reads them (gitignore(5)): blank lines and
// "#" comments skipped, trailing spaces dropped unless escaped, "!" to
// include again, a trailing "/" for folders only, a "/" anywhere else
// anchoring the pattern to the file's folder (otherwise it matches a name
// at any depth below), and git's wildmatch for "*", "?", "[...]" and "**".
// A deeper file's rules win over a shallower one's, .ignore over .gitignore
// in one folder (as ripgrep has it), and the last matching rule in a file
// decides. A folder that is ignored is not entered, so, as in git, nothing
// below it can be included again. Matching folds case on the Mac, where git
// sets core.ignorecase for its case-insensitive file systems.
namespace Ignore {

struct Rule {
    std::string pattern;   // without "!", the trailing "/" and a leading "/"
    bool negate = false;
    bool dirOnly = false;
    bool anchored = false;   // matched against the path from the file's folder
};
std::vector<Rule> parse(const std::string& text);

// git's wildmatch with WM_PATHNAME: "*" and "?" never match "/", and "**"
// as a whole path component matches any number of folders. caseFold: ASCII
// letters match either case.
bool wildmatch(const std::string& pattern, const std::string& text, bool caseFold = false);

// Whether to fold case by default: true on the Mac.
bool defaultCaseFold();

// The rules in effect in one folder of a walk: its own ignore files and its
// parents'. Copying is cheap (they are shared), so a breadth-first walk can
// keep one per queued folder.
class Rules {
public:
    Rules() = default;   // nothing is ignored
    // At the walk's top: .git/info/exclude when the top holds .git, then its
    // own .gitignore and .ignore.
    static Rules atTop(const std::string& top, bool caseFold = defaultCaseFold());
    // The rules for a subfolder: these plus its own files. relDir is its
    // path from the top, '/' separated, no trailing '/'.
    Rules enter(const std::string& dirPath, const std::string& relDir) const;
    // The same with the files' text given ("" for one that is not there),
    // for tests and for callers that read them themselves.
    Rules with(const std::string& relDir, const std::string& gitignore,
               const std::string& ignore) const;
    // Is the entry at relPath (from the top) ignored?
    bool ignored(const std::string& relPath, bool isDir) const;
    bool empty() const { return !head_; }
    void setCaseFold(bool fold) { fold_ = fold; }

private:
    struct Node;
    std::shared_ptr<const Node> head_;
    bool fold_ = false;
};

// The rules at `scope` for a walk of it when `top`'s ignore files apply
// (top "", or a folder scope is not inside: scope's own). *scopeRel is the
// scope's path from the top ("" when they are one).
Rules rulesFor(const std::string& top, const std::string& scope, std::string* scopeRel,
               bool caseFold = defaultCaseFold());

}  // namespace Ignore

// Strict UTF-8 validation: no overlong forms, surrogates or values past
// U+10FFFF.
bool isValidUtf8(const std::string& s);

// Remove ANSI escape sequences (ESC, a byte in @-_, parameters, intermediates,
// a final byte) and then every remaining control or invisible format
// character: C0, DEL, C1, soft hyphen, zero-width and bidirectional controls,
// and the byte-order mark. Input must be valid UTF-8.
std::string cleanLine(const std::string& line);

// cleanLine, then whitespace trimmed at both ends, then cut to maxChars
// characters (never inside a character).
std::string displayText(const std::string& rawLine, std::size_t maxChars = 200);

// Find the first occurrence of `query` in `line` (both valid UTF-8). On a hit
// sets the byte offset and byte length within `line` and returns true.
bool findInLine(const std::string& line, const std::string& query,
                bool caseSensitive, std::size_t* byteColumn,
                std::size_t* byteLength);

// Search every eligible file under `root` for `query` (normalized first). An
// unsearchable query or a root that is not a folder gives an empty result.
// `cancel`, if given, is polled between files and every few thousand lines; a
// raised flag stops the search and sets `cancelled`, keeping what was found.
FolderSearchResult search(const std::string& root, const std::string& query,
                          const std::atomic<bool>* cancel = nullptr,
                          const FolderSearchOptions& options = {});

// The query search() matches with these options: plain text trimmed as
// normalizeQuery does, a regex as typed. Not usable() when there is nothing
// to search for (see TextFind::Query::error for a regex that does not
// compile).
TextFind::Query makeQuery(const std::string& query, const FolderSearchOptions& options);

// Whether search() would run this query: two characters of plain text, or a
// regex of one character or more.
bool isSearchable(const std::string& query, const FolderSearchOptions& options);

// Count what Replace in Folder would replace: the same walk and files as
// search(), every match in each.
FolderReplacePlan planReplace(const std::string& root, const std::string& query,
                              const std::atomic<bool>* cancel = nullptr,
                              const FolderSearchOptions& options = {});

// `content` (valid UTF-8) with every match of the query replaced, as
// Replace in Folder writes it: TextFind::replaceInText with makeQuery's
// query. Line endings are kept. *count says how many were replaced.
std::string replaceInText(const std::string& content, const std::string& query,
                          const std::string& replacement, const FolderSearchOptions& options,
                          std::size_t* count = nullptr);

// Only the match a result row stands for: the one starting at byteColumn of
// 1-based `line`. False when it is no longer there. With `expectedText`
// (the row's text, FolderSearchMatch::text) the line must also still read
// as it did when searched, so a file changed since cannot have another
// occurrence replaced in its place.
bool replaceMatchInText(const std::string& content, const std::string& query,
                        const std::string& replacement, const FolderSearchOptions& options,
                        int line, std::size_t byteColumn, std::string* out,
                        const std::string* expectedText = nullptr);

// Line `line` (1-based) of `content`, its line break left off, with lines
// ending as search() ends them. False past the last line.
bool lineAt(const std::string& content, int line, std::string* out);

// Read a file as search() does: a regular file (after symlinks) of at most
// maxFileSize bytes holding valid UTF-8. False otherwise.
bool readTextFile(const std::string& path, std::uintmax_t maxFileSize, std::string* out);

// What stat() says of a file (symlinks followed): enough to tell whether it
// changed since it was last read or written.
struct FileStamp {
    bool exists = false;     // something is there
    bool regular = false;    // a regular file
    std::int64_t mtimeSec = 0;
    long mtimeNsec = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    bool operator==(const FileStamp& o) const {
        return exists == o.exists && regular == o.regular && mtimeSec == o.mtimeSec &&
               mtimeNsec == o.mtimeNsec && device == o.device && inode == o.inode &&
               size == o.size;
    }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};
FileStamp stampOf(const std::string& path);

enum class ReadStatus { Ok, Missing, NotRegular, TooLarge, Unreadable };

// Read a file's bytes as they are (no UTF-8 check), and the stamp of the
// file the bytes came from, taken *before* reading: a writer that finishes
// while the file is read then shows as a change on the next look, never as
// the text read. The file is stat'ed again after the read, and read again
// (up to three times) when the two differ.
ReadStatus readFileStable(const std::string& path, std::uintmax_t maxFileSize,
                          std::string* out, FileStamp* stamp);

// A UTF-8 byte order mark at the start of `bytes`: removed, and true.
bool stripUtf8Bom(std::string* bytes);

// Write `content` to `path` so a reader sees the old file or the new one,
// never half of each: a temporary in the same folder, given the file's
// permissions, owner, extended attributes, ACL and (on the Mac) creation
// date, flushed to the disk itself (F_FULLFSYNC on the Mac), then renamed
// over it, and the folder synced. A symlink is followed, so the link stays
// a link; a dangling one too, creating the file it names (in a folder that
// must exist). A file with other hard links, or one this user cannot give its
// owner back to, is written in place instead, so the other names keep
// seeing it and its owner stays. False, with *error saying why, when
// nothing was written. *written, when given, is the file's stamp as
// written (taken from the open file, so a writer coming right after is
// never mistaken for this one).
bool writeFileAtomically(const std::string& path, const std::string& content,
                         std::string* error = nullptr, FileStamp* written = nullptr);

// The TODO list. Instead of a query, every line holding a TODO, FIXME,
// HACK, XXX or BUG tag (uppercase, a whole word) after a comment opener
// (//, #, --, ;, /*, *, <!--, %), or first on its line with a colon after
// it; and in Markdown files every open task ("- [ ] ..."), as the parser
// reads them, so none from a code block. The match covers the tag or the
// box. The same walk, limits and order as search().
FolderSearchResult findTodos(const std::string& root,
                             const std::atomic<bool>* cancel = nullptr,
                             const FolderSearchOptions& options = {});

// The first TODO tag on a line, as findTodos reads lines. In a Markdown file
// (`markdown`) only an HTML comment counts as a comment, since # starts a
// heading there.
bool findTodoInLine(const std::string& line, bool markdown, std::size_t* byteColumn,
                    std::size_t* byteLength);

// .md, .markdown, .mdown, .mkd or .mkdn, in any case.
bool isMarkdownPath(const std::string& path);

// ------------------------------------------- files to include / exclude
// A glob matched against a path relative to the searched folder, '/'
// separated: * is any run of characters but '/', ? one of them, ** as a
// whole segment any number of segments (none included), [abc], [a-z] and
// [!abc] (or [^abc]) one character of a set, {a,b} either alternative
// (nested braces too). Case counts, as in ripgrep's globs.
bool globMatch(const std::string& pattern, const std::string& path);

// The patterns of a comma-separated list, commas inside {} and [] kept,
// each trimmed, empty ones dropped.
std::vector<std::string> splitGlobList(const std::string& text);

// What VS Code's search view makes of the include and exclude boxes
// (queryBuilder.ts, parseSearchPaths):
// - "./src", "../x", "/abs/path" and "~/path" are search paths: that file or
//   folder, and everything under it, anchored at the searched folder (an
//   absolute one outside it matches nothing; "./" alone matches everything).
// - Anything else matches at any depth, as itself or as a folder holding
//   the path: "foo/bar" is **/foo/bar and **/foo/bar/**; ".js" is *.js.
class PathFilter {
public:
    PathFilter() = default;
    PathFilter(const std::string& text, const std::string& root);
    bool empty() const { return globs_.empty(); }
    // Whether a path relative to the root (a file, or a folder) matches.
    bool matches(const std::string& relativePath) const;
    // The globs it expanded to, for tests.
    const std::vector<std::string>& globs() const { return globs_; }

private:
    std::vector<std::string> globs_;
};

}  // namespace FolderSearch
