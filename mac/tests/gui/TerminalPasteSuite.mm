// TerminalPasteSuite.mm — `make guitest SUITES=terminal_paste`: pasting
// into the grid terminal and typing a password there.
//   - A bracketed paste cannot end its bracket early: the nested end mark
//     ("ESC[20" + "ESC[201~" + "1~") pasted at zsh's prompt leaves the
//     command in the line editor instead of running it.
//   - Several lines into a program that did not turn bracketed paste on
//     (cat) ask first, as VS Code does: Cancel sends nothing, Paste as One
//     Line takes the breaks out, Paste sends them; three lines ending in a
//     break are asked about as 3 lines, the preview showing all three with
//     no "…"; a command with one
//     trailing break goes in without it; terminal.multiline-paste-warning
//     = never and always. One line holding a control character (Ctrl+O,
//     ESC) asks too, with Paste and Cancel only; a Tab does not. The
//     question is a sheet on the terminal's window, and a paste while it
//     is up only beeps.
//   - Ctrl+C at zsh's prompt uses the prompt up: a command asked for at
//     once waits for the new prompt instead of losing its first bytes.
//   - Secure keyboard entry while `read -s` reads with echo off and the grid
//     has the keyboard, off when the keyboard leaves and when echo returns.
//   - Drops, as Terminal.app's: files type their paths quoted for the shell
//     (a space, a quote and a $ in the names), bracketed at zsh's prompt,
//     a space after each, with the copy cursor, and zsh reads them back
//     whole; the drop focuses the terminal it is on, with a split beside
//     it; a picture with no file goes to a PNG in $TMPDIR/Mutible-drop and
//     its path is typed; text is typed as a paste.
// The nonce after `exec bash` is checked in terminal_exec (LooseEndsSuite).
#import "Harness.h"
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#import "TerminalPanel.h"
#import <Carbon/Carbon.h>   // IsSecureEventInputEnabled
#include "TermDrop.h"

namespace {

TerminalGridView *Grid(TerminalView *t) { return [t valueForKey:@"grid"]; }

void Clip(NSString *text) {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:text forType:NSPasteboardTypeString];
}

// Edit > Paste with the grid as its target, as the menu sends it.
void Paste(TerminalView *t) { [NSApp sendAction:@selector(paste:) to:Grid(t) from:nil]; }

// The paste question was asked once more, as a sheet on `window`.
BOOL AskedOnSheet(NSUInteger sheetsBefore, NSWindow *window) {
    return MCGuiSheetsShown().count == sheetsBefore + 1 && MCGuiLastSheetWindow() == window;
}

NSString *Str(NSData *d) {
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"(not UTF-8)";
}

NSData *Bytes(NSString *text) { return [text dataUsingEncoding:NSUTF8StringEncoding]; }

NSString *Quoted(NSString *path) {
    return [NSString stringWithUTF8String:TermDrop::quotePath(path.fileSystemRepresentation).c_str()];
}

// A scratch pasteboard holding `objects`, as a drag's.
NSPasteboard *DragBoard(NSArray<id<NSPasteboardWriting>> *objects) {
    NSPasteboard *pb = [NSPasteboard pasteboardWithUniqueName];
    [pb clearContents];
    [pb writeObjects:objects];
    return pb;
}

}  // namespace

// What AppKit hands a drop's destination.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"
@interface MCTerminalSuiteDrag : NSObject <NSDraggingInfo>
@property(nonatomic, strong) NSPasteboard *board;
@end
@implementation MCTerminalSuiteDrag
- (id)draggingSource { return nil; }
- (NSPasteboard *)draggingPasteboard { return self.board; }
- (NSDragOperation)draggingSourceOperationMask { return NSDragOperationEvery; }
- (NSWindow *)draggingDestinationWindow { return nil; }
- (NSPoint)draggingLocation { return NSZeroPoint; }
- (NSInteger)draggingSequenceNumber { return 1; }
@end
#pragma clang diagnostic pop

namespace {

// The drag enters the grid (the operation it shows) and is dropped there,
// as AppKit asks: entered, prepare, perform. The operation entering, or
// NSDragOperationNone when the drop was refused.
NSDragOperation Drop(TerminalGridView *g, NSPasteboard *pb) {
    MCTerminalSuiteDrag *drag = [MCTerminalSuiteDrag new];
    drag.board = pb;
    const NSDragOperation op = [g draggingEntered:drag];
    if (op == NSDragOperationNone || ![g prepareForDragOperation:drag] ||
        ![g performDragOperation:drag])
        return NSDragOperationNone;
    return op;
}

NSString *OneLine(NSString *text) { return [[text componentsSeparatedByString:@"\n"] componentsJoinedByString:@""]; }

}  // namespace

@interface MCPasteState : NSObject
@property(nonatomic, strong) EditorController *c;
@property(nonatomic, strong) TerminalView *term, *other;
@property(nonatomic, copy) NSString *dir;
@property(nonatomic, strong) NSMutableData *sent;
@property(nonatomic) NSUInteger alerts, sheets;
@end
@implementation MCPasteState
@end

MC_GUI_SUITE(terminal_paste) {
    MCPasteState *s = [MCPasteState new];
    MCGuiStep(@"a terminal of its own, in the project folder", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        s.c = MCGuiController();
        [s.c newTerminal:nil];
        s.term = [s.c valueForKey:@"terminal"];
        GCHECK(s.term != nil && !s.term.lineMode);
        s.sent = [NSMutableData data];
        TerminalGridView *g = Grid(s.term);
        void (^forward)(NSData *) = g.onInput;
        g.onInput = ^(NSData *d) {
            [s.sent appendData:d];
            if (forward) forward(d);
        };
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return s.term.atPrompt && !s.term.busy; });
    MCGuiStep(@"cd", ^{
        [s.term runCommand:[NSString stringWithFormat:@"cd '%@'", MCGuiProject()]];
    });
    MCGuiWait(@"there, with bracketed paste on at the prompt", 10, ^BOOL {
        return s.term.atPrompt && [[s.term valueForKey:@"zleReady"] boolValue] &&
               Grid(s.term).screen->bracketedPaste();
    });

    // ---- the nested end mark
    MCGuiStep(@"paste the nested end mark at the prompt", ^{
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"\x1b[20\x1b[201~1~echo INJECTED-$((40+2))\r");
        Paste(s.term);
        NSString *sent = Str(s.sent);
        // One end mark, at the very end, and no ESC between the marks.
        GCHECK([sent hasPrefix:@"\x1b[200~"] && [sent hasSuffix:@"\x1b[201~"]);
        NSString *inside = sent.length >= 12 ? [sent substringWithRange:NSMakeRange(6, sent.length - 12)] : @"";
        GCHECK([inside rangeOfString:@"\x1b"].location == NSNotFound);
        GCHECK([inside containsString:@"␛[201~1~echo INJECTED"]);
        // Bracketed paste on: the lines are text, so nothing asked.
        GCHECK(MCGuiAlertsShown().count == s.alerts);
    });
    MCGuiPause(1.0);   // negative: the pasted line must not run
    MCGuiStep(@"nothing ran; the paste waits in the line editor", ^{
        NSString *recent = [s.term recentText:6];
        GCHECK(![recent containsString:@"INJECTED-42"]);
        if ([recent containsString:@"INJECTED-42"]) MCGuiNote(@"%@", recent);
        const char etx = 0x03;   // drop the line
        Grid(s.term).onInput([NSData dataWithBytes:&etx length:1]);
        // The prompt is used up until zsh's next B: a command asked for at
        // once waits for it, and is not typed into the line editor the
        // SIGINT is ending.
        GCHECK(![[s.term valueForKey:@"zleReady"] boolValue]);
        [s.term runCommand:@"echo after-ctrl-c-$((5*5))"];
    });
    MCGuiWait(@"the command asked for right after Ctrl+C runs whole", 10, ^BOOL {
        return [[s.term recentText:4] containsString:@"\nafter-ctrl-c-25"];
    });
    MCGuiWait(@"a fresh prompt", 10, ^BOOL {
        return s.term.atPrompt && !s.term.busy && [[s.term valueForKey:@"zleReady"] boolValue];
    });

    // ---- several lines into cat, which takes no bracketed paste
    MCGuiStep(@"cat", ^{ [s.term runCommand:@"cat"]; });
    MCGuiWait(@"cat runs, bracketed paste off", 10, ^BOOL {
        return s.term.busy && !Grid(s.term).screen->bracketedPaste();
    });
    MCGuiStep(@"three lines, Cancel: asked in a sheet on the window", ^{
        s.alerts = MCGuiAlertsShown().count;
        s.sheets = MCGuiSheetsShown().count;
        [s.sent setLength:0];
        Clip(@"one\ntwo\nthree");
        MCGuiAnswerAlerts(@[@(NSAlertThirdButtonReturn)]);
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts + 1);
        GCHECK(AskedOnSheet(s.sheets, s.c.window));
        GCHECK_EQ_STR(MCGuiSheetsShown().lastObject, @"Paste 3 lines into the terminal?");
        GCHECK(Grid(s.term).askingPaste);
        // A second paste while the sheet is up asks nothing more.
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts + 1);
    });
    MCGuiWait(@"answered", 5, ^BOOL { return !Grid(s.term).askingPaste; });
    MCGuiStep(@"nothing sent", ^{ GCHECK(s.sent.length == 0); });
    MCGuiStep(@"Paste as One Line", ^{
        [s.sent setLength:0];
        s.sheets = MCGuiSheetsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        Paste(s.term);
        GCHECK(AskedOnSheet(s.sheets, s.c.window));
    });
    MCGuiWait(@"the lines go as one", 5, ^BOOL { return [Str(s.sent) isEqualToString:@"onetwothree"]; });
    MCGuiStep(@"Paste", ^{
        [s.sent setLength:0];
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        Clip(@"alpha\r\nbeta");
        Paste(s.term);
    });
    MCGuiWait(@"the lines go as they are", 5, ^BOOL { return [Str(s.sent) isEqualToString:@"alpha\rbeta"]; });
    // Three copied lines that end in a line break are three lines, not four,
    // and the preview shows them, with no "…" as if more were hidden.
    MCGuiStep(@"three lines and a final break: 3 lines, the preview whole, Cancel", ^{
        [s.sent setLength:0];
        s.sheets = MCGuiSheetsShown().count;
        Clip(@"one\ntwo\nthree\n");
        MCGuiAnswerAlerts(@[@(NSAlertThirdButtonReturn)]);
        Paste(s.term);
        GCHECK(AskedOnSheet(s.sheets, s.c.window));
        GCHECK_EQ_STR(MCGuiSheetsShown().lastObject, @"Paste 3 lines into the terminal?");
        NSString *info = MCGuiLastAlertInformative();
        GCHECK([info hasSuffix:@"Preview:\none\ntwo\nthree"]);
        GCHECK([info rangeOfString:@"\u2026"].location == NSNotFound);
        if (![info hasSuffix:@"Preview:\none\ntwo\nthree"]) NSLog(@"informative: %@", info);
    });
    MCGuiWait(@"answered", 5, ^BOOL { return !Grid(s.term).askingPaste; });
    MCGuiStep(@"nothing sent", ^{ GCHECK(s.sent.length == 0); });
    // One line with a control character asks too (Ctrl+O at zsh's prompt
    // runs the line): Paste or Cancel, no Paste as One Line.
    MCGuiStep(@"one line with Ctrl+O, Cancel (the second of two buttons): nothing sent", ^{
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"echo hi\x0f");
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts + 1);
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject,
                      @"Paste text that contains control characters into the terminal?");
    });
    MCGuiWait(@"answered", 5, ^BOOL { return !Grid(s.term).askingPaste; });
    MCGuiStep(@"nothing sent", ^{ GCHECK(s.sent.length == 0); });
    MCGuiStep(@"one line with ESC, Paste", ^{
        [s.sent setLength:0];
        Clip(@"x\x1b");
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        Paste(s.term);
    });
    MCGuiWait(@"sent as it was", 5, ^BOOL { return [Str(s.sent) isEqualToString:@"x\x1b"]; });
    MCGuiStep(@"one line with a Tab: unasked", ^{
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"a\tb");
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts);
        GCHECK_EQ_STR(Str(s.sent), @"a\tb");
        const char nl = '\r';   // cat takes the line
        Grid(s.term).onInput([NSData dataWithBytes:&nl length:1]);
    });
    MCGuiStep(@"a command with its line break goes in without it, unasked", ^{
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"rm -rf build\n");
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts);
        GCHECK_EQ_STR(Str(s.sent), @"rm -rf build");
    });
    MCGuiStep(@"never: no question", ^{
        MCGuiSetSettings(@"lsp.enabled = false\nterminal.multiline-paste-warning = never\n");
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"x\ny\n");
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts);
        GCHECK_EQ_STR(Str(s.sent), @"x\ry\r");
    });
    // Ctrl+D alone: the line is empty, so cat reads the end of its input.
    // A Ctrl+C after it raced cat's exit: landing at the shell's precmd, it
    // cut the prompt mark off and no prompt was ever seen (CI's retry).
    MCGuiStep(@"cat ends", ^{
        const char eot = 0x04;
        Grid(s.term).onInput([NSData dataWithBytes:&eot length:1]);
    });
    MCGuiWait(@"the prompt", 10, ^BOOL {
        return s.term.atPrompt && !s.term.busy && [[s.term valueForKey:@"zleReady"] boolValue];
    });
    MCGuiStep(@"always: asks even at zsh's prompt", ^{
        MCGuiSetSettings(@"lsp.enabled = false\nterminal.multiline-paste-warning = always\n");
        GCHECK(Grid(s.term).screen->bracketedPaste());
        s.alerts = MCGuiAlertsShown().count;
        [s.sent setLength:0];
        Clip(@"echo a\necho b");
        MCGuiAnswerAlerts(@[@(NSAlertThirdButtonReturn)]);
        Paste(s.term);
        GCHECK(MCGuiAlertsShown().count == s.alerts + 1);
    });
    MCGuiWait(@"answered", 5, ^BOOL { return !Grid(s.term).askingPaste; });
    MCGuiStep(@"nothing sent", ^{
        GCHECK(s.sent.length == 0);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });

    // ---- drops
    MCGuiStep(@"two files dropped at zsh's prompt: quoted, bracketed, a space after each", ^{
        s.dir = [MCGuiProject() stringByAppendingPathComponent:@"drops"];
        NSString *a = [s.dir stringByAppendingPathComponent:@"shot one.png"];
        NSString *b = [s.dir stringByAppendingPathComponent:@"it's $x.png"];
        MCGuiWriteFile(a, @"x");
        MCGuiWriteFile(b, @"x");
        [s.c.window makeFirstResponder:nil];
        s.alerts = MCGuiAlertsShown().count;
        Grid(s.term).onInput(Bytes(@"printf '<%s>\\n' "));
        [s.sent setLength:0];
        GCHECK(Drop(Grid(s.term), DragBoard(@[[NSURL fileURLWithPath:a], [NSURL fileURLWithPath:b]])) ==
               NSDragOperationCopy);
        NSString *want = [NSString stringWithFormat:@"\x1b[200~%@/shot\\ one.png %@/it\\'s\\ \\$x.png \x1b[201~",
                                                    Quoted(s.dir), Quoted(s.dir)];
        GCHECK_EQ_STR(Str(s.sent), want);
        GCHECK(MCGuiAlertsShown().count == s.alerts);
        GCHECK(s.c.window.firstResponder == Grid(s.term));
        Grid(s.term).onInput(Bytes(@"\r"));
    });
    MCGuiWait(@"zsh reads each path back whole", 10, ^BOOL {
        NSString *out = OneLine([s.term recentText:8]);
        return [out containsString:@"/shot one.png>"] && [out containsString:@"/it's $x.png>"];
    });
    MCGuiWait(@"the prompt", 10, ^BOOL {
        return s.term.atPrompt && !s.term.busy && [[s.term valueForKey:@"zleReady"] boolValue];
    });
    MCGuiStep(@"split: the drop goes to the terminal it is on, which takes the keyboard", ^{
        MCTerminalPanel *panel = [s.c valueForKey:@"terminalPanel"];
        s.other = [panel splitActive];
        GCHECK(s.other != nil && s.other != s.term);
        [s.c.window makeFirstResponder:Grid(s.other)];
        GCHECK(panel.activeTerminal == s.other);
        [s.sent setLength:0];
        NSString *a = [s.dir stringByAppendingPathComponent:@"shot one.png"];
        GCHECK(Drop(Grid(s.term), DragBoard(@[[NSURL fileURLWithPath:a]])) == NSDragOperationCopy);
        GCHECK([Str(s.sent) containsString:@"/shot\\ one.png \x1b[201~"]);
        GCHECK(s.c.window.firstResponder == Grid(s.term));
        GCHECK(panel.activeTerminal == s.term);
        GCHECK([panel closeTerminal:s.other]);
        s.other = nil;
        const char etx = 0x03;   // drop the line
        Grid(s.term).onInput([NSData dataWithBytes:&etx length:1]);
    });
    MCGuiWait(@"a fresh prompt", 10, ^BOOL {
        return s.term.atPrompt && [[s.term valueForKey:@"zleReady"] boolValue];
    });
    MCGuiStep(@"text dropped: typed as a paste", ^{
        [s.sent setLength:0];
        GCHECK(Drop(Grid(s.term), DragBoard(@[@"echo dropped-$((6*7))"])) == NSDragOperationCopy);
        GCHECK_EQ_STR(Str(s.sent), @"\x1b[200~echo dropped-$((6*7))\x1b[201~");
        Grid(s.term).onInput(Bytes(@"\r"));
    });
    MCGuiWait(@"it runs", 10, ^BOOL { return [[s.term recentText:4] containsString:@"\ndropped-42"]; });
    MCGuiWait(@"the prompt", 10, ^BOOL {
        return s.term.atPrompt && !s.term.busy && [[s.term valueForKey:@"zleReady"] boolValue];
    });
    MCGuiStep(@"a picture with no file: written to a PNG in the drop folder, its path typed", ^{
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL pixelsWide:2 pixelsHigh:2 bitsPerSample:8 samplesPerPixel:4
                            hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                         bytesPerRow:0 bitsPerPixel:0];
        NSPasteboard *pb = [NSPasteboard pasteboardWithUniqueName];
        [pb clearContents];
        [pb setData:rep.TIFFRepresentation forType:NSPasteboardTypeTIFF];
        [s.sent setLength:0];
        GCHECK(Drop(Grid(s.term), pb) == NSDragOperationCopy);
        NSString *sent = Str(s.sent);
        NSString *folder = [TerminalGridView dropFolder];
        NSString *head = [NSString stringWithFormat:@"\x1b[200~%@/drop-", Quoted(folder)];
        GCHECK([sent hasPrefix:head] && [sent hasSuffix:@".png \x1b[201~"]);
        if (![sent hasPrefix:head]) MCGuiNote(@"sent %@, folder %@", sent, folder);
        NSString *name = [sent hasPrefix:head] && [sent hasSuffix:@".png \x1b[201~"]
            ? [sent substringWithRange:NSMakeRange(head.length - 5, sent.length - head.length + 5 - 7)]
            : @"";
        NSData *png = [NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:name]];
        const unsigned char magic[4] = {0x89, 'P', 'N', 'G'};
        GCHECK(png.length > 8 && memcmp(png.bytes, magic, 4) == 0);
        if (png.length <= 8) MCGuiNote(@"no PNG at %@ in %@", name, folder);
        const char etx = 0x03;   // drop the line
        Grid(s.term).onInput([NSData dataWithBytes:&etx length:1]);
    });
    MCGuiWait(@"a fresh prompt", 10, ^BOOL {
        return s.term.atPrompt && [[s.term valueForKey:@"zleReady"] boolValue];
    });

    // ---- secure keyboard entry
    // "a fresh prompt" above is the new one: the Ctrl+C clears zleReady
    // until zsh's next 133;B, so read -s is not typed into the line editor
    // the SIGINT is ending (its ESC [9999~ eaten, `[9999~read` run).
    MCGuiStep(@"read -s with the keyboard in the grid", ^{
        [s.c.window makeFirstResponder:Grid(s.term)];
        GCHECK(!Grid(s.term).secureInputOn);
        GCHECK([TerminalGridView secureInputViews] == 0);
        [s.term runCommand:@"read -s 'pw?Password: '; echo; echo got-${#pw}"];
    });
    MCGuiWait(@"secure entry on", 10, ^BOOL { return Grid(s.term).secureInputOn; });
    MCGuiStep(@"for the whole process, with the lock", ^{
        GCHECK(IsSecureEventInputEnabled());
        GCHECK([TerminalGridView secureInputViews] == 1);
        MCGuiSnapshot(Grid(s.term), @"terminal-secure-input");
        // The keyboard leaves the grid: off at once (the next turn).
        [s.c.window makeFirstResponder:nil];
    });
    MCGuiWait(@"off while the grid has no keyboard", 5, ^BOOL {
        return !Grid(s.term).secureInputOn && [TerminalGridView secureInputViews] == 0;
    });
    MCGuiStep(@"the keyboard back", ^{ [s.c.window makeFirstResponder:Grid(s.term)]; });
    MCGuiWait(@"on again", 5, ^BOOL { return Grid(s.term).secureInputOn; });
    MCGuiStep(@"the password typed", ^{
        Grid(s.term).onInput([@"secret\r" dataUsingEncoding:NSUTF8StringEncoding]);
    });
    MCGuiWait(@"read ends and secure entry goes off", 10, ^BOOL {
        return [[s.term recentText:4] containsString:@"\ngot-6"] && !Grid(s.term).secureInputOn &&
               [TerminalGridView secureInputViews] == 0;
    });
    MCGuiStep(@"the password was never echoed", ^{
        GCHECK(![[s.term recentText:6] containsString:@"secret"]);
    });
    MCGuiWait(@"the prompt", 10, ^BOOL { return s.term.atPrompt && !s.term.busy; });
    MCGuiStep(@"at the prompt, nothing is hidden; the terminal closes", ^{
        GCHECK(!Grid(s.term).hiddenInput && !Grid(s.term).secureInputOn);
        GCHECK([(MCTerminalPanel *)[s.c valueForKey:@"terminalPanel"] closeTerminal:s.term]);
    });
}
