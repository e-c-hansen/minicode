// CommandPalette.h — the command palette's decisions (Shift+Cmd+P): fuzzy
// matching of what is typed against the menu's commands, their order, the
// list of recently run commands, and a key equivalent drawn as glyphs.
//
// The same palette is Quick Open (Cmd+P: the folder's files), Go to Line
// (Ctrl+G, or ":" typed first), Go to Symbol in File (Shift+Cmd+O, "@") and
// Go to Symbol in Workspace (Cmd+T, "#"); ">" typed first switches to
// commands, as in VS Code (see Mode).
//
// Pure C++ in UTF-16 code units like LineComments and MultiCaret, so matched
// positions map straight onto NSRange. Which commands exist and whether they
// are enabled is the GUI's business (src/PalettePanel.mm walks the main
// menu); everything here works on their labels alone. The one piece of I/O
// is listFiles, the walk Quick Open lists, run off the main thread.
#pragma once
#include "LspClient.h"   // Lsp::Symbol

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace CommandPalette {

struct Entry {
    std::u16string label;   // "View > Toggle Terminal": matched and shown
    std::u16string title;   // "Toggle Terminal": the item's own title
    std::string id;         // the key in the recent list (the label, UTF-8)
};

// How good a match is, best first. Within a tier the most recently run
// command comes first, then the finer score, then the menu's order.
enum class Tier {
    Prefix = 0,      // the query starts the item's title (or the whole label)
    WordStarts = 1,  // every run of matched characters begins a word
    Substring = 2,   // one run inside a word
    Scattered = 3,   // any other subsequence
};

struct Match {
    size_t index = 0;                 // into the entries
    Tier tier = Tier::Scattered;
    int score = 0;                    // higher is better, within a tier
    std::vector<size_t> positions;    // the label's matched UTF-16 units
};

// Is query a subsequence of text (ASCII case ignored, spaces in the query
// skipped)? Then the best alignment's score and positions: word starts and
// runs of consecutive characters score high, gaps cost a little.
bool match(const std::u16string& query, const std::u16string& text, int* score,
           std::vector<size_t>* positions);

// The tier of an alignment found by match().
Tier tierOf(const std::u16string& text, const std::vector<size_t>& positions);

// Does text[i] begin a word: the first character, one after a space or
// punctuation, or a capital after a lowercase letter (camelCase)?
bool wordStart(const std::u16string& text, size_t i);

// The entries that match, best first (see Tier). An empty query lists
// every entry: recently run ones first, most recent first, then the rest
// in their order.
std::vector<Match> rank(const std::vector<Entry>& entries, const std::u16string& query,
                        const std::vector<std::string>& recent);

// The recent list after running id: id first, without its earlier place,
// at most cap long.
std::vector<std::string> noteRun(std::vector<std::string> recent, const std::string& id,
                                 size_t cap = 20);

// Modifier bits for keyLabel, as on a Mac menu.
enum Mod : unsigned { Shift = 1, Control = 2, Option = 4, Command = 8 };

// A key equivalent as the menu draws it: modifiers in Apple's order
// (⌃ ⌥ ⇧ ⌘), then the key: letters in capitals (a capital key implies
// Shift, as in NSMenuItem), F1 to F35, arrows, ⏎ ⇥ ⎋ ⌫ ⌦, Space. An empty
// key gives an empty label.
std::u16string keyLabel(const std::u16string& key, unsigned mods);

// ------------------------------------------------------------ the modes
// What the field's text asks for: a leading ">" is a command, a leading ":"
// a line of the file on show, "@" a symbol of that file ("@:" grouped by
// kind), "#" a symbol of the whole workspace, anything else a file of the
// folder.
enum class Mode { Files, Commands, Line, Symbols, WorkspaceSymbols };
struct Parsed {
    Mode mode = Mode::Files;
    std::u16string query;      // the text after the prefix, trimmed
    int line = 0, column = 0;  // Line mode: 0 when not typed (yet)
    bool lineValid = false;    // Line mode: digits, then ":digits" at most
    bool byKind = false;       // Symbols mode: "@:", grouped by kind
};
Parsed parse(const std::u16string& text);

// ------------------------------------------------------------ Quick Open
// The files below root that Quick Open lists, as '/'-separated paths
// relative to it, by Find in Folder's rules (FolderSearch.h) for what is
// skipped: names starting with '.', and node_modules, build, venv and the
// other folders isSkippedDirectory names. Symlinked folders are followed
// once each. Nearer files come first (breadth first, each folder's files in
// byte order), at most maxFiles; *truncated says it stopped at the cap. A
// raised cancel stops it early.
std::vector<std::string> listFiles(const std::string& root, size_t maxFiles = 20000,
                                   const std::atomic<bool>* cancel = nullptr,
                                   bool* truncated = nullptr);

// The files that match query, best first. A query without '/' is matched
// against the file name first (any such match beats one that needs the
// folders), then against the whole path; a query with '/' against the path.
// Within a tier, recently opened files (relative paths, most recent first)
// come first, then the score, then shorter paths. An empty query lists the
// recent files first, then the rest in their order. Positions are in the
// path. At most limit results.
std::vector<Match> rankFiles(const std::vector<std::u16string>& paths,
                             const std::u16string& query,
                             const std::vector<std::u16string>& recent,
                             size_t limit = 500);

// ------------------------------------------------------------ symbols
// A row of Go to Symbol's list: a symbol, or (grouped by kind) a heading
// naming a kind and how many of it are listed, "functions (3)".
struct SymbolRow {
    bool heading = false;
    size_t symbol = 0;        // a symbol row's index into the symbols
    std::u16string text;      // a heading's
    Match match;              // a symbol row's; positions are in its name
};
// The symbols whose names match query, by rank()'s rules; an empty query
// keeps them in their order (the file's). Grouped by kind, each kind gets
// a heading, the kinds in the order their first symbol comes. *preselect
// is the row to select first: with an empty query the row of the symbol
// at the caret (caretSymbol, an index into the symbols, -1 for none),
// otherwise, and when that symbol is not listed, the first symbol row; -1
// when there is none. At most limit symbols are listed.
std::vector<SymbolRow> symbolRows(const std::vector<Lsp::Symbol>& symbols,
                                  const std::u16string& query, bool byKind, int caretSymbol,
                                  int* preselect, size_t limit = 2000);

}  // namespace CommandPalette
