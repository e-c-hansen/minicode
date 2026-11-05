// CommandPalette.h — the command palette's decisions (Shift+Cmd+P): fuzzy
// matching of what is typed against the menu's commands, their order, the
// list of recently run commands, and a key equivalent drawn as glyphs.
//
// Pure C++, no I/O, in UTF-16 code units like LineComments and MultiCaret,
// so matched positions map straight onto NSRange. Which commands exist and
// whether they are enabled is the GUI's business (src/PalettePanel.mm walks
// the main menu); everything here works on their labels alone.
#pragma once
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

}  // namespace CommandPalette
