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
// The keys are spelled as Commands::keyLabel spells them for the platform
// (Spelling): the Mac's in symbols, "⇧⌘H"; Linux's with its modifiers in
// words, "Ctrl+Shift+H", "Ctrl+⏎", "Shift+Page Up". With Linux's spelling
// "control" finds Ctrl, "option" Alt, and "shift+ctrl+h" Ctrl+Shift+H.
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

// A key's glyph as the hints spell it (folded): the glyph on the Mac; on
// Linux a modifier's word ("ctrl"), and Home, End, Page Up and Page Down by
// name, as Commands::keyLabel writes them.
inline std::u16string spelled(char16_t glyph, Spelling sp) {
    if (sp == Spelling::Linux) {
        switch (glyph) {
            case u'⌃': return u"ctrl";
            case u'⌥': return u"alt";
            case u'⇧': return u"shift";
            case u'⌘': return u"cmd";
            case u'↖': return u"home";
            case u'↘': return u"end";
            case u'⇞': return u"page up";
            case u'⇟': return u"page down";
            default: break;
        }
    }
    return std::u16string(1, glyph);
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

// "cmd+shift+h" as the hints spell it, "⇧⌘h" on the Mac and "shift+cmd+h"
// on Linux (compared folded); empty when a part is no key ("+" alone,
// "a++b").
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
        for (const KeyName& k : keyNames())
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
    for (char16_t m : mods) out += spelled(m, sp) + u"+";
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
            for (char16_t g : glyphsNamed(w.text)) w.keys.push_back(spelled(g, sp));
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

}  // namespace HintsSearch
