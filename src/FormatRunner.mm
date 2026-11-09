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

// Where a plan's program may be found. `local`: the project's own copy, in
// its virtualenv for a Python tool or its node_modules/.bin for Prettier and
// Biome (at the version the project pins), nil for none; `outside`: one the
// user installed, from $VIRTUAL_ENV (activated where minicode started, so
// trusted as the servers trust it) or the usual search (PATH, Homebrew,
// ~/.local/bin, ..., never /usr/bin's xcrun shims). A path as given (from
// the settings) is only ever `outside`. A project copy outside the window's
// folder (a file opened from elsewhere) is never used: no folder's answer
// covers it.
struct MCFormatterLookup {
    NSString *local = nil;
    NSString *outside = nil;
};

static MCFormatterLookup FindFormatter(const std::string &program, const std::string &lang,
                                       NSString *path, NSString *root) {
    MCFormatterLookup found;
    NSString *name = NS(program);
    if ([name containsString:@"/"]) {
        found.outside = MCFindProgram(name);
        return found;
    }
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *venv = NSProcessInfo.processInfo.environment[@"VIRTUAL_ENV"] ?: @"";
    NSString *activated = venv.length
        ? [venv.stringByResolvingSymlinksInPath stringByAppendingPathComponent:@"bin"] : nil;
    std::vector<std::string> dirs;
    if (lang == "python")
        dirs = Formatter::venvBinDirs(Std(path), Std(root ?: @""), Std(venv));
    if (Formatter::fromNodeModules(program))
        for (const std::string &d : Formatter::nodeBinDirs(Std(path), Std(root ?: @"")))
            dirs.push_back(d);
    for (const std::string &dir : dirs) {
        NSString *p = [NS(dir) stringByAppendingPathComponent:name];
        if (![fm isExecutableFileAtPath:p]) continue;
        BOOL trusted = activated && ([NS(dir) isEqualToString:activated] ||
                                     [NS(dir).stringByResolvingSymlinksInPath
                                         isEqualToString:activated]);
        if (trusted) {
            if (!found.outside) found.outside = p;
        } else if (!found.local && Formatter::insideFolder(dir, Std(root ?: @""))) {
            found.local = p;
        }
    }
    if (!found.outside) found.outside = MCFindProgram(name);
    return found;
}

// The project folder a local tool comes from, as the window's folder names
// it: ".venv" for .venv/bin/black, "web/node_modules" for
// web/node_modules/.bin/prettier.
static NSString *ToolFolderName(NSString *program, NSString *root) {
    NSString *dir = program.stringByDeletingLastPathComponent.stringByDeletingLastPathComponent;
    NSString *prefix = [root stringByAppendingString:@"/"];
    if (root.length && [dir hasPrefix:prefix]) dir = [dir substringFromIndex:prefix.length];
    return dir;
}

// A file as the window's folder names it: "web/prettier.config.js"; one
// above the folder by its whole path, "~/Code/repo/prettier.config.js".
static NSString *FolderRelativeName(NSString *path, NSString *root) {
    NSString *prefix = [root stringByAppendingString:@"/"];
    if (root.length && [path hasPrefix:prefix]) return [path substringFromIndex:prefix.length];
    return path.stringByAbbreviatingWithTildeInPath;
}

// The folder's own copy of a program a package runner launches by name
// (npx prettier): node_modules/.bin in the file's folder and above it, up
// to the window's folder, as the runners look; nil for none.
static NSString *FolderNodeBin(const std::string &tool, NSString *path, NSString *root) {
    if (!root.length || tool.empty() || tool.find('/') != std::string::npos) return nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    for (const std::string &dir : Formatter::nodeBinDirs(Std(path), Std(root))) {
        NSString *p = [NS(dir) stringByAppendingPathComponent:NS(tool)];
        if ([fm isExecutableFileAtPath:p] && Formatter::insideFolder(Std(p), Std(root))) return p;
    }
    return nil;
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
        result.serverMayFormat = YES;
        result.error = NS(Formatter::unplannedMessage("", Std(ext)));
        finish();
        return;
    }
    const std::string setting = [AppSettings shared].settings.formatCommand(lang);
    NSString *display = NS(Formatter::displayName(lang));
    // The folder's answer for its own programs, read here, before the work
    // goes off the main thread.
    NSString *consent = root.length ? MCFolderToolsConsent(root) : nil;

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
        if (Formatter::serverMayFormat(lang, plan)) {
            // Markdown, JSON, YAML or CSS in a project not set up for a
            // formatter: the file's language server may format it.
            result.unavailable = YES;
            result.serverMayFormat = YES;
            result.error = NS(Formatter::unplannedMessage(lang, Std(ext)));
            finish();
            return;
        }

        const Formatter::Candidate *chosen = nullptr;
        NSString *program = nil, *kept = nil;   // kept: a folder copy not allowed
        NSString *keptRunner = nil;             // the runner that would have run it
        const std::string answer = Std(consent ?: @"");
        for (const Formatter::Candidate &c : plan.candidates) {
            MCFormatterLookup found = FindFormatter(c.program, lang, realPath, realRoot);
            // A package runner (npx prettier, pnpm exec, bunx, yarn) is
            // the program it launches: the folder's own copy when the
            // folder has one, and that program's configuration counts.
            const Formatter::Launch launch = Formatter::launched(c.command);
            const std::string tool = launch.tool.empty() ? c.program : launch.tool;
            NSString *launchedLocal = nil;   // the folder's copy a runner would run
            if (!launch.tool.empty()) {
                launchedLocal = FolderNodeBin(launch.tool, realPath, realRoot);
                if (!launchedLocal && launch.folderCode)
                    launchedLocal = [realRoot stringByAppendingPathComponent:@"package.json"];
            }
            // A configuration that runs code when the tool reads it
            // (prettier.config.js, a plugin), in the folder or above it, is
            // the folder's own program, whichever copy of the tool reads it.
            const std::string codeConfig = Formatter::codeConfigFile(
                tool, Std(realPath), Std(realRoot), plan.configFile, fs);
            Formatter::Source source = launch.tool.empty()
                ? Formatter::chooseProgram(plan.fromSettings, Std(found.local ?: @""),
                                           Std(found.outside ?: @""), answer,
                                           !codeConfig.empty())
                : Formatter::chooseLaunched(plan.fromSettings, launchedLocal != nil,
                                            Std(found.outside ?: @""), answer,
                                            !codeConfig.empty());
            if (launchedLocal) {
                // The runner stands in for the folder's copy.
                found.local = found.outside;
                found.outside = nil;
            }
            if (source == Formatter::Source::Ask) {
                // Nothing runs until the user answers for the folder; the
                // editor asks and formats again (formatSourceQuietly:).
                result.needsConsent = YES;
                result.tool = NS(tool);
                if (launchedLocal)
                    result.consentFolder = [launchedLocal.lastPathComponent
                                               isEqualToString:@"package.json"]
                        ? @"package.json" : ToolFolderName(launchedLocal, realRoot);
                else if (found.local) result.consentFolder = ToolFolderName(found.local, realRoot);
                if (!codeConfig.empty())
                    result.consentConfig = FolderRelativeName(NS(codeConfig), realRoot);
                finish();
                return;
            }
            if (source == Formatter::Source::Refused) {
                NSString *name = NS(tool);
                result.tool = name;
                result.error = [NSString stringWithFormat:
                    @"%@ can run code when %@ reads it, and this folder's own programs are not "
                     "allowed to run, so %@ did not run. Ask Again About This Folder's Tools "
                     "(minicode menu) asks again.",
                    FolderRelativeName(NS(codeConfig), realRoot), name, name];
                finish();
                return;
            }
            if (source == Formatter::Source::Local) program = found.local;
            if (source == Formatter::Source::Outside) program = found.outside;
            if (program) { chosen = &c; break; }
            if (found.local && !kept) {
                kept = launchedLocal ?: found.local;
                if (launchedLocal) keptRunner = NS(Formatter::programName(c.program));
            }
        }
        NSString *configName = NS(plan.configFile).lastPathComponent;
        if (!chosen && kept && keptRunner) {
            // A runner that would run the folder's own copy (or script).
            result.error = [NSString stringWithFormat:
                @"format.%@ runs %@, which would run this folder's own %@, and this folder's "
                 "own programs are not allowed to run.",
                NS(lang), keptRunner,
                [kept.lastPathComponent isEqualToString:@"package.json"]
                    ? @"package.json scripts" : ToolFolderName(kept, realRoot)];
            finish();
            return;
        }
        if (!chosen && kept) {
            // The folder has the tool, and the user said no to the folder's
            // own programs.
            NSString *tool = kept.lastPathComponent;
            if (!plan.installHint.empty() || !plan.fromSettings) result.installPrograms = @[tool];
            result.error = [NSString stringWithFormat:
                @"%@ is installed only in this folder's %@, and this folder's own programs "
                 "are not allowed to run. Install %@ yourself to use it here.",
                tool, ToolFolderName(kept, realRoot), tool];
            finish();
            return;
        }
        if (!chosen) {
            // How to install one is shown under the message (InstallPanel),
            // from the same table as plan.installHint.
            NSMutableArray<NSString *> *programs = [NSMutableArray array];
            for (const Formatter::Candidate &c : plan.candidates)
                [programs addObject:NS(Formatter::programName(c.program))];
            if (!plan.installHint.empty()) result.installPrograms = programs;
            if (plan.fromSettings) {
                result.error = [NSString stringWithFormat:
                    @"format.%@ in the settings runs %@, which was not found.",
                    NS(lang), NS(plan.candidates.front().program)];
            } else if (!plan.configFile.empty()) {
                result.error = [NSString stringWithFormat:
                    @"This project configures %@ (in %@), which is not installed.",
                    NS(plan.candidates.front().program), configName];
            } else {
                NSMutableArray *names = [NSMutableArray array];
                for (const Formatter::Candidate &c : plan.candidates)
                    [names addObject:NS(c.program)];
                result.error = [NSString stringWithFormat:
                    @"No formatter for %@ files was found (looked for %@).",
                    display, [names componentsJoinedByString:@" and "]];
            }
            finish();
            return;
        }

        result.tool = NS(Formatter::programName(chosen->program));
        result.style = chosen->projectStyle
            ? [NSString stringWithFormat:@"the project's %@", configName]
            : plan.fromSettings
                ? [NSString stringWithFormat:@"format.%@ in the settings", NS(lang)]
                : @"Google style";
        bool ranged = false;
        std::vector<std::string> words =
            Formatter::arguments(chosen->command, Std(path), (int)firstLine, (int)lastLine,
                                 &ranged, plan.configFile);
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
