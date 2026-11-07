// ToolsConsentSuite.mm — `make guitest SUITES=tools_consent`: taking back a
// folder's answer about running its own tools (its .venv for the language
// servers, its .venv or node_modules for Format Document).
//
//   - Ask Again About This Folder's Tools, in the minicode menu (and so the
//     palette), is enabled only while the folder has an answer;
//   - from the menu, an Allow is forgotten: the servers stop using the
//     folder's environment and the bar over a Python file asks again;
//   - the Language Servers popover says what was answered, and its Ask
//     Again takes a Don't Allow back the same way;
//   - a question put off with × this launch comes back once an answer
//     given meanwhile (Format Document's) is taken back.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "InstallPanel.h"
#import "Lsp.h"
#import "LspPanel.h"
#import "TreeDetails.h"
#include <sys/stat.h>

namespace {

LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }

NSMenuItem *ItemWithAction(NSMenu *menu, SEL action) {
    for (NSMenuItem *item in menu.itemArray) {
        if (item.action == action) return item;
        if (item.submenu) {
            NSMenuItem *found = ItemWithAction(item.submenu, action);
            if (found) return found;
        }
    }
    return nil;
}

void Collect(NSView *view, NSMutableArray<NSView *> *out) {
    [out addObject:view];
    for (NSView *v in view.subviews) Collect(v, out);
}

NSButton *ButtonTitled(NSView *root, NSString *title) {
    NSMutableArray<NSView *> *all = [NSMutableArray array];
    Collect(root, all);
    for (NSView *v in all)
        if ([v isKindOfClass:NSButton.class] && [((NSButton *)v).title isEqualToString:title])
            return (NSButton *)v;
    return nil;
}

BOOL ShowsText(NSView *root, NSString *text) {
    NSMutableArray<NSView *> *all = [NSMutableArray array];
    Collect(root, all);
    for (NSView *v in all)
        if ([v isKindOfClass:NSTextField.class] &&
            [((NSTextField *)v).stringValue containsString:text])
            return YES;
    return NO;
}

NSButton *BarButton(EditorController *c, NSString *part) {
    for (NSView *v in c.installBar.subviews)
        if ([v isKindOfClass:NSButton.class] && v != c.installBar.installButton &&
            [((NSButton *)v).title containsString:part])
            return (NSButton *)v;
    return nil;
}

}  // namespace

MC_GUI_SUITE(tools_consent) {
    __block EditorController *c = nil;
    __block NSString *venv = nil, *py = nil, *other = nil;
    __block NSMenuItem *item = nil;
    NSString *marker = [MCGuiScratch() stringByAppendingPathComponent:@"tools-consent-server-ran"];
    NSString *(^answer)(void) = ^NSString * { return MCFolderToolsConsent(c.rootPath); };
    void (^showPython)(void) = ^{
        GCHECK([c showFile:other]);
        GCHECK([c showFile:py]);
    };
    BOOL (^barAsks)(void) = ^BOOL {
        return [c.installBar.consentEnvironment isEqualToString:@".venv"] &&
               !c.installBar.hidden;
    };

    MCGuiStep(@"a .venv whose bin holds the only Python server, and no answer yet", ^{
        c = MCGuiController();
        MCGuiNote(@"the window's folder: %@", c.rootPath);
        [MCAppDefaults() removeObjectForKey:@"MCPythonEnvironmentConsent"];
        venv = [c.rootPath stringByAppendingPathComponent:@".venv"];
        NSString *bin = [venv stringByAppendingPathComponent:@"bin"];
        NSString *pyExe = [bin stringByAppendingPathComponent:@"python"];
        NSString *srv = [bin stringByAppendingPathComponent:@"mc-tools-server"];
        MCGuiWriteFile(pyExe, @"#!/bin/sh\nexit 0\n");
        MCGuiWriteFile(srv, [NSString stringWithFormat:@"#!/bin/sh\ntouch '%@'\ncat > /dev/null\n",
                                                       marker]);
        chmod(pyExe.fileSystemRepresentation, 0755);
        chmod(srv.fileSystemRepresentation, 0755);
        py = [c.rootPath stringByAppendingPathComponent:@"tools_consent.py"];
        other = [c.rootPath stringByAppendingPathComponent:@"tools_consent.txt"];
        MCGuiWriteFile(py, @"x = 1\n");
        MCGuiWriteFile(other, @"notes\n");
        MCGuiSetSettings(@"lsp.enabled = true\nlsp.python = mc-tools-server\n"
                         @"lsp.python.also = off\n");
        item = ItemWithAction(NSApp.mainMenu, @selector(forgetFolderToolsAnswer:));
    });
    MCGuiPause(1.0);
    MCGuiStep(@"the command is in the minicode menu, off while there is no answer", ^{
        // An earlier suite (language) may have put the question off with ×
        // for this folder; an answer taken back brings it back.
        MCSetFolderToolsConsent(c.rootPath, NO);
        [c forgetFolderToolsAnswer:nil];
        GCHECK(item != nil);
        GCHECK_EQ_STR(item.title, @"Ask Again About This Folder's Tools");
        NSMenuItem *select = ItemWithAction(NSApp.mainMenu, @selector(selectPythonInterpreter:));
        GCHECK(select && item.menu == select.menu);
        GCHECK([item.menu indexOfItem:item] == [select.menu indexOfItem:select] + 1);
        GCHECK(item.keyEquivalent.length == 0);
        showPython();
        GCHECK(barAsks());
        GCHECK(answer() == nil);
        GCHECK(![c validateMenuItem:item]);
        GCHECK(!c.canForgetFolderToolsAnswer);
        NSView *popover = MCLanguageServersView(Session(c), nil);
        GCHECK(ButtonTitled(popover, @"Ask Again") == nil);
    });
    MCGuiStep(@"Use It", ^{ [c.installBar.installButton performClick:nil]; });
    MCGuiWait(@"the server from .venv runs", 10, ^BOOL {
        return [[NSFileManager defaultManager] fileExistsAtPath:marker];
    });
    MCGuiStep(@"allowed: the command is on, and the popover says so", ^{
        GCHECK_EQ_STR(answer(), @"allow");
        GCHECK([[Session(c) serverPythonInterpreterWithSource:nil] hasSuffix:@".venv/bin/python"]);
        GCHECK([c validateMenuItem:item]);
        NSView *popover = MCLanguageServersView(Session(c), nil);
        GCHECK(ShowsText(popover, @"you allowed them"));
        GCHECK(ButtonTitled(popover, @"Ask Again") != nil);
        MCGuiSnapshot(popover, @"tools-consent-popover");
    });
    MCGuiStep(@"Ask Again About This Folder's Tools, from the menu", ^{
        GCHECK([NSApp sendAction:item.action to:item.target from:item]);
    });
    MCGuiWait(@"the bar asks again", 5, barAsks);
    MCGuiStep(@"forgotten: no answer, the servers leave .venv alone, a note says so", ^{
        GCHECK(answer() == nil);
        GCHECK(Session(c).folderEnvironmentConsent == nil);
        GCHECK_EQ_STR(Session(c).folderEnvironmentAwaitingConsent, @".venv");
        NSString *source = nil;
        NSString *interp = [Session(c) serverPythonInterpreterWithSource:&source];
        GCHECK(![interp containsString:@".venv"]);
        GCHECK([source containsString:@"waits for your answer"]);
        NSString *note = [[c valueForKey:@"settingsLabel"] stringValue];
        MCGuiNote(@"the note: %@", note);
        GCHECK([note containsString:@"ask before running this folder's own tools"]);
        GCHECK(![c validateMenuItem:item]);
    });
    MCGuiStep(@"Don't Use It, then the popover's Ask Again", ^{
        NSButton *mute = BarButton(c, @"Use");
        GCHECK(mute != nil);
        [mute performClick:nil];
        GCHECK_EQ_STR(answer(), @"deny");
        GCHECK(c.installBar.hidden || c.installBar.consentEnvironment == nil);
        NSView *popover = MCLanguageServersView(Session(c), nil);
        GCHECK(ShowsText(popover, @"you said not to"));
        NSButton *again = ButtonTitled(popover, @"Ask Again");
        GCHECK(again != nil);
        [again performClick:nil];
    });
    MCGuiWait(@"the bar asks again", 5, barAsks);
    MCGuiStep(@"the Don't Allow is gone too", ^{
        GCHECK(answer() == nil);
        NSView *popover = MCLanguageServersView(Session(c), nil);
        GCHECK(ButtonTitled(popover, @"Ask Again") == nil);
    });
    MCGuiStep(@"×, then an answer from elsewhere (Format Document's Don't Allow)", ^{
        NSButton *close = BarButton(c, @"×");
        GCHECK(close != nil);
        [close performClick:nil];
        // The question is put off (the bar may offer the missing server instead).
        MCGuiNote(@"the bar: %@", c.installBar.hidden ? @"hidden" : c.installBar.text);
        GCHECK(c.installBar.hidden || c.installBar.consentEnvironment == nil);
        showPython();
        GCHECK(c.installBar.hidden || c.installBar.consentEnvironment == nil);
        MCSetFolderToolsConsent(c.rootPath, NO);
        showPython();
        GCHECK(c.installBar.hidden || c.installBar.consentEnvironment == nil);
        GCHECK([c validateMenuItem:item]);
        [c forgetFolderToolsAnswer:item];
    });
    MCGuiWait(@"taken back, the question put off with × comes back", 5, barAsks);
    MCGuiStep(@"tidy up: servers off, no .venv, no answer", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        [[NSFileManager defaultManager] removeItemAtPath:venv error:nil];
        [[NSFileManager defaultManager] removeItemAtPath:py error:nil];
        [[NSFileManager defaultManager] removeItemAtPath:other error:nil];
        [MCAppDefaults() removeObjectForKey:@"MCPythonEnvironmentConsent"];
    });
    MCGuiPause(1.0);
}
