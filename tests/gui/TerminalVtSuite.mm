// TerminalVtSuite.mm — `make guitest SUITES=terminal_vt`: what programs ask
// of the terminal reaches the screen, the drawing and the keys.
//   - In a real zsh: `printf '\e[6 q'` turns the cursor into a bar (pixels:
//     the cell's left edge in the text color, its middle the background),
//     `\e[4 q` an underline, `\e[2 q` a block again; `\e[5 q` blinks it;
//     `\e[0 q` gives terminal.cursor-style's shape back.
//   - OSC 11 asked from the shell (printf, read back with read -d) answers
//     the light theme's background, as a program would learn it.
//   - zsh's own count of a flag, a heart, an Indic syllable and an emoji
//     (${(m)#x}) is the screen's code-point width (mode 2027 at the
//     prompt), and typing them at the prompt then Ctrl+A brings the cursor
//     back exactly to the prompt's end (zsh moves it back by its own count).
//   - On a grid of its own: a curly underline is drawn as a wave in its own
//     color (red pixels at several heights), a straight one at one height,
//     hidden text not at all; with the kitty keyboard flag pushed, Escape
//     and Ctrl+C reach the program as CSI u.
#import "Harness.h"
#import "AppSettings.h"
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#import <Carbon/Carbon.h>   // kVK_*
#include <memory>
#include <set>
#include <string>

namespace {

TerminalGridView *VtGrid(TerminalView *t) { return [t valueForKey:@"grid"]; }

NSBitmapImageRep *VtPicture(TerminalGridView *g) {
    NSBitmapImageRep *rep = [g bitmapImageRepForCachingDisplayInRect:g.bounds];
    [g cacheDisplayInRect:g.bounds toBitmapImageRep:rep];
    return rep;
}

// The color at a point of a picture of the grid (points from the top left).
NSColor *VtPixel(TerminalGridView *g, NSBitmapImageRep *rep, CGFloat x, CGFloat y) {
    CGFloat scale = rep.pixelsWide / NSWidth(g.bounds);
    return [[rep colorAtX:(NSInteger)(x * scale) y:(NSInteger)(y * scale)]
        colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
}

double VtDistance(NSColor *a, uint32_t rgb) {
    if (!a) return 9;
    return fabs(a.redComponent - ((rgb >> 16) & 0xFF) / 255.0) +
           fabs(a.greenComponent - ((rgb >> 8) & 0xFF) / 255.0) +
           fabs(a.blueComponent - (rgb & 0xFF) / 255.0);
}

uint32_t VtText() { return [AppSettings shared].settings.text(Surface::Terminal).rgb(); }
uint32_t VtBackground() { return [AppSettings shared].settings.background(Surface::Terminal).rgb(); }

// The cursor's cell in the grid's points.
NSRect VtCursorCell(TerminalGridView *g) {
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    const TerminalScreen *s = g.screen;
    return NSMakeRect(pad + s->cursorCol() * cs.width, pad + s->cursorRow() * cs.height,
                      cs.width, cs.height);
}

// Where the cursor is drawn: which of the cell's left edge, middle and
// bottom edge show the text color (the cursor) rather than the background.
struct VtCursorLook { bool left, middle, bottom; };
VtCursorLook VtLook(TerminalGridView *g) {
    NSBitmapImageRep *rep = VtPicture(g);
    NSRect c = VtCursorCell(g);
    // Cells in the default background draw nothing (the panel's layer
    // paints it), so they are see-through in the picture.
    auto lit = [&](CGFloat x, CGFloat y) {
        NSColor *p = VtPixel(g, rep, x, y);
        return p && p.alphaComponent > 0.5 && VtDistance(p, VtText()) < VtDistance(p, VtBackground());
    };
    return {lit(NSMinX(c) + 0.5, NSMidY(c)), lit(NSMidX(c) + 1, NSMidY(c) - 2),
            lit(NSMidX(c) + 1, NSMaxY(c) - 0.6)};
}

std::string VtScreenText(TerminalView *t) {
    const TerminalScreen *s = VtGrid(t).screen;
    std::string all;
    for (int l = -s->historySize(); l < s->rows(); l++) all += s->lineText(l) + "\n";
    return all;
}

int VtEnded(TerminalView *t) {
    int n = 0;
    for (const TermCommand &c : VtGrid(t).screen->commands()) n += c.ended;
    return n;
}

// A grid on a screen of its own in a window that is never shown.
struct VtRig {
    std::unique_ptr<TerminalScreen> screen;
    NSWindow *window = nil;
    TerminalGridView *grid = nil;
    std::string sent;
    VtRig(int cols, int rows) {
        screen = std::make_unique<TerminalScreen>(cols, rows);
        NSSize cs = [TerminalGridView cellSize];
        CGFloat pad = [TerminalGridView padding];
        NSRect frame = NSMakeRect(0, 0, cols * cs.width + 2 * pad, rows * cs.height + 2 * pad);
        window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskBorderless
                                               backing:NSBackingStoreBuffered defer:YES];
        window.releasedWhenClosed = NO;
        grid = [[TerminalGridView alloc] initWithFrame:frame];
        grid.screen = screen.get();
        grid.textInput = YES;
        std::string *out = &sent;
        grid.onInput = ^(NSData *bytes) { out->append((const char *)bytes.bytes, bytes.length); };
        [window.contentView addSubview:grid];
    }
    ~VtRig() {
        grid.onInput = nil;
        [grid removeFromSuperview];
        grid.screen = nullptr;
        [window close];
    }
    void Feed(const std::string &bytes) {
        screen->feed(bytes);
        [grid screenChanged];
    }
    void Key(CGKeyCode code, CGEventFlags flags) {
        CGEventRef ce = CGEventCreateKeyboardEvent(NULL, code, true);
        CGEventSetFlags(ce, flags);
        NSEvent *e = [NSEvent eventWithCGEvent:ce];
        CFRelease(ce);
        [grid keyDown:e];
    }
};

// The device pixel rows in row `r`'s lower part where `rgb` shows, over
// columns c0..c1 of a VtRig's grid.
std::set<long> VtRowsWithColor(TerminalGridView *g, int r, int c0, int c1, uint32_t rgb) {
    NSBitmapImageRep *rep = VtPicture(g);
    NSSize cs = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    CGFloat scale = rep.pixelsWide / NSWidth(g.bounds);
    std::set<long> ys;
    long y0 = lround((pad + r * cs.height + cs.height * 0.55) * scale);
    long y1 = lround((pad + (r + 1) * cs.height) * scale);
    long x0 = lround((pad + c0 * cs.width) * scale), x1 = lround((pad + c1 * cs.width) * scale);
    for (long y = y0; y < y1 && y < rep.pixelsHigh; y++)
        for (long x = x0; x < x1 && x < rep.pixelsWide; x++) {
            NSColor *p = [[rep colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
            if (VtDistance(p, rgb) < 0.35) { ys.insert(y); break; }
        }
    return ys;
}

}  // namespace

@interface MCVtState : NSObject
@property(nonatomic, strong) EditorController *c;
@property(nonatomic, strong) TerminalView *term;
@property(nonatomic) int ended;
@property(nonatomic) int promptCol;
@end
@implementation MCVtState
@end

MC_GUI_SUITE(terminal_vt) {
    MCVtState *s = [MCVtState new];
    MCGuiStep(@"a light theme, and a terminal of its own", ^{
        MCGuiSetSettings(@"lsp.enabled = false\ntheme = light-modern\n");
        s.c = MCGuiController();
        [s.c newTerminal:nil];
        s.term = [s.c valueForKey:@"terminal"];
        GCHECK(s.term != nil && !s.term.lineMode);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return s.term.atPrompt && !s.term.busy; });
    auto run = ^(NSString *what, NSString *command) {
        MCGuiStep(what, ^{
            s.ended = VtEnded(s.term);
            [s.term runCommand:command];
        });
        MCGuiWait([what stringByAppendingString:@": done"], 10, ^BOOL {
            return VtEnded(s.term) > s.ended && s.term.atPrompt &&
                   [[s.term valueForKey:@"zleReady"] boolValue];
        });
    };
    run(@"cd", [NSString stringWithFormat:@"cd '%@'", MCGuiProject()]);

    // Round 6's terminal defect 6: the integration's folder is this
    // process's own, not one every minicode shares.
    run(@"where the integration is", @"print -r -- \"ZD=${__minicode_zdotdir:t}=\"");
    MCGuiStep(@"a folder named for this process", ^{
        const std::string text = VtScreenText(s.term);
        const std::string want = "ZD=MiniCode-zsh-grid-" + std::to_string(getpid()) + "-";
        GCHECK(text.find(want) != std::string::npos);
        if (text.find(want) == std::string::npos) NSLog(@"wanted %s in:\n%s", want.c_str(), text.c_str());
    });

    // ------------------------------------------------------------ cursor
    run(@"printf a bar", @"printf '\\e[6 q'");
    MCGuiStep(@"a steady bar: the cell's left edge only", ^{
        TerminalGridView *g = VtGrid(s.term);
        [s.c.window makeFirstResponder:g];
        GCHECK(g.screen->cursorShape() == TermCursorShape::Bar && !g.screen->cursorBlinks());
        VtCursorLook look = VtLook(g);
        GCHECK(look.left && !look.middle);
        if (!look.left || look.middle) {
            NSLog(@"bar: left %d middle %d bottom %d, cursor %d,%d, first responder %@ key %d",
                  look.left, look.middle, look.bottom, g.screen->cursorRow(), g.screen->cursorCol(),
                  g.window.firstResponder, g.window.isKeyWindow);
            const char *dir = getenv("MC_VT_DUMP");
            if (dir) [[VtPicture(g) representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
                writeToFile:[NSString stringWithFormat:@"%s/bar.png", dir] atomically:YES];
        }
    });
    run(@"printf an underline", @"printf '\\e[4 q'");
    MCGuiStep(@"an underline: the bottom edge only", ^{
        TerminalGridView *g = VtGrid(s.term);
        GCHECK(g.screen->cursorShape() == TermCursorShape::Underline);
        VtCursorLook look = VtLook(g);
        GCHECK(look.bottom && !look.middle);
    });
    run(@"printf a block", @"printf '\\e[2 q'");
    MCGuiStep(@"a block: the middle too", ^{
        TerminalGridView *g = VtGrid(s.term);
        GCHECK(g.screen->cursorShape() == TermCursorShape::Block);
        VtCursorLook look = VtLook(g);
        GCHECK(look.middle && look.left);
    });
    run(@"printf a blinking bar", @"printf '\\e[5 q'");
    MCGuiWait(@"it blinks off", 5, ^BOOL {
        return [[VtGrid(s.term) valueForKey:@"blinkOff"] boolValue];
    });
    MCGuiStep(@"while off, no cursor in the cell", ^{
        VtCursorLook look = VtLook(VtGrid(s.term));
        GCHECK(!look.left && !look.middle);
    });
    MCGuiWait(@"and on again", 5, ^BOOL {
        return ![[VtGrid(s.term) valueForKey:@"blinkOff"] boolValue];
    });
    MCGuiStep(@"terminal.cursor-style = underline, and the program's choice dropped", ^{
        MCGuiSetSettings(@"lsp.enabled = false\ntheme = light-modern\nterminal.cursor-style = underline\n");
    });
    run(@"printf the default", @"printf '\\e[0 q'");
    MCGuiStep(@"the setting's underline, not blinking", ^{
        TerminalGridView *g = VtGrid(s.term);
        GCHECK(g.screen->cursorShape() == TermCursorShape::Default && !g.screen->cursorBlinks());
        GCHECK(![[g valueForKey:@"blinkOff"] boolValue]);
        VtCursorLook look = VtLook(g);
        GCHECK(look.bottom && !look.middle);
        MCGuiSetSettings(@"lsp.enabled = false\ntheme = light-modern\n");
    });

    // ------------------------------------------------------------- OSC 11
    run(@"ask for the background", @"printf '\\e]11;?\\a'; read -rs -t 3 -d $'\\a' r; print -r -- \"BG=${r#*;}\"");
    MCGuiStep(@"the light theme's background came back", ^{
        const uint32_t bg = VtBackground();
        char want[64];
        snprintf(want, sizeof want, "BG=rgb:%04x/%04x/%04x", ((bg >> 16) & 0xFF) * 257,
                 ((bg >> 8) & 0xFF) * 257, (bg & 0xFF) * 257);
        GCHECK(bg != 0x181818);                    // the theme took
        const std::string text = VtScreenText(s.term);
        GCHECK(text.find(want) != std::string::npos);
        if (text.find(want) == std::string::npos) NSLog(@"wanted %s in:\n%s", want, text.c_str());
    });

    // -------------------------------------------------------- mode 2027
    run(@"zsh counts columns", @"x=$'\\U1F1FA\\U1F1F8\\u2764\\u0915\\u093F\\U1F468'; print -r -- \"W${(m)#x}W\"");
    MCGuiStep(@"as termCodepointWidth does", ^{
        const uint32_t cps[] = {0x1F1FA, 0x1F1F8, 0x2764, 0x0915, 0x093F, 0x1F468};
        int want = 0;
        for (uint32_t cp : cps) want += termCodepointWidth(cp);
        std::string mark = "W" + std::to_string(want) + "W";
        const std::string text = VtScreenText(s.term);
        GCHECK(want == 8 && text.find(mark) != std::string::npos);
        if (text.find(mark) == std::string::npos) NSLog(@"wanted %s in:\n%s", mark.c_str(), text.c_str());
        s.promptCol = VtGrid(s.term).screen->cursorCol();
        GCHECK(!VtGrid(s.term).screen->clusterWidths());   // zsh is reading
    });
    MCGuiStep(@"type a flag, a heart and an emoji at the prompt", ^{
        GCHECK([s.term typeCommand:@"\U0001F1FA\U0001F1F8❤\U0001F468"]);
    });
    MCGuiWait(@"they show, 7 columns on", 5, ^BOOL {
        return VtGrid(s.term).screen->cursorCol() == s.promptCol + 7;
    });
    MCGuiStep(@"Ctrl+A", ^{
        VtGrid(s.term).onInput([NSData dataWithBytes:"\x01" length:1]);
    });
    MCGuiWait(@"the cursor is back at the prompt's end, where zsh put it", 5, ^BOOL {
        return VtGrid(s.term).screen->cursorCol() == s.promptCol;
    });
    MCGuiStep(@"Ctrl+K clears the line", ^{
        VtGrid(s.term).onInput([NSData dataWithBytes:"\x0b" length:1]);
    });
    MCGuiStep(@"close the terminal", ^{
        [s.term close];
    });

    // ------------------------------------------------- drawing and keys
    MCGuiStep(@"curly, straight and hidden underlines, in red", ^{
        VtRig rig(20, 4);
        rig.Feed("\x1b[4:3;58;2;255;0;0mcurly curly\x1b[m\r\n"
                 "\x1b[4;58:2::255:0:0mstraight st\x1b[m\r\n"
                 "\x1b[8;4;58;2;255;0;0mhiddenhidden\x1b[m");
        std::set<long> curly = VtRowsWithColor(rig.grid, 0, 0, 11, 0xFF0000);
        std::set<long> straight = VtRowsWithColor(rig.grid, 1, 0, 11, 0xFF0000);
        std::set<long> hidden = VtRowsWithColor(rig.grid, 2, 0, 12, 0xFF0000);
        const CGFloat scale = rig.window.backingScaleFactor > 0 ? rig.window.backingScaleFactor : 1;
        GCHECK(!curly.empty() && !straight.empty());
        // The wave spans more device rows than the straight line's thickness.
        long curlySpan = curly.empty() ? 0 : *curly.rbegin() - *curly.begin() + 1;
        long straightSpan = straight.empty() ? 0 : *straight.rbegin() - *straight.begin() + 1;
        GCHECK(curlySpan >= straightSpan + lround(scale));
        GCHECK(straightSpan <= lround(2 * scale));
        GCHECK(hidden.empty());
        NSLog(@"curly spans %ld device rows, straight %ld", curlySpan, straightSpan);
    });
    MCGuiStep(@"kitty keys: Escape and Ctrl+C as CSI u once the flag is pushed", ^{
        VtRig rig(20, 4);
        rig.Key(kVK_Escape, 0);
        GCHECK(rig.sent == "\x1b");
        rig.sent.clear();
        rig.Feed("\x1b[>1u");
        GCHECK(rig.screen->takeReplies().empty());
        rig.Key(kVK_Escape, 0);
        GCHECK(rig.sent == "\x1b[27u");
        rig.sent.clear();
        rig.Key(kVK_ANSI_C, kCGEventFlagMaskControl);
        GCHECK(rig.sent == "\x1b[99;5u");
        rig.sent.clear();
        rig.Key(kVK_ANSI_A, (CGEventFlags)(kCGEventFlagMaskControl | kCGEventFlagMaskShift));
        GCHECK(rig.sent == "\x1b[97;6u");
        rig.sent.clear();
        rig.Key(kVK_Return, 0);
        GCHECK(rig.sent == "\r");
        rig.sent.clear();
        rig.Feed("\x1b[<u");
        rig.Key(kVK_ANSI_C, kCGEventFlagMaskControl);
        GCHECK(rig.sent == "\x03");
    });
}
