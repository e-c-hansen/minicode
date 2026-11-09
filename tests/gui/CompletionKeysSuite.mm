// CompletionKeysSuite.mm — `make guitest SUITES=completion_keys`: Tab and
// Return take the completion list's selected item whenever the list shows,
// as VS Code's do (editor.acceptSuggestionOnEnter "on", Tab accepting),
// with every key pressed as the hardware makes it and handed to the editor
// as the window would (the key monitor and the menus first).
//
// Against the scripted server (build/fakelsp): Return on a list that came
// up by itself while typing; Tab; a word typed out in full that some item
// is (Return closes the list, a second Return is the new line); the arrows
// then Return; a letter and Tab pressed before the list was filtered for
// the letter (the item for the word as typed); Shift+Return and Esc (a new
// line, the list closed); inside auto-closed brackets; at a snippet's tab
// stop. Then, where it is installed, the same keys under basedpyright,
// whose lists are incomplete and asked for again at every letter.
#import "Harness.h"
#import "EditorController.h"
#import "Keymap.h"
#import "Lsp.h"
#import "SnippetSession.h"
#import <Carbon/Carbon.h>
#include <pwd.h>

namespace {

CodeTextView *Editor(EditorController *c) { return [c valueForKey:@"textView"]; }
LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }

CGEventFlags CGFlagsFor(NSEventModifierFlags f) {
    CGEventFlags g = 0;
    if (f & NSEventModifierFlagCommand) g |= kCGEventFlagMaskCommand;
    if (f & NSEventModifierFlagShift) g |= kCGEventFlagMaskShift;
    if (f & NSEventModifierFlagOption) g |= kCGEventFlagMaskAlternate;
    if (f & NSEventModifierFlagControl) g |= kCGEventFlagMaskControl;
    return g;
}

NSEvent *KeyEvent(CGKeyCode code, NSEventModifierFlags flags) {
    CGEventRef ce = CGEventCreateKeyboardEvent(NULL, code, true);
    CGEventSetFlags(ce, CGFlagsFor(flags));
    NSEvent *e = [NSEvent eventWithCGEvent:ce];
    CFRelease(ce);
    return e;
}

// A press as it reaches the editor: the key monitor, then the window's key
// equivalents and the menus, then the view's keyDown: (one undo group, as
// a key press is). NO when something before the view took it.
BOOL Press(NSWindow *w, CodeTextView *tv, CGKeyCode code, NSEventModifierFlags flags) {
    NSEvent *e = KeyEvent(code, flags);
    if (MCKeymapHandleKeyDown(e)) return NO;
    if (MCGuiPerformMenuKey(w, code, flags, nil, nil)) return NO;
    // The text input context is active only in a key window on screen, and
    // NSTextView interprets no key without it.
    [tv.inputContext activate];
    MCGuiUndoGroup(tv.undoManager, ^{ [tv keyDown:e]; });
    return YES;
}

// The US layout's key for a character this suite types.
BOOL KeyFor(unichar ch, CGKeyCode *code, BOOL *shift) {
    static const CGKeyCode letters[26] = {
        kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G,
        kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N,
        kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U,
        kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z};
    *shift = NO;
    if (ch >= 'a' && ch <= 'z') { *code = letters[ch - 'a']; return YES; }
    if (ch >= 'A' && ch <= 'Z') { *code = letters[ch - 'A']; *shift = YES; return YES; }
    switch (ch) {
    case '.': *code = kVK_ANSI_Period; return YES;
    case '(': *code = kVK_ANSI_9; *shift = YES; return YES;
    case '_': *code = kVK_ANSI_Minus; *shift = YES; return YES;
    case ' ': *code = kVK_Space; return YES;
    }
    return NO;
}

void TypeKeys(NSWindow *w, CodeTextView *tv, NSString *keys) {
    for (NSUInteger i = 0; i < keys.length; i++) {
        CGKeyCode code;
        BOOL shift;
        if (!GCHECK(KeyFor([keys characterAtIndex:i], &code, &shift))) return;
        GCHECK(Press(w, tv, code, shift ? NSEventModifierFlagShift : 0));
    }
}

NSString *LineAtCaret(NSTextView *tv) {
    NSString *s = tv.string;
    NSRange line = [s lineRangeForRange:NSMakeRange(MIN(tv.selectedRange.location, s.length), 0)];
    return [[s substringWithRange:line]
        stringByTrimmingCharactersInSet:[NSCharacterSet newlineCharacterSet]];
}

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

NSDictionary *Item(NSString *label, int kind, NSString *sort) {
    return @{@"label": label, @"kind": @(kind), @"sortText": sort};
}

NSString *const kText = @"import os\n\ndef main():\n    x = 1\n    \n";

}  // namespace

MC_GUI_SUITE(completion_keys) {
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    NSString *file = MCGuiProjectFile(@"keys/main.py");
    NSDictionary *script = @{
        @"name": @"fakelsp",
        @"rules": @[@{@"method": @"textDocument/completion", @"result": @{
            // Incomplete, as basedpyright's are: asked again at each letter.
            @"isIncomplete": @YES,
            @"items": @[Item(@"print", 3, @"a"), Item(@"property", 7, @"b"),
                        Item(@"process_time", 3, @"c"), Item(@"path", 6, @"d"),
                        Item(@"pathsep", 6, @"e"), Item(@"PathLike", 7, @"f"),
                        @{@"label": @"call", @"kind": @15, @"sortText": @"g",
                          @"insertTextFormat": @2, @"insertText": @"call(${1:arg})$0"}]}}]};

    // The file as it starts, the caret on the indented empty line.
    void (^fresh)(void) = ^{
        [c.window makeFirstResponder:tv];
        [Session(c) handleCommand:@selector(cancelOperation:)];
        [tv.snippets end];
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:kText replacementRange:NSMakeRange(0, tv.string.length)];
        });
        tv.selectedRange = NSMakeRange(kText.length - 1, 0);
    };
    BOOL (^shows)(NSString *) = ^BOOL(NSString *label) {
        return Session(c).completionVisible &&
               [Session(c).completionSelectedLabel isEqualToString:label];
    };
    BOOL (^key)(CGKeyCode, NSEventModifierFlags) = ^BOOL(CGKeyCode code, NSEventModifierFlags f) {
        return Press(c.window, tv, code, f);
    };

    MCGuiStep(@"a Python file and a server that completes", ^{
        c = MCGuiController();
        tv = Editor(c);
        MCGuiWriteFile(file, kText);
        NSString *cmd = MCGuiFakeLsp(@"keys", script, nil, nil);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
        GCHECK([c showFile:file]);
    });
    MCGuiWait(@"the server has the file", 20, ^BOOL {
        return MCGuiFakeLspReceived(@"keys", @"textDocument/didOpen").count > 0;
    });

    // ---------------------------------------------- Return, on its own list
    MCGuiStep(@"pr typed", ^{ fresh(); TypeKeys(c.window, tv, @"pr"); });
    MCGuiWait(@"the list came up by itself", 10, ^BOOL { return shows(@"print"); });
    MCGuiStep(@"Return takes print: no new line", ^{
        GCHECK(key(kVK_Return, 0));
        GCHECK_EQ_STR(LineAtCaret(tv), @"    print");
        GCHECK_EQ_STR(tv.string, @"import os\n\ndef main():\n    x = 1\n    print\n");
        GCHECK(!Session(c).completionVisible);
    });
    MCGuiStep(@"one Cmd+Z takes it back", ^{
        [tv.undoManager undo];
        GCHECK_EQ_STR(LineAtCaret(tv), @"    pr");
    });

    // ------------------------------------------------------------------ Tab
    MCGuiStep(@"pr typed again", ^{ fresh(); TypeKeys(c.window, tv, @"pr"); });
    MCGuiWait(@"the list", 10, ^BOOL { return shows(@"print"); });
    MCGuiStep(@"Tab takes print: no indentation", ^{
        GCHECK(key(kVK_Tab, 0));
        GCHECK_EQ_STR(LineAtCaret(tv), @"    print");
        GCHECK(!Session(c).completionVisible);
    });

    // --------------------------------------- a word typed out that is an item
    MCGuiStep(@"path typed out", ^{ fresh(); TypeKeys(c.window, tv, @"path"); });
    MCGuiWait(@"the list, path first", 10, ^BOOL { return shows(@"path"); });
    MCGuiStep(@"Return takes it (the text as it was) and closes the list", ^{
        GCHECK(key(kVK_Return, 0));
        GCHECK_EQ_STR(LineAtCaret(tv), @"    path");
        GCHECK(!Session(c).completionVisible);
        GCHECK(tv.selectedRange.location == [tv.string rangeOfString:@"path"].location + 4);
    });
    MCGuiStep(@"a second Return is the new line, indented", ^{
        GCHECK(key(kVK_Return, 0));
        GCHECK_EQ_STR(tv.string, @"import os\n\ndef main():\n    x = 1\n    path\n    \n");
    });

    // ------------------------------------------------------ arrows, Return
    MCGuiStep(@"pa typed", ^{ fresh(); TypeKeys(c.window, tv, @"pa"); });
    MCGuiWait(@"the list", 10, ^BOOL { return shows(@"path"); });
    MCGuiStep(@"Down, then Return takes pathsep", ^{
        GCHECK(key(kVK_DownArrow, 0));
        GCHECK(shows(@"pathsep"));
        GCHECK(key(kVK_Return, 0));
        GCHECK_EQ_STR(LineAtCaret(tv), @"    pathsep");
    });

    // ------------------------------- a letter and Tab before the list catches up
    MCGuiStep(@"pr typed once more", ^{ fresh(); TypeKeys(c.window, tv, @"pr"); });
    MCGuiWait(@"the list", 10, ^BOOL { return shows(@"print"); });
    MCGuiStep(@"o and Tab in one go: the item for pro, not print", ^{
        // No turn of the run loop between the two presses, as when a quick
        // typist's keys wait in the queue together.
        TypeKeys(c.window, tv, @"o");
        GCHECK(key(kVK_Tab, 0));
        GCHECK_EQ_STR(LineAtCaret(tv), @"    property");
    });
    MCGuiStep(@"and Return the same way", ^{ fresh(); TypeKeys(c.window, tv, @"pr"); });
    MCGuiWait(@"the list", 10, ^BOOL { return shows(@"print"); });
    MCGuiStep(@"o, c and Return in one go: process_time", ^{
        TypeKeys(c.window, tv, @"oc");
        GCHECK(key(kVK_Return, 0));
        GCHECK_EQ_STR(LineAtCaret(tv), @"    process_time");
    });

    // ------------------------------------------------ Shift+Return and Esc
    MCGuiStep(@"pr typed for Shift+Return", ^{ fresh(); TypeKeys(c.window, tv, @"pr"); });
    MCGuiWait(@"the list", 10, ^BOOL { return shows(@"print"); });
    MCGuiStep(@"Shift+Return is a new line and closes the list", ^{
        GCHECK(key(kVK_Return, NSEventModifierFlagShift));
        GCHECK(!Session(c).completionVisible);
        GCHECK([tv.string hasSuffix:@"    pr\n    \n"]);
    });
    MCGuiStep(@"pr typed for Esc", ^{ fresh(); TypeKeys(c.window, tv, @"pr"); });
    MCGuiWait(@"the list", 10, ^BOOL { return shows(@"print"); });
    MCGuiStep(@"Esc closes the list and leaves the text", ^{
        GCHECK(key(kVK_Escape, 0));
        GCHECK(!Session(c).completionVisible);
        GCHECK_EQ_STR(LineAtCaret(tv), @"    pr");
    });

    // ------------------------------------------- inside auto-closed brackets
    MCGuiStep(@"( then pr", ^{ fresh(); TypeKeys(c.window, tv, @"(pr"); });
    MCGuiWait(@"the list", 10, ^BOOL { return shows(@"print"); });
    MCGuiStep(@"Return takes print inside the pair", ^{
        GCHECK(key(kVK_Return, 0));
        GCHECK_EQ_STR(LineAtCaret(tv), @"    (print)");
        GCHECK(tv.selectedRange.location == [tv.string rangeOfString:@")"
                                                              options:NSBackwardsSearch].location);
    });

    // -------------------------------------------------- at a snippet's stop
    MCGuiStep(@"ca typed", ^{ fresh(); TypeKeys(c.window, tv, @"ca"); });
    MCGuiWait(@"the list", 10, ^BOOL { return shows(@"call"); });
    MCGuiStep(@"Tab takes the snippet: arg selected", ^{
        GCHECK(key(kVK_Tab, 0));
        GCHECK_EQ_STR(LineAtCaret(tv), @"    call(arg)");
        GCHECK(tv.snippets.active);
        TypeKeys(c.window, tv, @"pr");
    });
    MCGuiWait(@"the list at the stop", 10, ^BOOL { return shows(@"print"); });
    MCGuiStep(@"Return takes print there; Tab then leaves the stop", ^{
        GCHECK(key(kVK_Return, 0));
        GCHECK_EQ_STR(LineAtCaret(tv), @"    call(print)");
        GCHECK(key(kVK_Tab, 0));
        GCHECK(!tv.snippets.active);
        GCHECK(tv.selectedRange.location == [tv.string rangeOfString:@")"
                                                              options:NSBackwardsSearch].location + 1);
    });
    MCGuiStep(@"pr at a stop, Tab", ^{
        fresh();
        TypeKeys(c.window, tv, @"ca");
    });
    MCGuiWait(@"the list", 10, ^BOOL { return shows(@"call"); });
    MCGuiStep(@"Tab, then pr", ^{
        GCHECK(key(kVK_Tab, 0));
        TypeKeys(c.window, tv, @"pr");
    });
    MCGuiWait(@"the list at the stop", 10, ^BOOL { return shows(@"print"); });
    MCGuiStep(@"Tab takes print, not the next stop", ^{
        GCHECK(key(kVK_Tab, 0));
        GCHECK_EQ_STR(LineAtCaret(tv), @"    call(print)");
    });

    // ------------------------------------------------- a real basedpyright
    NSString *bp = BasedPyright();
    if (!bp) {
        MCGuiStep(@"basedpyright is not installed: only the scripted server", ^{
            MCGuiNote(@"basedpyright-langserver not found; the checks ran against fakelsp only");
        });
    } else {
        MCGuiStep(@"basedpyright on", ^{
            MCGuiSetSettings([NSString stringWithFormat:
                @"lsp.enabled = true\nlsp.python = %@ --stdio\nlsp.python.also = off\n", bp]);
            [c showFile:file];
        });
        MCGuiWait(@"basedpyright runs", 60, ^BOOL {
            LspSession *lsp = Session(c);
            return lsp.hasServer && [lsp.statusText containsString:@"basedpyright"] &&
                   ![lsp.statusText containsString:@"starting"];
        });
        MCGuiStep(@"pri typed", ^{ fresh(); TypeKeys(c.window, tv, @"pri"); });
        MCGuiWait(@"its list, print first", 30, ^BOOL { return shows(@"print"); });
        MCGuiStep(@"Return takes print", ^{
            MCGuiNote(@"basedpyright listed %@", Session(c).completionLabels);
            GCHECK(key(kVK_Return, 0));
            GCHECK_EQ_STR(LineAtCaret(tv), @"    print");
            GCHECK(!Session(c).completionVisible);
        });
        MCGuiStep(@"pri typed again", ^{ fresh(); TypeKeys(c.window, tv, @"pri"); });
        MCGuiWait(@"its list", 30, ^BOOL { return shows(@"print"); });
        MCGuiStep(@"Tab takes print", ^{
            GCHECK(key(kVK_Tab, 0));
            GCHECK_EQ_STR(LineAtCaret(tv), @"    print");
        });
        MCGuiStep(@"os.pat typed", ^{ fresh(); TypeKeys(c.window, tv, @"os.pat"); });
        MCGuiWait(@"os's members", 30, ^BOOL { return shows(@"path"); });
        MCGuiStep(@"h and Return in one go: os.path, no new line", ^{
            TypeKeys(c.window, tv, @"h");
            GCHECK(key(kVK_Return, 0));
            GCHECK_EQ_STR(LineAtCaret(tv), @"    os.path");
            GCHECK(!Session(c).completionVisible);
        });
    }
    MCGuiStep(@"language servers off again", ^{
        fresh();
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });
}
