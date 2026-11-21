// CommandPaletteSymbols.cpp — Go to Symbol's rows (CommandPalette.h's
// symbolRows), apart from the rest of the palette's decisions because it
// alone needs the language servers' names (LspClient.h): the session
// host's command line links CommandPalette.cpp for Quick Open's matching
// (SessionSearch.h) without them.
#include "CommandPalette.h"

#include <algorithm>
#include <string>

namespace CommandPalette {

namespace {

std::u16string trimmed(const std::u16string& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == ' ') a++;
    while (b > a && s[b - 1] == ' ') b--;
    return s.substr(a, b - a);
}

}  // namespace

std::vector<SymbolRow> symbolRows(const std::vector<Lsp::Symbol>& symbols,
                                  const std::u16string& query, bool byKind, int caretSymbol,
                                  int* preselect, size_t limit) {
    std::vector<Entry> entries;
    entries.reserve(symbols.size());
    for (const Lsp::Symbol& sym : symbols) {
        Entry e;
        e.label = e.title = Lsp::toUtf16(sym.name);
        entries.push_back(std::move(e));
    }
    std::vector<Match> ranked = rank(entries, query, {});
    if (ranked.size() > limit) ranked.resize(limit);
    std::vector<SymbolRow> rows;
    auto symbolRow = [](Match m) {
        SymbolRow r;
        r.symbol = m.index;
        r.match = std::move(m);
        return r;
    };
    if (!byKind) {
        for (Match& m : ranked) rows.push_back(symbolRow(std::move(m)));
    } else {
        std::vector<int> kinds;   // in the order their first symbol comes
        for (const Match& m : ranked) {
            int k = symbols[m.index].kind;
            if (std::find(kinds.begin(), kinds.end(), k) == kinds.end()) kinds.push_back(k);
        }
        for (int k : kinds) {
            size_t n = 0;
            for (const Match& m : ranked) n += symbols[m.index].kind == k;
            SymbolRow h;
            h.heading = true;
            h.text = Lsp::toUtf16(Lsp::symbolKindPlural(k) + " (" + std::to_string(n) + ")");
            rows.push_back(std::move(h));
            for (const Match& m : ranked)
                if (symbols[m.index].kind == k) rows.push_back(symbolRow(m));
        }
    }
    if (preselect) {
        *preselect = -1;
        const std::u16string q = trimmed(query);
        for (size_t i = 0; i < rows.size(); i++) {
            if (rows[i].heading) continue;
            if (*preselect < 0) *preselect = int(i);
            if (q.empty() && caretSymbol >= 0 && rows[i].symbol == size_t(caretSymbol)) {
                *preselect = int(i);
                break;
            }
            if (!q.empty() || caretSymbol < 0) break;
        }
    }
    return rows;
}

}  // namespace CommandPalette
