// MarkdownHtml.cpp — see MarkdownHtml.h.
#include "MarkdownHtml.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace MarkdownHtml {

std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

namespace {

// Whether an address may go into the page is decided on the address as a
// browser will read it, never on the bytes as written: a browser's URL
// parser (WHATWG URL, the basic URL parser) strips leading and trailing C0
// controls and spaces and removes every tab and line break, so "\tjava
// script:x" is a javascript: link. Each reading below is taken for its
// scheme, and every one must be on the allow list.

// `url` as a browser's URL parser reads it.
std::string browserForm(const std::string& url) {
    size_t a = 0, b = url.size();
    while (a < b && (unsigned char)url[a] <= 0x20) a++;
    while (b > a && (unsigned char)url[b - 1] <= 0x20) b--;
    std::string out;
    out.reserve(b - a);
    for (size_t k = a; k < b; k++)
        if (url[k] != '\t' && url[k] != '\n' && url[k] != '\r') out += url[k];
    return out;
}

// Stricter than any browser: every control, space and DEL removed.
std::string squeezed(const std::string& url) {
    std::string out;
    out.reserve(url.size());
    for (char c : url)
        if ((unsigned char)c > 0x20 && (unsigned char)c != 0x7F) out += c;
    return out;
}

// Character references decoded (&#9; &#x3A; &Tab; &NewLine; &colon; and
// the like), in case one the parser leaves alone is decoded by whatever
// reads the page next. The page itself escapes every '&', so a browser
// sees the parser's text as it is; this is a second reading, not how the
// page is written.
std::string decodedReferences(const std::string& url) {
    static const struct { const char* name; char c; } kNamed[] = {
        {"tab", '\t'},   {"newline", '\n'}, {"colon", ':'}, {"nbsp", ' '},
        {"sol", '/'},    {"num", '#'},      {"quest", '?'}, {"period", '.'},
        {"plus", '+'},   {"hyphen", '-'},   {"dash", '-'},  {"amp", '&'},
        {"lpar", '('},   {"rpar", ')'}};
    std::string out;
    out.reserve(url.size());
    for (size_t i = 0; i < url.size();) {
        const size_t semi = url[i] == '&' ? url.find(';', i + 1) : std::string::npos;
        if (semi == std::string::npos || semi - i > 34) {
            out += url[i++];
            continue;
        }
        std::string ref = url.substr(i + 1, semi - i - 1);
        for (char& ch : ref) ch = (char)std::tolower((unsigned char)ch);
        long cp = -1;
        if (ref.size() > 1 && ref[0] == '#') {
            const bool hex = ref[1] == 'x';
            const std::string digits = ref.substr(hex ? 2 : 1);
            if (!digits.empty() && digits.size() <= 7 &&
                digits.find_first_not_of(hex ? "0123456789abcdef" : "0123456789") ==
                    std::string::npos)
                cp = std::stol(digits, nullptr, hex ? 16 : 10);
        } else {
            for (const auto& n : kNamed)
                if (ref == n.name) cp = (unsigned char)n.c;
        }
        if (cp < 0) {
            out += url[i++];
            continue;
        }
        // Only ASCII can change how a scheme reads; anything else stands as
        // a control, which no scheme holds.
        out += cp < 0x80 ? (char)cp : '\x01';
        i = semi + 1;
    }
    return out;
}

// The scheme of an address ("https" for https://x), lowercased, or "" for a
// relative one, as a browser's URL parser decides it: an ASCII letter, then
// letters, digits, + - and ., up to a colon; anything else before the
// colon makes the address relative. A colon first is refused as before.
std::string schemeOf(const std::string& url) {
    if (!url.empty() && url[0] == ':') return "?";
    if (url.empty() || !std::isalpha((unsigned char)url[0])) return "";
    for (size_t k = 1; k < url.size(); k++) {
        const char c = url[k];
        if (c == ':') {
            std::string s = url.substr(0, k);
            for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
            return s;
        }
        if (!std::isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.') return "";
    }
    return "";
}

// Every reading of `url`: as a browser reads it, with nothing but its
// visible characters, and both again with character references decoded.
std::vector<std::string> readings(const std::string& url) {
    const std::string decoded = decodedReferences(url);
    return {browserForm(url), squeezed(url), browserForm(decoded), squeezed(decoded)};
}

bool linkAllowed(const std::string& url) {
    for (const std::string& form : readings(url)) {
        const std::string s = schemeOf(form);
        if (!(s.empty() || s == "http" || s == "https" || s == "mailto")) return false;
    }
    return true;
}

bool pictureAllowed(const std::string& src) {
    for (const std::string& form : readings(src)) {
        const std::string s = schemeOf(form);
        if (s.empty() || s == "https") continue;
        if (s != "data") return false;
        std::string head = form.substr(0, std::min<size_t>(form.size(), 11));
        for (char& ch : head) ch = (char)std::tolower((unsigned char)ch);
        if (head != "data:image/") return false;
    }
    return true;
}

// The end of a paragraph, heading, item or line of display math: a "\n"
// run that is neither the space between blocks nor a line break inside a
// paragraph.
bool endsUnit(const MdRun& r) {
    return r.text == "\n" && !r.gap && !r.hardBreak && !r.codeBlock && r.tableId == 0;
}

// What a unit sits in, outermost first: a list (and its item) per level,
// with a quote's levels between the list levels outside it and inside it.
struct Box {
    enum Kind { List, Item, Quote } kind;
    bool ordered = false;
    char delim = 0;   // an ordered list's "." or ")": another one starts another list
};

std::vector<Box> pathFor(const MdRun& r) {
    std::vector<Box> path;
    const int outside = r.quote ? r.quoteIndent : 0;
    const int depth = r.quote ? std::max(1, r.quoteDepth) : 0;
    const int levels = std::max(0, r.listDepth - outside);
    for (int k = 0; k < outside; k++) {
        path.push_back({Box::List, false});
        path.push_back({Box::Item, false});
    }
    for (int k = 0; k < depth; k++) path.push_back({Box::Quote, false});
    for (int k = 0; k < levels; k++) {
        path.push_back({Box::List, false});
        path.push_back({Box::Item, false});
    }
    // The list a marker starts an item in is ordered or not as the item is.
    if (r.marker && path.size() >= 2) {
        Box& list = path[path.size() - 2];
        list.ordered = r.ordered;
        const size_t d = r.ordered ? r.text.find_first_of(".)") : std::string::npos;
        if (d != std::string::npos) list.delim = r.text[d];
    }
    return path;
}

class Writer {
public:
    explicit Writer(const std::vector<MdRun>& runs) : runs_(runs) {}

    std::string body() {
        size_t i = 0;
        while (i < runs_.size()) {
            const MdRun& r = runs_[i];
            if (r.gap) {   // the space between two blocks: the end of a quote it is not in
                leaveQuotes(r.quote ? std::max(1, r.quoteDepth) : 0);
                i++;
                continue;
            }
            if (r.footnotes && !inNotes_) {
                closeTo(0);
                out_ += "<section class=\"footnotes\">\n";
                inNotes_ = true;
            }
            if (r.tableId > 0) {
                size_t end = i;
                while (end < runs_.size() && runs_[end].tableId == r.tableId) end++;
                enter(r, false);
                table(i, end);
                i = end;
                continue;
            }
            if (r.codeBlock) {
                size_t end = i;
                while (end < runs_.size() && runs_[end].codeBlock) end++;
                enter(r, false);
                code(i, end);
                i = end;
                continue;
            }
            if (r.rule) {
                closeTo(0);
                out_ += "<hr>\n";
                i++;
                continue;
            }
            size_t end = i;
            while (end < runs_.size() && !endsUnit(runs_[end]) && !runs_[end].gap &&
                   !runs_[end].codeBlock && runs_[end].tableId == 0)
                end++;
            unit(i, end);
            i = end < runs_.size() && endsUnit(runs_[end]) ? end + 1 : end;
        }
        closeTo(0);
        if (inNotes_) out_ += "</section>\n";
        return out_;
    }

private:
    const std::vector<MdRun>& runs_;
    std::string out_;
    std::vector<Box> open_;
    bool inNotes_ = false;
    std::set<std::string> anchors_;   // the heading anchors given

    void closeTo(size_t keep) {
        while (open_.size() > keep) {
            const Box b = open_.back();
            open_.pop_back();
            if (b.kind == Box::Item) out_ += "</li>\n";
            else if (b.kind == Box::Quote) out_ += "</blockquote>\n";
            else out_ += b.ordered ? "</ol>\n" : "</ul>\n";
        }
    }

    // Closes the quotes past the first `depth` (and what is inside them).
    void leaveQuotes(int depth) {
        int seen = 0;
        for (size_t k = 0; k < open_.size(); k++)
            if (open_[k].kind == Box::Quote && ++seen > depth) {
                closeTo(k);
                return;
            }
    }

    // Puts the writer inside the boxes `r` sits in, a new item for a marker.
    void enter(const MdRun& r, bool newItem, long start = 1, const std::string& itemId = "",
               bool task = false) {
        std::vector<Box> path = pathFor(r);
        size_t keep = 0;
        while (keep < open_.size() && keep < path.size()) {
            const Box& a = open_[keep];
            const Box& b = path[keep];
            if (a.kind != b.kind) break;
            const bool last = newItem && keep + 2 >= path.size();
            if (last && b.kind == Box::Item) break;                       // a new item
            if (last && b.kind == Box::List && (a.ordered != b.ordered || a.delim != b.delim))
                break;   // another list
            keep++;
        }
        closeTo(keep);
        for (size_t k = keep; k < path.size(); k++) {
            Box b = path[k];
            if (b.kind == Box::Quote) {
                out_ += "<blockquote>\n";
            } else if (b.kind == Box::List) {
                const bool fresh = newItem && k + 2 == path.size();
                if (b.ordered) {
                    out_ += fresh && start != 1 ? "<ol start=\"" + std::to_string(start) + "\">\n"
                                                : "<ol>\n";
                } else {
                    out_ += task && fresh ? "<ul class=\"contains-task-list\">\n" : "<ul>\n";
                }
            } else {
                const bool fresh = newItem && k + 1 == path.size();
                std::string li = "<li";
                if (fresh && !itemId.empty()) li += " id=\"" + escape(itemId) + "\"";
                if (fresh && task) li += " class=\"task-list-item\"";
                out_ += li + ">";
            }
            open_.push_back(b);
        }
    }

    void unit(size_t from, size_t to) {
        if (from >= to) return;
        const MdRun& r = runs_[from];
        if (r.marker) {
            // "  3. " or "  ☐ " or "  3. ☐ ": the number an ordered list starts at.
            long start = 1;
            const size_t digit = r.text.find_first_of("0123456789");
            if (r.ordered && digit != std::string::npos) {
                start = 0;
                for (size_t k = digit; k < r.text.size() && std::isdigit((unsigned char)r.text[k]) &&
                                       start < 1000000000;
                     k++)
                    start = start * 10 + (r.text[k] - '0');
            }
            enter(r, true, start, r.anchor, r.task != 0);
            if (r.task)
                out_ += r.task == 2 ? "<input type=\"checkbox\" disabled checked> "
                                    : "<input type=\"checkbox\" disabled> ";
            inline_(from + 1, to);
            return;
        }
        enter(r, false);
        if (r.heading > 0) {
            std::string title;
            for (size_t k = from; k < to; k++) title += runs_[k].text;
            // GitHub numbers repeats: intro, intro-1, intro-2.
            const std::string slug = MarkdownParser::anchor(title);
            std::string id = slug;
            for (int n = 1; !slug.empty() && anchors_.count(id); n++)
                id = slug + "-" + std::to_string(n);
            if (!id.empty()) anchors_.insert(id);
            const std::string h = std::to_string(std::min(6, r.heading));
            out_ += "<h" + h + (id.empty() ? "" : " id=\"" + escape(id) + "\"") + ">";
            inline_(from, to);
            out_ += "</h" + h + ">\n";
            return;
        }
        if (r.math == 2 && to == from + 1) {   // display math on a line of its own
            out_ += "<div class=\"math display\">\\[" + escape(r.text) + "\\]</div>\n";
            return;
        }
        out_ += "<p>";
        inline_(from, to);
        out_ += "</p>\n";
    }

    // The runs' text with its emphasis, code, links and pictures.
    void inline_(size_t from, size_t to) {
        std::string href;   // the link open now
        auto closeLink = [&] {
            if (!href.empty()) out_ += "</a>";
            href.clear();
        };
        for (size_t k = from; k < to; k++) {
            const MdRun& r = runs_[k];
            if (r.footnote > 0) {   // a reference: a raised number linking to the note
                closeLink();
                out_ += "<sup class=\"footnote-ref\"><a href=\"" + escape(r.url) + "\"";
                if (!r.anchor.empty()) out_ += " id=\"" + escape(r.anchor) + "\"";
                out_ += ">" + escape(r.text) + "</a></sup>";
                continue;
            }
            if (r.footnotes && r.link && r.url.rfind("#fnref-", 0) == 0) {   // the way back
                closeLink();
                out_ += "<a href=\"" + escape(r.url) + "\" class=\"footnote-backref\">" +
                        escape(r.text) + "</a>";
                continue;
            }
            const std::string want = r.link && !r.url.empty() && linkAllowed(r.url) ? r.url : "";
            if (want != href) {
                closeLink();
                if (!want.empty()) out_ += "<a href=\"" + escape(want) + "\">";
                href = want;
            }
            out_ += piece(r);
        }
        closeLink();
    }

    std::string piece(const MdRun& r) {
        std::string s;
        if (r.hardBreak) return "<br>\n";
        if (r.image) {
            if (!r.src.empty() && pictureAllowed(r.src))
                s = "<img src=\"" + escape(r.src) + "\" alt=\"" + escape(r.text) + "\">";
            else
                s = escape(r.text.empty() ? r.src : r.text);
        } else if (r.math == 1) {
            s = "<span class=\"math inline\">\\(" + escape(r.text) + "\\)</span>";
        } else if (r.math == 2) {   // in a heading or a cell, which stay one line
            s = "<span class=\"math display\">\\[" + escape(r.text) + "\\]</span>";
        } else if (r.code) {
            s = "<code>" + escape(r.text) + "</code>";
        } else {
            s = escape(r.text);
        }
        if (r.strike) s = "<del>" + s + "</del>";
        if (r.italic) s = "<em>" + s + "</em>";
        if (r.bold && !(r.table && r.tableRow == 0)) s = "<strong>" + s + "</strong>";
        return s;
    }

    void code(size_t from, size_t to) {
        const std::string& lang = runs_[from].lang;
        out_ += "<pre><code";
        if (!lang.empty()) out_ += " class=\"language-" + escape(lang) + "\"";
        out_ += ">";
        for (size_t k = from; k < to; k++) out_ += escape(runs_[k].text);
        out_ += "</code></pre>\n";
    }

    void table(size_t from, size_t to) {
        const int cols = runs_[from].tableCols;
        int rows = 0;
        for (size_t k = from; k < to; k++) rows = std::max(rows, runs_[k].tableRow + 1);
        if (cols <= 0 || rows <= 0) return;
        // Each cell's runs, in order.
        std::vector<std::vector<size_t>> cells((size_t)(rows * cols));
        std::vector<int> aligns((size_t)cols, 0);
        for (size_t k = from; k < to; k++) {
            const MdRun& r = runs_[k];
            if (r.tableRow < 0 || r.tableCol < 0 || r.tableCol >= cols) continue;
            cells[(size_t)(r.tableRow * cols + r.tableCol)].push_back(k);
            aligns[(size_t)r.tableCol] = r.tableAlign;
        }
        static const char* const kAlign[3] = {"", " style=\"text-align: center\"",
                                              " style=\"text-align: right\""};
        out_ += "<table>\n";
        for (int row = 0; row < rows; row++) {
            if (row == 0) out_ += "<thead>\n";
            if (row == 1) out_ += "<tbody>\n";
            out_ += "<tr>";
            for (int col = 0; col < cols; col++) {
                const char* tag = row == 0 ? "th" : "td";
                out_ += std::string("<") + tag + kAlign[std::min(std::max(aligns[(size_t)col], 0), 2)] + ">";
                // A cell's runs follow one another in the page.
                const std::vector<size_t>& cell = cells[(size_t)(row * cols + col)];
                if (!cell.empty()) inline_(cell.front(), cell.back() + 1);
                out_ += std::string("</") + tag + ">";
            }
            out_ += "</tr>\n";
            if (row == 0) out_ += "</thead>\n";
        }
        if (rows > 1) out_ += "</tbody>\n";
        out_ += "</table>\n";
    }
};

const char* const kStyle =
    ":root { color-scheme: light dark; }\n"
    "body { margin: 0; background: #ffffff; color: #1f2328; }\n"
    ".markdown-body { box-sizing: border-box; max-width: 860px; margin: 0 auto;"
    " padding: 32px; font: 16px/1.5 -apple-system, BlinkMacSystemFont, \"Segoe UI\","
    " Helvetica, Arial, sans-serif; }\n"
    "h1, h2 { padding-bottom: .3em; border-bottom: 1px solid #d1d9e0; }\n"
    "h1, h2, h3, h4, h5, h6 { margin: 24px 0 16px; line-height: 1.25; }\n"
    "p, blockquote, ul, ol, table, pre { margin: 0 0 16px; }\n"
    "a { color: #0969da; }\n"
    "code, pre { font: 85%/1.45 ui-monospace, SFMono-Regular, Menlo, monospace; }\n"
    "code { padding: .2em .4em; border-radius: 6px; background: rgba(129, 139, 152, .12); }\n"
    "pre { padding: 16px; overflow: auto; border-radius: 6px; background: #f6f8fa; }\n"
    "pre code { padding: 0; background: none; font-size: 100%; }\n"
    "blockquote { padding: 0 1em; color: #59636e; border-left: .25em solid #d1d9e0; }\n"
    "table { border-collapse: collapse; }\n"
    "th, td { padding: 6px 13px; border: 1px solid #d1d9e0; }\n"
    "th { background: #f6f8fa; }\n"
    "hr { height: .25em; margin: 24px 0; border: 0; background: #d1d9e0; }\n"
    "img { max-width: 100%; }\n"
    ".task-list-item { list-style: none; }\n"
    ".task-list-item input { margin: 0 .3em .25em -1.4em; vertical-align: middle; }\n"
    ".footnotes { font-size: 85%; color: #59636e; }\n"
    ".math.display { display: block; text-align: center; margin: 0 0 16px; }\n"
    "@media (prefers-color-scheme: dark) {\n"
    "  body { background: #0d1117; color: #f0f6fc; }\n"
    "  h1, h2, th, td, hr { border-color: #3d444d; }\n"
    "  hr { background: #3d444d; }\n"
    "  a { color: #4493f8; }\n"
    "  pre, th { background: #151b23; }\n"
    "  blockquote { color: #9198a1; border-left-color: #3d444d; }\n"
    "  .footnotes { color: #9198a1; }\n"
    "}\n";

}  // namespace

std::string titleFor(const std::vector<MdRun>& runs, const std::string& fallback) {
    std::string first, top;
    for (size_t k = 0; k < runs.size() && top.empty();) {
        const MdRun& r = runs[k];
        if (r.heading <= 0) {
            k++;
            continue;
        }
        std::string title;
        size_t j = k;
        for (; j < runs.size() && runs[j].heading > 0 && runs[j].line == r.line && runs[j].text != "\n";
             j++)
            title += runs[j].text;
        if (first.empty()) first = title;
        if (r.heading == 1) top = title;
        k = std::max(j, k + 1);
    }
    if (!top.empty()) return top;
    if (!first.empty()) return first;
    return fallback;
}

std::string render(const std::vector<MdRun>& runs, const std::string& title) {
    std::string out =
        "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<meta name=\"generator\" content=\"minicode\">\n<title>";
    out += escape(title);
    out += "</title>\n<style>\n";
    out += kStyle;
    out += "</style>\n</head>\n<body>\n<article class=\"markdown-body\">\n";
    out += Writer(runs).body();
    out += "</article>\n</body>\n</html>\n";
    return out;
}

std::string page(const std::string& markdown, const std::string& title) {
    const std::vector<MdRun> runs = MarkdownParser::parse(markdown);
    return render(runs, titleFor(runs, title));
}

}  // namespace MarkdownHtml
