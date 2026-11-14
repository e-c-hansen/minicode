// AgentPanelSuite.mm — the inline agent's bar (Cmd+I, AgentPanel.mm) with a
// stand-in for Claude Code: agent.command points at a script that prints a
// recorded stream (tests/fixtures/claude/stream) a line at a time, slowly,
// so the bar can be seen showing the reply before it ends. Claude Code
// itself never runs here.
//
//   make guitest SUITES=agent_panel
//
// Checked: the run asks for stream-json; the reply and what Claude is doing
// show while it is written; the edit waits as a diff in the bar with the
// lines it replaces tinted, the file untouched; Return keeps it as one undo
// step, Esc discards it; a second request builds on the edit waiting; a
// SEARCH part found in two places changes nothing and says so; an answer
// and a reported error from the stream.
#import "Harness.h"
#import "AgentPanel.h"
#import "EditorController.h"

#include <sys/stat.h>

namespace {

NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
MCAgentSession *Agent(EditorController *c) { return [c valueForKey:@"agentSession"]; }

NSString *Fixture(NSString *name) {
    return [MCGuiBuildDir().stringByDeletingLastPathComponent
        stringByAppendingPathComponent:[@"tests/fixtures/claude/stream" stringByAppendingPathComponent:name]];
}

// A stand-in agent: keeps its arguments and prompt, then prints `body`.
void WriteAgent(NSString *script, NSString *body) {
    NSString *dir = script.stringByDeletingLastPathComponent;
    MCGuiWriteFile(script, [NSString stringWithFormat:
        @"#!/bin/sh\nprintf '%%s\\n' \"$*\" > '%@/args.txt'\ncat > '%@/prompt.txt'\n%@\n",
        dir, dir, body]);
    chmod(script.fileSystemRepresentation, 0755);
}

// Prints a stream file a line at a time: quickly, but pausing after the
// tool call and before the result, so each stage stays up long enough to
// be seen.
NSString *SlowStream(NSString *file) {
    return [NSString stringWithFormat:
        @"while IFS= read -r line; do\n"
        @"  case \"$line\" in '{\"type\":\"result\"'*) sleep 1.5;; esac\n"
        @"  printf '%%s\\n' \"$line\"\n"
        @"  case \"$line\" in '{\"type\":\"assistant\"'*tool_use*) sleep 1.2;; *) sleep 0.03;; esac\n"
        @"done < '%@'", file];
}

NSString *PlainReply(NSString *reply) {
    NSString *quoted = [reply stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"];
    return [NSString stringWithFormat:@"printf '%%s' '%@'", quoted];
}

} // namespace

@interface MCAgentPanelState : NSObject
@property(nonatomic, strong) EditorController *c;
@property(nonatomic, strong) NSMutableArray<NSString *> *seen;   // messages shown while working
@end
@implementation MCAgentPanelState
@end

MC_GUI_SUITE(agent_panel) {
    MCAgentPanelState *s = [MCAgentPanelState new];
    s.seen = [NSMutableArray array];
    NSString *script = [MCGuiScratch() stringByAppendingPathComponent:@"agent-panel/fake-claude.sh"];
    NSString *file = MCGuiProjectFile(@"util.py");
    NSString *original = @"def double(x):\n    return x * 2\n";
    NSString *(^disk)(void) = ^NSString *(void) {
        return [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil];
    };
    // Waits for the run to end, writing down what the bar said meanwhile.
    void (^waitForReply)(NSString *) = ^(NSString *what) {
        MCGuiWait(what, 20, ^BOOL {
            MCAgentBar *bar = Agent(s.c).bar;
            if (bar.mode == MCAgentBarWorking) {
                if (bar.message.length && ![s.seen.lastObject isEqualToString:bar.message])
                    [s.seen addObject:bar.message];
                return NO;
            }
            return YES;
        });
    };
    void (^ask)(NSString *) = ^(NSString *request) {
        [s.seen removeAllObjects];
        [s.c focusEditor:nil];
        [s.c inlineAgent:nil];
        GCHECK(Agent(s.c).shown);
        [Agent(s.c) send:request];
        GCHECK(Agent(s.c).bar.mode == MCAgentBarWorking);
    };

    MCGuiStep(@"a file, and a stand-in agent that streams an edit slowly", ^{
        WriteAgent(script, SlowStream(Fixture(@"edit.jsonl")));
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = false\nagent.command = %@\n", script]);
        MCGuiWriteFile(file, original);
        s.c = MCGuiController();
        [s.c focusEditor:nil];
        GCHECK([s.c showFile:file]);
        Text(s.c).selectedRange = NSMakeRange([original rangeOfString:@"return"].location, 0);
        ask(@"triple it");
    });
    waitForReply(@"the streamed edit");
    MCGuiStep(@"the reply showed while it was written; the edit waits", ^{
        MCGuiNote(@"messages while working: %@", [s.seen componentsJoinedByString:@" | "]);
        NSString *args = [NSString stringWithContentsOfFile:
            [script.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"args.txt"]
                                                   encoding:NSUTF8StringEncoding error:nil];
        GCHECK([args containsString:@"--output-format stream-json --verbose --include-partial-messages"]);
        BOOL sawText = NO, sawTool = NO, sawEdit = NO;
        for (NSString *m in s.seen) {
            sawText |= [m containsString:@"I'll look at"];
            sawTool |= [m hasPrefix:@"Reading util.py…"];
            sawEdit |= [m containsString:@"Edit 1, in place of 1 line:\n    return x * 3"];
        }
        GCHECK(sawText);
        GCHECK(sawTool);
        GCHECK(sawEdit);
        MCAgentSession *agent = Agent(s.c);
        GCHECK(agent.bar.mode == MCAgentBarReview);
        GCHECK(agent.editPending);
        // Not in the file yet, nor on disk; the tab is clean.
        GCHECK_EQ_STR(Text(s.c).string, original);
        GCHECK_EQ_STR(disk(), original);
        GCHECK(![[s.c valueForKey:@"dirty"] boolValue]);
        GCHECK_EQ_STR(agent.pendingText, @"def double(x):\n    return x * 3\n");
        // The diff in the bar, old line then new.
        NSString *diff = agent.bar.attributedMessage.string;
        MCGuiNote(@"the bar: %@", diff);
        GCHECK([diff hasPrefix:@"The agent's change, 1 line, not made yet:\n"]);
        GCHECK([diff containsString:@"   2 -     return x * 2\n   2 +     return x * 3"]);
        // The line it replaces is tinted in the editor.
        NSLayoutManager *lm = Text(s.c).layoutManager;
        const NSUInteger at = [original rangeOfString:@"x * 2"].location;
        GCHECK([lm temporaryAttribute:NSBackgroundColorAttributeName atCharacterIndex:at
                       effectiveRange:NULL] != nil);
        GCHECK([lm temporaryAttribute:NSBackgroundColorAttributeName atCharacterIndex:0
                       effectiveRange:NULL] == nil);
        // Esc discards: nothing to put back.
        [agent.bar.textView doCommandBySelector:@selector(cancelOperation:)];
        GCHECK(!agent.shown && !agent.editPending);
        GCHECK_EQ_STR(Text(s.c).string, original);
        GCHECK(![[s.c valueForKey:@"dirty"] boolValue]);
        GCHECK([lm temporaryAttribute:NSBackgroundColorAttributeName atCharacterIndex:at
                       effectiveRange:NULL] == nil);
        GCHECK(![Text(s.c).undoManager canUndo]);
    });

    // Again, and Return keeps it: one undo step.
    MCGuiStep(@"again", ^{ ask(@"triple it"); });
    waitForReply(@"the second streamed edit");
    MCGuiStep(@"Return keeps it", ^{
        MCAgentSession *agent = Agent(s.c);
        GCHECK(agent.editPending);
        [agent.bar.textView doCommandBySelector:@selector(insertNewline:)];
        GCHECK(!agent.shown);
        GCHECK_EQ_STR(Text(s.c).string, @"def double(x):\n    return x * 3\n");
        GCHECK([[s.c valueForKey:@"dirty"] boolValue]);
    });
    MCGuiStep(@"one undo step takes it back", ^{
        [Text(s.c).undoManager undo];
        GCHECK_EQ_STR(Text(s.c).string, original);
    });

    // Two requests in one bar: the second is asked about the file with the
    // first edit made, and Keep makes both.
    MCGuiStep(@"a first request in a bar", ^{
        WriteAgent(script, PlainReply(@"<<<<<<< SEARCH\ndef double(x):\n=======\ndef double(x: int) -> int:\n>>>>>>> REPLACE\n"));
        ask(@"add types");
    });
    waitForReply(@"the first edit");
    MCGuiStep(@"it waits; a second request", ^{
        GCHECK(Agent(s.c).editPending);
        GCHECK_EQ_STR(Text(s.c).string, original);
        WriteAgent(script, PlainReply(@"<<<<<<< SEARCH\n    return x * 2\n=======\n    return x + x\n>>>>>>> REPLACE\n"));
        [Agent(s.c) send:@"no multiplication"];
    });
    waitForReply(@"the second edit");
    MCGuiStep(@"both wait, and the second was asked about the first's result", ^{
        NSString *prompt = [NSString stringWithContentsOfFile:
            [script.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"prompt.txt"]
                                                     encoding:NSUTF8StringEncoding error:nil];
        GCHECK([prompt containsString:@"def double(x: int) -> int:\n    return x * 2\n"]);
        GCHECK([prompt containsString:@"- add types"]);
        MCAgentSession *agent = Agent(s.c);
        GCHECK_EQ_STR(agent.pendingText, @"def double(x: int) -> int:\n    return x + x\n");
        GCHECK_EQ_STR(Text(s.c).string, original);
        GCHECK([agent.bar.attributedMessage.string hasPrefix:@"The agent's change, 2 lines"]);
        [agent keep];
        GCHECK_EQ_STR(Text(s.c).string, @"def double(x: int) -> int:\n    return x + x\n");
    });
    MCGuiStep(@"undone in one step", ^{
        [Text(s.c).undoManager undo];
        GCHECK_EQ_STR(Text(s.c).string, original);
    });

    // A SEARCH part in two places, the cursor in neither: nothing changes.
    NSString *twice = @"def double(x):\n    return x * 2\n\n\ndef twice(x):\n    return x * 2\n";
    NSString *file2 = MCGuiProjectFile(@"util2.py");
    MCGuiStep(@"an edit that matches two places", ^{
        MCGuiWriteFile(file2, twice);
        GCHECK([s.c showFile:file2]);
        GCHECK_EQ_STR(Text(s.c).string, twice);
        Text(s.c).selectedRange = NSMakeRange(0, 0);
        WriteAgent(script, PlainReply(@"<<<<<<< SEARCH\n    return x * 2\n=======\n    return x * 3\n>>>>>>> REPLACE\n"));
        ask(@"triple");
    });
    waitForReply(@"the ambiguous edit");
    MCGuiStep(@"refused, saying why", ^{
        MCAgentSession *agent = Agent(s.c);
        GCHECK(!agent.editPending && agent.bar.mode == MCAgentBarAnswer && agent.bar.messageIsError);
        GCHECK([agent.bar.message containsString:@"matches 2 places in the file, so nothing was changed"]);
        GCHECK_EQ_STR(Text(s.c).string, twice);
        [agent close];
    });
    // With the cursor in the second, that one.
    MCGuiStep(@"the cursor in one of them", ^{
        Text(s.c).selectedRange = NSMakeRange([twice rangeOfString:@"return" options:NSBackwardsSearch].location, 0);
        ask(@"triple");
    });
    waitForReply(@"the edit");
    MCGuiStep(@"that one", ^{
        GCHECK_EQ_STR(Agent(s.c).pendingText,
                      @"def double(x):\n    return x * 2\n\n\ndef twice(x):\n    return x * 3\n");
        [Agent(s.c) discard];
    });

    // AG-D1: Keep after the user typed elsewhere and moved the caret into
    // the other copy. The edit lands where the bar showed it, the second
    // copy, not where the caret is now.
    NSString *secondTripled = @"def double(x):\n    return x * 2\n\n\ndef twice(x):\n    return x * 3\n";
    MCGuiStep(@"the edit shown on the second copy", ^{
        Text(s.c).selectedRange = NSMakeRange([twice rangeOfString:@"return" options:NSBackwardsSearch].location, 0);
        ask(@"triple");
    });
    waitForReply(@"the edit on the second copy");
    MCGuiStep(@"typed elsewhere, the caret in the first copy, Keep: the second copy changes", ^{
        MCAgentSession *agent = Agent(s.c);
        GCHECK(agent.editPending);
        GCHECK_EQ_STR(agent.pendingText, secondTripled);
        NSTextView *tv = Text(s.c);
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"print(twice(2))\n" replacementRange:NSMakeRange(tv.string.length, 0)];
        });
        tv.selectedRange = NSMakeRange([twice rangeOfString:@"return"].location, 0);
        GCHECK(agent.editPending);
        [agent keep];
        GCHECK(!agent.shown);
        GCHECK_EQ_STR(tv.string, [secondTripled stringByAppendingString:@"print(twice(2))\n"]);
    });
    MCGuiStep(@"put back", ^{
        NSTextView *tv = Text(s.c);
        while (tv.undoManager.canUndo) [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, twice);
        tv.selectedRange = NSMakeRange([twice rangeOfString:@"return" options:NSBackwardsSearch].location, 0);
        ask(@"triple");
    });
    waitForReply(@"the edit on the second copy again");
    // The second copy deleted meanwhile: the only place left for the block
    // is the first copy, which the bar never showed changing. Keep shows
    // that change first; a second Return makes it.
    MCGuiStep(@"the second copy deleted, Keep: the new change is shown, nothing made", ^{
        MCAgentSession *agent = Agent(s.c);
        GCHECK(agent.editPending);
        NSTextView *tv = Text(s.c);
        const NSUInteger from = [twice rangeOfString:@"\n\n\ndef twice"].location + 1;
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"" replacementRange:NSMakeRange(from, tv.string.length - from)];
        });
        NSString *left = @"def double(x):\n    return x * 2\n";
        GCHECK_EQ_STR(tv.string, left);
        [agent keep];
        GCHECK(agent.shown && agent.editPending);
        GCHECK_EQ_STR(tv.string, left);
        GCHECK_EQ_STR(agent.pendingText, @"def double(x):\n    return x * 3\n");
        NSString *bar = agent.bar.attributedMessage.string;
        MCGuiNote(@"the bar: %@", bar);
        GCHECK([bar hasPrefix:@"The file changed while the edit waited, so it now makes this change, 1 line"]);
        GCHECK([bar containsString:@"   2 -     return x * 2\n   2 +     return x * 3"]);
        [agent keep];
        GCHECK(!agent.shown);
        GCHECK_EQ_STR(tv.string, @"def double(x):\n    return x * 3\n");
        while (tv.undoManager.canUndo) [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, twice);
    });

    // AG-D2: an edit that only adds lines replaces none, so the place they
    // go is marked: the line above underlined, its break tinted.
    MCGuiStep(@"an edit that only adds a line", ^{
        GCHECK([s.c showFile:file]);
        GCHECK_EQ_STR(Text(s.c).string, original);
        Text(s.c).selectedRange = NSMakeRange(4, 0);
        WriteAgent(script, PlainReply(@"<<<<<<< SEARCH\ndef double(x):\n=======\ndef double(x):\n    \"\"\"Twice x.\"\"\"\n>>>>>>> REPLACE\n"));
        ask(@"add a docstring");
    });
    waitForReply(@"the insertion");
    MCGuiStep(@"where the line goes is marked in the file", ^{
        MCAgentSession *agent = Agent(s.c);
        GCHECK(agent.editPending);
        GCHECK_EQ_STR(agent.pendingText, @"def double(x):\n    \"\"\"Twice x.\"\"\"\n    return x * 2\n");
        NSLayoutManager *lm = Text(s.c).layoutManager;
        const NSUInteger brk = [original rangeOfString:@"\n"].location;
        NSRange eff;
        id under = [lm temporaryAttribute:NSUnderlineStyleAttributeName atCharacterIndex:0
                           effectiveRange:&eff];
        GCHECK(under != nil && [under integerValue] != 0);
        GCHECK(NSEqualRanges(eff, NSMakeRange(0, brk)));
        GCHECK([lm temporaryAttribute:NSBackgroundColorAttributeName atCharacterIndex:brk
                       effectiveRange:NULL] != nil);
        // Nothing replaced, so no line is tinted as replaced.
        GCHECK([lm temporaryAttribute:NSBackgroundColorAttributeName atCharacterIndex:0
                       effectiveRange:NULL] == nil);
        GCHECK([lm temporaryAttribute:NSBackgroundColorAttributeName atCharacterIndex:brk + 5
                       effectiveRange:NULL] == nil);
        [agent discard];
        GCHECK([lm temporaryAttribute:NSUnderlineStyleAttributeName atCharacterIndex:0
                       effectiveRange:NULL] == nil);
        GCHECK([lm temporaryAttribute:NSBackgroundColorAttributeName atCharacterIndex:brk
                       effectiveRange:NULL] == nil);
        GCHECK([s.c showFile:file2]);   // the steps below ask about util2.py
    });

    // An answer, streamed.
    MCGuiStep(@"a question", ^{
        WriteAgent(script, SlowStream(Fixture(@"answer.jsonl")));
        ask(@"what does it do?");
    });
    waitForReply(@"the answer");
    MCGuiStep(@"the answer, the file as it was", ^{
        MCAgentSession *agent = Agent(s.c);
        GCHECK(agent.bar.mode == MCAgentBarAnswer && !agent.bar.messageIsError);
        GCHECK_EQ_STR(agent.bar.message, @"It doubles its argument.");
        BOOL partial = NO;
        for (NSString *m in s.seen) partial |= [m hasSuffix:@"\n\nIt doubles its argument."];
        MCGuiNote(@"messages while working: %@", [s.seen componentsJoinedByString:@" | "]);
        GCHECK(partial);
        GCHECK_EQ_STR(Text(s.c).string, twice);
        [agent close];
    });

    // An error Claude Code reports in its stream, with a failing status.
    MCGuiStep(@"a run that fails", ^{
        WriteAgent(script, [NSString stringWithFormat:@"cat '%@'\nexit 1", Fixture(@"error.jsonl")]);
        ask(@"anything");
    });
    waitForReply(@"the failure");
    MCGuiStep(@"its reason, not the JSON", ^{
        MCAgentSession *agent = Agent(s.c);
        GCHECK(agent.bar.messageIsError);
        GCHECK_EQ_STR(agent.bar.message, @"Credit balance is too low");
        [agent close];
    });

    // Esc while it works stops it.
    MCGuiStep(@"a slow agent; Esc stops it", ^{
        WriteAgent(script, @"sleep 30");
        ask(@"take your time");
        MCAgentSession *agent = Agent(s.c);
        GCHECK(agent.bar.mode == MCAgentBarWorking);
        [agent.bar.textView doCommandBySelector:@selector(cancelOperation:)];
        GCHECK(agent.bar.mode == MCAgentBarAnswer);
        GCHECK_EQ_STR(agent.bar.message, @"Stopped.");
        [agent close];
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });
}
