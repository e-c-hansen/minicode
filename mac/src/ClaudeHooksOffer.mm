// ClaudeHooksOffer.mm — see ClaudeHooksOffer.h.
#import "ClaudeHooksOffer.h"
#import "Environment.h"
#import "AppSettings.h"
#import "EditorFont.h"
#import "InstallPanel.h"
#import "Lsp.h"
#import "Terminal.h"
#import "TerminalPanel.h"

#include "ClaudeHooks.h"
#include "ControlProtocol.h"
#include "Settings.h"
#include <ctime>
#include <string>

NSString *const MCClaudeHooksOfferKey = @"claude-hooks";

static NSString *HNS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}
static std::string HU8(NSString *s) { return s.UTF8String ? std::string(s.UTF8String) : ""; }

NSString *MCClaudeSettingsPath(void) {
    const std::string home = HU8(MCEnvironment()[@"HOME"] ?: NSHomeDirectory());
    return HNS(ClaudeHooks::settingsPath(getenv("CLAUDE_CONFIG_DIR"), home.c_str()));
}

NSString *MCClaudeHooksCommand(void) {
    if (MCFindProgram(@"minicode")) return HNS(ClaudeHooks::kCommand);
    // A copy of the app put somewhere by hand: no `minicode` on the PATH
    // Claude Code's hooks see, so the launcher in this bundle by its path.
    NSString *launcher = [NSBundle.mainBundle pathForResource:@"minicode" ofType:nil];
    BOOL dir = NO;
    if (!launcher || ![NSFileManager.defaultManager fileExistsAtPath:launcher isDirectory:&dir] || dir)
        return HNS(ClaudeHooks::kCommand);
    return HNS(ClaudeHooks::commandFor(HU8(launcher)));
}

static BOOL ReadSettings(std::string &text, NSString **error) {
    bool exists = false;
    std::string err;
    if (ClaudeHooks::readSettingsFile(HU8(MCClaudeSettingsPath()), text, exists, err)) return YES;
    if (error) *error = HNS(err);
    return NO;
}

BOOL MCClaudeHooksInstalled(void) {
    std::string text;
    return ReadSettings(text, nil) && ClaudeHooks::installed(text);
}

NSString *MCClaudeHooksPreview(void) {
    std::string text;
    if (!ReadSettings(text, nil)) return nil;
    const std::string command = HU8(MCClaudeHooksCommand());
    ClaudeHooks::MergeResult r = ClaudeHooks::merge(text, command);
    if (!r.ok || !r.changed) return nil;
    return HNS(ControlChannel::prettyJson(ClaudeHooks::configuration(command, r.added)));
}

// ------------------------------------------------------------- the question
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

// A read-only box of the JSON that goes in, so nothing is added unseen.
static NSView *PreviewBox(NSString *json) {
    NSFont *font = MCMonoFont(11, NSFontWeightRegular);
    NSUInteger lines = [json componentsSeparatedByString:@"\n"].count;
    CGFloat h = MIN(260, MAX(80, lines * (font.ascender - font.descender + font.leading + 1) + 12));
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

void MCInstallClaudeHooks(NSWindow *window, void (^done)(BOOL installed)) {
    NSString *path = MCClaudeSettingsPath();
    NSString *command = MCClaudeHooksCommand();
    std::string text;
    NSString *error = nil;
    if (!ReadSettings(text, &error)) {
        RunAlert(Notice(@"Claude Code's settings could not be read", error), window,
                 ^(NSModalResponse) { if (done) done(NO); });
        return;
    }
    ClaudeHooks::MergeResult plan = ClaudeHooks::merge(text, HU8(command));
    if (!plan.ok) {
        RunAlert(Notice(@"Claude Code's settings were left alone",
                        [NSString stringWithFormat:@"%@ was not changed: %@. Fix the file, then "
                                                    "try again.", Tilde(path), HNS(plan.error)]),
                 window, ^(NSModalResponse) { if (done) done(NO); });
        return;
    }
    if (!plan.changed) {
        RunAlert(Notice(@"The hooks are already on",
                        [NSString stringWithFormat:@"%@ already runs %@ for every hook minicode "
                                                    "needs; nothing was changed.", Tilde(path),
                                                    command]),
                 window, ^(NSModalResponse) { if (done) done(YES); });
        return;
    }
    NSAlert *ask = [[NSAlert alloc] init];
    ask.messageText = @"Add minicode's hooks to Claude Code's settings?";
    ask.informativeText = [NSString stringWithFormat:
        @"This adds the lines below to %@: Claude Code's %@ hooks then run `%@`, which marks the "
         "minicode terminal a session runs in when it waits for you or has finished. Your own "
         "settings and hooks stay as they are, and the file is backed up first.",
        Tilde(path), HNS(ClaudeHooks::eventList(plan.added)), command];
    ask.accessoryView =
        PreviewBox(HNS(ControlChannel::prettyJson(ClaudeHooks::configuration(HU8(command), plan.added))));
    [ask addButtonWithTitle:@"Add the Hooks"];   // 1000, Return
    [ask addButtonWithTitle:@"Cancel"];          // 1001, Esc
    ask.buttons[1].keyEquivalent = @"\033";
    RunAlert(ask, window, ^(NSModalResponse answer) {
        if (answer != NSAlertFirstButtonReturn) {
            if (done) done(NO);
            return;
        }
        // Read and merged again: the file may have changed while the
        // question was up.
        ClaudeHooks::InstallResult r =
            ClaudeHooks::install(HU8(path), HU8(command), (long)time(nullptr));
        NSAlert *told;
        if (!r.ok) {
            told = Notice(@"The hooks were not added", HNS(r.error));
        } else if (!r.changed) {
            told = Notice(@"The hooks are already on",
                          [NSString stringWithFormat:@"%@ already has them; nothing was changed.",
                                                     Tilde(path)]);
        } else {
            NSString *backup = r.backup.empty() ? @"It is a new file, so there was nothing to back up."
                : [NSString stringWithFormat:@"The file as it was is in %@.", Tilde(HNS(r.backup))];
            told = Notice(@"The hooks are on",
                          [NSString stringWithFormat:@"Written to %@. %@ Claude Code sessions "
                                                      "started from now on in minicode's terminals "
                                                      "mark their tabs: orange while one waits for "
                                                      "you, green when it has finished.",
                                                      Tilde(path), backup]);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            RunAlert(told, window, ^(NSModalResponse) { if (done) done(r.ok); });
        });
    });
}

// ------------------------------------------------------------------- the bar
static NSHashTable<MCInstallBar *> *Bars(void) {
    static NSHashTable *bars;
    if (!bars) bars = [NSHashTable weakObjectsHashTable];
    return bars;
}

static void ColorBar(MCInstallBar *bar) {
    AppSettings *cfg = [AppSettings shared];
    [bar setBackground:[cfg background:Surface::Sidebar] text:[cfg text:Surface::Sidebar]];
}

static void RemoveBars(void) {
    for (MCInstallBar *bar in Bars().allObjects) {
        for (NSView *v = bar.superview; v; v = v.superview)
            if ([v isKindOfClass:MCTerminalPanel.class]) {
                if (((MCTerminalPanel *)v).noticeBar == bar) ((MCTerminalPanel *)v).noticeBar = nil;
                break;
            }
        [bar removeFromSuperview];
    }
    [Bars() removeAllObjects];
}

static MCTerminalPanel *PanelOf(NSView *view) {
    for (NSView *v = view.superview; v; v = v.superview)
        if ([v isKindOfClass:MCTerminalPanel.class]) return (MCTerminalPanel *)v;
    return nil;
}

BOOL MCOfferClaudeHooksForTerminal(TerminalView *terminal) {
    MCTerminalPanel *panel = PanelOf(terminal);
    if (!panel || panel.noticeBar) return NO;
    if (MCInstallBarMuted(MCClaudeHooksOfferKey) || MCClaudeHooksInstalled()) return NO;
    if (!MCInstallBarTakeOffer(MCClaudeHooksOfferKey)) return NO;
    MCInstallBar *bar = [[MCInstallBar alloc] initWithFrame:NSMakeRect(0, 0, 400, kMCInstallBarHeight)];
    [bar showMessage:@"minicode can show which Claude Code session is waiting on you."
                 tip:@"Claude Code's hooks then put an orange dot on a terminal's tab while its "
                      "session waits for you and a green one when it has finished. Turn On shows "
                      "what goes into Claude Code's settings before anything is written. Not Now "
                      "stops this offer; minicode > Install Claude Code Hooks… and the Agents "
                      "panel keep it."
              button:@"Turn On…"
                mute:@"Not Now"
               close:@"Close (it stays closed until minicode starts again)"];
    ColorBar(bar);
    __weak MCInstallBar *weakBar = bar;
    bar.onInstall = ^(NSButton *sender) {
        (void)sender;
        MCInstallClaudeHooks(weakBar.window, ^(BOOL installed) {
            if (installed) RemoveBars();
        });
    };
    bar.onMute = ^{
        MCInstallBarSetMuted(MCClaudeHooksOfferKey, YES);
        RemoveBars();
    };
    bar.onClose = ^{ RemoveBars(); };
    [Bars() addObject:bar];
    panel.noticeBar = bar;
    return YES;
}

// ---------------------------------------------------------------- listening
@interface MCClaudeHooksOfferListener : NSObject
@end

@implementation MCClaudeHooksOfferListener
+ (void)load {
    static MCClaudeHooksOfferListener *listener;
    listener = [MCClaudeHooksOfferListener new];
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserver:listener selector:@selector(claudeStarted:)
               name:MCTerminalClaudeDidStartNotification object:nil];
    [nc addObserver:listener selector:@selector(settingsChanged:)
               name:MCSettingsDidChangeNotification object:nil];
}

- (void)claudeStarted:(NSNotification *)n {
    if ([n.object isKindOfClass:TerminalView.class]) MCOfferClaudeHooksForTerminal(n.object);
}

- (void)settingsChanged:(NSNotification *)n {
    (void)n;
    for (MCInstallBar *bar in Bars().allObjects) ColorBar(bar);
}
@end
