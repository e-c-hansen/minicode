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
// - Entries whose name starts with '.' are skipped (files and folders), and so
//   are folders named node_modules, build, .build, DerivedData, dist, .venv,
//   venv and __pycache__. The scope folder itself is always searched, even if
//   its own name is on that list.
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
#include <string>
#include <vector>

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
};

struct FolderSearchOptions {
    std::size_t   maxMatches   = 2000;
    std::uintmax_t maxFileSize = 1024 * 1024;   // bytes; larger files are skipped
    std::size_t   maxLineChars = 200;           // display text cap, in characters
    bool          caseSensitive = false;        // the Mac panel is insensitive
};

struct FolderSearchResult {
    std::vector<FolderSearchMatch> matches;
    std::size_t filesSearched = 0;   // text files actually scanned
    std::size_t filesMatched  = 0;   // files with at least one match
    bool truncated = false;          // stopped at maxMatches
    bool cancelled = false;          // the cancel flag was raised mid-search
};

namespace FolderSearch {

// The shortest query, in characters, that is searched.
constexpr std::size_t kMinQueryChars = 2;

// The query as it will be searched: surrounding whitespace trimmed.
std::string normalizeQuery(const std::string& query);

// True when normalizeQuery(query) has at least kMinQueryChars characters.
bool isSearchable(const std::string& query);

// Directory names never descended into (besides every name starting with '.').
bool isSkippedDirectory(const std::string& name);

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

}  // namespace FolderSearch
