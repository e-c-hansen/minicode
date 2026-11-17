// SpellCheckSuite.mm — `make guitest SUITES=spell_check`: spell checking
// in the editor (editor.spell-check, SpellMarks.h over SpellCheck.h). In a
// Markdown file a misspelled word in a paragraph is marked, and the same
// word in a code span, a fenced block or a link's address is not. A Python
// file gets no marks with the default, prose-only; with on, its comment and
// its docstring are marked and its identifiers never. The right-click
// items offer the checker's correction, which goes in as one undo step;
// Ignore Spelling takes the word's marks away and keeps them away after the
// next check. NSTextView's own continuous checking stays off in the editor,
// and the commit message box checks its own text.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "ControlServer.h"
#import "Lsp.h"          // CodeTextView
#import "SpellMarks.h"

namespace {
CodeTextView *Text(EditorController *c) { return (CodeTextView *)[c valueForKey:@"textView"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }

// The marked words, as text.
NSArray<NSString *> *Marked(EditorController *c) {
    CodeTextView *tv = Text(c);
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (NSValue *v in tv.spellMarks.marks) {
        const NSRange r = v.rangeValue;
        if (NSMaxRange(r) <= tv.string.length) [out addObject:[tv.string substringWithRange:r]];
    }
    return out;
}

// Whether a mark covers any character of the first `what` in the text.
BOOL MarkOn(EditorController *c, NSString *what) {
    CodeTextView *tv = Text(c);
    const NSRange w = [tv.string rangeOfString:what];
    if (w.location == NSNotFound) return NO;
    for (NSValue *v in tv.spellMarks.marks)
        if (NSIntersectionRange(v.rangeValue, w).length) return YES;
    return NO;
}

NSUInteger Checks(EditorController *c) { return Text(c).spellMarks.checksDone; }

NSMenuItem *ItemTitled(NSArray<NSMenuItem *> *items, NSString *title) {
    for (NSMenuItem *it in items)
        if ([it.title isEqualToString:title]) return it;
    return nil;
}
}  // namespace

MC_GUI_SUITE(spell_check) {
    __block EditorController *c = nil;
    __block NSString *saved = nil;
    __block NSUInteger checks = 0;
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"spell_check"];
    NSString *md = [dir stringByAppendingPathComponent:@"notes.md"];
    NSString *py = [dir stringByAppendingPathComponent:@"tool.py"];
    NSString *mdText =
        @"# Notes\n"
        @"\n"
        @"This paragrafh has one mistake in it.\n"
        @"\n"
        @"Some `recieve` code and a [link to the page](http://exmaple.com/pagge).\n"
        @"\n"
        @"```python\n"
        @"def recieve(): pass\n"
        @"```\n";
    NSString *pyText =
        @"# A comment with a mispeled word in it.\n"
        @"def recieve_data(value):\n"
        @"    \"\"\"Return the recieved value to the caller.\"\"\"\n"
        @"    return value\n";

    MCGuiStep(@"a folder with a Markdown file and a Python file", ^{
        MCGuiWriteFile(md, mdText);
        MCGuiWriteFile(py, pyText);
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:dir];
        for (EditorController *w in MCGuiControllers())
            if ([w.rootPath isEqualToString:dir]) c = w;
        GCHECK(c != nil);
        MCGuiSetKeyWindow(c.window);
        saved = [NSString stringWithContentsOfFile:NSProcessInfo.processInfo.environment[
                                                       @"MINICODE_SETTINGS"]
                                          encoding:NSUTF8StringEncoding
                                             error:nil] ?: @"";
        GCHECK([c showFile:md]);
        // A Markdown file opens on its rendered page; the source is checked.
        if ([[c valueForKey:@"previewMode"] boolValue])
            [c performSelector:@selector(togglePreview:) withObject:nil];
        GCHECK(Text(c).editable);
        [Text(c) scrollRangeToVisible:NSMakeRange(0, 0)];
    });
    MCGuiWait(@"the Markdown file's prose is checked", 20, ^BOOL {
        return MarkOn(c, @"paragrafh");
    });
    MCGuiStep(@"its code span, fenced block and link address are not", ^{
        MCGuiNote(@"marked in the Markdown file: %@", [Marked(c) componentsJoinedByString:@", "]);
        GCHECK(MarkOn(c, @"paragrafh"));
        GCHECK(!MarkOn(c, @"recieve"));                 // the code span
        NSString *s = Text(c).string;
        const NSRange fence = [s rangeOfString:@"def recieve"];
        BOOL inFence = NO;
        for (NSValue *v in Text(c).spellMarks.marks)
            if (NSIntersectionRange(v.rangeValue, fence).length) inFence = YES;
        GCHECK(!inFence);
        GCHECK(!MarkOn(c, @"exmaple"));
        GCHECK(!MarkOn(c, @"pagge"));
        // AppKit's own checking stays off, even when asked.
        [Text(c) setContinuousSpellCheckingEnabled:YES];
        GCHECK(!Text(c).continuousSpellCheckingEnabled);
        MCGuiSnapshot(Text(c), @"spell_check_markdown");
    });

    MCGuiStep(@"a Python file, with the default prose-only", ^{
        checks = Checks(c);
        GCHECK([c showFile:py]);
        GCHECK(Text(c).editable);
        [Text(c) scrollRangeToVisible:NSMakeRange(0, 0)];
    });
    // A negative check: nothing is checked in code with prose-only, so
    // nothing may be marked in this time.
    MCGuiPause(1.5);
    MCGuiStep(@"no marks in code with prose-only", ^{
        GCHECK(Text(c).spellMarks.marks.count == 0);
        checks = Checks(c);
        MCGuiSetSettings([saved stringByAppendingString:@"\neditor.spell-check = on\n"]);
    });
    MCGuiWait(@"with on, its comment and docstring are checked", 20, ^BOOL {
        return MarkOn(c, @"mispeled") && MarkOn(c, @"recieved");
    });
    MCGuiStep(@"its identifiers and keywords never", ^{
        MCGuiNote(@"marked in the Python file: %@", [Marked(c) componentsJoinedByString:@", "]);
        GCHECK(!MarkOn(c, @"recieve_data"));
        GCHECK(!MarkOn(c, @"def "));
        MCGuiSnapshot(Text(c), @"spell_check_python");
    });

    MCGuiStep(@"the right-click items: the checker's correction, Learn, Ignore", ^{
        CodeTextView *tv = Text(c);
        const NSUInteger at = [tv.string rangeOfString:@"recieved"].location + 2;
        NSArray<NSMenuItem *> *items = [tv.spellMarks itemsAtIndex:at];
        NSMutableArray *titles = [NSMutableArray array];
        for (NSMenuItem *it in items) [titles addObject:it.title];
        MCGuiNote(@"items: %@", [titles componentsJoinedByString:@" | "]);
        GCHECK(ItemTitled(items, @"Learn Spelling") != nil);
        GCHECK(ItemTitled(items, @"Ignore Spelling") != nil);
        NSMenuItem *fix = ItemTitled(items, @"received");
        GCHECK(fix != nil);
        // None where no word is marked.
        GCHECK([tv.spellMarks itemsAtIndex:[tv.string rangeOfString:@"value"].location].count == 0);
        // The correction goes in as one step to undo.
        if (fix) {
            MCGuiUndoGroup(tv.undoManager, ^{
                [NSApp sendAction:fix.action to:fix.target from:fix];
            });
            GCHECK([tv.string containsString:@"Return the received value"]);
            [tv.undoManager undo];
            GCHECK([tv.string isEqualToString:pyText]);
        }
    });

    MCGuiStep(@"Ignore Spelling takes the word's marks away", ^{
        CodeTextView *tv = Text(c);
        const NSUInteger at = [tv.string rangeOfString:@"mispeled"].location + 1;
        NSMenuItem *ignore = ItemTitled([tv.spellMarks itemsAtIndex:at], @"Ignore Spelling");
        GCHECK(ignore != nil);
        if (ignore) [NSApp sendAction:ignore.action to:ignore.target from:ignore];
        GCHECK(!MarkOn(c, @"mispeled"));
        // An edit at the end, so the lines are checked again.
        checks = Checks(c);
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv replaceRanges:@[[NSValue valueWithRange:NSMakeRange(tv.string.length, 0)]]
                  withStrings:@[@"\n"]
                   actionName:@"Typing"];
        });
    });
    MCGuiWait(@"checked again after the edit", 20, ^BOOL {
        return Checks(c) > checks && MarkOn(c, @"recieved");
    });
    MCGuiStep(@"and the ignored word stays unmarked in this document", ^{
        GCHECK(!MarkOn(c, @"mispeled"));
        GCHECK(MarkOn(c, @"recieved"));
        [Text(c).undoManager undo];
    });

    MCGuiStep(@"the commit message box checks its own text; off turns it off", ^{
        NSTextView *box = [c.gitPanel valueForKey:@"message"];
        if (box) GCHECK(box.continuousSpellCheckingEnabled);
        MCGuiSetSettings([saved stringByAppendingString:@"\neditor.spell-check = off\n"]);
        if (box) GCHECK(!box.continuousSpellCheckingEnabled);
        GCHECK(Text(c).spellMarks.marks.count == 0);
    });

    MCGuiStep(@"put back: the settings, the window, the files", ^{
        MCGuiSetSettings(saved);
        while (Active(c)) GCHECK([c closeTab:Active(c) prompt:NO]);
        [c.window close];
        c = nil;
        MCGuiSetKeyWindow(nil);
        [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
    });
}
