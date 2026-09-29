// MathTex.cpp — see MathTex.h.
//
// The batch is one LaTeX document with each formula set in a box of its own
// (\setbox, never a macro argument, so a formula is read as it would be in
// a document), measured, and shipped out as a page exactly that size plus
// a little pad. The log gets a line as TeX begins each formula and one with
// the box's size as it ships it, so a formula's errors, page and baseline
// are all found by reading the log in order. tectonic runs with
// -Z continue-on-errors, so one bad formula does not stop the others; the
// ones check() refuses never reach TeX, because they could desynchronise
// everything after them.
#include "MathTex.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace MathTex {

const int kVersion = 1;

namespace {

bool isLetter(char c) { return std::isalpha((unsigned char)c) != 0; }

// Whether the control word `name` (with its backslash) starts at `i`: the
// name, then something that is not a letter.
bool wordAt(const std::string& s, size_t i, const char* name) {
    const size_t n = std::strlen(name);
    return s.compare(i, n, name) == 0 && (i + n >= s.size() || !isLetter(s[i + n]));
}

// `s` with every control word `from` replaced by `to`.
std::string renameWord(const std::string& s, const char* from, const char* to) {
    std::string out;
    const size_t n = std::strlen(from);
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\\' && wordAt(s, i, from)) {
            out += to;
            i += n;
        } else if (s[i] == '\\' && i + 1 < s.size()) {
            out += s.substr(i, 2);
            i += 2;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// The index just past the braced group opening at `open` ('{'), or npos.
size_t groupEnd(const std::string& s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        if (s[i] == '\\') { i++; continue; }
        if (s[i] == '{') depth++;
        else if (s[i] == '}' && --depth == 0) return i + 1;
    }
    return std::string::npos;
}

// `s` with each `\name{...}` (and `\name*{...}` when `star`) replaced by
// what `with` makes of the group's inside and whether it had the star.
template <class F>
std::string replaceCommand(const std::string& s, const char* name, F with) {
    std::string out;
    const size_t n = std::strlen(name);
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\\' && wordAt(s, i, name)) {
            size_t k = i + n;
            const bool starred = k < s.size() && s[k] == '*';
            if (starred) k++;
            while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) k++;
            const size_t end = k < s.size() && s[k] == '{' ? groupEnd(s, k) : std::string::npos;
            if (end != std::string::npos) {
                out += with(s.substr(k + 1, end - k - 2), starred);
                i = end;
                continue;
            }
        }
        if (s[i] == '\\' && i + 1 < s.size()) {
            out += s.substr(i, 2);
            i += 2;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// Environments that need a display of their own in LaTeX, and what stands
// for them inside $...$ ("" to drop the environment and keep its inside).
const struct { const char* env; const char* inlineEnv; } kEnvironments[] = {
    {"align", "aligned"},       {"align*", "aligned"},     {"flalign", "aligned"},
    {"flalign*", "aligned"},    {"eqnarray", "aligned"},   {"eqnarray*", "aligned"},
    {"alignat", "alignedat"},   {"alignat*", "alignedat"}, {"gather", "gathered"},
    {"gather*", "gathered"},    {"multline", "multlined"}, {"multline*", "multlined"},
    {"equation", ""},           {"equation*", ""},         {"displaymath", ""},
};

std::string renameEnvironments(const std::string& s) {
    std::string out = s;
    for (const auto& e : kEnvironments) {
        for (const char* side : {"\\begin{", "\\end{"}) {
            const std::string from = std::string(side) + e.env + "}";
            const std::string to = *e.inlineEnv ? std::string(side) + e.inlineEnv + "}" : "";
            for (size_t at = out.find(from); at != std::string::npos; at = out.find(from, at)) {
                out.replace(at, from.size(), to);
                at += to.size();
            }
        }
    }
    return out;
}

// Characters people type into formulas that Computer Modern has no glyph
// for in a LaTeX document, with the TeX that draws them.
const struct { uint32_t cp; const char* tex; } kUnicode[] = {
    {0x391, "A"}, {0x392, "B"}, {0x393, "\\Gamma"}, {0x394, "\\Delta"}, {0x395, "E"},
    {0x396, "Z"}, {0x397, "H"}, {0x398, "\\Theta"}, {0x399, "I"}, {0x39A, "K"},
    {0x39B, "\\Lambda"}, {0x39C, "M"}, {0x39D, "N"}, {0x39E, "\\Xi"}, {0x39F, "O"},
    {0x3A0, "\\Pi"}, {0x3A1, "P"}, {0x3A3, "\\Sigma"}, {0x3A4, "T"}, {0x3A5, "\\Upsilon"},
    {0x3A6, "\\Phi"}, {0x3A7, "X"}, {0x3A8, "\\Psi"}, {0x3A9, "\\Omega"},
    {0x3B1, "\\alpha"}, {0x3B2, "\\beta"}, {0x3B3, "\\gamma"}, {0x3B4, "\\delta"},
    {0x3B5, "\\varepsilon"}, {0x3B6, "\\zeta"}, {0x3B7, "\\eta"}, {0x3B8, "\\theta"},
    {0x3B9, "\\iota"}, {0x3BA, "\\kappa"}, {0x3BB, "\\lambda"}, {0x3BC, "\\mu"},
    {0x3BD, "\\nu"}, {0x3BE, "\\xi"}, {0x3BF, "o"}, {0x3C0, "\\pi"}, {0x3C1, "\\rho"},
    {0x3C2, "\\varsigma"}, {0x3C3, "\\sigma"}, {0x3C4, "\\tau"}, {0x3C5, "\\upsilon"},
    {0x3C6, "\\varphi"}, {0x3C7, "\\chi"}, {0x3C8, "\\psi"}, {0x3C9, "\\omega"},
    {0x3D1, "\\vartheta"}, {0x3D5, "\\phi"}, {0x3F5, "\\epsilon"},
    {0xB1, "\\pm"}, {0xB7, "\\cdot"}, {0xD7, "\\times"}, {0xF7, "\\div"}, {0xAC, "\\neg"},
    {0x2032, "\\prime"}, {0x2026, "\\ldots"}, {0x22EF, "\\cdots"}, {0x2016, "\\Vert"},
    {0x2190, "\\leftarrow"}, {0x2192, "\\rightarrow"}, {0x2194, "\\leftrightarrow"},
    {0x21D0, "\\Leftarrow"}, {0x21D2, "\\Rightarrow"}, {0x21D4, "\\Leftrightarrow"},
    {0x21A6, "\\mapsto"}, {0x2200, "\\forall"}, {0x2202, "\\partial"}, {0x2203, "\\exists"},
    {0x2205, "\\emptyset"}, {0x2207, "\\nabla"}, {0x2208, "\\in"}, {0x2209, "\\notin"},
    {0x220F, "\\prod"}, {0x2211, "\\sum"}, {0x2212, "-"}, {0x2213, "\\mp"},
    {0x2218, "\\circ"}, {0x221A, "\\surd"}, {0x221D, "\\propto"}, {0x221E, "\\infty"},
    {0x2227, "\\wedge"}, {0x2228, "\\vee"}, {0x2229, "\\cap"}, {0x222A, "\\cup"},
    {0x222B, "\\int"}, {0x223C, "\\sim"}, {0x2243, "\\simeq"}, {0x2245, "\\cong"},
    {0x2248, "\\approx"}, {0x2260, "\\neq"}, {0x2261, "\\equiv"}, {0x2264, "\\leq"},
    {0x2265, "\\geq"}, {0x226A, "\\ll"}, {0x226B, "\\gg"}, {0x2282, "\\subset"},
    {0x2283, "\\supset"}, {0x2286, "\\subseteq"}, {0x2287, "\\supseteq"},
    {0x2295, "\\oplus"}, {0x2297, "\\otimes"}, {0x2299, "\\odot"}, {0x22A4, "\\top"},
    {0x22A5, "\\perp"}, {0x27E8, "\\langle"}, {0x27E9, "\\rangle"},
    {0x2102, "\\mathbb{C}"}, {0x2115, "\\mathbb{N}"}, {0x211A, "\\mathbb{Q}"},
    {0x211D, "\\mathbb{R}"}, {0x2124, "\\mathbb{Z}"},
};

// Commands whose braced argument is text, not math.
const char* const kTextCommands[] = {"\\text", "\\textrm", "\\textit", "\\textbf", "\\textsf",
                                     "\\texttt", "\\mbox", "\\hbox", "\\textnormal"};

// `s` with the characters of kUnicode spelled in TeX: in math as the
// command and a space (so x_α is x_\alpha, one token), in text inside
// \ensuremath{}.
std::string spellUnicode(const std::string& s) {
    std::string out;
    std::vector<bool> groups;   // open braces: true for a text argument
    bool textNext = false;      // the next { opens a text argument
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            textNext = false;
            for (const char* w : kTextCommands)
                if (wordAt(s, i, w)) textNext = true;
            size_t e = i + 1;
            if (e < s.size() && isLetter(s[e])) {
                while (e < s.size() && isLetter(s[e])) e++;
            } else if (e < s.size()) {
                e++;
            }
            out += s.substr(i, e - i);
            i = e;
            continue;
        }
        if (c == '{') {
            groups.push_back(textNext);
            textNext = false;
        } else if (c == '}') {
            if (!groups.empty()) groups.pop_back();
        } else if (c != ' ' && c != '*') {
            textNext = false;
        }
        if (c < 0x80) {
            out += s[i++];
            continue;
        }
        size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + n > s.size()) n = 1;
        uint32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
        for (size_t k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        const char* tex = nullptr;
        for (const auto& u : kUnicode)
            if (u.cp == cp) tex = u.tex;
        bool inText = false;
        for (bool g : groups) inText = inText || g;
        if (!tex) out += s.substr(i, n);
        else if (inText) out += std::string("\\ensuremath{") + tex + "}";
        else out += std::string(tex) + (tex[0] == '\\' ? " " : "");
        i += n;
    }
    return out;
}

// Control words a formula has no business with, which could end the batch
// early or write outside a page.
const char* const kRefused[] = {
    "\\input", "\\include", "\\endinput", "\\documentclass", "\\usepackage",
    "\\shipout", "\\output", "\\openout", "\\write", "\\immediate", "\\end{document}",
    "\\begin{document}", "\\newpage", "\\clearpage",
};

}  // namespace

std::string check(const std::string& tex) {
    int depth = 0;
    for (size_t i = 0; i < tex.size(); i++) {
        const char c = tex[i];
        if (c == '\\') {
            for (const char* word : kRefused) {
                const bool env = std::strchr(word, '{') != nullptr;
                if (env ? tex.compare(i, std::strlen(word), word) == 0 : wordAt(tex, i, word))
                    return std::string(word) + " is not allowed in a formula";
            }
            i++;   // the escaped character, or the control word's first letter
            continue;
        }
        if (c == '%') {
            while (i < tex.size() && tex[i] != '\n') i++;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}' && --depth < 0) return "a } with no { before it";
        else if (c == '$' && depth == 0) return "a $ inside the formula";
    }
    if (depth > 0) return "a { that is never closed";
    return "";
}

std::string prepare(const std::string& tex, bool display) {
    std::string s = renameWord(spellUnicode(tex), "\\mathbbm", "\\mathds");
    // \mathbb{1} and \mathbb 1: amssymb's blackboard font has no digits.
    for (size_t at = s.find("\\mathbb"); at != std::string::npos; at = s.find("\\mathbb", at + 1)) {
        if (!wordAt(s, at, "\\mathbb")) continue;
        size_t k = at + 7;
        while (k < s.size() && s[k] == ' ') k++;
        if (s.compare(k, 3, "{1}") == 0 || (k < s.size() && s[k] == '1' && k > at + 7))
            s.replace(at, 7, "\\mathds");
    }
    s = renameEnvironments(s);
    s = replaceCommand(s, "\\label", [](const std::string&, bool) { return std::string(); });
    if (display)
        s = replaceCommand(s, "\\tag", [](const std::string& inside, bool starred) {
            return "\\qquad\\text{" + (starred ? inside : "(" + inside + ")") + "}";
        });
    s = renameWord(s, "\\nonumber", "");
    s = renameWord(s, "\\notag", "");
    // Blank lines out: in math, one is an error.
    std::string out;
    for (size_t a = 0; a < s.size();) {
        size_t e = s.find('\n', a);
        if (e == std::string::npos) e = s.size();
        const std::string line = s.substr(a, e - a);
        if (line.find_first_not_of(" \t\r") != std::string::npos) {
            if (!out.empty()) out += '\n';
            out += line;
        }
        a = e + 1;
    }
    return out;
}

std::string document(const std::vector<Formula>& formulas) {
    char pad[32];
    std::snprintf(pad, sizeof pad, "%.2fpt", kPadPt);
    std::string d =
        "\\documentclass[10pt]{article}\n"
        "\\usepackage{amsmath,amssymb,amsfonts,mathtools,bm,dsfont,mathrsfs,cancel,xcolor}\n"
        "\\hoffset=-1in \\voffset=-1in \\topmargin=0pt \\headheight=0pt \\headsep=0pt\n"
        "\\oddsidemargin=0pt \\evensidemargin=0pt \\parindent=0pt \\pagestyle{empty}\n"
        // Pages come only from \mcship; whatever an error leaves on the page
        // is thrown away, so the page numbers stay the formulas'.
        "\\output={\\setbox0\\box255 \\deadcycles=0 }\n"
        // What MathJax and KaTeX know and LaTeX does not, where LaTeX has
        // nothing of that name already.
        "\\providecommand{\\R}{\\mathbb{R}}\\providecommand{\\N}{\\mathbb{N}}\n"
        "\\providecommand{\\Z}{\\mathbb{Z}}\\providecommand{\\Q}{\\mathbb{Q}}\n"
        "\\providecommand{\\C}{\\mathbb{C}}\\providecommand{\\Bbb}{\\mathbb}\n"
        "\\providecommand{\\bold}{\\mathbf}\\providecommand{\\lt}{<}\\providecommand{\\gt}{>}\n"
        "\\providecommand{\\argmax}{\\operatorname*{arg\\,max}}\n"
        "\\providecommand{\\argmin}{\\operatorname*{arg\\,min}}\n"
        "\\providecommand{\\sgn}{\\operatorname{sgn}}\\providecommand{\\tr}{\\operatorname{tr}}\n"
        "\\providecommand{\\diag}{\\operatorname{diag}}\n"
        "\\providecommand{\\abs}[1]{\\left\\lvert #1\\right\\rvert}\n"
        "\\providecommand{\\norm}[1]{\\left\\lVert #1\\right\\rVert}\n"
        "\\newbox\\mcbox \\newcount\\mcpage \\newdimen\\mcpad \\mcpad=";
    d += pad;
    d += "\n"
         "\\def\\mcship#1{%\n"
         "  \\pdfpagewidth=\\dimexpr\\wd\\mcbox+2\\mcpad\\relax\n"
         "  \\pdfpageheight=\\dimexpr\\ht\\mcbox+\\dp\\mcbox+2\\mcpad\\relax\n"
         "  \\global\\advance\\mcpage by 1\n"
         "  \\typeout{MC:#1:\\the\\mcpage:\\the\\wd\\mcbox:\\the\\ht\\mcbox:\\the\\dp\\mcbox}%\n"
         "  \\shipout\\vbox{\\kern\\mcpad\\hbox{\\kern\\mcpad\\box\\mcbox}}}\n"
         "\\begin{document}\n";
    for (size_t k = 0; k < formulas.size(); k++) {
        const std::string n = std::to_string(k);
        d += "\\typeout{MC:BEGIN:" + n + "}\n";
        // The TeX on lines of its own: a % comment in it then ends at its
        // own line, not at the closing $.
        d += formulas[k].display ? "\\setbox\\mcbox\\hbox{$\\displaystyle\n" : "\\setbox\\mcbox\\hbox{$\n";
        d += prepare(formulas[k].tex, formulas[k].display);
        d += "\n$}\n\\mcship{" + n + "}\n";
    }
    d += "\\typeout{MC:END}\n\\end{document}\n";
    return d;
}

std::vector<Box> readLog(const std::string& log, size_t count) {
    std::vector<Box> out(count);
    long cur = -1;
    size_t a = 0;
    while (a < log.size()) {
        size_t e = log.find('\n', a);
        if (e == std::string::npos) e = log.size();
        std::string line = log.substr(a, e - a);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        a = e + 1;
        if (line.compare(0, 3, "MC:") == 0) {
            const std::string rest = line.substr(3);
            if (rest.compare(0, 6, "BEGIN:") == 0) {
                const unsigned long k = std::strtoul(rest.c_str() + 6, nullptr, 10);
                cur = k < count ? (long)k : -1;
                if (cur >= 0) out[(size_t)cur].reached = true;
            } else if (rest == "END") {
                cur = -1;
            } else {
                unsigned long k = 0;
                int page = 0;
                double w = 0, h = 0, dp = 0;
                if (std::sscanf(rest.c_str(), "%lu:%d:%lfpt:%lfpt:%lfpt", &k, &page, &w, &h, &dp) ==
                        5 && k < count) {
                    Box& b = out[k];
                    b.measured = true;
                    b.page = page;
                    b.width = w;
                    b.height = h;
                    b.depth = dp;
                }
            }
            continue;
        }
        if (line.size() > 2 && line[0] == '!' && line[1] == ' ' && cur >= 0 &&
            out[(size_t)cur].error.empty()) {
            std::string msg = line.substr(2);
            while (!msg.empty() && (msg.back() == '.' || msg.back() == ' ')) msg.pop_back();
            if (msg.compare(0, 13, "LaTeX Error: ") == 0) msg = msg.substr(13);
            if (msg == "Undefined control sequence") {
                // The context line after it ends with the culprit.
                size_t b = a;
                for (int tries = 0; tries < 3 && b < log.size(); tries++) {
                    size_t f = log.find('\n', b);
                    if (f == std::string::npos) f = log.size();
                    const std::string ctx = log.substr(b, f - b);
                    b = f + 1;
                    if (ctx.find_first_not_of(' ') == std::string::npos) continue;
                    size_t slash = std::string::npos;
                    for (size_t i = 0; i < ctx.size(); i++) {
                        if (ctx[i] != '\\') continue;
                        slash = i;
                        if (i + 1 < ctx.size() && !isLetter(ctx[i + 1])) i++;
                    }
                    if (slash != std::string::npos) {
                        size_t end = slash + 1;
                        while (end < ctx.size() && (isLetter(ctx[end]) || ctx[end] == '@')) end++;
                        if (end == slash + 1 && end < ctx.size()) end++;
                        msg += ": " + ctx.substr(slash, end - slash);
                    }
                    break;
                }
            }
            out[(size_t)cur].error = msg;
        }
    }
    return out;
}

std::string firstError(const std::string& output) {
    for (size_t a = 0; a < output.size();) {
        size_t e = output.find('\n', a);
        if (e == std::string::npos) e = output.size();
        const std::string line = output.substr(a, e - a);
        a = e + 1;
        if (line.compare(0, 7, "error: ") == 0) return line.substr(7);
    }
    return "";
}

}  // namespace MathTex
