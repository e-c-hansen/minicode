// ClangdFlagsSuite.mm — `make guitest SUITES=clangd_flags`: clangd with no
// compile flags for the project, against the scripted server running
// under the name clangd (build/fakelsp).
//
// In a folder with no compile_commands.json or compile_flags.txt anywhere
// up the tree, clangd guesses each file's flags, so a header in another
// folder is "file not found" and a cascade of errors follows. Opening a
// C++ file there puts a note in the status bar once for the folder, and
// clangd's card in the Language Servers popover says why its errors may
// be wrong and how projects make compile_commands.json. clang's "too many
// errors emitted" diagnostic, which clang puts on the file's first
// character, is counted in the status bar and listed in Problems but not
// drawn: no squiggle, gutter dot, hover or F8 stop. Once the folder has a
// compile_commands.json, the card says nothing of it.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"

namespace {
LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }
CodeTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
MCLspServerInfo *CppInfo(EditorController *c) {
    for (MCLspServerInfo *i in [Session(c) serverInfos])
        if ([i.key isEqualToString:@"cpp"]) return i;
    return nil;
}
NSDictionary *Diag(int line, int c0, int c1, NSString *message, NSString *code) {
    return @{@"range": @{@"start": @{@"line": @(line), @"character": @(c0)},
                         @"end": @{@"line": @(line), @"character": @(c1)}},
             @"severity": @1, @"message": message, @"source": @"clang", @"code": code};
}
}  // namespace

MC_GUI_SUITE(clangd_flags) {
    __block EditorController *c = nil;
    NSString *dir = MCGuiProjectFile(@"clangdflags");
    NSString *file = [dir stringByAppendingPathComponent:@"mac/model.mm"];
    NSString *other = [dir stringByAppendingPathComponent:@"mac/other.mm"];
    NSString *text = @"// model.mm: a comment on the first line.\n#include \"Index.h\"\n"
                     @"std::string name;\n";

    MCGuiStep(@"a C++ file in a folder with no compile flags, under clangd", ^{
        MCGuiWriteFile(file, text);
        MCGuiWriteFile(other, @"int x;\n");
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"src/Index.h"], @"#pragma once\n");
        NSString *cmd = MCGuiFakeLsp(@"clangd_flags", @{@"name": @"clangd", @"rules": @[
            @{@"method": @"textDocument/didOpen", @"when": @{@"uriEndsWith": @"model.mm"},
              @"notify": @[@{@"method": @"textDocument/publishDiagnostics", @"params": @{
                  @"uri": @"${uri}", @"version": @"${version}",
                  @"diagnostics": @[
                      Diag(1, 9, 18, @"'Index.h' file not found", @"pp_file_not_found"),
                      Diag(2, 5, 11, @"no type named 'string' in namespace 'std'",
                           @"typename_nested_not_found"),
                      Diag(0, 0, 0, @"too many errors emitted, stopping now",
                           @"fatal_too_many_errors")]}}]}]},
            @"clangd", nil);
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = true\nlsp.cpp = %@\n", cmd]);
        c = MCGuiController();
        [c focusEditor:nil];
        GCHECK([c showFile:file]);
    });
    MCGuiWait(@"the three errors arrive", 20, ^BOOL {
        return Text(c).diagnostics.count == 3;
    });
    MCGuiStep(@"the status bar says it once, and the card says why", ^{
        MCGuiNote(@"note: %@", Session(c).lastNote);
        GCHECK([Session(c).lastNote hasPrefix:@"clangd has no compile flags for this project"]);
        MCLspServerInfo *info = CppInfo(c);
        MCGuiNote(@"card: %@", info.compileFlagsNote);
        GCHECK([info.compileFlagsNote containsString:@"so it guesses"]);
        GCHECK([info.compileFlagsNote containsString:@"compile_commands.json"]);
        GCHECK([info.compileFlagsNote containsString:@"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]);
        // Shown once for the folder: another file there adds no note.
        [Session(c) setValue:nil forKey:@"lastNote"];
        GCHECK([c showFile:other]);
        GCHECK(Session(c).lastNote == nil);
        GCHECK(CppInfo(c).compileFlagsNote.length > 0);   // the card still says it
        GCHECK([c showFile:file]);
    });
    MCGuiWait(@"model.mm's errors drawn again", 10, ^BOOL {
        return Text(c).diagnostics.count == 3;
    });
    MCGuiStep(@"too many errors: counted and listed, not drawn on line 1", ^{
        NSArray<MCDiagnosticMark *> *marks = Text(c).diagnostics;
        NSUInteger quiet = 0;
        for (MCDiagnosticMark *m in marks)
            if (m.quiet) {
                quiet++;
                GCHECK(m.range.location == 0);
                GCHECK([m.message containsString:@"too many errors"]);
            }
        GCHECK(quiet == 1);
        MCGuiNote(@"status: %@", Session(c).statusText);
        GCHECK([Session(c).statusText containsString:@"3 errors"]);
        GCHECK(Session(c).problemErrors == 3);
        BOOL listed = NO;
        for (MCProblem *p in [Session(c) problemRows])
            if (!p.heading && [p.code isEqualToString:@"fatal_too_many_errors"]) listed = YES;
        GCHECK(listed);
        // No hover text on the comment's first character.
        GCHECK([Text(c) diagnosticMessageAtIndex:0] == nil);
        GCHECK([[Text(c) diagnosticMessageAtIndex:text.length - 8] containsString:@"string"]);
    });
    MCGuiStep(@"with a compile_commands.json at the folder's top, the card is quiet", ^{
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"compile_commands.json"], @"[]\n");
        GCHECK(CppInfo(c).compileFlagsNote == nil);
        [[NSFileManager defaultManager]
            removeItemAtPath:[dir stringByAppendingPathComponent:@"compile_commands.json"]
                       error:nil];
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"build/compile_flags.txt"],
                       @"-Isrc\n");
        GCHECK(CppInfo(c).compileFlagsNote == nil);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });
}
