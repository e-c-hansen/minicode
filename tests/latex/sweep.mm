// sweep.mm — double-clicks every word of a typeset LaTeX document through the
// preview's real click path (MCLatexSpanAtPoint in src/Latex.mm) and reports
// how often it finds the source of what was clicked. Run it with sweep.sh,
// which typesets the document the way the app does.
//
//   sweep <source.tex> <typeset-copy.tex> <out.pdf> <out.synctex.gz> [-v]
//
// Every word is double-clicked at its first, middle and last character, and
// each click is judged against the word's own text, folded to plain lowercase
// letters and digits (ligatures split, accents dropped):
//   found    the span's text contains the word, and at the right place: the
//            word is unique in the source, or the span holds it next to a
//            neighbouring word from the page, or the span is that word alone
//   unsure   the span contains the word, but it is common and nothing
//            confirms this is the occurrence clicked (-v lists them)
//   partial  the word straddles spans (foo\emph{bar}baz) and the span holds
//            the part that was clicked
//   refused  no span offered
//   wrong    a span was offered that does not hold the word
// Words that do not occur in the source at all (section numbers, page
// numbers, list labels) are counted apart as "generated": refusing those is
// right. Single characters (section and item numbers, "a") are counted apart
// too: the matcher places those by position, not by text. -v prints every miss with the candidate lines.
#import <Quartz/Quartz.h>
#import "Latex.h"
#include "LatexDoc.h"
#include "SyncTex.h"
#include <zlib.h>
#include <cstdio>
#include <string>

static std::string Utf8(NSString *s) {
    const char *c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

static std::string ReadGzip(const char *path) {
    std::string out;
    gzFile f = gzopen(path, "rb");
    if (!f) return out;
    char buf[64 * 1024];
    int got;
    while ((got = gzread(f, buf, sizeof(buf))) > 0) out.append(buf, (size_t)got);
    gzclose(f);
    return out;
}

// The harness's own notion of "the same text", independent of LatexDoc's.
static NSString *Fold(NSString *s) {
    NSMutableString *m = [[s stringByFoldingWithOptions:
        NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch |
        NSWidthInsensitiveSearch locale:nil] mutableCopy];
    [m replaceOccurrencesOfString:@"ß" withString:@"ss" options:0
                            range:NSMakeRange(0, m.length)];
    NSMutableString *out = [NSMutableString string];
    NSString *decomposed = m.decomposedStringWithCompatibilityMapping;  // ﬁ -> fi
    NSCharacterSet *keep = [NSCharacterSet alphanumericCharacterSet];
    for (NSUInteger k = 0; k < decomposed.length; k++) {
        unichar c = [decomposed characterAtIndex:k];
        if (c < 128 ? isalnum(c) : [keep characterIsMember:c])
            [out appendFormat:@"%C", (unichar)tolower(c)];
    }
    // Keep only ASCII: accents dropped by the decomposition above.
    NSData *ascii = [out dataUsingEncoding:NSASCIIStringEncoding
                      allowLossyConversion:YES];
    NSString *plain = [[NSString alloc] initWithData:ascii
                                            encoding:NSASCIIStringEncoding];
    NSMutableString *clean = [NSMutableString string];
    for (NSUInteger k = 0; k < plain.length; k++) {
        unichar c = [plain characterAtIndex:k];
        if (isalnum(c)) [clean appendFormat:@"%C", c];
    }
    return clean;
}

// The source text a span shows, folded. TeX escapes for accents (\'e, \"o,
// \ss) are resolved here by hand, so the harness does not trust the code it
// is testing.
static NSString *FoldTex(const std::string &tex) {
    NSMutableString *m = [[NSString stringWithUTF8String:tex.c_str()] mutableCopy] ?: [NSMutableString string];
    NSDictionary *subs = @{@"\\ss": @"ss", @"\\S": @"", @"\\c": @"", @"\\\"": @"",
                           @"\\'": @"", @"\\~": @"", @"\\`": @"", @"\\^": @""};
    for (NSString *k in subs)
        [m replaceOccurrencesOfString:k withString:subs[k] options:0
                                range:NSMakeRange(0, m.length)];
    // Drop command names (\textbf, \emph ...), keep their arguments.
    NSRegularExpression *cmd = [NSRegularExpression
        regularExpressionWithPattern:@"\\\\[A-Za-z]+\\*?" options:0 error:nil];
    [cmd replaceMatchesInString:m options:0 range:NSMakeRange(0, m.length)
                   withTemplate:@" "];
    return Fold(m);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 5) {
            fprintf(stderr, "usage: sweep source.tex typeset.tex out.pdf out.synctex.gz [-v]\n");
            return 2;
        }
        bool verbose = argc > 5 && std::string(argv[5]) == "-v";
        NSString *srcPath = @(argv[1]);
        NSString *source = [NSString stringWithContentsOfFile:srcPath
                                                     encoding:NSUTF8StringEncoding
                                                        error:nil];
        if (!source) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
        std::string src = Utf8(source);
        LatexDoc doc = LatexDoc::parse(src);
        SyncTexIndex sync = SyncTexIndex::parse(ReadGzip(argv[4]));
        int tag = sync.tagForPath(argv[2]);
        PDFDocument *pdf = [[PDFDocument alloc]
            initWithURL:[NSURL fileURLWithPath:@(argv[3])]];
        if (!pdf || !sync.valid()) { fprintf(stderr, "no pdf or synctex\n"); return 2; }

        // Everything the source could show, for telling generated words apart.
        // Comments are dropped; \newcommand bodies stay, so text a macro
        // expands to counts as source (it is, just not where it appears).
        NSRegularExpression *comment = [NSRegularExpression
            regularExpressionWithPattern:@"(?<!\\\\)%[^\n]*" options:0 error:nil];
        NSString *uncommented = [comment stringByReplacingMatchesInString:source
            options:0 range:NSMakeRange(0, source.length) withTemplate:@""];
        NSString *wholeFolded = FoldTex(Utf8(uncommented));

        auto count = [](NSString *hay, NSString *needle) {
            NSUInteger c = 0;
            NSRange from = NSMakeRange(0, hay.length);
            for (;;) {
                NSRange r = [hay rangeOfString:needle options:0 range:from];
                if (r.location == NSNotFound) return c;
                c++;
                from = NSMakeRange(r.location + 1, hay.length - r.location - 1);
            }
        };

        int found = 0, unsure = 0, partial = 0, refused = 0, wrong = 0,
            generated = 0, shortWords = 0;
        for (NSUInteger p = 0; p < pdf.pageCount; p++) {
            PDFPage *page = [pdf pageAtIndex:p];
            NSString *text = page.string ?: @"";
            NSMutableArray<NSString *> *words = [NSMutableArray array];
            NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
            [text enumerateSubstringsInRange:NSMakeRange(0, text.length)
                                     options:NSStringEnumerationByWords
                                  usingBlock:^(NSString *w, NSRange r, NSRange, BOOL *) {
                [words addObject:w];
                [ranges addObject:[NSValue valueWithRange:r]];
            }];
            for (NSUInteger w = 0; w < words.count; w++) {
                NSString *word = words[w];
                NSRange r = ranges[w].rangeValue;
                NSString *want = Fold(word);
                if (want.length == 0) continue;
                NSString *prev = w > 0 ? Fold(words[w - 1]) : @"";
                NSString *next = w + 1 < words.count ? Fold(words[w + 1]) : @"";
                bool inSource = [wholeFolded rangeOfString:want].location != NSNotFound;
                bool unique = count(wholeFolded, want) == 1;

                // Double-click the first, middle and last character, as a
                // person might. characterBoundsAtIndex: drifts away from
                // page.string's indices; a one-character selection does not.
                NSMutableOrderedSet<NSNumber *> *at = [NSMutableOrderedSet orderedSet];
                [at addObject:@(r.location)];
                [at addObject:@(r.location + r.length / 2)];
                [at addObject:@(NSMaxRange(r) - 1)];
                for (NSNumber *idx in at) {
                    NSRect box = [[page selectionForRange:NSMakeRange(idx.unsignedIntegerValue, 1)]
                                     boundsForPage:page];
                    NSPoint pt = NSMakePoint(NSMidX(box), NSMidY(box));
                    std::vector<SyncTexHit> hits;
                    const LatexSpan *sp = MCLatexSpanAtPoint(doc, sync, tag, page,
                                                             (int)p + 1, pt, &hits);
                    NSString *clicked = [page selectionForWordAtPoint:pt].string ?: @"";
                    std::string spanText;
                    if (sp) spanText = src.substr(sp->start, sp->end - sp->start);
                    NSString *have = sp ? FoldTex(spanText) : @"";
                    // The right occurrence of a common word: the span holds it
                    // next to a neighbour from the page, or is that word alone.
                    bool placed = unique || [have isEqualToString:want] ||
                        (prev.length && [have rangeOfString:[prev stringByAppendingString:want]].location != NSNotFound) ||
                        (next.length && [have rangeOfString:[want stringByAppendingString:next]].location != NSNotFound);
                    const char *verdict;
                    if (want.length < 2) { shortWords++; verdict = "short"; }
                    else if (!inSource) { generated++; verdict = sp ? "gen+span" : "gen"; }
                    else if (!sp) { refused++; verdict = "REFUSED"; }
                    else if ([have rangeOfString:want].location != NSNotFound) {
                        if (placed) { found++; verdict = "found"; }
                        else { unsure++; verdict = "unsure"; }
                    } else if (have.length >= 2 &&
                               [want rangeOfString:have].location != NSNotFound) {
                        partial++; verdict = "partial";
                    } else { wrong++; verdict = "WRONG"; }

                    if (verbose && verdict[0] != 'f') {
                        std::string lines;
                        for (const SyncTexHit &h : hits)
                            if (tag == 0 || h.tag == tag)
                                lines += std::to_string(h.line) + " ";
                        printf("%-8s p%lu word=[%s] clicked=[%s] lines=[%s]",
                               verdict, (unsigned long)p + 1, Utf8(word).c_str(),
                               Utf8(clicked).c_str(), lines.c_str());
                        if (sp) printf(" span@%d=[%.60s]", sp->line, spanText.c_str());
                        printf("\n");
                    }
                }
            }
        }
        int total = found + unsure + partial + refused + wrong;
        printf("clicks %d: found %d (+%d not placed), partial %d, refused %d, "
               "wrong %d (hit rate %.1f%%); generated %d, single characters %d\n",
               total, found, unsure, partial, refused, wrong,
               total ? 100.0 * (found + unsure + partial) / total : 0.0,
               generated, shortWords);
        return 0;
    }
}
