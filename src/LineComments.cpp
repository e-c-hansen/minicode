// LineComments.cpp — see LineComments.h.
#include "LineComments.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace LineComments {

std::string markerFor(const std::string& filename) {
    std::string name;
    for (char c : filename) name += (char)std::tolower((unsigned char)c);
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);

    static const std::unordered_map<std::string, std::string> byName = {
        {"makefile", "#"}, {"gnumakefile", "#"}, {"dockerfile", "#"},
        {"cmakelists.txt", "#"}, {"gemfile", "#"}, {"rakefile", "#"},
        {"podfile", "#"}, {"brewfile", "#"}, {"procfile", "#"},
    };
    auto n = byName.find(name);
    if (n != byName.end()) return n->second;

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = name.substr(dot + 1);

    static const std::unordered_map<std::string, std::string> byExt = {
        // #
        {"py", "#"}, {"sh", "#"}, {"bash", "#"}, {"zsh", "#"}, {"fish", "#"},
        {"yml", "#"}, {"yaml", "#"}, {"toml", "#"}, {"conf", "#"}, {"cfg", "#"},
        {"rb", "#"}, {"pl", "#"}, {"pm", "#"}, {"r", "#"}, {"mk", "#"},
        {"cmake", "#"}, {"nix", "#"}, {"tf", "#"}, {"properties", "#"},
        {"env", "#"}, {"gitignore", "#"}, {"dockerignore", "#"},
        {"zshrc", "#"}, {"zshenv", "#"}, {"zprofile", "#"}, {"bashrc", "#"},
        {"bash_profile", "#"}, {"profile", "#"}, {"ex", "#"}, {"exs", "#"},
        {"jl", "#"}, {"ps1", "#"}, {"tcl", "#"}, {"awk", "#"},
        // //
        {"c", "//"}, {"h", "//"}, {"cc", "//"}, {"cpp", "//"}, {"cxx", "//"},
        {"hpp", "//"}, {"hxx", "//"}, {"m", "//"}, {"mm", "//"}, {"java", "//"},
        {"go", "//"}, {"rs", "//"}, {"js", "//"}, {"mjs", "//"}, {"cjs", "//"},
        {"ts", "//"}, {"jsx", "//"}, {"tsx", "//"}, {"swift", "//"},
        {"kt", "//"}, {"kts", "//"}, {"scala", "//"}, {"cs", "//"},
        {"dart", "//"}, {"php", "//"}, {"proto", "//"}, {"zig", "//"},
        {"glsl", "//"}, {"metal", "//"}, {"jsonc", "//"}, {"gradle", "//"},
        {"groovy", "//"}, {"v", "//"}, {"sol", "//"}, {"scss", "//"},
        {"less", "//"},
        // --
        {"sql", "--"}, {"lua", "--"}, {"hs", "--"}, {"elm", "--"}, {"ada", "--"},
        // ;
        {"ini", ";"}, {"asm", ";"}, {"s", ";"}, {"lisp", ";"}, {"clj", ";"},
        {"el", ";"}, {"scm", ";"},
        // %
        {"tex", "%"}, {"erl", "%"},
        // "
        {"vim", "\""},
    };
    auto e = byExt.find(ext);
    return e == byExt.end() ? "" : e->second;
}

namespace {

bool isSpace(char16_t c) { return c == u' ' || c == u'\t'; }

struct Edit {
    size_t pos;          // in the original text
    long delta;          // +inserted, -removed
};

// Where offset `x` lands after the edits. An insertion at exactly `x` pushes
// it along unless `stayBefore` (the start of a non-empty selection that
// begins where the marker goes, so the marker ends up selected).
size_t mapOffset(size_t x, const std::vector<Edit>& edits, bool stayBefore) {
    long shift = 0;
    for (const Edit& e : edits) {
        if (e.delta > 0) {
            if (x > e.pos || (x == e.pos && !stayBefore)) shift += e.delta;
        } else {
            size_t removed = (size_t)(-e.delta);
            if (x >= e.pos + removed) shift += e.delta;
            else if (x > e.pos) shift -= (long)(x - e.pos);
        }
    }
    return (size_t)((long)x + shift);
}

}  // namespace

Result toggle(const std::u16string& text, size_t selStart, size_t selEnd,
              const std::string& marker) {
    Result r{text, selStart, selEnd, false, 0, 0, u""};
    if (marker.empty()) return r;
    if (selStart > selEnd) std::swap(selStart, selEnd);
    selStart = std::min(selStart, text.size());
    selEnd = std::min(selEnd, text.size());
    std::u16string mark(marker.begin(), marker.end());

    // Lines touched: from the start of selStart's line to the end of
    // selEnd's line, not counting a line the selection only reaches the
    // start of.
    size_t first = selStart;
    while (first > 0 && text[first - 1] != u'\n') first--;
    size_t last = selEnd;
    if (selEnd > selStart && selEnd > 0 && text[selEnd - 1] == u'\n') last = selEnd - 1;

    struct Line { size_t start, end, indent; bool blank; };
    std::vector<Line> lines;
    size_t pos = first;
    while (true) {
        size_t end = text.find(u'\n', pos);
        if (end == std::u16string::npos) end = text.size();
        size_t i = pos;
        while (i < end && isSpace(text[i])) i++;
        lines.push_back({pos, end, i - pos, i == end});
        if (end >= last || end == text.size()) break;
        pos = end + 1;
    }

    bool allBlank = std::all_of(lines.begin(), lines.end(),
                                [](const Line& l) { return l.blank; });
    auto commented = [&](const Line& l) {
        return text.compare(l.start + l.indent, mark.size(), mark) == 0;
    };
    bool uncomment = !allBlank;
    for (const Line& l : lines)
        if (!l.blank && !commented(l)) { uncomment = false; break; }

    std::vector<Edit> edits;
    if (uncomment) {
        for (const Line& l : lines) {
            if (l.blank) continue;
            size_t at = l.start + l.indent;
            size_t n = mark.size();
            if (at + n < l.end && text[at + n] == u' ') n++;
            edits.push_back({at, -(long)n});
        }
    } else {
        size_t indent = SIZE_MAX;
        for (const Line& l : lines)
            if (allBlank || !l.blank) indent = std::min(indent, l.indent);
        std::u16string ins = mark + u" ";
        for (const Line& l : lines) {
            if (l.blank && !allBlank) continue;
            edits.push_back({l.start + std::min(indent, l.indent),
                             (long)ins.size()});
        }
    }
    if (edits.empty()) return r;

    // Apply back to front so earlier positions stay valid.
    std::u16string out = text;
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        if (it->delta > 0) out.insert(it->pos, mark + u" ");
        else out.erase(it->pos, (size_t)(-it->delta));
    }
    r.text = out;
    r.changed = true;
    r.replaceStart = lines.front().start;
    r.replaceLength = lines.back().end - lines.front().start;
    long total = 0;
    for (const Edit& e : edits) total += e.delta;
    r.replacement = out.substr(r.replaceStart, (size_t)((long)r.replaceLength + total));
    bool range = selEnd > selStart;
    r.selStart = mapOffset(selStart, edits, range);
    r.selEnd = range ? mapOffset(selEnd, edits, false) : r.selStart;
    return r;
}

}  // namespace LineComments
