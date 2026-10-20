// RunCommand.h — the command line Run File (Shift+Cmd+Return) types into the
// terminal for a file, from the run.<language> settings (RunFile.h has the
// template rules).
#import <Foundation/Foundation.h>

// What {python} stands for: the python of the nearest virtualenv (.venv,
// venv or env, from the file's folder up to `root`), else python3 from PATH.
// The fallback when the window has no interpreter of its own to offer.
NSString *MCPythonInterpreterFor(NSString *filePath, NSString *root);

// The command for a file, or nil with a sentence for the status bar in
// *problem when there is no command for its kind of file. `python`, when
// given, is what {python} stands for (the window's chosen interpreter, from
// Select Python Interpreter); nil falls back to MCPythonInterpreterFor.
NSString *MCRunCommandForFile(NSString *filePath, NSString *root, NSString *python,
                              NSString **problem);
