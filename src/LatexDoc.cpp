// LatexDoc.cpp — see LatexDoc.h. A structural reader, not a typesetter.
#include "LatexDoc.h"
#include <algorithm>
#include <cctype>
#include <initializer_list>

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

// Commands whose braced arguments are never prose: file names, labels, lengths,
// package options. Everything else is looked *through*, because a real document
// wraps its text in all sorts of commands, including the author's own macros,
// and text the reader cannot see is text the preview cannot edit.
const char *const kSkipCommands[] = {
    "label", "ref", "eqref", "pageref", "cite", "citep", "citet", "nocite",
    "includegraphics", "usepackage", "documentclass", "input", "include",
    "bibliography", "bibliographystyle", "pagestyle", "thispagestyle",
    "setlength", "addtolength", "setcounter", "addtocounter", "vspace",
    "hspace", "rule", "hyphenation", "graphicspath", "hypersetup",
    "definecolor", "pagenumbering", "newcommand", "renewcommand",
    "providecommand", "newenvironment", "renewenvironment", "DeclareRobustCommand",
    "DeclareMathOperator", "geometry", "bibitem", "hyperref",
    "newcolumntype", "columnwidth", "multicolumn", nullptr};

// Commands that print a character or a logo in running text. They stay part
// of the run around them, so "Stra\ss e" is one span, not "Stra" and "e".
const char *const kTextSymbols[] = {
    "ss", "SS", "o", "O", "ae", "AE", "oe", "OE", "aa", "AA", "l", "L", "i",
    "j", "S", "P", "dag", "ddag", "copyright", "pounds", "euro", "ldots",
    "dots", "textellipsis", "textendash", "textemdash", "textquoteleft",
    "textquoteright", "textquotedblleft", "textquotedblright", "textbullet",
    "textperiodcentered", "textregistered", "texttrademark", "textdegree",
    "textasciitilde", "textbackslash", "textbar", "textless", "textgreater",
    "TeX", "LaTeX", "LaTeXe", "XeTeX", "BibTeX", nullptr};

// Accents written as a letter command over a braced letter: Fran\c{c}ais,
// \v{s}, \H{o}. Also part of the run, argument and all.
const char *const kLetterAccents[] = {"c", "v", "u", "H", "k", "r", "d", "b",
                                      "t", nullptr};

// What each text symbol reads as, for matching against the PDF.
struct SymbolText { const char *name; const char *text; };
const SymbolText kSymbolText[] = {
    {"ss", "ss"}, {"SS", "SS"}, {"o", "o"}, {"O", "O"}, {"ae", "ae"},
    {"AE", "AE"}, {"oe", "oe"}, {"OE", "OE"}, {"aa", "a"}, {"AA", "A"},
    {"l", "l"}, {"L", "L"}, {"i", "i"}, {"j", "j"}, {"ldots", "..."},
    {"dots", "..."}, {"textellipsis", "..."}, {"textendash", "-"},
    {"textemdash", "-"}, {"TeX", "TeX"}, {"LaTeX", "LaTeX"},
    {"LaTeXe", "LaTeX2e"}, {"XeTeX", "XeTeX"}, {"BibTeX", "BibTeX"},
    {nullptr, nullptr}};

// Single-character accent escapes: \'e \"o \^o \`a \~n \=o \.z.
bool isAccentChar(char c) {
    return c == '\'' || c == '"' || c == '^' || c == '`' || c == '~' ||
           c == '=' || c == '.';
}

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

// Is this braced argument a piece of prose, or a machine argument that happens
// to sit in braces? Commands vary too much to tabulate, so judge the content:
// "Senior Research Engineer" is text, "0.5em", "l", "sec:intro",
// "https://example.com" and "GDM.png" are not.
bool looksLikeProse(const std::string &g) {
    if (g.empty()) return false;
    bool hasLetter = false, hasSpace = false, hasColon = false, hasDigit = false;
    bool hasCommand = false;
    for (std::size_t k = 0; k < g.size(); k++) {
        char c = g[k];
        if (isLetter(c)) hasLetter = true;
        else if (c == ' ' || c == '\n' || c == '\t') hasSpace = true;
        else if (c == ':') hasColon = true;
        else if (c >= '0' && c <= '9') hasDigit = true;
        else if (c == '\\' && k + 1 < g.size() && isLetter(g[k + 1]))
            hasCommand = true;
    }
    // Letters, or a span of digits with spaces in it: "2014 - 2019" is text a
    // reader would want to change, "2" is an argument.
    if (!hasLetter && !(hasDigit && hasSpace)) return false;
    // Labels, URLs and mailto: are one unbroken token. A sentence that merely
    // holds a link, "see \href{https://x.org}{here} for more", is prose; the
    // \href inside is judged on its own once the reader steps in.
    if (!hasSpace && (hasColon || g.find("://") != std::string::npos))
        return false;
    // A group of commands, \textbf{\href{...}{Name}}, is a wrapper whose
    // contents get the same judgement one level down. Stepping in is harmless:
    // machinery inside is still stepped over.
    if (hasCommand) return true;

    // A length: digits, then a unit, with nothing else.
    std::size_t k = 0;
    if (k < g.size() && (g[k] == '-' || g[k] == '+')) k++;
    std::size_t digits = k;
    while (k < g.size() && ((g[k] >= '0' && g[k] <= '9') || g[k] == '.')) k++;
    if (k > digits) {
        std::string unit = g.substr(k);
        static const char *const kUnits[] = {"pt", "em", "ex", "cm", "mm", "in",
                                             "bp", "sp", "mu", "px", "", nullptr};
        if (contains(kUnits, unit)) return false;
    }
    // A file name for an image or another source file.
    std::size_t dot = g.find_last_of('.');
    if (dot != std::string::npos && !hasSpace) {
        static const char *const kExts[] = {"png", "jpg", "jpeg", "pdf", "eps",
                                            "tex", "bib", "cls", "sty", nullptr};
        std::string ext = g.substr(dot + 1);
        for (char &c : ext) c = (char)std::tolower((unsigned char)c);
        if (contains(kExts, ext)) return false;
    }
    // A single letter is a column or alignment specifier, not a word.
    if (g.size() == 1) return false;
    return true;
}

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
        sp.endLine = sp.line + (int)std::count(s.begin() + (long)a,
                                               s.begin() + (long)b, '\n');
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
        if (name == "\\") { flush(cmdStart); skipOptionalArgs(); return; }

        // An escaped character (\&, \%, \_) stays part of the surrounding text,
        // and so does an accent, braced argument and all: r\'{e}sum\'{e}.
        if (name.size() == 1 && !isLetter(name[0]) && name != "[" &&
            name != "]" && name != "(" && name != ")") {
            if (runStart == kNpos) runStart = cmdStart;
            if (isAccentChar(name[0]) && i < n && s[i] == '{')
                group('{', '}', nullptr, nullptr);
            return;
        }

        // Characters spelled as commands (\ss, \o, \LaTeX) and letter accents
        // (\c{c}) are text, not markup: the word they sit in stays one run.
        if (contains(kTextSymbols, name) || contains(kLetterAccents, name)) {
            if (runStart == kNpos) runStart = cmdStart;
            if (i + 1 < n && s[i] == '{' && s[i + 1] == '}') i += 2;   // \ss{}
            else if (contains(kLetterAccents, name) && i < n && s[i] == '{')
                group('{', '}', nullptr, nullptr);
            return;
        }

        if (name == "maketitle") {
            doc.titleLines.push_back(LatexDoc::lineAt(s, cmdStart));
        }
        // A bare URL is printed as it is written, so it is editable text.
        if (name == "url") {
            flush(cmdStart);
            std::size_t a = 0, b = 0, save = i;
            skipBlanks();
            if (i < n && s[i] == '{' && group('{', '}', &a, &b))
                push(LatexSpanKind::Field, name, a, b);
            else
                i = save;
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
        if (contains(kSkipCommands, name)) {
            // Its arguments are machinery; step over them whole.
            skipOptionalArgs();
            skipBraceArgs(2);
            return;
        }
        // Everything else is looked through: \normalfont{...}, \raisebox{1em}{...},
        // \href{url}{text} and the author's own macros all wrap prose we want to
        // reach. Arguments that are clearly not prose are stepped over, and the
        // first one that is gets entered, so its text becomes a span of its own
        // (the matching '}' ends the run, in the main loop).
        skipOptionalArgs();
        for (int arg = 0; arg < 3; arg++) {
            std::size_t save = i;
            skipBlanks();
            if (!(i < n && s[i] == '{')) { i = save; return; }
            std::size_t a = 0, b = 0, open = i;
            group('{', '}', &a, &b);
            if (looksLikeProse(s.substr(a, b - a))) {
                i = open + 1;                  // step inside and read it as text
                return;
            }
            skipOptionalArgs();                // e.g. \makebox[8em][l]{...}
        }
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

// Would the PDF show the end of `a` and the start of `b` as one word? Only if
// nothing but markup separates them: foo\emph{bar}baz, not "foo \emph{bar}".
bool joinsAsWord(const std::string &s, const LatexSpan &a, const LatexSpan &b) {
    if (a.kind != LatexSpanKind::Text || b.kind != LatexSpanKind::Text) return false;
    if (a.end > b.start || a.listIndex != b.listIndex || a.itemIndex != b.itemIndex)
        return false;
    if (a.display.empty() || b.display.empty()) return false;
    auto wordChar = [](char c) {
        return std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80;
    };
    if (!wordChar(a.display.back()) || !wordChar(b.display.front())) return false;
    std::string gap = s.substr(a.end, b.start - a.end);
    for (char c : gap)
        if (isSpace(c) || c == '%' || c == '&' || c == '$') return false;
    for (const char *stop : {"\\\\", "\\item", "\\begin", "\\end", "\\par"})
        if (gap.find(stop) != kNpos) return false;
    return true;
}

// Braces in [a, b) open and close in order, so replacing the range cannot
// unbalance the document.
bool bracesBalanced(const std::string &s, std::size_t a, std::size_t b) {
    int depth = 0;
    for (std::size_t k = a; k < b && k < s.size(); k++) {
        if (s[k] == '\\') { k++; continue; }
        if (s[k] == '{') depth++;
        else if (s[k] == '}' && --depth < 0) return false;
    }
    return depth == 0;
}

// How much page text either side of a span is kept for telling spans apart.
const std::size_t kContext = 48;

// Give each span the keys of the text around it, taken from its neighbours in
// source order, so machinery (URLs, labels, comments) never gets in.
void buildContext(LatexDoc &doc) {
    std::vector<LatexSpan *> order;
    for (LatexSpan &sp : doc.spans) order.push_back(&sp);
    std::stable_sort(order.begin(), order.end(),
                     [](const LatexSpan *x, const LatexSpan *y) {
                         return x->start < y->start;
                     });
    std::vector<std::string> keys;
    for (const LatexSpan *sp : order) keys.push_back(LatexDoc::matchKey(sp->display));
    for (std::size_t k = 0; k < order.size(); k++) {
        std::string lead;
        for (std::size_t j = k; j > 0 && lead.size() < kContext; j--)
            lead = keys[j - 1] + lead;
        if (lead.size() > kContext) lead = lead.substr(lead.size() - kContext);
        std::string trail;
        for (std::size_t j = k + 1; j < order.size() && trail.size() < kContext; j++)
            trail += keys[j];
        if (trail.size() > kContext) trail.resize(kContext);
        order[k]->lead = lead;
        order[k]->trail = trail;
    }
}

void buildJoins(const std::string &s, LatexDoc &doc) {
    std::vector<const LatexSpan *> order;
    for (const LatexSpan &sp : doc.spans) order.push_back(&sp);
    std::stable_sort(order.begin(), order.end(),
                     [](const LatexSpan *x, const LatexSpan *y) {
                         return x->start < y->start;
                     });
    for (std::size_t k = 0; k < order.size();) {
        std::size_t last = k;
        while (last + 1 < order.size() && joinsAsWord(s, *order[last], *order[last + 1]))
            last++;
        if (last > k && bracesBalanced(s, order[k]->start, order[last]->end)) {
            LatexSpan j = *order[k];
            j.end = order[last]->end;
            j.endLine = order[last]->endLine;
            j.display = LatexDoc::displayText(s.substr(j.start, j.end - j.start));
            j.trail = order[last]->trail;
            doc.joins.push_back(j);
        }
        k = last + 1;
    }
}

}  // namespace

LatexDoc LatexDoc::parse(const std::string &src) {
    Parser p(src);
    p.run();
    buildContext(p.doc);
    buildJoins(src, p.doc);
    return p.doc;
}

std::vector<const LatexSpan *> LatexDoc::spansOnLine(int line) const {
    std::vector<const LatexSpan *> out;
    for (const LatexSpan &sp : spans)
        if (sp.line == line) out.push_back(&sp);
    return out;
}

namespace {

// One code point from UTF-8 at s[i], advancing i. A stray byte comes back as
// itself, so bad input never stalls the loop.
unsigned nextCodePoint(const std::string &s, std::size_t &i) {
    unsigned char c = (unsigned char)s[i];
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (c < 0x80 || i + extra >= s.size() || extra == 0) { i++; return c; }
    unsigned cp = c & (0x3F >> extra);
    for (int k = 1; k <= extra; k++) {
        unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) { i++; return c; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += extra + 1;
    return cp;
}

// The ASCII spelling of a Latin letter with a diacritic, or of a ligature the
// PDF reports as one glyph. Empty when there is none.
std::string asciiFold(unsigned cp) {
    static const char kLatin1[] =        // U+00C0..U+00FF, '?' = special/none
        "AAAAAA?CEEEEIIIIDNOOOOO?OUUUUY??aaaaaa?ceeeeiiiidnooooo?ouuuuy?y";
    static const char kExtendedA[] =     // U+0100..U+017F
        "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi??JjKkkLlLlLlLlLl"
        "NnNnNnnNnOoOoOo??RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    switch (cp) {
        case 0xC6: return "AE";  case 0xE6: return "ae";
        case 0xDE: return "TH";  case 0xFE: return "th";
        case 0xDF: return "ss";
        case 0x132: return "IJ"; case 0x133: return "ij";
        case 0x152: return "OE"; case 0x153: return "oe";
        case 0xFB00: return "ff";  case 0xFB01: return "fi";
        case 0xFB02: return "fl";  case 0xFB03: return "ffi";
        case 0xFB04: return "ffl"; case 0xFB05: case 0xFB06: return "st";
        default: break;
    }
    if (cp >= 0xC0 && cp <= 0xFF && kLatin1[cp - 0xC0] != '?')
        return std::string(1, kLatin1[cp - 0xC0]);
    if (cp >= 0x100 && cp <= 0x17F && kExtendedA[cp - 0x100] != '?')
        return std::string(1, kExtendedA[cp - 0x100]);
    return std::string();
}

// Punctuation and symbols outside ASCII: quotes, dashes, bullets, arrows.
// They never decide a match, so they are dropped like ASCII punctuation.
bool isSymbol(unsigned cp) {
    return (cp >= 0x80 && cp <= 0xBF) || cp == 0xD7 || cp == 0xF7 ||
           (cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x303F) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFFF0);
}

void appendUtf8(std::string &out, unsigned cp) {
    if (cp < 0x80) { out += (char)cp; return; }
    if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
    }
    out += (char)(0x80 | (cp & 0x3F));
}

// Does the span hold this key? A short key ("at", "is", "5pm") must be
// a whole word of the span, or it would be found inside nearly every span:
// "at" in "Math", "is" in "Visit". Math is the exception, since the PDF reads
// x^2 back as the one word "x2".
bool spanHolds(const LatexSpan &sp, const std::string &hay,
               const std::string &needle) {
    if (needle.size() <= 3 && sp.kind != LatexSpanKind::Math) {
        // The span is cut into pieces at spaces and punctuation, and the key
        // must be a run of whole pieces: the PDF splits "end-to-end" into
        // three words, but keeps "e.g" and "5~pm" whole.
        const std::string &d = sp.display;
        std::vector<std::string> pieces;
        std::size_t a = 0;
        for (std::size_t j = 0; j <= d.size(); j++) {
            bool cut = j == d.size() || ((unsigned char)d[j] < 0x80 &&
                                         !std::isalnum((unsigned char)d[j]));
            if (!cut) continue;
            if (j > a) pieces.push_back(LatexDoc::matchKey(d.substr(a, j - a)));
            a = j + 1;
        }
        for (std::size_t p = 0; p < pieces.size(); p++) {
            std::string run;
            for (std::size_t q = p; q < pieces.size() && run.size() < needle.size(); q++) {
                run += pieces[q];
                if (run == needle) return true;
            }
        }
        return false;
    }
    return hay.find(needle) != std::string::npos;
}

// The other way round: the click reported a whole line (or a word longer than
// any one span), and the span is inside it.
bool spanInside(const std::string &hay, const std::string &needle) {
    return hay.size() >= 4 && needle.size() > hay.size() &&
           needle.find(hay) != std::string::npos;
}

// What to look for, most faithful first. The PDF glues a footnote mark onto
// its word ("footnote1", and "1The" at the foot of the page), so when the
// whole key matches nothing, the letters alone get a second try.
std::vector<std::string> needlesFor(const std::string &word) {
    std::vector<std::string> out;
    std::string key = LatexDoc::matchKey(word);
    if (key.empty()) return out;
    out.push_back(key);
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    std::size_t a = 0, b = key.size();
    while (a < b && digit(key[a])) a++;
    while (b > a && digit(key[b - 1])) b--;
    if (b - a >= 3 && b - a < key.size()) out.push_back(key.substr(a, b - a));
    return out;
}

bool covers(const LatexSpan &sp, int line) {
    return sp.line <= line && line <= sp.endLine;
}

// How well the text around `needle` in this span agrees with the text the page
// shows around the clicked word: matching characters just before it plus just
// after it, at its best occurrence.
std::size_t contextScore(const LatexSpan &sp, const std::string &hay,
                         const std::string &needle, const std::string &before,
                         const std::string &after) {
    if (before.empty() && after.empty()) return 0;
    std::size_t best = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + 1)) {
        std::string left = sp.lead + hay.substr(0, p);
        std::string right = hay.substr(p + needle.size()) + sp.trail;
        std::size_t l = 0, r = 0;
        while (l < left.size() && l < before.size() &&
               left[left.size() - 1 - l] == before[before.size() - 1 - l])
            l++;
        while (r < right.size() && r < after.size() && right[r] == after[r]) r++;
        best = std::max(best, l + r);
    }
    return best;
}

}  // namespace

std::string LatexDoc::matchKey(const std::string &text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        unsigned cp = nextCodePoint(text, i);
        if (cp < 0x80) {
            if (std::isalnum((int)cp)) out += (char)std::tolower((int)cp);
            continue;
        }
        std::string folded = asciiFold(cp);
        if (!folded.empty()) {
            for (char c : folded) out += (char)std::tolower((unsigned char)c);
        } else if (!isSymbol(cp)) {
            appendUtf8(out, cp);       // a letter with no ASCII form: keep it
        }
    }
    return out;
}

const LatexSpan *LatexDoc::spanForClick(const std::vector<int> &lines,
                                        const std::string &word,
                                        const std::string &before,
                                        const std::string &after) const {
    // A paragraph's box is closed on a later line than its text, so look a
    // little above the reported line, and just below it, before giving up.
    static const int kNearby[] = {0, -1, -2, 1, -3};

    // Everything near the click, best placed first: spans covering a reported
    // line (or one just above it), the title block when the line is
    // \maketitle, and last the joined spans for words split by a font change.
    std::vector<const LatexSpan *> near;
    auto add = [&near](const LatexSpan *sp) {
        if (std::find(near.begin(), near.end(), sp) == near.end())
            near.push_back(sp);
    };
    auto isTitleLine = [this](int line) {
        return std::find(titleLines.begin(), titleLines.end(), line) !=
               titleLines.end();
    };
    for (int line : lines) {
        for (int delta : kNearby) {
            if (isTitleLine(line + delta))
                for (const LatexSpan &sp : spans)
                    if (sp.kind == LatexSpanKind::Field &&
                        (sp.command == "title" || sp.command == "author" ||
                         sp.command == "date" || sp.command == "subtitle" ||
                         sp.command == "thanks" || sp.command == "institute"))
                        add(&sp);
            for (const LatexSpan &sp : spans)
                if (covers(sp, line + delta)) add(&sp);
        }
    }
    for (int line : lines)
        for (int delta : kNearby)
            for (const LatexSpan &sp : joins)
                if (covers(sp, line + delta)) add(&sp);

    // A single letter or digit is refused outright. It is in almost every
    // span, so only the text around it could say which one, and it is often
    // text TeX made up (a page, section, item or footnote number) that has no
    // place in the source at all. Context cannot rule out landing on some
    // other span that happens to read the same, and the word beside it opens
    // the same span anyway, so refusing costs little. Characters, not bytes:
    // a Greek letter in math is one character too.
    std::vector<std::string> needles = needlesFor(word);
    if (!needles.empty()) {
        std::size_t chars = 0;
        for (char c : needles.front())
            if (((unsigned char)c & 0xC0) != 0x80) chars++;
        if (chars < 2) return nullptr;
    }
    std::vector<std::string> hays;
    for (const LatexSpan *sp : near) hays.push_back(matchKey(sp->display));
    std::string beforeKey = matchKey(before), afterKey = matchKey(after);
    if (beforeKey.size() > kContext)
        beforeKey = beforeKey.substr(beforeKey.size() - kContext);
    if (afterKey.size() > kContext) afterKey.resize(kContext);

    for (const std::string &needle : needles) {
        // Every nearby span holding the word is a candidate; the one whose
        // surroundings read like the page's wins, and the nearest breaks ties.
        // Context has to win clearly to overrule the nearer span, because the
        // page's reading order is not always the source's: a table reads
        // back column by column.
        static const std::size_t kMargin = 4;
        const LatexSpan *pick = nullptr;
        std::size_t pickScore = 0;
        for (std::size_t k = 0; k < near.size(); k++) {
            if (!spanHolds(*near[k], hays[k], needle)) continue;
            std::size_t score = contextScore(*near[k], hays[k], needle,
                                             beforeKey, afterKey);
            if (!pick || score >= pickScore + kMargin) {
                pick = near[k];
                pickScore = score;
            }
        }
        if (pick) return pick;
        // A line of text holds several spans; the longest is the likeliest
        // to be the one under the pointer, and a short one ("and", "words")
        // is found inside nearly any line.
        const LatexSpan *best = nullptr;
        std::size_t bestLen = 0;
        for (std::size_t k = 0; k < near.size(); k++)
            if (spanInside(hays[k], needle) && hays[k].size() > bestLen) {
                best = near[k];
                bestLen = hays[k].size();
            }
        if (best) return best;
    }

    // Nothing matched what was clicked. With a real word to go on, refuse
    // rather than offer the wrong text; with no letters or digits at all (a
    // logo, a glyph the PDF cannot name), the nearest span is the best guess.
    if (!needles.empty()) return nullptr;
    return near.empty() ? nullptr : near.front();
}

int LatexDoc::lineAt(const std::string &src, std::size_t offset) {
    int line = 1;
    for (std::size_t k = 0; k < offset && k < src.size(); k++)
        if (src[k] == '\n') line++;
    return line;
}

namespace {

// Greek letters in math read back from the PDF as themselves.
struct Greek { const char *name; unsigned cp; };
const Greek kGreek[] = {
    {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4},
    {"epsilon", 0x3F5}, {"varepsilon", 0x3B5}, {"zeta", 0x3B6}, {"eta", 0x3B7},
    {"theta", 0x3B8}, {"vartheta", 0x3D1}, {"iota", 0x3B9}, {"kappa", 0x3BA},
    {"lambda", 0x3BB}, {"mu", 0x3BC}, {"nu", 0x3BD}, {"xi", 0x3BE},
    {"pi", 0x3C0}, {"rho", 0x3C1}, {"sigma", 0x3C3}, {"tau", 0x3C4},
    {"upsilon", 0x3C5}, {"phi", 0x3D5}, {"varphi", 0x3C6}, {"chi", 0x3C7},
    {"psi", 0x3C8}, {"omega", 0x3C9}, {"Gamma", 0x393}, {"Delta", 0x394},
    {"Theta", 0x398}, {"Lambda", 0x39B}, {"Xi", 0x39E}, {"Pi", 0x3A0},
    {"Sigma", 0x3A3}, {"Phi", 0x3A6}, {"Psi", 0x3A8}, {"Omega", 0x3A9},
    {nullptr, 0}};

}  // namespace

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
            if (!isLetter(next)) {
                i += 2;
                if (next == '\\' || next == ',' || next == ';' || next == ':' ||
                    next == '!' || next == ' ')
                    pendingSpace = true;           // a break or a small space
                else if (isAccentChar(next) || next == '-' || next == '/')
                    ;                              // \'e, \- and \/ print no mark
                else
                    emit(next);                    // \& \% \_ \# \$ \{ \}
                continue;
            }
            i++;
            std::string name;
            while (i < n && isLetter(tex[i])) name += tex[i++];
            if (i < n && tex[i] == '*') i++;
            // A character spelled as a command reads as that character, and
            // like any control word it swallows the spaces after it.
            const char *symbol = nullptr;
            for (const SymbolText *t = kSymbolText; t->name; ++t)
                if (name == t->name) symbol = t->text;
            unsigned greek = 0;
            for (const Greek *g = kGreek; g->name; ++g)
                if (name == g->name) greek = g->cp;
            if (symbol || greek || contains(kLetterAccents, name)) {
                if (symbol) for (const char *p = symbol; *p; ++p) emit(*p);
                if (greek) {
                    std::string u;
                    appendUtf8(u, greek);
                    for (char ch : u) emit(ch);
                }
                while (i < n && isBlank(tex[i])) i++;
                if (i + 1 < n && tex[i] == '{' && tex[i + 1] == '}') i += 2;
                continue;
            }
            pendingSpace = true;               // a command: drop name, keep args
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
