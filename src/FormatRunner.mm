// FormatRunner.mm — see FormatRunner.h.
#import "FormatRunner.h"
#import "AppSettings.h"
#import "Lsp.h"   // MCFindProgram, MCSearchDirs
#include "Formatter.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

@implementation MCFormatResult
@end

static NSString *NS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

static std::string Std(NSString *s) {
    const char *c = s.UTF8String;
    return c ? c : "";
}

// A program named by a plan: a path as given, else the project's virtualenv
// for Python tools, else the usual search (PATH, Homebrew, ~/.local/bin, ...,
// never /usr/bin's xcrun shims).
static NSString *FindFormatter(const std::string &program, const std::string &lang,
                               NSString *path, NSString *root) {
    NSString *name = NS(program);
    if ([name containsString:@"/"]) return MCFindProgram(name);
    if (lang == "python") {
        NSString *venv = NSProcessInfo.processInfo.environment[@"VIRTUAL_ENV"] ?: @"";
        NSFileManager *fm = [NSFileManager defaultManager];
        for (const std::string &dir :
             Formatter::venvBinDirs(Std(path), Std(root ?: @""), Std(venv))) {
            NSString *p = [NS(dir) stringByAppendingPathComponent:name];
            if ([fm isExecutableFileAtPath:p]) return p;
        }
    }
    return MCFindProgram(name);
}

// Run argv with `input` on stdin. Blocks; call it off the main thread.
static void RunFormatter(NSArray<NSString *> *argv, NSString *cwd, NSData *input,
                         NSTimeInterval timeout, int *status, BOOL *timedOut,
                         NSData **outData, NSData **errData, NSString **launchError) {
    *status = -1;
    *timedOut = NO;
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:argv[0]];
    task.arguments = [argv subarrayWithRange:NSMakeRange(1, argv.count - 1)];
    task.currentDirectoryURL = [NSURL fileURLWithPath:cwd isDirectory:YES];
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
    NSMutableArray *dirs = [NSMutableArray arrayWithObject:
                               argv[0].stringByDeletingLastPathComponent];
    for (NSString *d in MCSearchDirs())
        if (![dirs containsObject:d]) [dirs addObject:d];
    env[@"PATH"] = [dirs componentsJoinedByString:@":"];
    env[@"PYTHONUTF8"] = @"1";   // a Finder-launched app has no LANG
    task.environment = env;
    NSPipe *inPipe = [NSPipe pipe], *outPipe = [NSPipe pipe], *errPipe = [NSPipe pipe];
    task.standardInput = inPipe;
    task.standardOutput = outPipe;
    task.standardError = errPipe;
    dispatch_semaphore_t exited = dispatch_semaphore_create(0);
    task.terminationHandler = ^(NSTask *t) {
        (void)t;
        dispatch_semaphore_signal(exited);
    };
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        *launchError = error.localizedDescription ?: @"could not be started";
        return;
    }

    // Write stdin and drain stdout and stderr at the same time, so a large
    // file can never stall the formatter on a full pipe.
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_group_t io = dispatch_group_create();
    __block NSData *outBytes = nil, *errBytes = nil;
    NSFileHandle *outRead = outPipe.fileHandleForReading;
    NSFileHandle *errRead = errPipe.fileHandleForReading;
    NSFileHandle *inWrite = inPipe.fileHandleForWriting;
    dispatch_group_async(io, q, ^{ outBytes = [outRead readDataToEndOfFile]; });
    dispatch_group_async(io, q, ^{ errBytes = [errRead readDataToEndOfFile]; });
    dispatch_group_async(io, q, ^{
        int fd = inWrite.fileDescriptor;
        fcntl(fd, F_SETNOSIGPIPE, 1);   // a formatter that exits early: EPIPE
        const char *bytes = (const char *)input.bytes;
        size_t left = input.length, at = 0;
        while (left > 0) {
            ssize_t n = write(fd, bytes + at, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            at += (size_t)n;
            left -= (size_t)n;
        }
        [inWrite closeFile];
    });

    dispatch_time_t limit = dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * NSEC_PER_SEC));
    if (dispatch_semaphore_wait(exited, limit) != 0) {
        *timedOut = YES;
        pid_t pid = task.processIdentifier;
        [task terminate];
        if (dispatch_semaphore_wait(exited, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) != 0) {
            kill(pid, SIGKILL);
            dispatch_semaphore_wait(exited, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        }
        return;
    }
    *status = task.terminationStatus;
    if (task.terminationReason == NSTaskTerminationReasonUncaughtSignal)
        *status = 128 + task.terminationStatus;
    // A child the formatter left running could hold the pipes open; do not
    // wait for it for ever.
    if (dispatch_group_wait(io, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0) {
        *launchError = @"left its output open after exiting";
        return;
    }
    *outData = outBytes;
    *errData = errBytes;
}

void MCFormatText(NSString *text, NSString *path, NSString *root, NSInteger firstLine,
                  NSInteger lastLine, NSTimeInterval timeout,
                  void (^completion)(MCFormatResult *result)) {
    MCFormatResult *result = [MCFormatResult new];
    auto finish = ^{
        dispatch_async(dispatch_get_main_queue(), ^{ completion(result); });
    };
    const std::string lang = Formatter::languageFor(Std(path));
    NSString *ext = path.pathExtension.length ? path.pathExtension : path.lastPathComponent;
    if (lang.empty()) {
        result.unavailable = YES;
        result.error = [NSString stringWithFormat:@"MiniCode has no formatter for .%@ files.",
                                                  ext];
        finish();
        return;
    }
    const std::string setting = [AppSettings shared].settings.formatCommand(lang);
    NSString *display = NS(Formatter::displayName(lang));

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSFileManager *fm = [NSFileManager defaultManager];
        Formatter::Fs fs;
        fs.exists = [fm](const std::string &p) { return (bool)[fm fileExistsAtPath:NS(p)]; };
        fs.read = [](const std::string &p) {
            NSString *s = [NSString stringWithContentsOfFile:NS(p) encoding:NSUTF8StringEncoding
                                                       error:nil];
            return s ? Std(s) : std::string();
        };
        // Planned on one spelling of both (the tree may say /tmp where the
        // file says /private/tmp), so "inside the open folder" holds.
        NSString *realPath = path.stringByResolvingSymlinksInPath;
        NSString *realRoot = root.length ? root.stringByResolvingSymlinksInPath : @"";
        Formatter::Plan plan =
            Formatter::plan(lang, setting, Std(realPath), Std(realRoot), fs);
        if (plan.off) {
            result.unavailable = YES;
            result.error = [NSString stringWithFormat:
                @"Formatting is off for %@ files (format.%@ = off in the settings).",
                display, NS(lang)];
            finish();
            return;
        }

        const Formatter::Candidate *chosen = nullptr;
        NSString *program = nil;
        for (const Formatter::Candidate &c : plan.candidates) {
            program = FindFormatter(c.program, lang, realPath, realRoot);
            if (program) { chosen = &c; break; }
        }
        NSString *configName = NS(plan.configFile).lastPathComponent;
        if (!chosen) {
            NSString *hint = plan.installHint.empty() ? @""
                : [NSString stringWithFormat:@"\n\nInstall it with:\n%@", NS(plan.installHint)];
            if (plan.fromSettings) {
                result.error = [NSString stringWithFormat:
                    @"format.%@ in the settings runs %@, which was not found.%@",
                    NS(lang), NS(plan.candidates.front().program), hint];
            } else if (!plan.configFile.empty()) {
                result.error = [NSString stringWithFormat:
                    @"This project configures %@ (in %@), which is not installed.%@",
                    NS(plan.candidates.front().program), configName, hint];
            } else {
                NSMutableArray *names = [NSMutableArray array];
                for (const Formatter::Candidate &c : plan.candidates)
                    [names addObject:NS(c.program)];
                result.error = [NSString stringWithFormat:
                    @"No formatter for %@ files was found (looked for %@).%@",
                    display, [names componentsJoinedByString:@" and "], hint];
            }
            finish();
            return;
        }

        result.tool = NS(Formatter::programName(chosen->program));
        result.style = plan.fromSettings
            ? [NSString stringWithFormat:@"format.%@ in the settings", NS(lang)]
            : chosen->projectStyle
                ? [NSString stringWithFormat:@"the project's %@", configName]
                : @"Google style";
        bool ranged = false;
        std::vector<std::string> words =
            Formatter::arguments(chosen->command, Std(path), (int)firstLine, (int)lastLine,
                                 &ranged);
        result.ranged = ranged;
        NSMutableArray<NSString *> *argv = [NSMutableArray arrayWithObject:program];
        for (size_t i = 1; i < words.size(); i++) [argv addObject:NS(words[i])];

        NSString *cwd = path.stringByDeletingLastPathComponent;
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:cwd isDirectory:&isDir] || !isDir)
            cwd = root.length ? root : NSHomeDirectory();

        NSData *input = [text dataUsingEncoding:NSUTF8StringEncoding] ?: [NSData data];
        int status = -1;
        BOOL timedOut = NO;
        NSData *outData = nil, *errData = nil;
        NSString *launchError = nil;
        RunFormatter(argv, cwd, input, timeout, &status, &timedOut, &outData, &errData,
                     &launchError);

        NSString *tool = result.tool;
        if (timedOut) {
            result.error = [NSString stringWithFormat:
                @"%@ took longer than %.0f seconds and was stopped. The file was not changed.",
                tool, timeout];
        } else if (launchError) {
            result.error = [NSString stringWithFormat:@"%@ %@.", tool, launchError];
        } else if (status != 0) {
            std::string err((const char *)errData.bytes, errData.length);
            std::string out((const char *)outData.bytes, outData.length);
            NSString *summary = NS(Formatter::errorSummary(err, out, Std(path)));
            result.error = summary.length
                ? [NSString stringWithFormat:@"%@ could not format %@:\n%@", tool,
                                             path.lastPathComponent, summary]
                : [NSString stringWithFormat:@"%@ exited with status %d.", tool, status];
        } else {
            NSString *formatted = [[NSString alloc] initWithData:outData ?: [NSData data]
                                                        encoding:NSUTF8StringEncoding];
            if (!formatted) {
                result.error = [NSString stringWithFormat:
                    @"%@ wrote something that is not UTF-8 text. The file was not changed.",
                    tool];
            } else if (formatted.length == 0 && input.length > 0) {
                result.error = [NSString stringWithFormat:
                    @"%@ returned no text. The file was not changed.", tool];
            } else {
                result.text = formatted;
            }
        }
        finish();
    });
}
