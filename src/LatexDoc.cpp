// LatexDoc.cpp — see LatexDoc.h. A structural reader, not a typesetter.
#include "LatexDoc.h"
#include <algorithm>
#include <cctype>

namespace {

const std::size_t kNpos = std::string::npos;

bool isLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// Environment name with any trailing star removed ("align*" -> "align").
std::string bare(const std::string &env) {
    if (!env.empty() && env.back() == '*') return env.substr(0, env.size() - 1);
    return env;
}

bool contains(const char *const *list, const std::string &name) {
    for (const char *const *p = list; *p; ++p)
        if (name == *p) return true;
    return false;
}

// Commands whose braced argument is a piece of prose the preview can edit.
const char *const kFieldCommands[] = {
    "title", "author", "date", "part", "chapter", "section", "subsection",
    "subsubsection", "paragraph", "subparagraph", "caption", "thanks",
    "institute", "subtitle", "titlerunning", "authorrunning", nullptr};

// Commands we look *through*: their content is ordinary prose, so it becomes
// Text spans of its own rather than being skipped with the command.
const char *const kTransparentCommands[] = {
    "textbf", "textit", "emph", "texttt", "textsc", "textrm", "textsf",
    "underline", "footnote", "mbox", "text", "textnormal", "uline", nullptr};

// Content that must be passed over verbatim.
const char *const kVerbatimEnvs[] = {"verbatim", "Verbatim", "lstlisting",
                                     "minted", "alltt", "comment", nullptr};

// Environments whose body is math (editable only as raw TeX).
const char *const kMathEnvs[] = {"equation", "align",   "gather", "multline",
                                 "eqnarray", "displaymath", "alignat",
                                 "flalign",  "math",    "split",  nullptr};

const char *const kListEnvs[] = {"itemize", "enumerate", "description",
                                 nullptr};

// Environments that take a column specification we must not read as text.
const char *const kSpecEnvs[] = {"tabular", "tabularx", "array", "longtable",
                                 "tabulary", nullptr};

// The whitespace at the start of the line containing `off`.
std::string lineIndent(const std::string &s, std::size_t off) {
    std::size_t bol = s.rfind('\n', off == 0 ? 0 : off - 1);
    bol = (bol == kNpos) ? 0 : bol + 1;
    std::string indent;
    for (std::size_t i = bol; i < s.size() && isBlank(s[i]); i++) indent += s[i];
    return indent;
}

struct Parser {
    const std::string &s;
    std::size_t n;
    std::size_t i = 0;
    LatexDoc doc;
    bool inDocument = false;
    std::vector<int> listStack;       // innermost list last
    std::vector<int> itemStack;       // its current item index
    std::size_t runStart = kNpos;     // start of the plain-text run in progress

    explicit Parser(const std::string &src) : s(src), n(src.size()) {}

    int curList() const { return listStack.empty() ? -1 : listStack.back(); }
    int curItem() const { return itemStack.empty() ? -1 : itemStack.back(); }

    void push(LatexSpanKind kind, const std::string &cmd, std::size_t a,
              std::size_t b) {
        while (a < b && isSpace(s[a])) a++;
        while (b > a && isSpace(s[b - 1])) b--;
        if (a >= b) return;
        LatexSpan sp;
        sp.kind = kind;
        sp.command = cmd;
        sp.start = a;
        sp.end = b;
        sp.line = LatexDoc::lineAt(s, a);
        sp.display = LatexDoc::displayText(s.substr(a, b - a));
        sp.listIndex = curList();
        sp.itemIndex = curItem();
        doc.spans.push_back(sp);
    }

    // End the plain-text run, if any, at `end`.
    void flush(std::size_t end) {
        if (runStart == kNpos) return;
        std::size_t a = runStart;
        runStart = kNpos;
        if (inDocument) push(LatexSpanKind::Text, "", a, end);
    }

    void skipBlanks() {
        while (i < n && isBlank(s[i])) i++;
    }
    void skipSpace() {
        while (i < n && isSpace(s[i])) i++;
    }

    // With s[i] == '{' (or '['), move past the balanced group and return the
    // range of its contents. Returns false if there is no group here.
    bool group(char open, char close, std::size_t *innerStart,
               std::size_t *innerEnd) {
        if (i >= n || s[i] != open) return false;
        std::size_t depth = 0, start = i + 1;
        while (i < n) {
            char c = s[i];
            if (c == '\\' && i + 1 < n) { i += 2; continue; }   // escaped brace
            if (c == open) depth++;
            else if (c == close) {
                depth--;
                if (depth == 0) {
                    if (innerStart) *innerStart = start;
                    if (innerEnd) *innerEnd = i;
                    i++;
                    return true;
                }
            }
            i++;
        }
        if (innerStart) *innerStart = start;   // unterminated: to end of file
        if (innerEnd) *innerEnd = n;
        return true;
    }

    void skipOptionalArgs() {
        for (;;) {
            std::size_t save = i;
            skipBlanks();
            if (i < n && s[i] == '[') { group('[', ']', nullptr, nullptr); continue; }
            i = save;
            return;
        }
    }

    void skipBraceArgs(int count) {
        for (int k = 0; k < count; k++) {
            std::size_t save = i;
            skipBlanks();
            if (!(i < n && s[i] == '{')) { i = save; return; }
            group('{', '}', nullptr, nullptr);
        }
    }

    // Find "\end{name}" from position i; returns its offset, or npos.
    std::size_t findEnd(const std::string &name) const {
        std::string needle = "\\end{" + name + "}";
        return s.find(needle, i);
    }

    void run() {
        while (i < n) {
            char c = s[i];
            if (c == '%') {
                flush(i);
                while (i < n && s[i] != '\n') i++;
                continue;
            }
            if (c == '\\') { command(); continue; }
            if (c == '$') { dollarMath(); continue; }
            if (c == '{' || c == '}') { flush(i); i++; continue; }
            // A table cell separator ends the run, so each cell edits alone.
            if (c == '&') { flush(i); i++; continue; }
            if (c == '\n' && paragraphBreakAt(i)) { flush(i); i++; continue; }
            if (runStart == kNpos && !isSpace(c)) runStart = i;
            i++;
        }
        flush(n);
        // An unterminated list still gets its last item closed.
        while (!listStack.empty()) closeList(n);
    }

    // A newline followed by another newline (with only blanks between) ends a
    // paragraph, and so ends the editable run.
    bool paragraphBreakAt(std::size_t at) const {
        std::size_t k = at + 1;
        while (k < n && isBlank(s[k])) k++;
        return k < n && s[k] == '\n';
    }

    void dollarMath() {
        flush(i);
        bool display = (i + 1 < n && s[i + 1] == '$');
        std::size_t open = i + (display ? 2 : 1);
        std::size_t k = open;
        while (k < n) {
            if (s[k] == '\\' && k + 1 < n) { k += 2; continue; }
            if (s[k] == '$') break;
            k++;
        }
        push(LatexSpanKind::Math, display ? "$$" : "$", open, k);
        i = (k < n) ? k + (display && k + 1 < n && s[k + 1] == '$' ? 2 : 1) : n;
    }

    void command() {
        std::size_t cmdStart = i;
        i++;                                   // past the backslash
        if (i >= n) { flush(cmdStart); return; }
        std::string name;
        if (isLetter(s[i])) {
            while (i < n && isLetter(s[i])) name += s[i++];
            if (i < n && s[i] == '*') i++;     // \section* and friends
        } else {
            name = std::string(1, s[i]);
            i++;
        }

        // A row break ends the run, like a cell separator.
        if (name == "\\\\") { flush(cmdStart); skipOptionalArgs(); return; }

        // An escaped character (\&, \%, \_) stays part of the surrounding text.
        if (name.size() == 1 && !isLetter(name[0]) && name != "[" &&
            name != "]" && name != "(" && name != ")") {
            if (runStart == kNpos) runStart = cmdStart;
            return;
        }

        if (name == "[" || name == "(") {
            flush(cmdStart);
            std::string close = (name == "[") ? "\\]" : "\\)";
            std::size_t k = s.find(close, i);
            std::size_t stop = (k == kNpos) ? n : k;
            push(LatexSpanKind::Math, name == "[" ? "\\[" : "\\(", i, stop);
            i = (k == kNpos) ? n : k + 2;
            return;
        }
        if (name == "]" || name == ")") { flush(cmdStart); return; }

        if (name == "begin") { flush(cmdStart); beginEnv(cmdStart); return; }
        if (name == "end")   { flush(cmdStart); endEnv(cmdStart);   return; }
        if (name == "item")  { flush(cmdStart); item(cmdStart);     return; }

        flush(cmdStart);
        if (contains(kFieldCommands, name)) {
            skipOptionalArgs();
            std::size_t save = i;
            skipSpace();
            std::size_t a = 0, b = 0;
            if (i < n && s[i] == '{' && group('{', '}', &a, &b))
                push(LatexSpanKind::Field, name, a, b);
            else
                i = save;
            return;
        }
        if (contains(kTransparentCommands, name)) {
            skipOptionalArgs();
            std::size_t save = i;
            skipSpace();
            if (i < n && s[i] == '{') i++;     // step inside; '}' ends the run
            else i = save;
            return;
        }
        // Anything else: step over its arguments so their contents (file
        // names, labels, package options) are never offered as text.
        skipOptionalArgs();
        skipBraceArgs(2);
    }

    void beginEnv(std::size_t cmdStart) {
        std::size_t a = 0, b = 0;
        skipSpace();
        if (!(i < n && s[i] == '{' && group('{', '}', &a, &b))) return;
        std::string env = s.substr(a, b - a);
        std::string base = bare(env);
        skipOptionalArgs();
        if (contains(kSpecEnvs, base)) skipBraceArgs(2);

        if (contains(kVerbatimEnvs, base)) {
            std::size_t k = findEnd(env);
            i = (k == kNpos) ? n : k + 6 + env.size();
            return;
        }
        if (contains(kMathEnvs, base)) {
            std::size_t k = findEnd(env);
            std::size_t stop = (k == kNpos) ? n : k;
            push(LatexSpanKind::Math, env, i, stop);
            i = (k == kNpos) ? n : k + 6 + env.size();
            return;
        }
        if (env == "document") { inDocument = true; return; }
        if (contains(kListEnvs, base)) {
            LatexList list;
            list.environment = env;
            list.start = cmdStart;
            list.end = n;
            list.indent = lineIndent(s, cmdStart) + "  ";
            list.line = LatexDoc::lineAt(s, cmdStart);
            doc.lists.push_back(list);
            listStack.push_back((int)doc.lists.size() - 1);
            itemStack.push_back(-1);
        }
    }

    void endEnv(std::size_t cmdStart) {
        std::size_t a = 0, b = 0;
        skipSpace();
        if (!(i < n && s[i] == '{' && group('{', '}', &a, &b))) return;
        std::string env = s.substr(a, b - a);
        if (env == "document") { inDocument = false; return; }
        if (contains(kListEnvs, bare(env)) && !listStack.empty())
            closeList(cmdStart);
    }

    void closeList(std::size_t atEnd) {
        LatexList &list = doc.lists[listStack.back()];
        if (!list.itemBodyStart.empty() &&
            list.itemBodyEnd.size() < list.itemBodyStart.size())
            list.itemBodyEnd.push_back(atEnd);
        list.end = i;
        listStack.pop_back();
        itemStack.pop_back();
    }

    void item(std::size_t cmdStart) {
        if (listStack.empty()) return;
        LatexList &list = doc.lists[listStack.back()];
        if (!list.itemBodyStart.empty() &&
            list.itemBodyEnd.size() < list.itemBodyStart.size())
            list.itemBodyEnd.push_back(cmdStart);
        // \item[label] in a description list: the label is a field of its own.
        std::size_t save = i;
        skipBlanks();
        if (i < n && s[i] == '[') {
            std::size_t a = 0, b = 0;
            if (group('[', ']', &a, &b)) push(LatexSpanKind::Field, "item", a, b);
        } else {
            i = save;
        }
        list.itemBodyStart.push_back(i);
        list.indent = lineIndent(s, cmdStart);
        itemStack.back() = (int)list.itemBodyStart.size() - 1;
    }
};

}  // namespace

LatexDoc LatexDoc::parse(const std::string &src) {
    Parser p(src);
    p.run();
    return p.doc;
}

std::vector<const LatexSpan *> LatexDoc::spansOnLine(int line) const {
    std::vector<const LatexSpan *> out;
    for (const LatexSpan &sp : spans)
        if (sp.line == line) out.push_back(&sp);
    return out;
}

namespace {

// Letters and digits only, lowercased. The PDF's idea of a word and the
// source's rarely agree on punctuation: math reads back as "x2" where the
// source says "x^2", and quotes and dashes differ too.
std::string normalized(const std::string &s) {
    std::string out;
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u)) out += (char)std::tolower(u);
    }
    return out;
}

}  // namespace

const LatexSpan *LatexDoc::spanForClick(const std::vector<int> &lines,
                                        const std::string &word) const {
    // A paragraph's box is closed on a later line than its text, so look a
    // little above the reported line, and just below it, before giving up.
    static const int kNearby[] = {0, -1, -2, 1, -3};
    std::string needle = normalized(word);
    const LatexSpan *firstSeen = nullptr;

    for (int pass = 0; pass < 2; pass++) {
        // Pass 0 insists on the clicked word; pass 1 takes the nearest span.
        if (pass == 1 && needle.empty()) break;
        for (int line : lines) {
            for (int delta : kNearby) {
                for (const LatexSpan *sp : spansOnLine(line + delta)) {
                    if (!firstSeen) firstSeen = sp;
                    if (pass == 1) return sp;
                    if (!needle.empty() &&
                        normalized(sp->display).find(needle) != std::string::npos)
                        return sp;
                }
            }
        }
    }
    return firstSeen;
}

int LatexDoc::lineAt(const std::string &src, std::size_t offset) {
    int line = 1;
    for (std::size_t k = 0; k < offset && k < src.size(); k++)
        if (src[k] == '\n') line++;
    return line;
}

std::string LatexDoc::displayText(const std::string &tex) {
    std::string out;
    std::size_t i = 0, n = tex.size();
    bool pendingSpace = false;
    auto emit = [&](char c) {
        if (pendingSpace && !out.empty()) out += ' ';
        pendingSpace = false;
        out += c;
    };
    while (i < n) {
        char c = tex[i];
        if (isSpace(c)) { pendingSpace = true; i++; continue; }
        if (c == '~') { pendingSpace = true; i++; continue; }
        if (c == '%') { while (i < n && tex[i] != '\n') i++; continue; }
        if (c == '{' || c == '}' || c == '&') { i++; continue; }
        if (c == '\\' && i + 1 < n) {
            char next = tex[i + 1];
            if (!isLetter(next)) {           // \& \% \_ \# \$ \{ \}
                if (next == '\\') pendingSpace = true;
                else emit(next);
                i += 2;
                continue;
            }
            i++;                              // a command: drop name and args
            while (i < n && isLetter(tex[i])) i++;
            if (i < n && tex[i] == '*') i++;
            pendingSpace = true;
            continue;
        }
        emit(c);
        i++;
    }
    return out;
}

LatexDoc::Edit LatexDoc::replaceSpan(const std::string &src,
                                     const LatexSpan &span,
                                     const std::string &replacement) {
    Edit e;
    std::size_t a = std::min(span.start, src.size());
    std::size_t b = std::min(span.end, src.size());
    if (b < a) b = a;
    e.source = src.substr(0, a) + replacement + src.substr(b);
    e.start = a;
    e.end = a + replacement.size();
    return e;
}

LatexDoc::Edit LatexDoc::addItem(const std::string &src, const LatexList &list,
                                 int afterItem, const std::string &text) {
    Edit e;
    std::size_t pos;
    const std::size_t count = list.itemBodyEnd.size();
    if (count == 0) {
        // An empty list: start on the line after \begin{...}.
        pos = src.find('\n', list.start);
        pos = (pos == kNpos) ? std::min(list.end, src.size()) : pos;
    } else {
        std::size_t idx = (afterItem < 0 || (std::size_t)afterItem >= count)
                              ? count - 1
                              : (std::size_t)afterItem;
        pos = std::min(list.itemBodyEnd[idx], src.size());
    }
    while (pos > 0 && isSpace(src[pos - 1])) pos--;   // past the line's end
    std::string prefix = "\n" + list.indent + "\\item ";
    e.source = src.substr(0, pos) + prefix + text + src.substr(pos);
    e.start = pos + prefix.size();
    e.end = e.start + text.size();
    return e;
}
