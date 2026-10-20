// RunCommand.mm — see RunCommand.h. Objective-C++.
#import "RunCommand.h"
#import "AppSettings.h"
#include "RunFile.h"

static std::string Utf8(NSString *s) {
    const char *p = s.fileSystemRepresentation;
    return p ? std::string(p) : std::string();
}

NSString *MCPythonInterpreterFor(NSString *filePath, NSString *root) {
    std::string found = RunFile::findVirtualenvPython(Utf8(filePath), root.length ? Utf8(root) : "");
    if (found.empty()) return @"python3";
    return [[NSFileManager defaultManager] stringWithFileSystemRepresentation:found.c_str()
                                                                       length:found.size()];
}

NSString *MCRunCommandForFile(NSString *filePath, NSString *root, NSString **problem) {
    NSString *ext = filePath.pathExtension.lowercaseString;
    std::string tmpl = [AppSettings shared].settings.runTemplate(ext.UTF8String ?: "");
    if (tmpl.empty()) {
        if (problem)
            *problem = ext.length
                ? [NSString stringWithFormat:@"No run command for .%@ files "
                                              "(add run.%@ to the settings)", ext, ext]
                : @"No run command for files without an extension";
        return nil;
    }
    std::map<std::string, std::string> vars = {
        {"file", Utf8(filePath)},
        {"dir", Utf8(filePath.stringByDeletingLastPathComponent)},
        {"root", Utf8(root.length ? root : filePath.stringByDeletingLastPathComponent)},
        {"name", Utf8(filePath.lastPathComponent)},
    };
    // Only looked for when the command uses it.
    if (tmpl.find("{python}") != std::string::npos)
        vars["python"] = Utf8(MCPythonInterpreterFor(filePath, root));
    std::string line = RunFile::expand(tmpl, vars);
    return [NSString stringWithUTF8String:line.c_str()];
}
