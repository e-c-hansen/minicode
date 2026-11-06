// LspWatchSuite.mm — `make guitest SUITES=lsp_watch`: files changed on disk
// reach the language servers (workspace/didChangeWatchedFiles), against the
// scripted server (build/fakelsp). It registers watchers once initialized,
// as basedpyright and Ruff do: `**/*.py`, and a RelativePattern on a
// folder outside the project. A file no tab holds is created, changed in
// place and deleted, and each reaches the server with its URI and type;
// a .txt and a file inside .git do not; a file below the outside folder
// does. When the server unregisters, the folders are no longer watched.
// Where basedpyright and Ruff are installed, the real thing too: an import
// of a module that does not exist yet stops being an error once the module
// is written to disk, and a change to ruff.toml changes Ruff's diagnostics,
// with no tab on either file and no restart.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#include <pwd.h>
#include <unistd.h>

namespace {
LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }

NSString *Real(NSString *p) {
    char buf[PATH_MAX];
    return realpath(p.fileSystemRepresentation, buf) ? @(buf) : p;
}

// Every change the server named `name` was sent, in order, as "type uri".
NSArray<NSString *> *Changes(NSString *name) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSDictionary *m in MCGuiFakeLspReceived(name, @"workspace/didChangeWatchedFiles"))
        for (NSDictionary *c in m[@"params"][@"changes"])
            [out addObject:[NSString stringWithFormat:@"%@ %@", c[@"type"], c[@"uri"]]];
    return out;
}

BOOL Sent(NSString *name, int type, NSString *path) {
    NSString *want = [NSString stringWithFormat:@"%d file://%@", type, path];
    return [Changes(name) containsObject:want];
}

BOOL MentionsAny(NSString *name, NSString *part) {
    for (NSString *c in Changes(name))
        if ([c containsString:part]) return YES;
    return NO;
}

void Append(NSString *path, NSString *text) {
    NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:path];
    [h seekToEndOfFile];
    [h writeData:[text dataUsingEncoding:NSUTF8StringEncoding]];
    [h closeFile];
}

NSString *Installed(NSString *program) {
    NSMutableArray *dirs = [[NSProcessInfo.processInfo.environment[@"PATH"]
        componentsSeparatedByString:@":"] mutableCopy];
    struct passwd *pw = getpwuid(getuid());   // HOME is the scratch one here
    if (pw) [dirs addObject:[@(pw->pw_dir) stringByAppendingPathComponent:@".local/bin"]];
    [dirs addObject:@"/opt/homebrew/bin"];
    for (NSString *d in dirs) {
        NSString *p = [d stringByAppendingPathComponent:program];
        if ([[NSFileManager defaultManager] isExecutableFileAtPath:p]) return p;
    }
    return nil;
}

BOOL HasDiagnostic(EditorController *c, NSString *part) {
    for (MCDiagnosticMark *m in ((CodeTextView *)Text(c)).diagnostics)
        if ([m.message containsString:part]) return YES;
    return NO;
}
}  // namespace

MC_GUI_SUITE(lsp_watch) {
    __block EditorController *c = nil;
    NSString *dir = MCGuiProjectFile(@"watch");
    NSString *a = [dir stringByAppendingPathComponent:@"a.py"];
    NSString *b = [dir stringByAppendingPathComponent:@"b.py"];
    NSString *fresh = [dir stringByAppendingPathComponent:@"pkg/fresh.py"];
    NSString *notes = [dir stringByAppendingPathComponent:@"notes.txt"];
    NSString *inGit = [dir stringByAppendingPathComponent:@".git/hooks/x.py"];
    NSString *marker = [dir stringByAppendingPathComponent:@"marker.py"];
    NSString *lib = [MCGuiScratch() stringByAppendingPathComponent:@"watchlib"];
    NSString *libFile = [lib stringByAppendingPathComponent:@"ext/native.so"];
    __block NSString *rd = nil, *rlib = nil;   // as the kernel spells them

    MCGuiStep(@"a server that registers watchers once initialized", ^{
        c = MCGuiController();
        MCGuiWriteFile(a, @"print('a')\n");
        MCGuiWriteFile(b, @"print('b')\n");
        [[NSFileManager defaultManager] createDirectoryAtPath:[dir stringByAppendingPathComponent:@".git/hooks"]
                                  withIntermediateDirectories:YES attributes:nil error:nil];
        [[NSFileManager defaultManager] createDirectoryAtPath:[lib stringByAppendingPathComponent:@"ext"]
                                  withIntermediateDirectories:YES attributes:nil error:nil];
        rd = Real(dir);
        rlib = Real(lib);
        NSDictionary *watchers = @{@"registrations": @[
            @{@"id": @"py", @"method": @"workspace/didChangeWatchedFiles",
              @"registerOptions": @{@"watchers": @[@{@"globPattern": @"**/*.py"}]}},
            @{@"id": @"lib", @"method": @"workspace/didChangeWatchedFiles",
              @"registerOptions": @{@"watchers": @[
                  @{@"globPattern": @{@"baseUri": [@"file://" stringByAppendingString:rlib],
                                      @"pattern": @"**/*.so"}}]}}]};
        NSDictionary *drop = @{@"unregisterations": @[
            @{@"id": @"py", @"method": @"workspace/didChangeWatchedFiles"},
            @{@"id": @"lib", @"method": @"workspace/didChangeWatchedFiles"}]};
        NSString *cmd = MCGuiFakeLsp(@"watch", @{@"name": @"fakelsp", @"rules": @[
            @{@"method": @"initialized",
              @"request": @[@{@"method": @"client/registerCapability", @"params": watchers}]},
            @{@"method": @"textDocument/didOpen", @"when": @{@"uriEndsWith": @"b.py"},
              @"request": @[@{@"method": @"client/unregisterCapability", @"params": drop}]}]},
                                     nil, nil);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
        GCHECK([c showFile:a]);
    });
    MCGuiWait(@"the folders are watched", 20, ^BOOL {
        return Session(c).hasServer && Session(c).watchedFolders.count == 2;
    });
    MCGuiStep(@"the project and the outside folder, resolved; the reply was null", ^{
        NSArray *folders = Session(c).watchedFolders;
        GCHECK([folders containsObject:Real(MCGuiProject())] && [folders containsObject:rlib]);
        BOOL answered = NO;
        for (NSDictionary *m in MCGuiFakeLspReceived(@"watch", nil))
            if (!m[@"method"] && [m[@"id"] isEqual:@1000] && m[@"result"] == [NSNull null] &&
                !m[@"error"])
                answered = YES;
        GCHECK(answered);
        GCHECK(Changes(@"watch").count == 0);
        MCGuiWriteFile(fresh, @"x = 1\n");
    });
    MCGuiWait(@"a new file no tab holds is reported as created", 10, ^BOOL {
        return Sent(@"watch", 1, [rd stringByAppendingPathComponent:@"pkg/fresh.py"]);
    });
    // A batch of its own, though FSEvents' created flag is still on.
    MCGuiStep(@"changed in place", ^{ Append(fresh, @"y = 2\n"); });
    MCGuiWait(@"reported as changed", 10, ^BOOL {
        return Sent(@"watch", 2, [rd stringByAppendingPathComponent:@"pkg/fresh.py"]);
    });
    MCGuiStep(@"deleted, with a .txt and a file in .git written meanwhile", ^{
        MCGuiWriteFile(notes, @"not python\n");
        MCGuiWriteFile(inGit, @"# a hook\n");
        [[NSFileManager defaultManager] removeItemAtPath:fresh error:nil];
    });
    MCGuiWait(@"reported as deleted", 10, ^BOOL {
        return Sent(@"watch", 3, [rd stringByAppendingPathComponent:@"pkg/fresh.py"]);
    });
    MCGuiStep(@"a file below the outside folder, and a marker after it", ^{
        MCGuiWriteFile(libFile, @"elf");
        MCGuiWriteFile([lib stringByAppendingPathComponent:@"ext/readme.md"], @"no");
    });
    MCGuiWait(@"the outside folder's file arrives", 10, ^BOOL {
        return Sent(@"watch", 1, [rlib stringByAppendingPathComponent:@"ext/native.so"]);
    });
    MCGuiStep(@"a marker", ^{ MCGuiWriteFile(marker, @"pass\n"); });
    MCGuiWait(@"the marker arrives", 10, ^BOOL {
        return Sent(@"watch", 1, [rd stringByAppendingPathComponent:@"marker.py"]);
    });
    MCGuiStep(@"what no watcher wants, and what is inside .git, never went", ^{
        GCHECK(!MentionsAny(@"watch", @"notes.txt"));
        GCHECK(!MentionsAny(@"watch", @"/.git/"));
        GCHECK(!MentionsAny(@"watch", @"readme.md"));
        // One notification for each batch, never an empty one.
        for (NSDictionary *m in MCGuiFakeLspReceived(@"watch", @"workspace/didChangeWatchedFiles"))
            GCHECK([m[@"params"][@"changes"] count] > 0);
        GCHECK(Session(c).watchedBatches >= 4);
        GCHECK([c showFile:b]);   // the server unregisters when b.py opens
    });
    MCGuiWait(@"unregistered, nothing is watched", 10, ^BOOL {
        return Session(c).watchedFolders.count == 0;
    });
    MCGuiStep(@"and nothing more is sent", ^{
        MCGuiFakeLspClearLog(@"watch");
        MCGuiWriteFile(fresh, @"z = 3\n");
    });
    MCGuiPause(1.0);
    MCGuiStep(@"the new file went unreported", ^{
        GCHECK(Changes(@"watch").count == 0);
        MCGuiSetSettings(@"lsp.enabled = false\n");
        [c closeAllTabs:nil];
    });
    MCGuiWait(@"the server is gone", 10, ^BOOL { return !Session(c).hasServer; });

    // ------------------------------------------------ the real servers
    NSString *bp = Installed(@"basedpyright-langserver"), *ruff = Installed(@"ruff");
    if (!bp || !ruff) {
        MCGuiStep(@"basedpyright or Ruff is not installed: the scripted server only", ^{
            MCGuiNote(@"basedpyright-langserver or ruff not found; the real-server checks are skipped");
        });
        return;
    }
    NSString *real = MCGuiProjectFile(@"watchreal");
    NSString *mainPy = [real stringByAppendingPathComponent:@"main.py"];
    NSString *helper = [real stringByAppendingPathComponent:@"helper_mod.py"];
    NSString *ruffToml = [real stringByAppendingPathComponent:@"ruff.toml"];
    MCGuiStep(@"basedpyright and Ruff on a file importing a module not written yet", ^{
        MCGuiWriteFile(ruffToml, @"[lint]\nselect = [\"E\"]\n");
        MCGuiWriteFile(mainPy, @"import os\nimport helper_mod\n\nprint(helper_mod.VALUE)\n");
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@ --stdio\nlsp.python.also = %@ server\n", bp, ruff]);
        GCHECK([c showFile:mainPy]);
    });
    MCGuiWait(@"basedpyright says the import cannot be resolved", 60, ^BOOL {
        return HasDiagnostic(c, @"helper_mod") && Session(c).watchedFolders.count > 0;
    });
    MCGuiStep(@"Ruff, selecting only E, says nothing of the unused import", ^{
        GCHECK(!HasDiagnostic(c, @"imported but unused"));
        MCGuiWriteFile(helper, @"VALUE = 42\n");
    });
    MCGuiWait(@"the module written to disk resolves the import, with no tab on it", 30, ^BOOL {
        return !HasDiagnostic(c, @"helper_mod");
    });
    MCGuiStep(@"ruff.toml now selects F", ^{
        MCGuiWriteFile(ruffToml, @"[lint]\nselect = [\"F\"]\n");
    });
    MCGuiWait(@"Ruff reloads it and reports the unused import, with no restart", 30, ^BOOL {
        return HasDiagnostic(c, @"imported but unused");
    });
    MCGuiStep(@"language servers off again", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        [c closeAllTabs:nil];
    });
    MCGuiWait(@"the servers are gone", 10, ^BOOL { return !Session(c).hasServer; });
}
