// StatusLineOffer.mm — see StatusLineOffer.h.
#import "StatusLineOffer.h"
#import "ClaudeHooksOffer.h"
#import "EditorFont.h"
#import "Environment.h"
#import "Lsp.h"

#include "ClaudeHooks.h"
#include "ClaudeStatusLine.h"
#include "ControlProtocol.h"
#include <ctime>
#include <string>

static NSString *SNS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}
static std::string SU8(NSString *s) { return s.UTF8String ? std::string(s.UTF8String) : ""; }

NSString *MCUsageDir(void) {
    NSString *own = MCEnvironment()[@"MUTIBLE_USAGE_DIR"];
    const std::string home = SU8(MCEnvironment()[@"HOME"] ?: NSHomeDirectory());
    return SNS(ClaudeStatusLine::usageDir(own.length ? own.UTF8String : nullptr, home.c_str(),
                                          nullptr, true));
}

NSString *MCClaudeStatusLineCommand(void) {
    if (MCFindProgram(@"mutible")) return SNS(ClaudeStatusLine::kCommand);
    NSString *launcher = [NSBundle.mainBundle pathForResource:@"mutible" ofType:nil];
    BOOL dir = NO;
    if (!launcher || ![NSFileManager.defaultManager fileExistsAtPath:launcher isDirectory:&dir] || dir)
        return SNS(ClaudeStatusLine::kCommand);
    return SNS(ClaudeStatusLine::commandFor(SU8(launcher)));
}

static BOOL ReadSettings(std::string &text, NSString **error) {
    bool exists = false;
    std::string err;
    if (ClaudeHooks::readSettingsFile(SU8(MCClaudeSettingsPath()), text, exists, err)) return YES;
    if (error) *error = SNS(err);
    return NO;
}

BOOL MCClaudeStatusLineInstalled(void) {
    std::string text;
    return ReadSettings(text, nil) && ClaudeStatusLine::installed(text);
}

// ------------------------------------------------------------- the questions
static NSString *Tilde(NSString *path) { return [path stringByAbbreviatingWithTildeInPath]; }

static void RunAlert(NSAlert *a, NSWindow *window, void (^then)(NSModalResponse)) {
    if (window && !window.attachedSheet) {
        [a beginSheetModalForWindow:window completionHandler:then];
        return;
    }
    then([a runModal]);
}

static NSAlert *Notice(NSString *message, NSString *info) {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = message;
    a.informativeText = info ?: @"";
    [a addButtonWithTitle:@"OK"];
    return a;
}

// A read-only box of the JSON that goes in, so nothing is changed unseen.
static NSView *PreviewBox(NSString *json) {
    NSFont *font = MCMonoFont(11, NSFontWeightRegular);
    NSUInteger lines = [json componentsSeparatedByString:@"\n"].count;
    CGFloat h = MIN(200, MAX(64, lines * (font.ascender - font.descender + font.leading + 1) + 12));
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 440, h)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    NSTextView *text = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 440, h)];
    text.editable = NO;
    text.selectable = YES;
    text.font = font;
    text.string = json;
    text.textContainerInset = NSMakeSize(4, 4);
    text.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = text;
    return scroll;
}

static NSString *StatusLineJson(const Json &value) {
    Json root = Json::object();
    root.set("statusLine", value);
    return SNS(ControlChannel::prettyJson(root));
}

void MCInstallClaudeStatusLine(NSWindow *window, void (^done)(BOOL installed)) {
    NSString *path = MCClaudeSettingsPath();
    NSString *command = MCClaudeStatusLineCommand();
    std::string text;
    NSString *error = nil;
    if (!ReadSettings(text, &error)) {
        RunAlert(Notice(@"Claude Code's settings could not be read", error), window,
                 ^(NSModalResponse) { if (done) done(NO); });
        return;
    }
    ClaudeStatusLine::Plan plan = ClaudeStatusLine::planInstall(text, SU8(command));
    if (!plan.ok) {
        RunAlert(Notice(@"Claude Code's settings were left alone",
                        [NSString stringWithFormat:@"%@ was not changed: %@. Fix the file, then "
                                                    "try again.", Tilde(path), SNS(plan.error)]),
                 window, ^(NSModalResponse) { if (done) done(NO); });
        return;
    }
    if (!plan.changed) {
        RunAlert(Notice(@"Plan usage is already logged",
                        [NSString stringWithFormat:@"Claude Code's status line in %@ already runs "
                                                    "%@; nothing was changed.", Tilde(path), command]),
                 window, ^(NSModalResponse) { if (done) done(YES); });
        return;
    }
    NSString *chain = plan.previous.empty()
        ? @"You have no status line now, so it prints the model and both windows' usage."
        : [NSString stringWithFormat:@"It then runs your own status line, `%@`, with the same "
                                      "input and shows what it prints, as before.",
                                     SNS(plan.previous)];
    NSAlert *ask = [[NSAlert alloc] init];
    ask.messageText = @"Log Claude Code's plan usage for mutible's forecast?";
    ask.informativeText = [NSString stringWithFormat:
        @"This changes the status line in %@ to the lines below: Claude Code then runs `%@`, which "
         "notes the 5-hour and weekly usage Claude Code reports in mutible's own folder (%@). %@ "
         "Your other settings stay as they are, the file is backed up first, and Stop Logging "
         "puts it back.",
        Tilde(path), command, Tilde(MCUsageDir()), chain];
    ask.accessoryView = PreviewBox(StatusLineJson(plan.statusLine));
    [ask addButtonWithTitle:@"Log Plan Usage"];   // 1000, Return
    [ask addButtonWithTitle:@"Cancel"];           // 1001, Esc
    ask.buttons[1].keyEquivalent = @"\033";
    RunAlert(ask, window, ^(NSModalResponse answer) {
        if (answer != NSAlertFirstButtonReturn) {
            if (done) done(NO);
            return;
        }
        // Read and planned again inside: the file may have changed while
        // the question was up.
        ClaudeStatusLine::Result r = ClaudeStatusLine::install(
            SU8(path), SU8(command), ClaudeStatusLine::recordPath(SU8(MCUsageDir())), (long)time(nullptr));
        NSAlert *told;
        if (!r.ok) {
            told = Notice(@"The status line was not changed", SNS(r.error));
        } else if (!r.changed) {
            told = Notice(@"Plan usage is already logged",
                          [NSString stringWithFormat:@"%@ already has it; nothing was changed.",
                                                     Tilde(path)]);
        } else {
            NSString *backup = r.backup.empty() ? @"It is a new file, so there was nothing to back up."
                : [NSString stringWithFormat:@"The file as it was is in %@.", Tilde(SNS(r.backup))];
            told = Notice(@"Plan usage is logged",
                          [NSString stringWithFormat:@"Written to %@. %@ The forecast fills in "
                                                      "after Claude Code's next reply.",
                                                      Tilde(path), backup]);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            RunAlert(told, window, ^(NSModalResponse) { if (done) done(r.ok); });
        });
    });
}

void MCUninstallClaudeStatusLine(NSWindow *window, void (^done)(BOOL installed)) {
    NSString *path = MCClaudeSettingsPath();
    std::string text;
    NSString *error = nil;
    if (!ReadSettings(text, &error)) {
        RunAlert(Notice(@"Claude Code's settings could not be read", error), window,
                 ^(NSModalResponse) { if (done) done(YES); });
        return;
    }
    ClaudeStatusLine::Plan plan = ClaudeStatusLine::planUninstall(text);
    if (!plan.ok || !plan.changed) {
        RunAlert(Notice(@"Plan usage is not logged",
                        [NSString stringWithFormat:@"Claude Code's status line in %@ does not run "
                                                    "mutible; nothing was changed.", Tilde(path)]),
                 window, ^(NSModalResponse) { if (done) done(NO); });
        return;
    }
    NSString *back = plan.previous.empty()
        ? @"You had no status line before, so it is removed."
        : [NSString stringWithFormat:@"Your own status line, `%@`, runs as it did.", SNS(plan.previous)];
    NSAlert *ask = [[NSAlert alloc] init];
    ask.messageText = @"Stop logging plan usage?";
    ask.informativeText = [NSString stringWithFormat:
        @"This puts Claude Code's status line in %@ back as it was before mutible changed it. %@ "
         "The readings logged so far stay in %@.",
        Tilde(path), back, Tilde(MCUsageDir())];
    [ask addButtonWithTitle:@"Put It Back"];   // 1000, Return
    [ask addButtonWithTitle:@"Cancel"];        // 1001, Esc
    ask.buttons[1].keyEquivalent = @"\033";
    RunAlert(ask, window, ^(NSModalResponse answer) {
        if (answer != NSAlertFirstButtonReturn) {
            if (done) done(YES);
            return;
        }
        ClaudeStatusLine::Result r = ClaudeStatusLine::uninstall(
            SU8(path), ClaudeStatusLine::recordPath(SU8(MCUsageDir())), (long)time(nullptr));
        NSAlert *told;
        if (!r.ok) {
            told = Notice(@"The status line was not changed", SNS(r.error));
        } else {
            NSString *how = r.exact ? @"The file is exactly as it was before."
                : r.backup.empty() ? @""
                : [NSString stringWithFormat:@"The file before this change is in %@.", Tilde(SNS(r.backup))];
            told = Notice(@"The status line is back as it was",
                          [NSString stringWithFormat:@"Written to %@. %@", Tilde(path), how]);
        }
        const BOOL still = !r.ok;
        dispatch_async(dispatch_get_main_queue(), ^{
            RunAlert(told, window, ^(NSModalResponse) { if (done) done(still); });
        });
    });
}
