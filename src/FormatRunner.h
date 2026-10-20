// FormatRunner.h — Objective-C++. Format Document's process side: finds the
// formatter the plan in Formatter.h names, runs it on the buffer's text in the
// background and hands back its output or its complaint. The editor applies
// the result (EditorController formatDocument:).
#import <Cocoa/Cocoa.h>

@interface MCFormatResult : NSObject
// The formatted text; nil when formatting failed.
@property(nonatomic, copy) NSString *text;
// The program that ran ("pyink"), nil when none was found.
@property(nonatomic, copy) NSString *tool;
// Which style it applied: "Google style", "the project's .clang-format",
// "format.python in settings".
@property(nonatomic, copy) NSString *style;
// What went wrong, for the user: the formatter's first error lines, or that
// none was found. nil on success.
@property(nonatomic, copy) NSString *error;
// When none was found: the formatters that would do, best first ("pyink",
// "yapf"), for the note's install commands (InstallPanel). nil otherwise.
@property(nonatomic, copy) NSArray<NSString *> *installPrograms;
// YES when only the asked lines were formatted.
@property(nonatomic, assign) BOOL ranged;
// The file's language has no formatter, or its format.<lang> is off.
@property(nonatomic, assign) BOOL unavailable;
@end

// Format `text`, the current contents of the file at `path` (saved or not),
// in the background. `root` is the open folder. firstLine..lastLine (1-based)
// asks for those lines only, when the formatter can do that; 0, 0 for the
// whole file. The completion runs on the main queue. The process gets the
// text on stdin, runs in the file's folder so it finds the project's
// configuration, and is stopped after `timeout` seconds.
void MCFormatText(NSString *text, NSString *path, NSString *root, NSInteger firstLine,
                  NSInteger lastLine, NSTimeInterval timeout,
                  void (^completion)(MCFormatResult *result));
