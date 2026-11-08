// SnippetsSuite.mm — `make guitest SUITES=snippets`: snippet completions
// with tab stops (Snippet.h, SnippetSession.mm), against the scripted
// server (build/fakelsp), which offers
//
//   for ${1:i} in ${2:range(10)}:\n\t$0        (insertTextFormat 2)
//   ${1|alpha,beta,gamma|} = $1                 (a choice, mirrored)
//
// Accepting the first puts it in indented as the line (a tab at a line's
// start becomes the file's four spaces) with "i" selected; one Cmd+Z takes
// it all back. Typed over, the first stop holds what was typed; Tab goes to
// "range(10)", Tab again to the end of the indented body, where the session
// ends; undoing the typing and then the insertion goes back to the text as
// it was. Esc ends a session, and Tab then indents as usual. The choice
// opens the list with its options at both of its places (two carets), Tab
// takes the one the arrow chose at both, and Tab again goes to the end.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#import "MultiCaretView.h"
#import "SnippetSession.h"

namespace {

LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }
CodeTextView *Text(EditorController *c) { return (CodeTextView *)[c valueForKey:@"textView"]; }

// A key press's command (Tab, Shift+Tab, Esc) through the text view, as
// the key bindings deliver it. One that only moves the selection gets no
// undo group (an empty one would stay on top, which a real key press never
// leaves); one that edits gets its own.
void Command(NSTextView *tv, SEL sel) { [tv doCommandBySelector:sel]; }
void EditCommand(NSTextView *tv, SEL sel) {
    MCGuiUndoGroup(tv.undoManager, ^{ [tv doCommandBySelector:sel]; });
}

void Type(NSTextView *tv, NSString *keys) {
    for (NSUInteger i = 0; i < keys.length; i++) {
        NSString *ch = [keys substringWithRange:NSMakeRange(i, 1)];
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:ch replacementRange:NSMakeRange(NSNotFound, 0)];
        });
    }
}

NSString *Selected(NSTextView *tv) {
    NSRange r = tv.selectedRange;
    if (NSMaxRange(r) > tv.string.length) return @"(out of range)";
    return [tv.string substringWithRange:r];
}

NSString *const kOriginal = @"def f():\n    fo\n    pi\n";
NSString *const kForSnippet = @"for ${1:i} in ${2:range(10)}:\n\t$0";
NSString *const kChoiceSnippet = @"${1|alpha,beta,gamma|} = $1";

}  // namespace

MC_GUI_SUITE(snippets) {
    __block EditorController *c = nil;
    NSString *file = MCGuiProjectFile(@"snip/main.py");
    NSDictionary *script = @{
        @"name": @"fakelsp",
        @"rules": @[@{@"method": @"textDocument/completion", @"result": @{
            @"isIncomplete": @NO,
            @"items": @[@{@"label": @"for", @"kind": @15, @"filterText": @"for",
                          @"insertTextFormat": @2, @"insertText": kForSnippet},
                        @{@"label": @"pick", @"kind": @15, @"filterText": @"pick",
                          @"insertTextFormat": @2, @"insertText": kChoiceSnippet}]}}]};

    // The caret after `word` (on its own line) and the list asked for.
    void (^askAfter)(NSString *) = ^(NSString *word) {
        NSTextView *tv = Text(c);
        [c.window makeFirstResponder:tv];
        tv.selectedRange = NSMakeRange(NSMaxRange([tv.string rangeOfString:word]), 0);
        [Session(c) triggerCompletion];
    };
    BOOL (^listShows)(NSString *) = ^BOOL(NSString *label) {
        return Session(c).completionVisible &&
               [Session(c).completionSelectedLabel isEqualToString:label];
    };
    void (^accept)(void) = ^{
        NSTextView *tv = Text(c);
        MCGuiUndoGroup(tv.undoManager, ^{
            GCHECK([Session(c) handleCommand:@selector(insertTab:)]);
        });
    };

    MCGuiStep(@"a Python file and a server that sends snippets", ^{
        c = MCGuiController();
        MCGuiWriteFile(file, kOriginal);
        NSString *cmd = MCGuiFakeLsp(@"snip", script, nil, nil);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
        GCHECK([c showFile:file]);
    });
    MCGuiWait(@"the server has the file", 20, ^BOOL {
        return MCGuiFakeLspReceived(@"snip", @"textDocument/didOpen").count > 0;
    });
    MCGuiStep(@"it was told snippets are taken", ^{
        NSArray *init = MCGuiFakeLspReceived(@"snip", @"initialize");
        GCHECK(init.count == 1);
        GCHECK([init.firstObject[@"params"][@"capabilities"][@"textDocument"][@"completion"]
                    [@"completionItem"][@"snippetSupport"] boolValue]);
    });

    // ---------------------------------------- the insertion is one undo step
    MCGuiStep(@"ask at fo", ^{ askAfter(@"    fo"); });
    MCGuiWait(@"the list", 10, ^BOOL { return listShows(@"for"); });
    MCGuiStep(@"Tab: the snippet, indented, with i selected", ^{
        accept();
        CodeTextView *tv = Text(c);
        GCHECK_EQ_STR(tv.string, @"def f():\n    for i in range(10):\n        \n    pi\n");
        GCHECK(tv.snippets.active);
        GCHECK(tv.snippets.currentStopIndex == 1);
        GCHECK_EQ_STR(Selected(tv), @"i");
    });
    MCGuiStep(@"one Cmd+Z takes it all back and ends the session", ^{
        CodeTextView *tv = Text(c);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, kOriginal);
        GCHECK(!tv.snippets.active);
    });

    // ---------------------------------------- typing, Tab, Tab to the end
    MCGuiStep(@"again", ^{ askAfter(@"    fo"); });
    MCGuiWait(@"the list", 10, ^BOOL { return listShows(@"for"); });
    MCGuiStep(@"Tab, then type over the first stop", ^{
        accept();
        CodeTextView *tv = Text(c);
        Type(tv, @"key");
        GCHECK([tv.string containsString:@"    for key in range(10):\n"]);
        GCHECK(tv.snippets.active && tv.snippets.currentStopIndex == 1);
        NSRange key = [tv.string rangeOfString:@"key"];
        GCHECK(tv.snippets.currentRanges.count == 1 &&
               NSEqualRanges(tv.snippets.currentRanges[0].rangeValue, key));
    });
    MCGuiStep(@"Tab goes to the second stop", ^{
        CodeTextView *tv = Text(c);
        Command(tv, @selector(insertTab:));
        GCHECK(tv.snippets.active && tv.snippets.currentStopIndex == 2);
        GCHECK_EQ_STR(Selected(tv), @"range(10)");
        // Typed in the same editing session as an attribute change across
        // the line (AppKit's font fixing does that), so the session's
        // editedRange spans the whole line: the stops still follow the
        // replacement itself (in a whole run the session used to end
        // here). NSTextView puts the caret at the end of such an edited
        // range; it goes back after the d, where typing leaves it.
        NSTextStorage *ts = tv.textStorage;
        const NSRange line = [tv.string lineRangeForRange:tv.selectedRange];
        const NSUInteger at = tv.selectedRange.location;
        [ts beginEditing];
        [ts addAttribute:@"MCGuiProbe" value:@YES range:line];
        Type(tv, @"d");
        [ts endEditing];
        tv.selectedRange = NSMakeRange(at + 1, 0);
        GCHECK([tv.string containsString:@"    for key in d:\n"]);
        GCHECK(tv.snippets.active && tv.snippets.currentStopIndex == 2);
        GCHECK(tv.snippets.currentRanges.count == 1 &&
               NSEqualRanges(tv.snippets.currentRanges[0].rangeValue, NSMakeRange(at, 1)));
    });
    MCGuiStep(@"Shift+Tab goes back to the first, Tab forward again", ^{
        CodeTextView *tv = Text(c);
        Command(tv, @selector(insertBacktab:));
        GCHECK(tv.snippets.currentStopIndex == 1);
        GCHECK_EQ_STR(Selected(tv), @"key");
        Command(tv, @selector(insertTab:));
        GCHECK_EQ_STR(Selected(tv), @"d");
    });
    MCGuiStep(@"Tab again: the end of the body, and the session is over", ^{
        CodeTextView *tv = Text(c);
        Command(tv, @selector(insertTab:));
        GCHECK(!tv.snippets.active);
        NSString *expected = @"def f():\n    for key in d:\n        \n    pi\n";
        GCHECK_EQ_STR(tv.string, expected);
        NSUInteger body = NSMaxRange([expected rangeOfString:@"d:\n        "]);
        GCHECK(tv.selectedRange.location == body && tv.selectedRange.length == 0);
        // Typing at $0 is plain typing now.
        Type(tv, @"pass");
        GCHECK([tv.string containsString:@"        pass\n"]);
    });
    MCGuiStep(@"undo takes the typing back, then the snippet in one step", ^{
        CodeTextView *tv = Text(c);
        NSUInteger undos = 0;
        while (![tv.string isEqualToString:kOriginal] && undos < 20 && tv.undoManager.canUndo) {
            [tv.undoManager undo];
            undos++;
            if ([tv.string isEqualToString:kOriginal]) break;
            // Until the last step, the snippet is still there.
            GCHECK([tv.string containsString:@"    for "]);
        }
        GCHECK_EQ_STR(tv.string, kOriginal);
        MCGuiNote(@"%lu undo steps", (unsigned long)undos);
    });

    // ---------------------------------------- Esc, then Tab indents again
    MCGuiStep(@"once more", ^{ askAfter(@"    fo"); });
    MCGuiWait(@"the list", 10, ^BOOL { return listShows(@"for"); });
    MCGuiStep(@"Esc ends the session; Tab then indents", ^{
        accept();
        CodeTextView *tv = Text(c);
        GCHECK(tv.snippets.active);
        Command(tv, @selector(cancelOperation:));
        GCHECK(!tv.snippets.active);
        NSString *before = [tv.string copy];
        tv.selectedRange = NSMakeRange(NSMaxRange([tv.string rangeOfString:@"for i"]), 0);
        EditCommand(tv, @selector(insertTab:));
        GCHECK(tv.string.length > before.length);
        GCHECK([tv.string containsString:@"range(10)"]);
        GCHECK(tv.selectedRange.length == 0);
        [tv.undoManager undo];
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, kOriginal);
    });

    // ---------------------------------------- a click outside ends it
    MCGuiStep(@"and once more", ^{ askAfter(@"    fo"); });
    MCGuiWait(@"the list", 10, ^BOOL { return listShows(@"for"); });
    MCGuiStep(@"the caret moved out of the stop", ^{
        accept();
        CodeTextView *tv = Text(c);
        GCHECK(tv.snippets.active);
        tv.selectedRange = NSMakeRange(0, 0);
    });
    MCGuiWait(@"the session ends", 5, ^BOOL { return !Text(c).snippets.active; });
    MCGuiStep(@"and Tab is the editor's", ^{
        CodeTextView *tv = Text(c);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, kOriginal);
    });

    // ---------------------------------------- a choice at two places
    MCGuiStep(@"ask at pi", ^{ askAfter(@"    pi"); });
    MCGuiWait(@"the list", 10, ^BOOL { return listShows(@"pick"); });
    MCGuiStep(@"Tab: the first option at both places, and the options listed", ^{
        accept();
        CodeTextView *tv = Text(c);
        GCHECK([tv.string containsString:@"    alpha = alpha\n"]);
        GCHECK(tv.snippets.active && tv.snippets.currentStopIndex == 1);
        GCHECK(tv.multiCaretActive && tv.multiCarets.caretRanges.count == 2);
        NSArray *labels = Session(c).completionLabels;
        GCHECK([labels isEqualToArray:(@[@"alpha", @"beta", @"gamma"])]);
    });
    MCGuiStep(@"Down, then Tab takes beta at both", ^{
        CodeTextView *tv = Text(c);
        GCHECK([Session(c) handleCommand:@selector(moveDown:)]);
        GCHECK_EQ_STR(Session(c).completionSelectedLabel, @"beta");
        EditCommand(tv, @selector(insertTab:));
        GCHECK([tv.string containsString:@"    beta = beta\n"]);
        GCHECK(!Session(c).completionVisible);
        GCHECK(tv.snippets.active);
    });
    MCGuiStep(@"Tab goes to the end", ^{
        CodeTextView *tv = Text(c);
        Command(tv, @selector(insertTab:));
        GCHECK(!tv.snippets.active);
        NSUInteger end = NSMaxRange([tv.string rangeOfString:@"beta = beta"]);
        GCHECK(tv.selectedRange.location == end);
        GCHECK(!tv.multiCaretActive);
    });
    // Left as it was found, for the suites after this one: the text back,
    // no tabs, and the servers off (symbols reads a file's outline with no
    // server, and a scripted one still running answered it with nothing).
    MCGuiStep(@"servers off", ^{
        CodeTextView *tv = Text(c);
        while (tv.undoManager.canUndo && ![tv.string isEqualToString:kOriginal])
            [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, kOriginal);
        MCGuiSetSettings(@"lsp.enabled = false\n");
        [c closeAllTabs:nil];
    });
    MCGuiWait(@"the server is gone", 10, ^BOOL { return !Session(c).hasServer; });
}
