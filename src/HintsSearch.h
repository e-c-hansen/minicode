// HintsSearch.h — searching the shortcut hints (Shift+Cmd+H): what a
// query in the panel's box finds in a shortcut's key and label, where to
// highlight it, and the list with what it found moved first
// (bubbleMatches). Header only, no dependencies, UTF-16 (the Mac's
// strings; a port with UTF-8 converts). The Mac draws, the core decides.
// Tested in tests/run_tests.cpp (hints:search, hints:key-labels,
// hints:bubble).
//
// A query is words split at spaces; a shortcut matches when every word is
// found in it. A word is matched without regard to case in the key and the
// label alike, and in the key also by name: "cmd" or "command" finds ⌘,
// "shift" ⇧, "opt", "option" or "alt" ⌥, "ctrl" or "control" ⌃, "return"
// or "enter" ⏎, and so on, a name's first three letters or more counting
// ("sh" is only text, "shi" is ⇧ too). A word joined with "+" is one key
// as the hints spell it, its modifiers in the hints' order whatever order
// they were typed in: "cmd+shift+h" finds ⇧⌘H.
//
// The keys are spelled as Commands::keyLabel spells them for the platform
// (Spelling): the Mac's in Apple's symbols, "⇧⌘H"; Linux's with Ctrl, Alt
// and Super in words and the rest in symbols, a thin space between the
// parts, "Ctrl ⇧ H", "Ctrl ⏎", "Ctrl Alt ⇧ Page Down". With Linux's
// spelling "ctrl", "control", "cmd" and "command" find Ctrl (a Mac user's
// Cmd is Ctrl there); "alt", "opt" and "option" Alt; "super", "win" and
// "meta" Super; "shift" ⇧; and "shift+ctrl+h" finds Ctrl ⇧ H.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace HintsSearch {

// How the keys being searched are spelled (Commands::keyLabel).
enum class Spelling { Mac, Linux };

// One word of the query: its text in lower case, and what it is as a key
// (spellings, looked for in keys only).
struct Word {
    std::u16string text;
    std::vector<std::u16string> keys;
};

// A shortcut's parts as shown: its key (or keys, "⌘= ⌘-") and its label,
// and a pane's state ("open").
struct Field {
    std::u16string text;
    bool key = false;
};

// A stretch of one field to highlight, in UTF-16 units.
struct Span {
    size_t field = 0, start = 0, length = 0;
};

struct Match {
    bool matches = true;      // every word found (an empty query finds all)
    std::vector<Span> spans;  // sorted, none overlapping; empty when !matches
};

inline char16_t fold(char16_t c) {
    if (c >= u'A' && c <= u'Z') return char16_t(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return char16_t(c + 32);   // Latin-1 capitals
    return c;
}

inline std::u16string folded(const std::u16string& s) {
    std::u16string out(s);
    for (char16_t& c : out) c = fold(c);
    return out;
}

// The keys' names, as Commands::glyphs spells them.
struct KeyName {
    const char16_t* name;
    char16_t glyph;
    bool modifier;
};

inline const std::vector<KeyName>& keyNames() {
    static const std::vector<KeyName> names = {
        {u"cmd", u'⌘', true},       {u"command", u'⌘', true},
        {u"shift", u'⇧', true},     {u"opt", u'⌥', true},
        {u"option", u'⌥', true},    {u"alt", u'⌥', true},
        {u"ctrl", u'⌃', true},      {u"control", u'⌃', true},
        {u"return", u'⏎', false},   {u"enter", u'⏎', false},
        {u"esc", u'⎋', false},      {u"escape", u'⎋', false},
        {u"tab", u'⇥', false},      {u"backspace", u'⌫', false},
        {u"delete", u'⌫', false},   {u"delete", u'⌦', false},
        {u"up", u'↑', false},       {u"down", u'↓', false},
        {u"left", u'←', false},     {u"right", u'→', false},
        {u"home", u'↖', false},     {u"end", u'↘', false},
        {u"pageup", u'⇞', false},   {u"pagedown", u'⇟', false},
    };
    return names;
}

// The modifiers' order in a key as the hints show it: ⌃ ⌥ ⇧ ⌘.
inline int modifierRank(char16_t g) {
    switch (g) {
        case u'⌃': return 0;
        case u'⌥': return 1;
        case u'⇧': return 2;
        case u'⌘': return 3;
        default: return -1;
    }
}

// The keys' names on Linux: the Mac's, with "cmd" and "command" naming Ctrl
// (a Mac user's Cmd is Ctrl there), and "super", "win" and "meta" naming
// Super, which the table holds as ⌘ (Commands::Key's Cmd), spelled "super".
inline const std::vector<KeyName>& linuxKeyNames() {
    static const std::vector<KeyName> names = [] {
        std::vector<KeyName> v;
        for (KeyName k : keyNames()) {
            if (k.glyph == u'⌘') k.glyph = u'⌃';
            v.push_back(k);
        }
        v.push_back({u"super", u'⌘', true});
        v.push_back({u"win", u'⌘', true});
        v.push_back({u"meta", u'⌘', true});
        return v;
    }();
    return names;
}

inline const std::vector<KeyName>& keyNames(Spelling sp) {
    return sp == Spelling::Linux ? linuxKeyNames() : keyNames();
}

// What stands between the parts of a key in Linux's spelling: a thin space
// (Commands::kLinuxKeyJoin).
constexpr char16_t kLinuxJoin = u' ';

// A key's glyph as the hints spell it (folded): the glyph on the Mac; on
// Linux Ctrl, Alt and Super (⌘) by name, and Home, End, Page Up and Page
// Down, as Commands::keyLabel writes them; ⇧ and the rest as on the Mac.
inline std::u16string spelled(char16_t glyph, Spelling sp) {
    if (sp == Spelling::Linux) {
        switch (glyph) {
            case u'⌃': return u"ctrl";
            case u'⌥': return u"alt";
            case u'⌘': return u"super";
            case u'↖': return u"home";
            case u'↘': return u"end";
            case u'⇞': return u"page up";
            case u'⇟': return u"page down";
            default: break;
        }
    }
    return std::u16string(1, glyph);
}

// The glyphs a word names (keyNames(sp)): exactly, or as the first three
// letters or more of a name.
inline std::vector<char16_t> glyphsNamed(const std::u16string& w, Spelling sp = Spelling::Mac) {
    std::vector<char16_t> out;
    if (w.empty()) return out;
    for (const KeyName& k : keyNames(sp)) {
        const std::u16string name(k.name);
        const bool named = name == w || (w.size() >= 3 && name.compare(0, w.size(), w) == 0);
        if (named && std::find(out.begin(), out.end(), k.glyph) == out.end())
            out.push_back(k.glyph);
    }
    return out;
}

// "cmd+shift+h" as the hints spell it (compared folded): "⇧⌘h" on the Mac;
// "ctrl ⇧ h" on Linux, a thin space between the parts, Cmd being Ctrl
// there. Empty when a part is no key ("+" alone, "a++b").
inline std::u16string combination(const std::u16string& w, Spelling sp = Spelling::Mac) {
    std::vector<std::u16string> parts;
    std::u16string part;
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == u'+' && !part.empty()) {
            parts.push_back(part);
            part.clear();
        } else {
            part += w[i];   // a "+" first in a part is the key "+"
        }
    }
    if (!part.empty()) parts.push_back(part);
    if (parts.size() < 2) return u"";
    std::vector<char16_t> mods;
    std::u16string rest;
    for (const std::u16string& p : parts) {
        char16_t modifier = 0, other = 0;
        for (const KeyName& k : keyNames(sp))
            if (p == k.name && !modifier && !other) (k.modifier ? modifier : other) = k.glyph;
        if (modifier) {
            if (std::find(mods.begin(), mods.end(), modifier) == mods.end()) mods.push_back(modifier);
        } else if (!rest.empty()) {
            return u"";   // two keys that are not modifiers
        } else {
            rest = other ? spelled(other, sp) : p;
        }
    }
    std::sort(mods.begin(), mods.end(),
              [](char16_t a, char16_t b) { return modifierRank(a) < modifierRank(b); });
    if (sp == Spelling::Mac) return std::u16string(mods.begin(), mods.end()) + rest;
    std::u16string out;
    for (char16_t m : mods) out += spelled(m, sp) + kLinuxJoin;
    return out + rest;
}

inline bool isSpace(char16_t c) {
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == 0xA0 || c == 0x3000;
}

inline std::vector<Word> parse(const std::u16string& query, Spelling sp = Spelling::Mac) {
    std::vector<Word> words;
    std::u16string q = folded(query);
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && isSpace(q[i])) ++i;
        size_t j = i;
        while (j < q.size() && !isSpace(q[j])) ++j;
        if (j > i) {
            Word w;
            w.text = q.substr(i, j - i);
            for (char16_t g : glyphsNamed(w.text, sp)) w.keys.push_back(spelled(g, sp));
            const std::u16string combo = combination(w.text, sp);
            if (!combo.empty()) w.keys.push_back(combo);
            words.push_back(std::move(w));
        }
        i = j;
    }
    return words;
}

// Every place `needle` is in `hay` (both folded), not overlapping.
inline void findAll(const std::u16string& hay, const std::u16string& needle, size_t field,
                    std::vector<Span>& out, bool& found) {
    if (needle.empty() || needle.size() > hay.size()) return;
    for (size_t at = hay.find(needle); at != std::u16string::npos;
         at = hay.find(needle, at + needle.size())) {
        out.push_back({field, at, needle.size()});
        found = true;
    }
}

inline Match match(const std::vector<Word>& words, const std::vector<Field>& fields) {
    Match m;
    if (words.empty()) return m;
    std::vector<std::u16string> hay;
    hay.reserve(fields.size());
    for (const Field& f : fields) hay.push_back(folded(f.text));
    for (const Word& w : words) {
        bool found = false;
        for (size_t f = 0; f < fields.size(); ++f) {
            findAll(hay[f], w.text, f, m.spans, found);
            if (fields[f].key)
                for (const std::u16string& k : w.keys) findAll(hay[f], folded(k), f, m.spans, found);
        }
        if (!found) {
            m.matches = false;
            m.spans.clear();
            return m;
        }
    }
    std::sort(m.spans.begin(), m.spans.end(), [](const Span& a, const Span& b) {
        return a.field != b.field ? a.field < b.field : a.start < b.start;
    });
    std::vector<Span> merged;
    for (const Span& s : m.spans) {
        if (!merged.empty() && merged.back().field == s.field &&
            s.start <= merged.back().start + merged.back().length) {
            Span& b = merged.back();
            b.length = std::max(b.start + b.length, s.start + s.length) - b.start;
        } else {
            merged.push_back(s);
        }
    }
    m.spans = std::move(merged);
    return m;
}

inline Match match(const std::u16string& query, const std::vector<Field>& fields,
                   Spelling sp = Spelling::Mac) {
    return match(parse(query, sp), fields);
}

// ------------------------------------------------------- matches first
//
// While the box holds a query, the panel shows what it found first, so
// nobody scrolls three pages to find it, and the rest after it, dimmed
// (the owner's, 2026-11-19; GTK's HintsColumns first, the Mac to follow).
// bubbleMatches reorders the panel's list, the one PageMeasure::layoutHints
// lays out, and the app lays the reordered list out the same way.
//
// The list is given as Items, one for each of layoutHints' items, in the
// same order. A section begins at a Heading, or at a Gap (the blank line
// before "Hide these hints": what follows it is under no heading); the
// items before the first Heading or Gap are a section with neither. A unit
// is a Shortcut, or a Line (a line of text, GTK's note on the terminal's
// keys), or several of them joined by `withNext` (a pair's two shortcuts on
// the Mac; a note's lines on GTK); a unit matches when any item in it
// does, and moves whole.
//
// The order returned is:
//   1. every section that has a unit that matches, in the list's order:
//      its Heading or Gap (none for a first section without one), then its
//      units that match, in their order;
//   2. from `rest` on, the list as it was without the units that moved: a
//      section's Heading or Gap stays unless every unit under it moved
//      (then nothing is left to head), a Heading with nothing under it in
//      the first place stays. All of these are drawn dimmed.
// So a Heading or a Gap can be in the order twice, once in each part;
// every other item is there exactly once. With nothing found the order is
// the list's own and `rest` is 0, everything dimmed; when every unit
// matches, `rest` is the order's size. Call it only for a query that is
// not empty: with none, the list is shown as it is and nothing is dimmed.
//
// The Mac, adopting it (EditorController+StatusBar.mm's entries): a
// Heading, a Gap and a Shortcut map to the same Kind, `matches` is the
// entry's found.matches, and a pair's first shortcut sets `withNext`; lay
// out `measured` in the order returned, and dim from `rest` on. The
// list's first item is a Heading on both, so its blank line above it
// parts the two halves. A pair's half that the query did not find is
// still drawn as not found, as before, though it moves with its pair.
struct Item {
    enum Kind { Heading, Shortcut, Line, Gap };
    Kind kind = Shortcut;
    bool matches = false;    // a Shortcut or a Line the query found
    bool withNext = false;   // a Shortcut or a Line that moves with the next one
};

struct Bubbled {
    std::vector<size_t> order;   // indices into the items, as they are to be laid out
    size_t rest = 0;             // order[rest] on are the rest, dimmed
    bool operator==(const Bubbled& o) const { return order == o.order && rest == o.rest; }
    bool operator!=(const Bubbled& o) const { return !(*this == o); }
};

inline Bubbled bubbleMatches(const std::vector<Item>& items) {
    struct Unit {
        size_t first, end;
        bool matches;
    };
    struct Section {
        size_t head;   // the Heading or Gap, or npos
        std::vector<Unit> units;
    };
    const size_t none = (size_t)-1;
    auto joins = [](const Item& it) { return it.kind == Item::Shortcut || it.kind == Item::Line; };
    std::vector<Section> sections(1, Section{none, {}});
    for (size_t i = 0; i < items.size();) {
        if (!joins(items[i])) {
            sections.push_back(Section{i, {}});
            ++i;
            continue;
        }
        size_t end = i + 1;
        bool matches = items[i].matches;
        while (end < items.size() && items[end - 1].withNext && joins(items[end])) {
            matches = matches || items[end].matches;
            ++end;
        }
        sections.back().units.push_back(Unit{i, end, matches});
        i = end;
    }
    Bubbled b;
    b.order.reserve(items.size() + 8);
    for (const Section& s : sections) {
        bool any = false;
        for (const Unit& u : s.units) any = any || u.matches;
        if (!any) continue;
        if (s.head != none) b.order.push_back(s.head);
        for (const Unit& u : s.units)
            if (u.matches)
                for (size_t i = u.first; i < u.end; ++i) b.order.push_back(i);
    }
    b.rest = b.order.size();
    for (const Section& s : sections) {
        bool left = s.units.empty();
        for (const Unit& u : s.units) left = left || !u.matches;
        if (!left) continue;
        if (s.head != none) b.order.push_back(s.head);
        for (const Unit& u : s.units)
            if (!u.matches)
                for (size_t i = u.first; i < u.end; ++i) b.order.push_back(i);
    }
    return b;
}

}  // namespace HintsSearch
