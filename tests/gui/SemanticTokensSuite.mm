// SemanticTokensSuite.mm — `make guitest SUITES=semantic_tokens`: a
// language server's semantic tokens over the lexer's colors, against the
// scripted server (build/fakelsp) with basedpyright's legend.
//
// The server answers /full with the parameters of a function, its name, and
// a variable far down the file, then /full/delta (800 ms late) with the
// edit after a space is typed. Checked: the parameters take
// syntax.parameter once the answer is in and the function keeps
// syntax.function; the variable far down is painted only once it scrolls
// into view; after the edit the line typed in has the lexer's colors until
// the delta arrives, the other lines keep theirs, and the delta asked from
// the first answer's resultId puts them at their new places; turning
// editor.semantic-highlighting off leaves only the lexer's colors, and on
// brings the server's back. Where basedpyright is installed, its own tokens
// color a parameter, a class, a property and a constant. Then the cost:
// 60,000 spans over a 20,000-line
// file handed to the editor, and a scroll's painting, in the main thread's
// CPU time.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#include "LspClient.h"
#include <pwd.h>
#include <time.h>
#include <unistd.h>

namespace {
LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }
NSTextView *Text(EditorController *c) { return (NSTextView *)[c valueForKey:@"textView"]; }

// The color at a character as #RRGGBB.
NSString *ColorAt(EditorController *c, NSUInteger i) {
    NSTextStorage *ts = Text(c).textStorage;
    if (i >= ts.length) return @"(past the end)";
    NSColor *col = [ts attribute:NSForegroundColorAttributeName atIndex:i effectiveRange:NULL];
    NSColor *rgb = [col colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!rgb) return @"(none)";
    return [NSString stringWithFormat:@"#%02X%02X%02X", (int)lround(rgb.redComponent * 255),
                                      (int)lround(rgb.greenComponent * 255),
                                      (int)lround(rgb.blueComponent * 255)];
}

// Where the n-th (0-based) occurrence of a word starts.
NSUInteger Nth(NSString *s, NSString *word, int n) {
    NSRange r = NSMakeRange(0, 0);
    NSUInteger from = 0;
    for (int k = 0; k <= n; k++) {
        r = [s rangeOfString:word options:0 range:NSMakeRange(from, s.length - from)];
        if (r.location == NSNotFound) return NSNotFound;
        from = NSMaxRange(r);
    }
    return r.location;
}

// The characters on screen.
NSRange Visible(EditorController *c) {
    NSTextView *tv = Text(c);
    NSLayoutManager *lm = tv.layoutManager;
    NSRange g = [lm glyphRangeForBoundingRect:tv.visibleRect inTextContainer:tv.textContainer];
    return [lm characterRangeForGlyphRange:g actualGlyphRange:NULL];
}

// basedpyright's server, on PATH or in ~/.local/bin (uv's), or nil.
NSString *BasedPyright() {
    NSMutableArray *dirs = [[NSProcessInfo.processInfo.environment[@"PATH"]
        componentsSeparatedByString:@":"] mutableCopy];
    struct passwd *pw = getpwuid(getuid());   // HOME is the scratch one here
    if (pw) [dirs addObject:[@(pw->pw_dir) stringByAppendingPathComponent:@".local/bin"]];
    for (NSString *d in dirs) {
        NSString *p = [d stringByAppendingPathComponent:@"basedpyright-langserver"];
        if ([[NSFileManager defaultManager] isExecutableFileAtPath:p]) return p;
    }
    return nil;
}

double ThreadCpu() {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

NSString *const kParameter = @"#FF0000";   // syntax.parameter, set below
NSString *const kVariable = @"#00FF00";    // syntax.variable
NSString *const kFunction = @"#DCDCAA";    // Dark+'s syntax.function
NSString *const kPlain = @"#D4D4D4";       // Dark+'s editor.text
NSString *const kSettings = @"syntax.parameter = #FF0000\nsyntax.variable = #00FF00\n";
}  // namespace

MC_GUI_SUITE(semantic_tokens) {
    __block EditorController *c = nil;
    NSString *file = MCGuiProjectFile(@"semantic/main.py");
    const int kFarLine = 400;
    NSMutableString *body = [NSMutableString stringWithString:
        @"def area(width, height):\n    return width * height\n"];
    for (int k = 2; k < kFarLine; k++) [body appendFormat:@"# padding line %d\n", k];
    [body appendString:@"total = 1\n"];
    NSString *original = [body copy];
    // area function, width and height parameters (twice), total a variable.
    NSArray *data = @[@0, @4, @4, @9, @1,  @0, @5, @5, @5, @1,  @0, @7, @6, @5, @1,
                      @1, @11, @5, @5, @0,  @0, @8, @6, @5, @0,
                      @(kFarLine - 1), @0, @5, @6, @1];
    NSDictionary *legend = @{
        @"tokenTypes": @[@"namespace", @"type", @"class", @"enum", @"typeParameter",
                         @"parameter", @"variable", @"property", @"enumMember", @"function",
                         @"method", @"keyword", @"decorator", @"selfParameter", @"clsParameter"],
        @"tokenModifiers": @[@"declaration", @"definition", @"readonly", @"static", @"async",
                             @"defaultLibrary", @"builtin", @"classMember", @"parameter"]};
    // After a space typed before "return": line 1's first token one later.
    NSDictionary *script = @{
        @"name": @"fakelsp",
        @"capabilities": @{@"textDocumentSync": @1,
                           @"semanticTokensProvider": @{@"legend": legend,
                                                        @"full": @{@"delta": @YES}}},
        @"rules": @[
            @{@"method": @"textDocument/semanticTokens/full",
              @"result": @{@"resultId": @"1", @"data": data}},
            @{@"method": @"textDocument/semanticTokens/full/delta",
              @"when": @{@"paramsContain": @"\"previousResultId\":\"1\""},
              @"delayMs": @800,
              @"result": @{@"resultId": @"2",
                           @"edits": @[@{@"start": @16, @"deleteCount": @1,
                                         @"data": @[@12]}]}}]};
    __block NSUInteger answers = 0;

    MCGuiStep(@"a server with semantic tokens", ^{
        c = MCGuiController();
        MCGuiWriteFile(file, original);
        NSString *cmd = MCGuiFakeLsp(@"semantic", script, nil, nil);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n%@", cmd, kSettings]);
        GCHECK([c showFile:file]);
    });
    MCGuiWait(@"its first answer", 20, ^BOOL { return Session(c).semanticAnswers > 0; });
    MCGuiStep(@"the parameters are parameters, the function a function", ^{
        NSString *s = Text(c).string;
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"width", 0)), kParameter);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"height", 0) + 5), kParameter);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"width", 1)), kParameter);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"area", 0)), kFunction);
        // Between them the lexer's own: the comma, and "return" a keyword.
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @",", 0)), kPlain);
        GCHECK(![ColorAt(c, Nth(s, @"return", 0)) isEqualToString:kParameter]);
        // The variable far down is not on screen, so not painted yet.
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"total", 0)), kPlain);
        NSArray *full = MCGuiFakeLspReceived(@"semantic", @"textDocument/semanticTokens/full");
        GCHECK(full.count == 1);
        GCHECK(Session(c).semanticRequests == 1);
        answers = Session(c).semanticAnswers;
        NSTextView *tv = Text(c);
        [tv scrollRangeToVisible:NSMakeRange(Nth(s, @"total", 0), 5)];
    });
    MCGuiWait(@"scrolled into view, it is painted", 5, ^BOOL {
        return [ColorAt(c, Nth(Text(c).string, @"total", 0)) isEqualToString:kVariable];
    });

    MCGuiStep(@"a space typed before return", ^{
        NSTextView *tv = Text(c);
        [c.window makeFirstResponder:tv];
        MCGuiFakeLspClearLog(@"semantic");
        NSUInteger at = Nth(tv.string, @"return", 0);
        [tv setSelectedRange:NSMakeRange(at, 0)];   // the caret, and the view, at the top
        [tv scrollRangeToVisible:NSMakeRange(0, 0)];
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@" " replacementRange:NSMakeRange(at, 0)];
        });
        GCHECK(NSLocationInRange(at, Visible(c)));
        // The line typed in has the lexer's colors at once; the line
        // above keeps the server's, which moved with their text.
        NSString *s = tv.string;
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"width", 1)), kPlain);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"height", 1)), kPlain);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"width", 0)), kParameter);
    });
    MCGuiWait(@"the delta, asked from the first answer, arrives", 10, ^BOOL {
        return Session(c).semanticAnswers > answers;
    });
    MCGuiStep(@"and paints the tokens at their new places", ^{
        NSString *s = Text(c).string;
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"width", 1)), kParameter);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"height", 1)), kParameter);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"width", 1) - 1), kPlain);   // the space before it
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"width", 0)), kParameter);
        NSArray *delta =
            MCGuiFakeLspReceived(@"semantic", @"textDocument/semanticTokens/full/delta");
        GCHECK(delta.count == 1);
        GCHECK_EQ_STR(delta.firstObject[@"params"][@"previousResultId"], @"1");
        GCHECK(MCGuiFakeLspReceived(@"semantic", @"textDocument/semanticTokens/full").count == 0);
        answers = Session(c).semanticAnswers;
        // Off: only the lexer's colors.
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n%@"
            @"editor.semantic-highlighting = off\n",
            MCGuiFakeLsp(@"semantic", script, nil, nil), kSettings]);
    });
    MCGuiWait(@"turned off, the parameters lose their color", 5, ^BOOL {
        return [ColorAt(c, Nth(Text(c).string, @"width", 0)) isEqualToString:kPlain];
    });
    MCGuiStep(@"and turned on", ^{
        GCHECK(Session(c).semanticAnswers == answers);
        GCHECK_EQ_STR(ColorAt(c, Nth(Text(c).string, @"area", 0)), kFunction);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n%@",
            MCGuiFakeLsp(@"semantic", script, nil, nil), kSettings]);
    });
    MCGuiWait(@"the server's colors come back", 10, ^BOOL {
        return [ColorAt(c, Nth(Text(c).string, @"width", 0)) isEqualToString:kParameter];
    });

    // ------------------------------------- basedpyright, where it is installed
    NSString *bp = BasedPyright();
    NSString *real = MCGuiProjectFile(@"semantic/real.py");
    if (bp) {
        MCGuiStep(@"basedpyright on a file of its own", ^{
            MCGuiWriteFile(real, @"import os\n\nLIMIT: int = 3\n\n\nclass Box:\n"
                                 @"    def __init__(self, width: int) -> None:\n"
                                 @"        self.width = width\n\n\n"
                                 @"def grow(box: Box, extra: int) -> int:\n"
                                 @"    return box.width + extra + LIMIT + len(os.sep)\n");
            answers = Session(c).semanticAnswers;
            MCGuiSetSettings([NSString stringWithFormat:
                @"lsp.enabled = true\nlsp.python = %@ --stdio\nlsp.python.also = off\n%@",
                bp, kSettings]);
            GCHECK([c showFile:real]);
        });
        MCGuiWait(@"its tokens arrive", 60, ^BOOL { return Session(c).semanticAnswers > answers; });
        MCGuiWait(@"and say what each name is", 20, ^BOOL {
            NSString *s = Text(c).string;
            NSUInteger extra = Nth(s, @"extra", 1);   // a use, not the declaration
            return [ColorAt(c, extra) isEqualToString:kParameter];
        });
        MCGuiStep(@"parameters, a class, a property", ^{
            NSString *s = Text(c).string;
            MCGuiNote(@"box %@, Box %@, width (property) %@, LIMIT %@, len %@",
                      ColorAt(c, Nth(s, @"box", 1)), ColorAt(c, Nth(s, @"Box", 1)),
                      ColorAt(c, Nth(s, @"width", 3)), ColorAt(c, Nth(s, @"LIMIT", 1)),
                      ColorAt(c, Nth(s, @"len", 0)));
            GCHECK_EQ_STR(ColorAt(c, Nth(s, @"box", 1)), kParameter);
            GCHECK_EQ_STR(ColorAt(c, Nth(s, @"Box", 1)), @"#4EC9B0");     // syntax.class
            GCHECK_EQ_STR(ColorAt(c, Nth(s, @"width", 3)), @"#9CDCFE");   // syntax.property
            GCHECK_EQ_STR(ColorAt(c, Nth(s, @"LIMIT", 1)), @"#4FC1FF");   // syntax.constant
            GCHECK_EQ_STR(ColorAt(c, Nth(s, @"grow", 0)), kFunction);
        });
    } else {
        MCGuiStep(@"basedpyright is not installed: the scripted server only", ^{
            MCGuiNote(@"basedpyright-langserver not found");
        });
    }

    // ------------------------------------------------------------- the cost
    NSString *big = MCGuiProjectFile(@"semantic/big.py");
    const int kLines = 20000;
    MCGuiStep(@"a 20,000-line file", ^{
        NSMutableString *t = [NSMutableString string];
        for (int k = 0; k < kLines; k++)
            [t appendFormat:@"value_%d = other_%d + third_%d\n", k, k, k];
        MCGuiWriteFile(big, t);
        // No server for this one: the spans are handed over directly.
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = false\n%@", kSettings]);
        GCHECK([c showFile:big]);
    });
    MCGuiStep(@"60,000 spans handed to the editor, then a scroll", ^{
        NSTextView *tv = Text(c);
        NSString *s = tv.string;
        std::vector<Lsp::SemanticSpan> spans;
        spans.reserve(kLines * 3);
        NSUInteger lineStart = 0;
        for (int k = 0; k < kLines; k++) {
            NSRange line = [s lineRangeForRange:NSMakeRange(lineStart, 0)];
            NSString *text = [s substringWithRange:line];
            NSRange eq = [text rangeOfString:@" = "], plus = [text rangeOfString:@" + "];
            const uint32_t a = (uint32_t)line.location;
            spans.push_back({a, (uint32_t)eq.location, Lsp::SemanticStyle::Variable});
            spans.push_back({a + (uint32_t)NSMaxRange(eq),
                             (uint32_t)(plus.location - NSMaxRange(eq)),
                             Lsp::SemanticStyle::Parameter});
            spans.push_back({a + (uint32_t)NSMaxRange(plus),
                             (uint32_t)(line.length - 1 - NSMaxRange(plus)),
                             Lsp::SemanticStyle::Constant});
            lineStart = NSMaxRange(line);
        }
        NSData *d = [NSData dataWithBytes:spans.data()
                                   length:spans.size() * sizeof(Lsp::SemanticSpan)];
        double t0 = ThreadCpu();
        [c performSelector:@selector(semanticTokensArrived:) withObject:d];
        const double arrived = ThreadCpu() - t0;
        GCHECK_EQ_STR(ColorAt(c, 0), kVariable);
        GCHECK_EQ_STR(ColorAt(c, Nth(s, @"other_0", 0)), kParameter);
        // Far down, unpainted until scrolled to.
        const NSUInteger far = Nth(s, @"value_15000 ", 0);
        GCHECK_EQ_STR(ColorAt(c, far), kPlain);
        // The scroll lays the text out down to there (TextKit's cost, not
        // the tokens'); the paint it schedules is timed on its own.
        [tv scrollRangeToVisible:NSMakeRange(far, 1)];
        (void)Visible(c);
        t0 = ThreadCpu();
        [c performSelector:@selector(paintSemanticVisible)];
        const double scrolled = ThreadCpu() - t0;
        GCHECK_EQ_STR(ColorAt(c, far), kVariable);
        NSIndexSet *painted = [c valueForKey:@"semanticPainted"];
        MCGuiNote(@"60,000 spans: %.1f ms to take them and paint the screen, %.1f ms to paint "
                  @"a screen scrolled to (CPU); %lu characters painted of %lu",
                  arrived * 1000, scrolled * 1000, (unsigned long)painted.count,
                  (unsigned long)s.length);
        GCHECK(painted.count < s.length / 20);   // the screens shown, not the file
        GCHECK(arrived < 0.05);
        GCHECK(scrolled < 0.05);
    });
    MCGuiStep(@"done", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        [c closeAllTabs:nil];
    });
    MCGuiWait(@"the server is gone", 10, ^BOOL { return !Session(c).hasServer; });
}
