// ServerConsentSuite.mm — `make guitest SUITES=server_consent`: a language
// server that would run the folder's own code waits for the folder's
// answer (Lsp::checkProjectCode), against the scripted server standing in
// for rust-analyzer (lsp.rust).
//
//   - unanswered: it starts with build scripts, proc macros and cargo check
//     off, in initializationOptions and the settings it is sent
//     (didChangeConfiguration, and what workspace/configuration answers),
//     and the bar over the Rust file asks, Allow and Don't Allow;
//   - Allow: it starts again with its defaults (nothing sent);
//   - the answer taken back: it starts again with the three off;
//   - a rust-toolchain.toml in the crate (rustup would run a toolchain it
//     names by path): it does not start at all, the status bar says it
//     waits, and the bar names the file; Allow starts it with its defaults.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"   // installBar
#import "InstallPanel.h"
#import "Lsp.h"

namespace {

LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }

NSArray<NSDictionary *> *Initializes(void) {
    return MCGuiFakeLspReceived(@"rust-consent", @"initialize");
}

// Build scripts, proc macros and cargo check all off in `options`.
BOOL AllOff(NSDictionary *options) {
    if (![options isKindOfClass:NSDictionary.class]) return NO;
    return [options[@"cargo"][@"buildScripts"][@"enable"] isEqual:@NO] &&
           [options[@"procMacro"][@"enable"] isEqual:@NO] && [options[@"checkOnSave"] isEqual:@NO];
}

MCLspServerInfo *RustInfo(EditorController *c) {
    for (MCLspServerInfo *i in Session(c).serverInfos)
        if ([i.key isEqualToString:@"rust"]) return i;
    return nil;
}

}  // namespace

MC_GUI_SUITE(server_consent) {
    __block EditorController *c = nil;
    NSString *file = MCGuiProjectFile(@"consent/src/main.rs");
    NSString *other = MCGuiProjectFile(@"consent/notes.txt");
    NSString *toolchain = MCGuiProjectFile(@"consent/rust-toolchain.toml");
    // The workspace/configuration answer, asked for by the server once it
    // is initialized (rust-analyzer pulls its settings so).
    NSDictionary *script = @{@"name": @"rust-analyzer", @"rules": @[
        @{@"method": @"initialized", @"request": @[@{
            @"method": @"workspace/configuration",
            @"params": @{@"items": @[@{@"section": @"rust-analyzer"}]}}]}]};
    BOOL (^barAsks)(NSString *) = ^BOOL(NSString *part) {
        return [c.installBar.consentServer isEqualToString:@"rust"] && !c.installBar.hidden &&
               [c.installBar.text containsString:part];
    };
    void (^showRust)(void) = ^{
        GCHECK([c showFile:other]);
        GCHECK([c showFile:file]);
    };

    MCGuiStep(@"a Rust file, a stand-in for rust-analyzer, no answer for the folder", ^{
        c = MCGuiController();
        // An earlier suite may have left the window on another folder: the
        // crate's own folders are read wherever it is.
        MCGuiNote(@"the window's folder: %@; the file: %@", c.rootPath, file);
        [[NSFileManager defaultManager] removeItemAtPath:toolchain error:nil];
        MCForgetFolderToolsConsent(c.rootPath);
        MCGuiWriteFile(file, @"fn main() {}\n");
        MCGuiWriteFile(other, @"notes\n");
        NSString *cmd = MCGuiFakeLsp(@"rust-consent", script, nil, nil);
        MCGuiFakeLspClearLog(@"rust-consent");
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = true\nlsp.rust = %@\n", cmd]);
        showRust();
    });
    MCGuiWait(@"it starts, its settings asked for", 20, ^BOOL {
        return MCGuiFakeLspReceived(@"rust-consent", @"workspace/didChangeConfiguration").count > 0;
    });
    MCGuiWait(@"the bar asks about build scripts", 5, ^BOOL { return barAsks(@"build scripts"); });
    MCGuiStep(@"build scripts, proc macros and cargo check off, every way it reads them", ^{
        NSArray<NSDictionary *> *inits = Initializes();
        GCHECK(inits.count == 1);
        NSDictionary *options = inits.lastObject[@"params"][@"initializationOptions"];
        MCGuiNote(@"initializationOptions: %@", options);
        GCHECK(AllOff(options));
        NSDictionary *settings = MCGuiFakeLspReceived(@"rust-consent",
            @"workspace/didChangeConfiguration").lastObject[@"params"][@"settings"];
        GCHECK(AllOff(settings[@"rust-analyzer"]));
        GCHECK_EQ_STR(Session(c).folderCodeAwaitingConsent, @"rust");
        GCHECK(MCFolderToolsConsent(c.rootPath) == nil);
        GCHECK_EQ_STR(c.installBar.installButton.title, @"Allow");
    });
    MCGuiWait(@"the configuration answer, the three off too", 10, ^BOOL {
        // The client's answer to workspace/configuration: a result list.
        for (NSDictionary *m in MCGuiFakeLspReceived(@"rust-consent", nil))
            if (m[@"result"] && !m[@"method"] && [m[@"result"] isKindOfClass:NSArray.class])
                return AllOff([m[@"result"] firstObject]);
        return NO;
    });
    MCGuiStep(@"Allow", ^{
        MCGuiFakeLspClearLog(@"rust-consent");
        [c.installBar.installButton performClick:nil];
        GCHECK_EQ_STR(MCFolderToolsConsent(c.rootPath), @"allow");
    });
    MCGuiWait(@"it starts again", 20, ^BOOL {
        return Initializes().count == 1 && Session(c).hasServer;
    });
    MCGuiStep(@"with its own defaults, and no question", ^{
        NSDictionary *params = Initializes().lastObject[@"params"];
        MCGuiNote(@"initializationOptions: %@", params[@"initializationOptions"]);
        GCHECK(params[@"initializationOptions"] == nil ||
               [params[@"initializationOptions"] isKindOfClass:NSNull.class]);
        GCHECK(Session(c).folderCodeAwaitingConsent == nil);
        GCHECK(c.installBar.hidden || c.installBar.consentServer == nil);
        MCGuiFakeLspClearLog(@"rust-consent");
        MCForgetFolderToolsConsent(c.rootPath);
    });
    MCGuiWait(@"taken back: it starts again with the three off", 20, ^BOOL {
        return Initializes().count == 1 &&
               AllOff(Initializes().lastObject[@"params"][@"initializationOptions"]);
    });
    MCGuiWait(@"and the bar asks again", 5, ^BOOL { return barAsks(@"build scripts"); });

    MCGuiStep(@"a rust-toolchain.toml in the crate, and Don't Allow", ^{
        MCGuiWriteFile(toolchain, @"[toolchain]\npath = \"/nowhere\"\n");
        MCGuiFakeLspClearLog(@"rust-consent");
        NSButton *mute = nil;
        for (NSView *v in c.installBar.subviews)
            if ([v isKindOfClass:NSButton.class] && v != c.installBar.installButton &&
                [((NSButton *)v).title isEqualToString:@"Don't Allow"])
                mute = (NSButton *)v;
        GCHECK(mute != nil);
        [mute performClick:nil];
        GCHECK_EQ_STR(MCFolderToolsConsent(c.rootPath), @"deny");
    });
    MCGuiWait(@"it waits, not running", 10, ^BOOL {
        return RustInfo(c).state == MCLspStateWaiting && !Session(c).hasServer;
    });
    // Negative check: a stopped server that would start again would have
    // sent initialize by now.
    MCGuiPause(1.0);
    MCGuiStep(@"nothing started, the status bar says it waits, no question (answered)", ^{
        GCHECK(Initializes().count == 0);
        MCGuiNote(@"status: %@", Session(c).statusText);
        GCHECK([Session(c).statusText containsString:@"waits for your answer"]);
        GCHECK(Session(c).folderCodeAwaitingConsent == nil);
        GCHECK([RustInfo(c).message containsString:@"rust-toolchain.toml"]);
        MCForgetFolderToolsConsent(c.rootPath);
        showRust();
    });
    MCGuiWait(@"taken back, the bar names the file", 5, ^BOOL {
        return barAsks(@"rust-toolchain.toml");
    });
    MCGuiStep(@"still not started; Allow", ^{
        GCHECK(Initializes().count == 0 && !Session(c).hasServer);
        [c.installBar.installButton performClick:nil];
    });
    MCGuiWait(@"it starts, with its defaults", 20, ^BOOL {
        NSArray<NSDictionary *> *inits = Initializes();
        id options = inits.lastObject[@"params"][@"initializationOptions"];
        return inits.count == 1 && Session(c).hasServer &&
               (options == nil || [options isKindOfClass:NSNull.class]);
    });

    MCGuiStep(@"tidy up: servers off, no answer, no tabs or files", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        MCForgetFolderToolsConsent(c.rootPath);
        // A Rust file left on show would start the real rust-analyzer in
        // a later suite that turns the servers on.
        for (NSString *p in @[file, other]) {
            GCHECK([c showFile:p]);
            GCHECK([c closeTab:[c valueForKey:@"activeTab"] prompt:NO]);
        }
        [[NSFileManager defaultManager]
            removeItemAtPath:[MCGuiProjectFile(@"consent") stringByStandardizingPath] error:nil];
    });
    MCGuiWait(@"the servers are gone", 10, ^BOOL { return !Session(c).hasServer; });
}
