// FolderPicker.h — the decisions of mutible's own folder picker, the
// palette that Open Folder… shows in place of the system's open panel: what
// the typed text asks for, which rows to list and in what order, and what
// Tab makes of the text.
//
// The text is either a name or a path:
//
//   a name ("mini")         matched by the palette's ranking (CommandPalette
//                           rankFiles) against the recent folders, then the
//                           subfolders of the base folder (the window's)
//   a path ("~/Code/mi",    one holding a '/' or starting "~": the subfolders
//   "/Users/", "../")       of the folder before the last '/' (relative ones
//                           from the base folder) matched against the name
//                           after it
//
// Hidden folders (a leading dot) are listed only when the name typed starts
// with a dot. Paths are spelled with "~" for the home folder in the rows.
//
// A scoped picker (Add Folder to Focus…, the scope the window's folder)
// offers only folders strictly inside the scope: rows, the folder typed
// out, and no Choose in Finder…; its rows are labelled by their path from
// the scope ("linux/src"), and Tab after a name completes that path.
// Pure C++17; listSubfolders is the one piece of I/O, run off the main
// thread with a cancel flag.
#pragma once
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace FolderPicker {

// What the text asks for.
struct Input {
    bool isPath = false;
    std::string dir;       // absolute: the folder whose subfolders are listed
                           // (a path), or the base folder (a name)
    std::string partial;   // the name typed after the last '/' (a path), or
                           // the whole query, trimmed (a name)
    std::string typed;     // the folder the text names, "" for none: the dir
                           // itself when nothing follows the '/', else
                           // dir/partial; a name: base/query
    bool showHidden = false;   // partial starts with '.'
    std::string scope;         // absolute, normalized; "" for none
};

// home and base are absolute; base is the window's folder (or home). scope,
// when given, is the folder whose subfolders alone are offered (absolute).
Input parse(const std::string& text, const std::string& home, const std::string& base,
            const std::string& scope = "");

// Is path strictly inside scope (lexically, both absolute)? Everything is
// when scope is empty.
bool inScope(const std::string& path, const std::string& scope);

// Lexically: "//" to "/", "." and ".." resolved ("/.." stays "/"), no
// trailing '/' except for "/" itself. Symlinks are not followed.
std::string normalize(const std::string& path);
// dir and name with one '/' between.
std::string join(const std::string& dir, const std::string& name);
// path with home spelled "~" ("~" alone for home itself).
std::string abbreviate(const std::string& path, const std::string& home);
// The last component; "/" for the root, "~" for home spelled so.
std::string lastComponent(const std::string& path);

// Tab, and '/' typed after a name: the text that lists the folder at path.
// A path keeps its own spelling of the folder before the last '/' and gets
// path's name and a '/'; a name becomes path abbreviated with a '/'.
// With a scope, a name becomes path's path from the scope with a '/'.
std::string complete(const std::string& text, const std::string& path, const std::string& home,
                     const std::string& scope = "");
// Does a '/' typed at the end of text complete the selected row (rather
// than go into the text as typed)? Not after nothing, a '/', "~", "." or
// "..", whose '/' is a path of its own.
bool slashCompletes(const std::string& text);

// A folder's subfolders, by name (symlinks to folders count), sorted with
// ASCII case ignored, hidden ones included; at most maxEntries. *ok is
// false when the folder cannot be read. A raised cancel stops early.
std::vector<std::string> listSubfolders(const std::string& dir,
                                        const std::atomic<bool>* cancel = nullptr,
                                        bool* ok = nullptr, size_t maxEntries = 20000);

// A folder's listing as the picker has it: not yet (done false), failed
// (ok false), or its subfolders.
struct Listing {
    bool done = false;
    bool ok = false;
    std::vector<std::string> names;
};

enum class RowKind { Folder, OpenTyped, ChooseInFinder };
struct Row {
    RowKind kind = RowKind::Folder;
    std::string path;              // absolute; "" for ChooseInFinder
    std::u16string label;          // the path abbreviated ("~/Code/mutible")
    size_t nameAt = 0;             // where the last component starts in label
    std::vector<size_t> positions; // label's matched UTF-16 units
    bool recent = false;
};

// The rows for the input, best first: for a name, the recent folders and
// the listing's (base's) subfolders that match (an empty query lists them
// in that order, recent ones first, each once); for a path, the listing's
// (dir's) subfolders whose names match. OpenTyped is the folder the text
// names when the listing shows it exists: first when the text ends in '/'
// (Return opens it), else after the matches. ChooseInFinder is always last,
// except with a scope (in.scope), which also drops every folder not
// strictly inside it. At most limit folder rows.
std::vector<Row> rows(const Input& in, const std::vector<std::string>& recent,
                      const Listing& listing, const std::string& home, size_t limit = 2000);

// What to say under the field when no folder row is listed: listing, a
// folder that cannot be read, a folder outside the scope, or nothing
// matching.
std::string emptyText(const Input& in, const Listing& listing, const std::string& home);

}  // namespace FolderPicker
