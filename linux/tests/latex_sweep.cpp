// latex_sweep.cpp — double-clicks every word of a typeset LaTeX document
// through the Linux preview's click path (latexSpanAtPoint in PageWords.cpp,
// over pageTextOf in LatexClick.cpp) and reports how often it finds the source
// of what was clicked. The Linux twin of tests/latex/sweep.mm; run it with
// tests/latex/sweep-linux.sh, which typesets the document the way the app does.
//
//   minicode-latex-sweep <source.tex> <typeset-copy.tex> <out.pdf> <out.synctex.gz> [-v]
//
// Every word is double-clicked at the middle of its first, middle and last
// character, and each click is judged against the word's own text, folded to
// plain lowercase ASCII letters and digits (ligatures split, accents dropped),
// with the same verdicts as the Mac harness:
//   found    the span's text contains the word, and at the right place: the
//            word is unique in the source, or the span holds it next to a
//            neighbouring word from the page, or the span is that word alone
//   unsure   the span contains the word, but it is common and nothing
//            confirms this is the occurrence clicked (-v lists them)
//   partial  the word straddles spans (foo\emph{bar}baz) and the span holds
//            the part that was clicked
//   refused  no span offered
//   wrong    a span was offered that does not hold the word
// Words that do not occur in the source at all (section and page numbers) are
// counted apart as "generated". Single characters are counted apart too, as on
// the Mac: the matcher refuses every one of them (a page, section or list
// number has no text of its own to match), so any that still offered a span
// are reported, as "short+span" under -v.
//
// One difference: the Mac enumerates words with NSString's word breaker,
// independently of PDFKit's word under the pointer. Here the words come from
// PageText::words(), the same segmentation the click uses, since poppler has
// no word breaker of its own. The verdicts still judge the span against the
// page's text, not against anything the matcher computed.
#include <glib.h>
#include <poppler.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "LatexClick.h"
#include "LatexDoc.h"
#include "PageWords.h"
#include "SyncTex.h"

namespace {

// The harness's own notion of "the same text", independent of LatexDoc's.
std::string fold(const std::string& s) {
    std::string in;
    for (size_t i = 0; i < s.size(); ++i) {   // ß has no decomposition
        if (s.compare(i, 2, "\xC3\x9F") == 0) { in += "ss"; ++i; }
        else in += s[i];
    }
    char* nfkd = g_utf8_normalize(in.c_str(), -1, G_NORMALIZE_ALL);   // ﬁ -> fi, é -> e + ´
    std::string out;
    if (!nfkd) return out;
    for (const char* p = nfkd; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x80 && g_ascii_isalnum(c)) out += (char)g_ascii_tolower(c);
    }
    g_free(nfkd);
    return out;
}

// The source text a span shows, folded. TeX escapes for accents are resolved
// by hand here, so the harness does not trust the code it is testing.
std::string foldTex(std::string m) {
    auto replaceAll = [&](const std::string& from, const std::string& to) {
        for (size_t at = 0; (at = m.find(from, at)) != std::string::npos; at += to.size())
            m.replace(at, from.size(), to);
    };
    replaceAll("\\ss", "ss");
    for (const char* k : {"\\S", "\\c", "\\\"", "\\'", "\\~", "\\`", "\\^"}) replaceAll(k, "");
    // Drop command names (\textbf, \emph ...), keep their arguments.
    std::string out;
    for (size_t i = 0; i < m.size();) {
        if (m[i] == '\\' && i + 1 < m.size() && g_ascii_isalpha(m[i + 1])) {
            size_t j = i + 1;
            while (j < m.size() && g_ascii_isalpha(m[j])) ++j;
            if (j < m.size() && m[j] == '*') ++j;
            out += ' ';
            i = j;
        } else {
            out += m[i++];
        }
    }
    return fold(out);
}

std::string dropComments(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && (i == 0 || s[i - 1] != '\\')) {
            while (i < s.size() && s[i] != '\n') ++i;
            if (i < s.size()) out += '\n';
            continue;
        }
        out += s[i];
    }
    return out;
}

size_t countOf(const std::string& hay, const std::string& needle) {
    size_t c = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++c;
    return c;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: minicode-latex-sweep source.tex typeset.tex "
                             "out.pdf out.synctex.gz [-v]\n");
        return 2;
    }
    const bool verbose = argc > 5 && std::strcmp(argv[5], "-v") == 0;
    char* raw = nullptr;
    gsize rawLen = 0;
    if (!g_file_get_contents(argv[1], &raw, &rawLen, nullptr)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    const std::string src(raw, rawLen);
    g_free(raw);
    const LatexDoc doc = LatexDoc::parse(src);
    const SyncTexIndex sync = SyncTexIndex::parse(readGzipFile(argv[4]));
    const int tag = sync.tagForPath(argv[2]);

    char* uri = g_filename_to_uri(argv[3], nullptr, nullptr);
    PopplerDocument* pdf = uri ? poppler_document_new_from_file(uri, nullptr, nullptr) : nullptr;
    g_free(uri);
    if (!pdf || !sync.valid()) { std::fprintf(stderr, "no pdf or synctex\n"); return 2; }

    // Everything the source could show, for telling generated words apart.
    const std::string wholeFolded = foldTex(dropComments(src));

    int found = 0, unsure = 0, partial = 0, refused = 0, wrong = 0, generated = 0,
        genSpan = 0, shortWords = 0, shortSpan = 0, mismatch = 0;
    const int pages = poppler_document_get_n_pages(pdf);
    for (int p = 0; p < pages; ++p) {
        PopplerPage* page = poppler_document_get_page(pdf, p);
        {
            // poppler promises a box per character; say so if it ever stops.
            char* t = poppler_page_get_text(page);
            PopplerRectangle* r = nullptr;
            guint n = 0;
            poppler_page_get_text_layout(page, &r, &n);
            if ((glong)n != g_utf8_strlen(t ? t : "", -1)) mismatch++;
            g_free(r);
            g_free(t);
        }
        const PageText text = pageTextOf(page);
        const auto words = text.words();
        for (size_t w = 0; w < words.size(); ++w) {
            const auto [s, e] = words[w];
            const std::string word = text.slice(s, e);
            const std::string want = fold(word);
            if (want.empty()) continue;
            const std::string prev = w > 0 ? fold(text.slice(words[w - 1].first, words[w - 1].second)) : "";
            const std::string next = w + 1 < words.size()
                ? fold(text.slice(words[w + 1].first, words[w + 1].second)) : "";
            const bool inSource = contains(wholeFolded, want);
            const bool unique = countOf(wholeFolded, want) == 1;

            // Double-click the first, middle and last character, as a person might.
            std::set<size_t> at = {s, s + (e - s) / 2, e - 1};
            for (size_t idx : at) {
                const PageBox& b = text.box(idx);
                const double x = (b.x1 + b.x2) / 2, y = (b.y1 + b.y2) / 2;
                std::vector<SyncTexHit> hits;
                PageClick click;
                const LatexSpan* sp = latexSpanAtPoint(doc, sync, tag, text, p + 1, x, y,
                                                       &hits, &click);
                std::string spanText;
                if (sp) spanText = src.substr(sp->start, sp->end - sp->start);
                const std::string have = sp ? foldTex(spanText) : "";
                // The right occurrence of a common word: the span holds it
                // next to a neighbour from the page, or is that word alone.
                const bool placed = unique || have == want ||
                    (!prev.empty() && contains(have, prev + want)) ||
                    (!next.empty() && contains(have, want + next));
                const char* verdict;
                if (want.size() < 2) {
                    shortWords++;
                    if (sp) shortSpan++;
                    verdict = sp ? "short+span" : "short";
                }
                else if (!inSource) {
                    generated++;
                    if (sp) genSpan++;
                    verdict = sp ? "gen+span" : "gen";
                }
                else if (!sp) { refused++; verdict = "REFUSED"; }
                else if (contains(have, want)) {
                    if (placed) { found++; verdict = "found"; }
                    else { unsure++; verdict = "unsure"; }
                } else if (have.size() >= 2 && contains(want, have)) {
                    partial++; verdict = "partial";
                } else { wrong++; verdict = "WRONG"; }

                if (verbose && verdict[0] != 'f') {
                    std::string lines;
                    for (const SyncTexHit& h : hits)
                        if (tag == 0 || h.tag == tag) lines += std::to_string(h.line) + " ";
                    std::printf("%-8s p%d word=[%s] clicked=[%s] lines=[%s]", verdict, p + 1,
                                word.c_str(), click.word.c_str(), lines.c_str());
                    if (sp) std::printf(" span@%d=[%.60s]", sp->line, spanText.c_str());
                    std::printf("\n");
                }
            }
        }
        g_object_unref(page);
    }
    g_object_unref(pdf);
    const int total = found + unsure + partial + refused + wrong;
    std::printf("clicks %d: found %d (+%d not placed), partial %d, refused %d, "
                "wrong %d (hit rate %.1f%%); generated %d (%d offered a span), "
                "single characters %d (%d offered a span)\n",
                total, found, unsure, partial, refused, wrong,
                total ? 100.0 * (found + unsure + partial) / total : 0.0,
                generated, genSpan, shortWords, shortSpan);
    if (mismatch) std::printf("warning: %d pages where poppler's boxes and characters "
                              "did not line up\n", mismatch);
    return wrong == 0 ? 0 : 1;
}
