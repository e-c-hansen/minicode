// ServersByExtensionSuite.mm — `make guitest SUITES=servers_by_extension`:
// any language server by extension, against the scripted server
// (build/fakelsp). A server of the settings' own (lsp.yaml with
// lsp.yaml.extensions) opens its files with its name as the languageId;
// an optional built-in one (taplo for TOML) that is not installed brings no
// status, no "not installed" and no install bar; installed while the file
// is open (a fake taplo in ~/.local/bin, one of the folders searched), it
// starts without a restart; and off, it stays away.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#import "LspServer.h"
#include <sys/stat.h>

namespace {
LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }

NSString *OpenedLanguage(NSString *server, NSString *file) {
    for (NSDictionary *m in MCGuiFakeLspReceived(server, @"textDocument/didOpen")) {
        NSDictionary *doc = m[@"params"][@"textDocument"];
        if ([doc[@"uri"] hasSuffix:file]) return doc[@"languageId"];
    }
    return nil;
}
}  // namespace

MC_GUI_SUITE(servers_by_extension) {
    __block EditorController *c = nil;
    __block BOOL taploHere = NO;
    __block NSString *base = nil;
    NSString *yml = MCGuiProjectFile(@"ext/conf.yml");
    NSString *toml = MCGuiProjectFile(@"ext/pyproject.toml");
    NSString *fakeTaplo = [NSHomeDirectory() stringByAppendingPathComponent:@".local/bin/taplo"];

    MCGuiStep(@"a server of the settings' own, for .yaml and .yml", ^{
        c = MCGuiController();
        NSString *cmd = MCGuiFakeLsp(@"yaml", @{@"name": @"yaml-ls"}, nil, nil);
        base = [NSString stringWithFormat:@"lsp.enabled = true\nlsp.yaml = %@\n"
                                          @"lsp.yaml.extensions = yaml yml\n", cmd];
        MCGuiSetSettings(base);
        MCGuiWriteFile(yml, @"key: value\n");
        GCHECK([c showFile:yml]);
    });
    MCGuiWait(@"it runs", 20, ^BOOL {
        return Session(c).hasServer && MCGuiFakeLspReceived(@"yaml", @"initialized").count > 0 &&
               OpenedLanguage(@"yaml", @"conf.yml") != nil;
    });
    MCGuiStep(@"told the file is yaml; no install bar for it", ^{
        GCHECK_EQ_STR(OpenedLanguage(@"yaml", @"conf.yml"), @"yaml");
        GCHECK_EQ_STR(Session(c).currentLanguage, @"yaml");
        GCHECK(Session(c).missingServer == nil);
        taploHere = MCResolveCommand("taplo lsp stdio", nil) != nil;
        if (taploHere) MCGuiNote(@"taplo is installed here: the not-installed checks are skipped");
        MCGuiWriteFile(toml, @"[project]\nname = \"x\"\n");
        GCHECK([c showFile:toml]);
    });
    MCGuiPause(0.5);
    MCGuiStep(@"TOML with no taplo: nothing at all", ^{
        if (taploHere) return;
        GCHECK(Session(c).currentLanguage == nil);
        GCHECK(!Session(c).hasServer);
        GCHECK(Session(c).missingServer == nil);
        GCHECK_EQ_STR(Session(c).statusText, @"");
        NSView *bar = [c valueForKey:@"installBar"];
        GCHECK(!bar || bar.hidden);
        // taplo arrives (as `brew install taplo` would put it on the path).
        NSString *cmd = MCGuiFakeLsp(@"taplo", @{@"name": @"taplo"}, nil, nil);
        MCGuiWriteFile(fakeTaplo, [NSString stringWithFormat:@"#!/bin/sh\nexec %@ \"$@\"\n", cmd]);
        chmod(fakeTaplo.fileSystemRepresentation, 0755);
        [Session(c) lookAgainForMissingServer];   // what a window coming forward does
    });
    MCGuiWait(@"taplo starts for the open file", 20, ^BOOL {
        return taploHere || (Session(c).hasServer && OpenedLanguage(@"taplo", @"pyproject.toml"));
    });
    MCGuiStep(@"as toml, under its own name", ^{
        if (taploHere) return;
        GCHECK_EQ_STR(OpenedLanguage(@"taplo", @"pyproject.toml"), @"toml");
        GCHECK_EQ_STR(Session(c).currentLanguage, @"toml");
        GCHECK_EQ_STR(Session(c).fileServerNames.firstObject, @"taplo");
        // Listed in the Language Servers popover, as is the settings' own.
        NSMutableArray *keys = [NSMutableArray array];
        for (MCLspServerInfo *i in Session(c).serverInfos) [keys addObject:i.key];
        GCHECK([keys containsObject:@"toml"] && [keys containsObject:@"yaml"] &&
               [keys containsObject:@"markdown"]);
        MCGuiSetSettings([base stringByAppendingString:@"lsp.toml = off\n"]);
    });
    MCGuiPause(0.5);
    MCGuiStep(@"lsp.toml = off: the file has no server and says nothing", ^{
        if (taploHere) return;
        [c showFile:yml];
        [c showFile:toml];
        GCHECK(!Session(c).hasServer && Session(c).currentLanguage == nil);
        GCHECK_EQ_STR(Session(c).statusText, @"");
    });
    MCGuiStep(@"language servers off again", ^{
        [[NSFileManager defaultManager] removeItemAtPath:fakeTaplo error:nil];
        MCGuiSetSettings(@"lsp.enabled = false\n");
        [c closeAllTabs:nil];
    });
    MCGuiPause(0.3);
}
