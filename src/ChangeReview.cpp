// ChangeReview.cpp — see ChangeReview.h.
#include "ChangeReview.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

#include "QuickDiff.h"
#include "SyntaxHighlighter.h"

namespace ChangeReview {

namespace {

// Where each line starts, then one past the end: line k is
// [starts[k], starts[k + 1]), its "\n" included.
std::vector<size_t> lineStarts(const std::u16string& text) {
    std::vector<size_t> starts{0};
    for (size_t i = 0; i < text.size(); i++)
        if (text[i] == u'\n') starts.push_back(i + 1);
    if (starts.back() != text.size()) starts.push_back(text.size());
    return starts;
}

size_t lineCountOf(const std::vector<size_t>& starts) { return starts.empty() ? 0 : starts.size() - 1; }

// The end of a line's text, before its "\n" or "\r\n".
size_t contentEnd(const std::u16string& text, size_t from, size_t to) {
    if (to > from && text[to - 1] == u'\n') to--;
    if (to > from && text[to - 1] == u'\r') to--;
    return to;
}

// A copy with every "\r" before a "\n" taken out: the lines stay the same
// lines, so aligning these aligns the texts, and a file written with CRLF
// against an index holding LF (core.autocrlf) shows no change on every line.
std::u16string withoutCr(const std::u16string& s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
        if (!(s[i] == u'\r' && i + 1 < s.size() && s[i + 1] == u'\n')) out += s[i];
    return out;
}

// Past this many UTF-16 units a side, no syntax colors (as the compare view).
const size_t kMaxColoredText = 2 * 1024 * 1024;

std::vector<SideBySide::Span> syntaxSpans(const std::u16string& text, const std::string& ext) {
    std::vector<SideBySide::Span> out;
    if (ext.empty() || text.empty() || text.size() > kMaxColoredText ||
        !SyntaxHighlighter::supports(ext))
        return out;
    IncrementalHighlighter<char16_t> hl(ext);
    std::vector<Token> tokens;
    hl.reset(StringSource<char16_t>(text), &tokens);
    out.reserve(tokens.size());
    for (const Token& t : tokens)
        if (t.style != TokenStyle::Plain && t.length)
            out.push_back({t.start, t.length, (int)t.style});
    return out;
}

std::string fileName(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::u16string toU16(const std::string& s) { return QuickDiff::fromUtf8(s); }

// The line break a text uses throughout, or "" when it mixes them (or has
// none): lines from the other side then keep their own.
std::u16string onlyBreak(const std::u16string& text) {
    size_t crlf = 0, lf = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != u'\n') continue;
        if (i > 0 && text[i - 1] == u'\r') crlf++;
        else lf++;
    }
    if (crlf && !lf) return u"\r\n";
    if (lf && !crlf) return u"\n";
    return u"";
}

}  // namespace

std::string File::key() const {
    return std::string(section == Section::Staged ? "S:" : section == Section::Changes ? "W:"
                                                                                      : "P:") +
           path;
}

std::string wholeOnlyReason(const File& f) {
    if (f.conflict) return "has a conflict";
    if (f.submodule) return "is a submodule";
    if (f.symlink) return "is a symbolic link";
    if (f.binary) return "is a binary file";
    if (f.tooLarge) return "is too large to show here";
    if (f.untracked) return "is not tracked yet";
    if (f.section == Section::Changes && f.letter == 'A') return "was added with git add -N";
    if (f.letter == 'T') return "changed its kind (a file, a link)";
    if (!f.oldExists) return "is new";
    if (!f.newExists) return "was deleted";
    return "";
}

FileDiff diffFile(File f, const std::string& ext) {
    FileDiff d;
    d.file = std::move(f);
    const File& file = d.file;
    if (file.binary || file.tooLarge || file.submodule || file.conflict || file.symlink) return d;
    const std::u16string& a = file.oldText;
    const std::u16string& b = file.newText;
    d.rows = SideBySide::align(withoutCr(a), withoutCr(b));
    d.blocks = SideBySide::hunks(d.rows);
    d.oldStarts = lineStarts(a);
    d.newStarts = lineStarts(b);
    for (const SideBySide::Row& r : d.rows) {
        if (r.kind == SideBySide::Kind::Same) continue;
        if (r.left >= 0) d.removed++;
        if (r.right >= 0) d.added++;
    }
    if (!a.empty() && !b.empty()) d.lastNewlineChanged = (a.back() == u'\n') != (b.back() == u'\n');
    d.oldSyntax = syntaxSpans(a, ext);
    d.newSyntax = syntaxSpans(b, ext);
    return d;
}

// A conflicted file read as text: its blocks can be shown and resolved.
static bool conflictReadable(const File& f) {
    return f.conflict && f.newExists && !f.binary && !f.tooLarge && !f.submodule && !f.symlink;
}

FileDiff conflictFile(File f, const std::string& ext) {
    FileDiff d;
    f.conflict = true;
    f.oldText = f.newText;   // one text: a row's two line numbers are the same line
    d.file = std::move(f);
    const File& file = d.file;
    if (!conflictReadable(file)) return d;
    const std::u16string& t = file.newText;
    d.newStarts = lineStarts(t);
    d.oldStarts = d.newStarts;
    d.conflicts = MergeConflicts::parse(t);
    const size_t lines = lineCountOf(d.newStarts);
    auto lineOf = [&](size_t offset) {
        return (size_t)(std::upper_bound(d.newStarts.begin(), d.newStarts.end(), offset) -
                        d.newStarts.begin()) - 1;
    };
    d.rows.reserve(lines);
    d.parts.assign(lines, ConflictPart::None);
    for (size_t i = 0; i < lines; i++) d.rows.push_back({SideBySide::Kind::Same, (int)i, (int)i});
    for (const MergeConflicts::Block& b : d.conflicts) {
        const size_t first = lineOf(b.start), last = lineOf(b.endMarker);
        for (size_t i = first; i <= last && i < lines; i++) {
            const size_t at = d.newStarts[i];
            ConflictPart part;
            if (i == first) part = ConflictPart::CurrentMarker;
            else if (at == b.endMarker) part = ConflictPart::IncomingMarker;
            else if (at == b.separator) part = ConflictPart::Separator;
            else if (b.hasBase() && at == b.baseMarker) part = ConflictPart::BaseMarker;
            else if (at < (b.hasBase() ? b.baseMarker : b.separator)) part = ConflictPart::Current;
            else if (at < b.separator) part = ConflictPart::Base;
            else part = ConflictPart::Incoming;
            d.parts[i] = part;
            d.rows[i].kind = SideBySide::Kind::Changed;
            if (part == ConflictPart::Current) d.removed++;
            if (part == ConflictPart::Incoming) d.added++;
        }
        d.blocks.push_back({first, std::min(last + 1, lines)});
    }
    d.newSyntax = syntaxSpans(t, ext);
    d.oldSyntax = d.newSyntax;
    return d;
}

std::u16string conflictResult(const FileDiff& d, size_t block, MergeConflicts::Keep keep) {
    if (block >= d.conflicts.size()) return d.file.newText;
    return MergeConflicts::resolved(d.file.newText, d.conflicts[block], keep);
}

bool canMarkResolved(const FileDiff& d) {
    return conflictReadable(d.file) && d.conflicts.empty();
}

std::vector<std::vector<SideBySide::Span>> syntaxByLine(const std::u16string& text,
                                                        const std::string& ext) {
    std::vector<std::vector<SideBySide::Span>> out;
    const std::vector<SideBySide::Span> spans = syntaxSpans(text, ext);
    if (spans.empty()) return out;
    const std::vector<size_t> starts = lineStarts(text);
    const size_t lines = lineCountOf(starts);
    out.resize(lines);
    size_t si = 0;
    for (size_t line = 0; line < lines; line++) {
        const size_t from = starts[line], to = contentEnd(text, from, starts[line + 1]);
        while (si < spans.size() && spans[si].start + spans[si].length <= from) si++;
        for (size_t k = si; k < spans.size() && spans[k].start < to; k++) {
            const size_t a = std::max(spans[k].start, from),
                         b = std::min(spans[k].start + spans[k].length, to);
            if (b > a) out[line].push_back({a - from, b - a, spans[k].style});
        }
    }
    return out;
}

size_t fileCount(const std::vector<FileDiff>& diffs) {
    std::set<std::string> paths;
    for (const FileDiff& d : diffs) paths.insert(d.file.path);
    return paths.size();
}

// Each diff `about` accepts, mixed in order.
template <class About>
static size_t fingerprintWhere(const std::vector<FileDiff>& diffs, About about) {
    size_t n = 0;
    for (const FileDiff& d : diffs) n += about(d) ? 1 : 0;
    size_t h = n;
    auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    for (const FileDiff& d : diffs) {
        if (!about(d)) continue;
        const File& f = d.file;
        mix(std::hash<std::string>()(f.key()));
        mix(std::hash<std::string>()(f.origPath));
        mix((size_t)(unsigned char)f.letter);
        mix((size_t)f.untracked | (size_t)f.conflict << 1 | (size_t)f.oldExists << 2 |
            (size_t)f.newExists << 3);
        mix(std::hash<std::u16string>()(f.oldText));
        mix(std::hash<std::u16string>()(f.newText));
    }
    return h;
}

size_t fingerprint(const std::vector<FileDiff>& diffs) {
    return fingerprintWhere(diffs, [](const FileDiff&) { return true; });
}

size_t fingerprintOf(const std::vector<FileDiff>& diffs, const std::vector<std::string>& paths) {
    if (paths.empty()) return fingerprint(diffs);
    return fingerprintWhere(diffs, [&paths](const FileDiff& d) {
        for (const std::string& p : paths)
            if (!p.empty() && (p == d.file.path || p == d.file.origPath)) return true;
        return false;
    });
}

void ActionNote::settle(const std::vector<FileDiff>& diffs) {
    settled = true;
    print = fingerprintOf(diffs, paths);
}

bool ActionNote::goesWith(const std::vector<FileDiff>& diffs, bool readHeadKnown,
                          const std::string& readHead) {
    if (headKnown && readHeadKnown && readHead != head) return true;
    const size_t now = fingerprintOf(diffs, paths);
    if (!settled) {
        settled = true;
        print = now;
        return false;
    }
    return now != print;
}

static std::u16string lineText(const std::u16string& text, const std::vector<size_t>& starts,
                               int line) {
    if (line < 0 || (size_t)line + 1 >= starts.size()) return u"";
    const size_t from = starts[(size_t)line];
    return text.substr(from, contentEnd(text, from, starts[(size_t)line + 1]) - from);
}

std::u16string oldLine(const FileDiff& d, int line) {
    return lineText(d.file.oldText, d.oldStarts, line);
}
std::u16string newLine(const FileDiff& d, int line) {
    return lineText(d.file.newText, d.newStarts, line);
}

// ---------------------------------------------------------------- acting
std::vector<Op> opsFor(Section s) {
    switch (s) {
    case Section::Changes: return {Op::Stage, Op::Discard};
    case Section::Staged: return {Op::Unstage};
    case Section::Plain: return {};
    }
    return {};
}

std::u16string pick(const FileDiff& d, const std::vector<bool>& takeOld,
                    const std::vector<bool>& takeNew, bool baseNew) {
    const std::u16string& a = d.file.oldText;
    const std::u16string& b = d.file.newText;
    const std::u16string eol = onlyBreak(baseNew ? b : a);
    std::u16string out;
    out.reserve(std::max(a.size(), b.size()));
    // A whole line with its break; one from the side that is not the base
    // takes the base's break when the base has only one kind.
    auto emit = [&](bool fromNew, int line) {
        const std::u16string& text = fromNew ? b : a;
        const std::vector<size_t>& starts = fromNew ? d.newStarts : d.oldStarts;
        if (line < 0 || (size_t)line + 1 >= starts.size()) return;
        const size_t from = starts[(size_t)line], to = starts[(size_t)line + 1];
        // The line before had no break (the old last line, with lines now
        // after it): it gets one.
        if (!out.empty() && out.back() != u'\n') out += eol.empty() ? u"\n" : eol;
        const size_t end = contentEnd(text, from, to);
        out.append(text, from, end - from);
        if (end == to) return;   // no break: the last line
        if (fromNew != baseNew && !eol.empty()) out += eol;
        else out.append(text, end, to - end);
    };
    const std::vector<SideBySide::Row>& rows = d.rows;
    for (size_t r = 0; r < rows.size();) {
        if (rows[r].kind == SideBySide::Kind::Same) {
            emit(baseNew, baseNew ? rows[r].right : rows[r].left);
            r++;
            continue;
        }
        size_t e = r;
        while (e < rows.size() && rows[e].kind != SideBySide::Kind::Same) e++;
        for (size_t k = r; k < e; k++) {
            const int l = rows[k].left;
            if (l >= 0 && !((size_t)l < takeOld.size() && takeOld[(size_t)l])) emit(false, l);
        }
        for (size_t k = r; k < e; k++) {
            const int l = rows[k].right;
            if (l >= 0 && (size_t)l < takeNew.size() && takeNew[(size_t)l]) emit(true, l);
        }
        r = e;
    }
    return out;
}

// The changed lines of block `block` (or of every block when `block` is
// -1) marked, the rest not; `invert` turns the marks over.
static void markBlock(const FileDiff& d, long block, bool invert, std::vector<bool>& takeOld,
                      std::vector<bool>& takeNew) {
    takeOld.assign(lineCountOf(d.oldStarts), invert);
    takeNew.assign(lineCountOf(d.newStarts), invert);
    for (size_t i = 0; i < d.blocks.size(); i++) {
        if (block >= 0 && (size_t)block != i) continue;
        for (size_t r = d.blocks[i].first; r < d.blocks[i].end; r++) {
            const SideBySide::Row& row = d.rows[r];
            if (row.left >= 0 && (size_t)row.left < takeOld.size()) takeOld[(size_t)row.left] = !invert;
            if (row.right >= 0 && (size_t)row.right < takeNew.size())
                takeNew[(size_t)row.right] = !invert;
        }
    }
}

std::u16string blockResult(const FileDiff& d, size_t block, Op op) {
    std::vector<bool> takeOld, takeNew;
    // Stage takes just this block into the old text (the index); Unstage
    // and Discard keep every other block of the new text and put this one
    // back as the old text has it.
    const bool invert = op != Op::Stage;
    markBlock(d, (long)block, invert, takeOld, takeNew);
    return pick(d, takeOld, takeNew, op != Op::Stage);
}

std::u16string linesResult(const FileDiff& d, const std::vector<int>& oldLines,
                           const std::vector<int>& newLines, Op op) {
    const bool invert = op != Op::Stage;
    std::vector<bool> takeOld, takeNew;
    // Every changed line as Stage or the others want it, then the chosen
    // ones turned over.
    markBlock(d, -1, false, takeOld, takeNew);
    std::vector<bool> changedOld = takeOld, changedNew = takeNew;
    for (size_t i = 0; i < takeOld.size(); i++) takeOld[i] = invert && changedOld[i];
    for (size_t i = 0; i < takeNew.size(); i++) takeNew[i] = invert && changedNew[i];
    // A line of a changed pair (an old line replaced by a new one) chosen
    // on either side takes the pair: the change of that line, as VS Code's
    // selected ranges take the original lines under the modified ones.
    std::vector<bool> chosenOld(takeOld.size(), false), chosenNew(takeNew.size(), false);
    for (int l : oldLines)
        if (l >= 0 && (size_t)l < chosenOld.size()) chosenOld[(size_t)l] = true;
    for (int l : newLines)
        if (l >= 0 && (size_t)l < chosenNew.size()) chosenNew[(size_t)l] = true;
    for (const SideBySide::Row& r : d.rows) {
        if (r.kind != SideBySide::Kind::Changed || r.left < 0 || r.right < 0) continue;
        if ((size_t)r.left >= chosenOld.size() || (size_t)r.right >= chosenNew.size()) continue;
        if (chosenOld[(size_t)r.left] || chosenNew[(size_t)r.right])
            chosenOld[(size_t)r.left] = chosenNew[(size_t)r.right] = true;
    }
    for (size_t l = 0; l < takeOld.size(); l++)
        if (chosenOld[l] && changedOld[l]) takeOld[l] = !invert;
    for (size_t l = 0; l < takeNew.size(); l++)
        if (chosenNew[l] && changedNew[l]) takeNew[l] = !invert;
    return pick(d, takeOld, takeNew, op != Op::Stage);
}

bool anyChanged(const FileDiff& d, const std::vector<int>& oldLines,
                const std::vector<int>& newLines) {
    std::vector<bool> o, n;
    markBlock(d, -1, false, o, n);
    for (int l : oldLines)
        if (l >= 0 && (size_t)l < o.size() && o[(size_t)l]) return true;
    for (int l : newLines)
        if (l >= 0 && (size_t)l < n.size() && n[(size_t)l]) return true;
    return false;
}

GitUi::ActionPlan plan(const FileDiff& d, Op op, bool lines) {
    GitUi::ActionPlan p;
    const File& f = d.file;
    const std::string name = fileName(f.path);
    const std::vector<Op> ops = opsFor(f.section);
    if (std::find(ops.begin(), ops.end(), op) == ops.end()) {
        p.refusal = f.section == Section::Staged ? "These changes are staged: unstage them first."
                    : f.section == Section::Changes ? "These changes are not staged."
                                                    : "Nothing can be changed here.";
        return p;
    }
    const std::string why = wholeOnlyReason(f);
    if (!why.empty()) {
        const char* whole = op == Op::Stage ? "Stage File" : op == Op::Unstage ? "Unstage File"
                                                                                : "Discard File";
        p.refusal = name + " " + why + ": use " + whole + " for all of it.";
        return p;
    }
    const std::string what = lines ? "the selected lines of " + name : "a change to " + name;
    switch (op) {
    case Op::Stage: p.doneText = "Staged " + what + "."; break;
    case Op::Unstage: p.doneText = "Unstaged " + what + "."; break;
    case Op::Discard:
        p.question = lines ? "Discard the selected lines of " + name + "?"
                           : "Discard this change to " + name + "?";
        p.detail = std::string(lines ? "They go" : "Its lines go") +
                   " back to the version git has (staged, or else in the last commit). The "
                   "file as it is now goes to the Trash first: Undo Discard brings it back.";
        p.button = lines ? "Discard Lines" : "Discard Change";
        p.doneText = "Discarded " + what + "; the file as it was is in the Trash.";
        break;
    }
    return p;
}

std::string staleText(const std::string& path, bool index) {
    const std::string name = fileName(path);
    return (index ? "The staged version of " + name : name) +
           " changed since this view was drawn, so nothing was done. The view now shows it "
           "as it is.";
}

// -------------------------------------------------------------- the page
std::u16string fileHeaderText(const FileDiff& d) {
    const File& f = d.file;
    std::string s = f.path;
    if (f.conflict) {
        if (!conflictReadable(f)) return toU16(s + "  (conflict)");
        std::string left = MergeConflicts::leftText(d.conflicts.size());
        left[0] = (char)tolower((unsigned char)left[0]);
        return toU16(s + "  " + left);
    }
    if (!f.origPath.empty()) s += "  (renamed from " + f.origPath + ")";
    else if (f.untracked || (!f.oldExists && f.newExists)) s += "  (new)";
    else if (!f.newExists && f.oldExists) s += "  (deleted)";
    std::string counts;
    if (d.added) counts += "+" + std::to_string(d.added);
    if (d.removed) counts += (counts.empty() ? "-" : " -") + std::to_string(d.removed);
    if (!counts.empty()) s += "  " + counts;
    return toU16(s);
}

std::string sectionTitle(Section s, size_t files) {
    const std::string n = "  " + std::to_string(files);
    if (s == Section::Staged) return "Staged Changes" + n;
    if (s == Section::Changes) return "Changes" + n;
    return "Changes" + n;
}

std::string gapText(size_t hidden) {
    return hidden == 1 ? "1 unchanged line" : std::to_string(hidden) + " unchanged lines";
}

static std::string noteFor(const FileDiff& d) {
    const File& f = d.file;
    if (f.conflict && canMarkResolved(d))
        return "No conflict marks are left in it. Mark Resolved stages it, which tells git the "
               "conflict is resolved.";
    if (f.conflict && !conflictReadable(f))
        return "This file has conflicts. Open it to resolve them, then stage it.";
    if (f.submodule) return "A submodule: its commit changed, or it has changes inside.";
    if (f.symlink) return "A symbolic link: its target changed.";
    if (f.binary) return "A binary file: its bytes are not shown.";
    if (f.tooLarge) return "Too large to show here. Side by Side or Open shows it.";
    return "";
}

Page layout(const std::vector<FileDiff>& files, const Opened& opened, size_t context) {
    Page p;
    auto addRow = [&](const PageRow& row, const std::u16string& text) {
        if (!p.rows.empty()) p.text += u'\n';
        p.starts.push_back(p.text.size());
        p.text += text;
        p.rows.push_back(row);
        return p.rows.size() - 1;
    };
    // Syntax spans of one line onto the row just added.
    auto addSyntax = [&](const std::vector<SideBySide::Span>& spans,
                         const std::vector<size_t>& starts, const std::u16string& text, int line,
                         size_t rowStart) {
        if (spans.empty() || line < 0 || (size_t)line + 1 >= starts.size()) return;
        const size_t from = starts[(size_t)line];
        const size_t to = contentEnd(text, from, starts[(size_t)line + 1]);
        // The first span that ends past the line's start.
        auto it = std::lower_bound(spans.begin(), spans.end(), from,
                                   [](const SideBySide::Span& s, size_t at) {
                                       return s.start + s.length <= at;
                                   });
        for (; it != spans.end() && it->start < to; ++it) {
            const size_t a = std::max(it->start, from), b = std::min(it->start + it->length, to);
            if (b > a) p.syntax.push_back({rowStart + (a - from), b - a, it->style});
        }
    };
    // Section counts first, for the section rows.
    size_t wordBudget = 4 * 1024 * 1024;
    std::map<int, size_t> perSection;
    for (const FileDiff& d : files) perSection[(int)d.file.section]++;

    for (size_t fi = 0; fi < files.size(); fi++) {
        const FileDiff& d = files[fi];
        const File& f = d.file;
        if (f.section != Section::Plain && (fi == 0 || files[fi - 1].file.section != f.section)) {
            PageRow s;
            s.kind = RowKind::Section;
            s.file = fi;
            addRow(s, toU16(sectionTitle(f.section, perSection[(int)f.section])));
        }
        PageRow h;
        h.kind = RowKind::File;
        h.file = fi;
        addRow(h, fileHeaderText(d));
        const std::string note = noteFor(d);
        if (!note.empty()) {
            PageRow n;
            n.kind = RowKind::Note;
            n.file = fi;
            addRow(n, toU16(note));
            continue;
        }
        if (d.rows.empty()) {
            PageRow n;
            n.kind = RowKind::Note;
            n.file = fi;
            addRow(n, u"An empty file.");
            continue;
        }
        // Which unchanged rows show: those within `context` of a change.
        const size_t nr = d.rows.size();
        std::vector<bool> near(nr, false);
        for (const SideBySide::Hunk& b : d.blocks) {
            const size_t lo = b.first > context ? b.first - context : 0;
            const size_t hi = std::min(nr, b.end + context);
            for (size_t r = lo; r < hi; r++) near[r] = true;
        }
        const auto openIt = opened.find(f.key());
        auto isOpen = [&](int id) {
            return openIt != opened.end() && openIt->second.count(id) > 0;
        };
        auto addContext = [&](const SideBySide::Row& row) {
            PageRow c;
            c.kind = RowKind::Context;
            c.file = fi;
            c.oldLine = row.left;
            c.newLine = row.right;
            const size_t at = addRow(c, newLine(d, row.right));
            addSyntax(d.newSyntax, d.newStarts, f.newText, row.right, p.starts[at]);
        };
        size_t blockIndex = 0;
        for (size_t r = 0; r < nr;) {
            const SideBySide::Row& row = d.rows[r];
            if (row.kind == SideBySide::Kind::Same) {
                // A run of unchanged rows not near a change.
                if (!near[r]) {
                    size_t e = r;
                    while (e < nr && d.rows[e].kind == SideBySide::Kind::Same && !near[e]) e++;
                    const int id = d.rows[r].right;
                    if (e - r >= 2 && !isOpen(id)) {
                        PageRow g;
                        g.kind = RowKind::Gap;
                        g.file = fi;
                        g.gap = id;
                        g.hidden = e - r;
                        addRow(g, toU16(gapText(e - r)));
                        r = e;
                        continue;
                    }
                    // Opened, or a single line: all of it shows.
                    for (; r < e; r++) addContext(d.rows[r]);
                    continue;
                }
                addContext(row);
                r++;
                continue;
            }
            // A conflict block: its lines as they are, markers too, each
            // tinted by its part.
            if (f.conflict) {
                const size_t e = blockIndex < d.blocks.size() ? d.blocks[blockIndex].end : r + 1;
                const int block = (int)blockIndex++;
                for (size_t k = r; k < e; k++) {
                    PageRow x;
                    x.kind = RowKind::Conflict;
                    x.file = fi;
                    x.block = block;
                    x.oldLine = x.newLine = d.rows[k].right;
                    x.part = k < d.parts.size() ? d.parts[k] : ConflictPart::None;
                    x.blockStart = k == r;
                    if (k == r) p.blockRows.push_back(p.rows.size());
                    const size_t at = addRow(x, newLine(d, x.newLine));
                    addSyntax(d.newSyntax, d.newStarts, f.newText, x.newLine, p.starts[at]);
                }
                r = e;
                continue;
            }
            // A block: its old lines, then its new ones.
            size_t e = r;
            while (e < nr && d.rows[e].kind != SideBySide::Kind::Same) e++;
            const int block = (int)blockIndex++;
            bool first = true;
            std::vector<size_t> oldRowOf(e - r, SIZE_MAX), newRowOf(e - r, SIZE_MAX);
            for (size_t k = r; k < e; k++) {
                if (d.rows[k].left < 0) continue;
                PageRow x;
                x.kind = RowKind::Removed;
                x.file = fi;
                x.block = block;
                x.oldLine = d.rows[k].left;
                x.blockStart = first;
                if (first) p.blockRows.push_back(p.rows.size());
                first = false;
                const size_t at = addRow(x, oldLine(d, x.oldLine));
                oldRowOf[k - r] = at;
                addSyntax(d.oldSyntax, d.oldStarts, f.oldText, x.oldLine, p.starts[at]);
            }
            for (size_t k = r; k < e; k++) {
                if (d.rows[k].right < 0) continue;
                PageRow x;
                x.kind = RowKind::Added;
                x.file = fi;
                x.block = block;
                x.newLine = d.rows[k].right;
                x.blockStart = first;
                if (first) p.blockRows.push_back(p.rows.size());
                first = false;
                const size_t at = addRow(x, newLine(d, x.newLine));
                newRowOf[k - r] = at;
                addSyntax(d.newSyntax, d.newStarts, f.newText, x.newLine, p.starts[at]);
            }
            // The words that changed inside each changed pair of lines; a
            // line longer than a screen is left to its tint, and past a few
            // megabytes of changed lines the rest are too (nobody reads a
            // huge rewrite word by word), as in the compare view.
            for (size_t k = r; k < e; k++) {
                if (d.rows[k].kind != SideBySide::Kind::Changed || wordBudget == 0) continue;
                const std::u16string o = oldLine(d, d.rows[k].left), n = newLine(d, d.rows[k].right);
                if (o.size() > 2000 || n.size() > 2000) continue;
                wordBudget -= std::min(wordBudget, o.size() + n.size());
                const SideBySide::WordChanges w = SideBySide::wordChanges(o, n);
                for (const SideBySide::Span& s : w.left)
                    p.words.push_back({p.starts[oldRowOf[k - r]] + s.start, s.length, 0});
                for (const SideBySide::Span& s : w.right)
                    p.words.push_back({p.starts[newRowOf[k - r]] + s.start, s.length, 1});
            }
            r = e;
        }
        if (d.lastNewlineChanged) {
            PageRow n;
            n.kind = RowKind::Note;
            n.file = fi;
            const bool newEnds = !f.newText.empty() && f.newText.back() == u'\n';
            addRow(n, newEnds ? u"A newline was added at the end of the file."
                              : u"The newline at the end of the file was taken away.");
        }
    }
    // Spans in page order (each file's rows came in order, words after
    // their block's lines).
    std::sort(p.words.begin(), p.words.end(),
              [](const SideBySide::Span& x, const SideBySide::Span& y) { return x.start < y.start; });
    return p;
}

long stepBlock(const Page& p, long row, bool forward) {
    const std::vector<size_t>& b = p.blockRows;
    if (b.empty()) return -1;
    if (forward) {
        for (size_t r : b)
            if ((long)r > row) return (long)r;
        return (long)b.front();
    }
    for (size_t i = b.size(); i-- > 0;)
        if ((long)b[i] < row) return (long)b[i];
    return (long)b.back();
}

Anchor anchorAt(const Page& p, const std::vector<FileDiff>& files, size_t row) {
    Anchor a;
    if (p.rows.empty() || files.empty()) return a;
    row = std::min(row, p.rows.size() - 1);
    // A section row stands for the file after it.
    const size_t fi = p.rows[row].file;
    if (fi >= files.size()) return a;
    a.fileKey = files[fi].file.key();
    a.fileIndex = fi;
    a.valid = true;
    for (size_t r = row + 1; r-- > 0;) {
        const PageRow& x = p.rows[r];
        if (x.file != fi || x.kind == RowKind::Section) break;
        if (x.kind == RowKind::File) {
            a.offset = (long)(row - r);
            return a;
        }
        if (x.newLine >= 0 || x.oldLine >= 0) {
            a.newLine = x.newLine;
            a.oldLine = x.newLine >= 0 ? -1 : x.oldLine;
            a.offset = (long)(row - r);
            return a;
        }
    }
    return a;
}

long fileRow(const Page& p, const std::vector<FileDiff>& files, const std::string& key) {
    for (size_t r = 0; r < p.rows.size(); r++)
        if (p.rows[r].kind == RowKind::File && p.rows[r].file < files.size() &&
            files[p.rows[r].file].file.key() == key)
            return (long)r;
    return -1;
}

size_t rowFor(const Page& p, const std::vector<FileDiff>& files, const Anchor& a) {
    if (p.rows.empty() || !a.valid) return 0;
    const long header = fileRow(p, files, a.fileKey);
    if (header < 0) {
        // The file went (its last change staged or discarded): the one now
        // at its place, else the last.
        if (files.empty()) return 0;
        const size_t fi = std::min(a.fileIndex, files.size() - 1);
        for (size_t r = 0; r < p.rows.size(); r++)
            if (p.rows[r].kind == RowKind::File && p.rows[r].file == fi) return r;
        return 0;
    }
    size_t at = (size_t)header;
    if (a.newLine >= 0 || a.oldLine >= 0) {
        const size_t fi = p.rows[at].file;
        for (size_t r = at + 1; r < p.rows.size() && p.rows[r].file == fi &&
                                p.rows[r].kind != RowKind::Section && p.rows[r].kind != RowKind::File;
             r++) {
            const PageRow& x = p.rows[r];
            const bool hit = a.newLine >= 0 ? (x.newLine >= 0 && x.newLine >= a.newLine)
                                            : (x.oldLine >= 0 && x.oldLine >= a.oldLine);
            if (hit) { at = r; break; }
            at = r;
        }
    }
    const size_t fi = p.rows[at].file;
    for (long k = 0; k < a.offset && at + 1 < p.rows.size() && p.rows[at + 1].file == fi &&
                     p.rows[at + 1].kind != RowKind::File && p.rows[at + 1].kind != RowKind::Section;
         k++)
        at++;
    return at;
}

// ------------------------------------------------------- reading from git
std::vector<std::string> lsFilesArgs(const std::vector<std::string>& paths) {
    std::vector<std::string> a = {"--literal-pathspecs", "ls-files", "--stage", "-z", "--"};
    a.insert(a.end(), paths.begin(), paths.end());
    return a;
}

std::vector<std::string> lsTreeArgs(const std::string& commit,
                                    const std::vector<std::string>& paths) {
    std::vector<std::string> a = {"--literal-pathspecs", "ls-tree", "-r", "-z", commit, "--"};
    a.insert(a.end(), paths.begin(), paths.end());
    return a;
}

std::vector<std::string> batchArgs() { return {"cat-file", "--batch"}; }

std::map<std::string, Entry> parseLsFiles(const std::string& out) {
    // "<mode> <object> <stage>\t<path>\0"
    std::map<std::string, Entry> m;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find('\0', pos);
        if (end == std::string::npos) end = out.size();
        const std::string rec = out.substr(pos, end - pos);
        pos = end + 1;
        const size_t s1 = rec.find(' '), s2 = rec.find(' ', s1 == std::string::npos ? 0 : s1 + 1),
                     tab = rec.find('\t');
        if (s1 == std::string::npos || s2 == std::string::npos || tab == std::string::npos ||
            !(s1 < s2 && s2 < tab))
            continue;
        if (rec.substr(s2 + 1, tab - s2 - 1) != "0") continue;
        m[rec.substr(tab + 1)] = {rec.substr(0, s1), rec.substr(s1 + 1, s2 - s1 - 1)};
    }
    return m;
}

std::map<std::string, Entry> parseLsTree(const std::string& out) {
    // "<mode> <type> <object>\t<path>\0"
    std::map<std::string, Entry> m;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find('\0', pos);
        if (end == std::string::npos) end = out.size();
        const std::string rec = out.substr(pos, end - pos);
        pos = end + 1;
        const size_t s1 = rec.find(' '), s2 = rec.find(' ', s1 == std::string::npos ? 0 : s1 + 1),
                     tab = rec.find('\t');
        if (s1 == std::string::npos || s2 == std::string::npos || tab == std::string::npos ||
            !(s1 < s2 && s2 < tab))
            continue;
        const std::string type = rec.substr(s1 + 1, s2 - s1 - 1);
        if (type != "blob" && type != "commit") continue;
        m[rec.substr(tab + 1)] = {rec.substr(0, s1), rec.substr(s2 + 1, tab - s2 - 1)};
    }
    return m;
}

std::map<std::string, std::string> parseBatch(const std::string& out) {
    // "<object> <type> <size>\n<bytes>\n", or "<name> missing\n".
    std::map<std::string, std::string> m;
    size_t pos = 0;
    while (pos < out.size()) {
        const size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) break;
        const std::string head = out.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t s1 = head.find(' '), s2 = head.rfind(' ');
        if (s1 == std::string::npos || s2 == s1) continue;   // "<name> missing"
        const std::string name = head.substr(0, s1);
        const std::string sizeText = head.substr(s2 + 1);
        if (sizeText.empty() || sizeText.find_first_not_of("0123456789") != std::string::npos)
            break;
        const size_t size = (size_t)std::stoull(sizeText);
        if (pos + size > out.size()) break;
        m[name] = out.substr(pos, size);
        pos += size + 1;
    }
    return m;
}

std::vector<std::string> commitHeaderArgs(const std::string& commit) {
    return {"show", "-s", "--no-color", std::string("--format=") + Git::showFormat(), commit, "--"};
}

std::vector<std::string> commitDiffArgs(const std::string& commit, const std::string& parent) {
    std::vector<std::string> a = {"-c", "core.quotePath=false", "diff-tree", "-r", "-z", "-M",
                                  "--no-commit-id", "--no-ext-diff", "--no-abbrev"};
    if (parent.empty()) {
        a.push_back("--root");
        a.push_back(commit);
    } else {
        a.push_back(parent);
        a.push_back(commit);
    }
    a.push_back("--");
    return a;
}

bool nullObject(const std::string& object) {
    return !object.empty() && object.find_first_not_of('0') == std::string::npos;
}

std::vector<RawChange> parseRawDiff(const std::string& out) {
    // ":<old mode> <new mode> <old object> <new object> <status>\0<path>\0",
    // a rename or copy with its source path first: "R100\0<old>\0<new>\0".
    std::vector<RawChange> v;
    size_t pos = 0;
    auto field = [&](std::string& into) {
        if (pos >= out.size()) return false;
        size_t end = out.find('\0', pos);
        if (end == std::string::npos) end = out.size();
        into = out.substr(pos, end - pos);
        pos = end + 1;
        return true;
    };
    std::string head;
    while (field(head)) {
        // A stray newline before the first record (diff-tree's commit id
        // line when --no-commit-id was not given) is skipped.
        while (!head.empty() && (head[0] == '\n' || head[0] == '\r')) head.erase(0, 1);
        if (head.size() < 2 || head[0] != ':') continue;
        std::vector<std::string> w;
        for (size_t at = 1; at <= head.size();) {
            size_t sp = head.find(' ', at);
            if (sp == std::string::npos) sp = head.size();
            w.push_back(head.substr(at, sp - at));
            at = sp + 1;
        }
        if (w.size() < 5 || w[4].empty()) continue;
        RawChange c;
        c.oldMode = w[0];
        c.newMode = w[1];
        c.oldObject = w[2];
        c.newObject = w[3];
        c.letter = w[4][0];
        std::string a, b;
        if (!field(a)) break;
        if (c.letter == 'R' || c.letter == 'C') {
            if (!field(b)) break;
            c.origPath = a;
            c.path = b;
        } else {
            c.path = a;
        }
        v.push_back(c);
    }
    return v;
}

std::string commitByline(const std::string& author, long long time, const std::string& hash,
                         size_t parents, long long now, GitUi::DateFormat date) {
    std::string s = GitUi::validUtf8(author) + ", " + Git::relativeTime(time, now) + " (" +
                    (date ? date(time) : GitUi::dateText(time)) + ")  " + Git::shortHash(hash);
    if (parents > 1) s += "  (a merge: its changes against the first parent, what it brought in)";
    return s;
}

std::vector<std::string> worktreeRawArgs(const std::string& base) {
    // --no-abbrev: `diff --raw` shortens object names, and cat-file --batch
    // answers with the whole name, so the old sides were never found.
    return {"-c", "core.quotePath=false", "diff", "--raw", "--no-abbrev", "-z", "-M", "--no-ext-diff",
            base, "--"};
}

std::vector<std::string> worktreeListArgs() { return {"worktree", "list", "--porcelain"}; }

std::string mainWorktreePath(const std::string& porcelain) {
    const std::string mark = "worktree ";
    if (porcelain.compare(0, mark.size(), mark) != 0) return "";
    const size_t nl = porcelain.find('\n');
    return porcelain.substr(mark.size(), (nl == std::string::npos ? porcelain.size() : nl) - mark.size());
}

std::vector<std::string> nulPaths(const std::string& out) {
    std::vector<std::string> v;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find('\0', pos);
        if (end == std::string::npos) end = out.size();
        if (end > pos) v.push_back(out.substr(pos, end - pos));
        pos = end + 1;
    }
    return v;
}

bool looksBinary(const std::string& bytes) {
    if (bytes.size() >= 2) {
        const unsigned char a = (unsigned char)bytes[0], b = (unsigned char)bytes[1];
        if ((a == 0xFF && b == 0xFE) || (a == 0xFE && b == 0xFF)) return false;
    }
    const size_t n = std::min<size_t>(bytes.size(), 8192);
    return bytes.find('\0') < n;
}

}  // namespace ChangeReview
