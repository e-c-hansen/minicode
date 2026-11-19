// HintsSearch.h — searching the shortcut hints (Shift+Cmd+H): what a
// query in the panel's box finds in a shortcut's key and label, and where
// to highlight it. Header only, no dependencies, UTF-16 (the Mac's
// strings; a port with UTF-8 converts). The Mac draws, the core decides.
// Tested in tests/run_tests.cpp (hints:search).
//
// A query is words split at spaces; a shortcut matches when every word is
// found in it. A word is matched without regard to case in the key and the
// label alike, and in the key also by name: "cmd" or "command" finds ⌘,
// "shift" ⇧, "opt", "option" or "alt" ⌥, "ctrl" or "control" ⌃, "return"
// or "enter" ⏎, and so on, a name's first three letters or more counting
// ("sh" is only text, "shi" is ⇧ too). A word joined with "+" is one key
// as the hints spell it, its modifiers in Apple's order whatever order they
// were typed in: "cmd+shift+h" finds ⇧⌘H.
//
// The Linux port writes keys in words (Commands::linuxLabel: "Ctrl Shift
// H", its modifiers Ctrl, Alt, Shift, Cmd in that order) and searches with
// Spelling::Words: a name finds its word ("control" Ctrl, "opt" Alt,
// "return" Enter, "pageup" Page Up), and a combination is found only as a
// whole key or the end of one ("shift+ctrl+h" finds Ctrl Shift H, so does
// "shift+h", as "cmd+h" finds ⇧⌘H; "ctrl+s" never finds Ctrl Shift P).
// Tested in hints:search-words.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace HintsSearch {

// One word of the query: its text in lower case, and what it is as a key
// (glyph spellings, looked for in keys only).
struct Word {
    std::u16string text;
    std::vector<std::u16string> keys;
    // Spellings looked for in keys only as whole words (Spelling::Words'
    // combinations).
    std::vector<std::u16string> whole;
};

// How the hints write keys: the Mac's glyphs, or the Linux port's words.
enum class Spelling { Glyphs, Words };

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

// The glyphs a word names: exactly, or as the first three letters or more
// of a name.
inline std::vector<char16_t> glyphsNamed(const std::u16string& w) {
    std::vector<char16_t> out;
    if (w.empty()) return out;
    for (const KeyName& k : keyNames()) {
        const std::u16string name(k.name);
        const bool named = name == w || (w.size() >= 3 && name.compare(0, w.size(), w) == 0);
        if (named && std::find(out.begin(), out.end(), k.glyph) == out.end())
            out.push_back(k.glyph);
    }
    return out;
}

// "cmd+shift+h" as the hints spell it, "⇧⌘h" (compared folded); empty when
// a part is no key ("+" alone, "a++b").
inline std::u16string combination(const std::u16string& w) {
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
        for (const KeyName& k : keyNames())
            if (p == k.name && !modifier && !other) (k.modifier ? modifier : other) = k.glyph;
        if (modifier) {
            if (std::find(mods.begin(), mods.end(), modifier) == mods.end()) mods.push_back(modifier);
        } else if (!rest.empty()) {
            return u"";   // two keys that are not modifiers
        } else {
            rest = other ? std::u16string(1, other) : p;
        }
    }
    std::sort(mods.begin(), mods.end(),
              [](char16_t a, char16_t b) { return modifierRank(a) < modifierRank(b); });
    return std::u16string(mods.begin(), mods.end()) + rest;
}

inline bool isSpace(char16_t c) {
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == 0xA0 || c == 0x3000;
}

// The keys' names as the Linux port writes them (Commands::linuxLabel),
// in lower case; a modifier with its place in the port's order.
struct WordName {
    const char16_t* name;
    const char16_t* word;
    int modifier;   // its rank, -1 for a key that is no modifier
};

inline const std::vector<WordName>& wordNames() {
    static const std::vector<WordName> names = {
        {u"ctrl", u"ctrl", 0},            {u"control", u"ctrl", 0},
        {u"alt", u"alt", 1},              {u"opt", u"alt", 1},
        {u"option", u"alt", 1},           {u"shift", u"shift", 2},
        {u"cmd", u"cmd", 3},              {u"command", u"cmd", 3},
        {u"return", u"enter", -1},        {u"enter", u"enter", -1},
        {u"esc", u"escape", -1},          {u"escape", u"escape", -1},
        {u"tab", u"tab", -1},             {u"space", u"space", -1},
        {u"backspace", u"backspace", -1}, {u"delete", u"delete", -1},
        {u"up", u"up", -1},               {u"down", u"down", -1},
        {u"left", u"left", -1},           {u"right", u"right", -1},
        {u"home", u"home", -1},           {u"end", u"end", -1},
        {u"pageup", u"page up", -1},      {u"pagedown", u"page down", -1},
        {u"menu", u"menu", -1},
    };
    return names;
}

// The words a word names, as glyphsNamed: exactly, or as the first three
// letters or more of a name.
inline std::vector<std::u16string> wordsNamed(const std::u16string& w) {
    std::vector<std::u16string> out;
    if (w.empty()) return out;
    for (const WordName& k : wordNames()) {
        const std::u16string name(k.name);
        const bool named = name == w || (w.size() >= 3 && name.compare(0, w.size(), w) == 0);
        if (named && std::find(out.begin(), out.end(), k.word) == out.end()) out.push_back(k.word);
    }
    return out;
}

// "shift+ctrl+h" as the Linux port writes it, "ctrl shift h" (compared
// folded); empty when a part is no key, as combination.
inline std::u16string wordCombination(const std::u16string& w) {
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
    std::vector<const WordName*> mods;
    std::u16string rest;
    for (const std::u16string& p : parts) {
        const WordName* named = nullptr;
        for (const WordName& k : wordNames())
            if (p == k.name) {
                named = &k;
                break;
            }
        if (named && named->modifier >= 0) {
            bool have = false;
            for (const WordName* m : mods) have = have || m->modifier == named->modifier;
            if (!have) mods.push_back(named);
        } else if (!rest.empty()) {
            return u"";   // two keys that are not modifiers
        } else {
            rest = named ? std::u16string(named->word) : p;
        }
    }
    std::sort(mods.begin(), mods.end(),
              [](const WordName* a, const WordName* b) { return a->modifier < b->modifier; });
    std::u16string out;
    for (const WordName* m : mods) {
        if (!out.empty()) out += u' ';
        out += m->word;
    }
    if (!rest.empty()) {
        if (!out.empty()) out += u' ';
        out += rest;
    }
    return out;
}

inline std::vector<Word> parse(const std::u16string& query, Spelling spelling = Spelling::Glyphs) {
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
            if (spelling == Spelling::Words) {
                w.keys = wordsNamed(w.text);
                const std::u16string combo = wordCombination(w.text);
                if (!combo.empty()) w.whole.push_back(combo);
            } else {
                for (char16_t g : glyphsNamed(w.text)) w.keys.push_back(std::u16string(1, g));
                const std::u16string combo = combination(w.text);
                if (!combo.empty()) w.keys.push_back(combo);
            }
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

inline bool isWordChar(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') ||
           c >= 0xC0;
}

// findAll, counting only a place with no letter or digit just before or
// just after it (where the needle itself starts or ends with one).
inline void findWhole(const std::u16string& hay, const std::u16string& needle, size_t field,
                      std::vector<Span>& out, bool& found) {
    if (needle.empty() || needle.size() > hay.size()) return;
    for (size_t at = hay.find(needle); at != std::u16string::npos; at = hay.find(needle, at + 1)) {
        const size_t end = at + needle.size();
        if ((at > 0 && isWordChar(hay[at - 1]) && isWordChar(needle.front())) ||
            (end < hay.size() && isWordChar(hay[end]) && isWordChar(needle.back())))
            continue;
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
            if (fields[f].key) {
                for (const std::u16string& k : w.keys) findAll(hay[f], folded(k), f, m.spans, found);
                for (const std::u16string& k : w.whole) findWhole(hay[f], folded(k), f, m.spans, found);
            }
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
                   Spelling spelling = Spelling::Glyphs) {
    return match(parse(query, spelling), fields);
}

}  // namespace HintsSearch
