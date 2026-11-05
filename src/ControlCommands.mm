// ControlCommands.mm — see ControlCommands.h.
#import "ControlCommands.h"
#import "ControlServer.h"
#import "EditorController.h"
#import "AgentNotes.h"
#import "CompareView.h"
#import "GitPanel.h"
#import "Terminal.h"
#include <objc/runtime.h>
#include <unistd.h>

using ControlChannel::Dispatcher;
using ControlChannel::Request;
using ControlChannel::Result;

// ------------------------------------------------------------------ ids
static char kWindowIDKey, kTerminalIDKey;

static NSInteger MCAssignID(id object, const void *key, NSInteger *counter) {
    if (!object) return 0;
    NSNumber *n = objc_getAssociatedObject(object, key);
    if (!n) {
        n = @(++*counter);
        objc_setAssociatedObject(object, key, n, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return n.integerValue;
}

NSInteger MCControlWindowID(EditorController *controller) {
    static NSInteger counter;
    return MCAssignID(controller, &kWindowIDKey, &counter);
}

NSInteger MCControlTerminalID(TerminalView *terminal) {
    static NSInteger counter;
    return MCAssignID(terminal, &kTerminalIDKey, &counter);
}

// ---------------------------------------------------------------- helpers
static NSString *NS(const Json &j) {
    return [NSString stringWithUTF8String:j.asString().c_str()] ?: @"";
}

static Json J(NSString *s) {
    return s ? Json(std::string(s.UTF8String ?: "")) : Json();
}

static Json JFromObject(id value) {
    if (!value || value == NSNull.null) return Json();
    if ([value isKindOfClass:NSString.class]) return J(value);
    if ([value isKindOfClass:NSNumber.class]) {
        NSNumber *n = value;
        if (strcmp(n.objCType, @encode(BOOL)) == 0 || n == (id)kCFBooleanTrue ||
            n == (id)kCFBooleanFalse)
            return Json((bool)n.boolValue);
        return Json(n.doubleValue);
    }
    if ([value isKindOfClass:NSArray.class]) {
        Json a = Json::array();
        for (id v in (NSArray *)value) a.push(JFromObject(v));
        return a;
    }
    if ([value isKindOfClass:NSDictionary.class]) {
        Json o = Json::object();
        NSDictionary *d = value;
        for (NSString *k in [d.allKeys sortedArrayUsingSelector:@selector(compare:)])
            o.set(k.UTF8String, JFromObject(d[k]));
        return o;
    }
    return J([value description]);
}

static BOOL MCIsDirectory(NSString *path) {
    BOOL dir = NO;
    return [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir] && dir;
}

static BOOL MCIsFile(NSString *path) {
    BOOL dir = NO;
    return [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir] && !dir;
}

static NSString *MCResolved(NSString *path) { return path.stringByResolvingSymlinksInPath; }

// `path` inside `root` spelled from the root as the window spells it, which
// the tree needs to find it; else as it is.
static NSString *MCSpelledFrom(EditorController *c, NSString *path) {
    NSString *root = MCResolved(c.rootPath ?: @"/");
    NSString *real = MCResolved(path);
    NSString *prefix = [root isEqualToString:@"/"] ? root : [root stringByAppendingString:@"/"];
    if ([real hasPrefix:prefix])
        return [c.rootPath stringByAppendingPathComponent:[real substringFromIndex:prefix.length]];
    return path;
}

// A path for showing: from the window's folder when inside it.
static NSString *MCShortPath(EditorController *c, NSString *path) {
    NSString *root = MCResolved(c.rootPath ?: @"");
    NSString *real = MCResolved(path);
    if (root.length > 1 && [real hasPrefix:[root stringByAppendingString:@"/"]])
        return [real substringFromIndex:root.length + 1];
    return [path stringByAbbreviatingWithTildeInPath];
}

static EditorController *MCWindowWithID(id<MCControlAppHost> host, NSInteger wid) {
    for (EditorController *c in [host controlControllers])
        if (MCControlWindowID(c) == wid) return c;
    return nil;
}

// The window a command acts in: --window, else --folder (opened if no window
// shows it), else `fallback`, else the frontmost.
static EditorController *MCTarget(id<MCControlAppHost> host, const Json &args,
                                  EditorController *fallback, std::string &error) {
    if (args.has("window")) {
        EditorController *c = MCWindowWithID(host, (NSInteger)args["window"].asInt());
        if (!c) error = "there is no window " + std::to_string(args["window"].asInt());
        return c;
    }
    if (args.has("folder")) {
        NSString *dir = MCResolved(NS(args["folder"]));
        if (!MCIsDirectory(dir)) {
            error = std::string("not a folder: ") + dir.UTF8String;
            return nil;
        }
        return [host controlControllerRootedAt:dir] ?: [host controlOpenWindowAtPath:dir];
    }
    EditorController *c = fallback ?: [host controlFrontController];
    if (!c) error = "no window is open";
    return c;
}

static Json MCWindowResult(EditorController *c) {
    Json r = Json::object();
    r.set("window", (int)MCControlWindowID(c));
    return r;
}

// ------------------------------------------------------------------ state
static Json MCTerminalState(TerminalView *t) {
    Json o = Json::object();
    o.set("id", (int)MCControlTerminalID(t));
    o.set("cwd", J(t.directory));
    o.set("title", J(t.title));
    NSString *cmd = t.runningCommand;
    o.set("command", cmd.length ? J(cmd) : Json());
    o.set("busy", (bool)t.busy);
    return o;
}

static Json MCWindowState(EditorController *c, BOOL front) {
    Json w = Json::object();
    w.set("id", (int)MCControlWindowID(c));
    w.set("folder", J(c.rootPath));
    w.set("front", (bool)front);
    Json tabs = Json::array();
    for (NSDictionary *t in [c controlTabs]) tabs.push(JFromObject(t));
    w.set("tabs", tabs);
    NSDictionary *sel = [c controlSelection];
    w.set("selection", sel ? JFromObject(sel) : Json());
    Json terms = Json::array();
    for (TerminalView *t in [c controlTerminals]) terms.push(MCTerminalState(t));
    w.set("terminals", terms);
    Json browser = Json::array();
    if (NSDictionary *b = [c controlBrowser]) browser.push(JFromObject(b));
    w.set("browser", browser);
    MCAgentNotes *notes = c.agentNotes;
    if (notes.noteShown) {
        Json n = Json::object();
        n.set("text", J(notes.noteText));
        n.set("label", J(notes.noteLabel));
        w.set("note", n);
    } else {
        w.set("note", Json());
    }
    if (notes.tourActive) {
        Json t = Json::object();
        t.set("stop", (int)notes.tourIndex + 1);
        t.set("of", (int)notes.tourCount);
        w.set("tour", t);
    } else {
        w.set("tour", Json());
    }
    return w;
}

// -------------------------------------------------------------- the diff
// git's text of `path` at `ref`: nil with *missing set when the file is not
// in that commit, nil with *error for anything else.
static NSString *MCTextAtRef(NSString *path, NSString *ref, BOOL *missing, NSString **error) {
    NSString *dir = path.stringByDeletingLastPathComponent;
    MCGitResult *top = MCGitRunSync(dir, @[@"rev-parse", @"--show-toplevel"]);
    if (!top.ok) {
        *error = top.status < 0 && !MCGitExecutable()
            ? @"git is not installed"
            : [NSString stringWithFormat:@"%@ is not in a git repository",
               path.lastPathComponent];
        return nil;
    }
    MCGitResult *verify = MCGitRunSync(dir, @[@"rev-parse", @"--verify", @"--quiet",
                                              [ref stringByAppendingString:@"^{commit}"]]);
    if (!verify.ok) {
        *error = [NSString stringWithFormat:@"no commit named %@", ref];
        return nil;
    }
    MCGitResult *show = MCGitRunSync(dir, @[@"show", [NSString stringWithFormat:@"%@:./%@", ref,
                                                      path.lastPathComponent]]);
    if (!show.ok) {
        *missing = YES;   // the commit exists; the file is not in it
        return nil;
    }
    NSString *text = [[NSString alloc] initWithData:show.output encoding:NSUTF8StringEncoding];
    if (!text) *error = [NSString stringWithFormat:@"%@ at %@ is not text", path.lastPathComponent, ref];
    return text;
}

static NSString *MCReadText(NSString *path, std::string &error) {
    if (!MCIsFile(path)) {
        error = std::string("no such file: ") + path.UTF8String;
        return nil;
    }
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!text) error = std::string(path.lastPathComponent.UTF8String) + " is not UTF-8 text";
    return text;
}

// ------------------------------------------------------------- commands
void MCControlRegisterCommands(Dispatcher &d, id<MCControlAppHost> hostIn) {
    __weak id<MCControlAppHost> weakHost = hostIn;

    d.on("ping", [](const Request &, Dispatcher::Respond respond) {
        Json r = Json::object();
        r.set("pid", (int)getpid());
        NSString *v = [NSBundle mainBundle].infoDictionary[@"CFBundleShortVersionString"];
        r.set("version", J(v ?: @""));
        respond(Result::success(r));
    });

    d.on("window", [weakHost](const Request &req, Dispatcher::Respond respond) {
        id<MCControlAppHost> host = weakHost;
        const Json &a = req.args;
        EditorController *c = nil;
        if (a.has("folder")) {
            NSString *dir = MCResolved(NS(a["folder"]));
            if (!MCIsDirectory(dir)) {
                respond(Result::failure(std::string("not a folder: ") + dir.UTF8String));
                return;
            }
            if (!a["new"].asBool()) c = [host controlControllerRootedAt:dir];
            if (!c) c = [host controlOpenWindowAtPath:dir];
        } else if (a["new"].asBool()) {
            NSString *root = [host controlFrontController].rootPath ?: NSHomeDirectory();
            c = [host controlOpenWindowAtPath:root];
        } else {
            c = [host controlFrontController];
        }
        if (!c) {
            respond(Result::failure("no window is open"));
            return;
        }
        [c.window makeKeyAndOrderFront:nil];
        Json r = MCWindowResult(c);
        r.set("folder", J(c.rootPath));
        respond(Result::success(r));
    });

    d.on("open", [weakHost](const Request &req, Dispatcher::Respond respond) {
        id<MCControlAppHost> host = weakHost;
        const Json &a = req.args;
        if (a.has("split")) {
            respond(Result::failure("the editor has one pane: open the file without split"));
            return;
        }
        const Json &files = a["files"];
        if (files.size() == 0) {
            respond(Result::failure("open needs a path"));
            return;
        }
        // Everything is checked before anything opens.
        for (size_t i = 0; i < files.size(); i++) {
            NSString *p = NS(files[i]["path"]);
            if (![[NSFileManager defaultManager] fileExistsAtPath:p]) {
                respond(Result::failure(std::string("no such file or folder: ") + p.UTF8String));
                return;
            }
        }
        const BOOL targeted = a.has("window") || a.has("folder");
        std::string error;
        EditorController *forced = targeted ? MCTarget(host, a, nil, error) : nil;
        if (targeted && !forced) {
            respond(Result::failure(error));
            return;
        }
        const BOOL hasNote = a.has("note") || a.has("to_line");
        const BOOL preview = a.has("note") && !a["keep"].asBool();
        EditorController *last = nil;
        Json opened = Json::array();
        for (size_t i = 0; i < files.size(); i++) {
            NSString *path = MCResolved(NS(files[i]["path"]));
            if (MCIsDirectory(path)) {
                // A folder: the window showing it, as the launcher does.
                EditorController *c = forced ?: [host controlControllerRootedAt:path]
                                              ?: [host controlOpenWindowAtPath:path];
                last = c;
                continue;
            }
            NSString *spelled = nil;
            EditorController *c = forced;
            if (c) spelled = MCSpelledFrom(c, path);
            else c = [host controlControllerContaining:path spelled:&spelled];
            if (!c) {
                c = [host controlFrontController];
                spelled = path;
            }
            if (!c) c = [host controlOpenWindowAtPath:path.stringByDeletingLastPathComponent];
            if (!spelled) spelled = path;
            last = c;
            if (![c openFileForAgent:spelled preview:preview]) {
                respond(Result::failure(std::string("could not open ") + path.UTF8String));
                return;
            }
            const int line = (int)files[i]["line"].asInt(0), col = (int)files[i]["column"].asInt(0);
            if (hasNote && i + 1 == files.size()) {
                [c.agentNotes showNote:NS(a["note"]) from:NS(a["from"])
                                  line:line > 0 ? line : 1 column:col
                                toLine:(int)a["to_line"].asInt(0) toColumn:(int)a["to_column"].asInt(0)];
            } else if (line > 0) {
                [c.agentNotes revealLine:line column:col];
            }
            opened.push(J(spelled));
        }
        Json r = last ? MCWindowResult(last) : Json::object();
        r.set("opened", opened);
        if (preview) r.set("preview", true);
        respond(Result::success(r));
    });

    d.on("browse", [weakHost](const Request &req, Dispatcher::Respond respond) {
        const Json &a = req.args;
        if (a["split"].asString() == "below") {
            respond(Result::failure("the browser opens beside the editor: use --right"));
            return;
        }
        NSString *url = [NS(a["url"]) stringByTrimmingCharactersInSet:
                         NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!url.length) {
            respond(Result::failure("browse needs an address"));
            return;
        }
        // What an agent means by an address the browser's own field would
        // take for words to search: a local server, or a file.
        if (![url containsString:@"://"]) {
            NSRegularExpression *hostPort = [NSRegularExpression
                regularExpressionWithPattern:@"^(localhost|[0-9.]+|[A-Za-z0-9.-]+:[0-9]+)(:[0-9]+)?(/.*)?$"
                                     options:0 error:nil];
            if ([url hasPrefix:@"/"] && [[NSFileManager defaultManager] fileExistsAtPath:url])
                url = [NSURL fileURLWithPath:url].absoluteString;
            else if ([hostPort firstMatchInString:url options:0 range:NSMakeRange(0, url.length)])
                url = [@"http://" stringByAppendingString:url];
        }
        std::string error;
        EditorController *c = MCTarget(weakHost, a, nil, error);
        if (!c) { respond(Result::failure(error)); return; }
        [c browseForAgent:url];
        respond(Result::success(MCWindowResult(c)));
    });

    d.on("term", [weakHost](const Request &req, Dispatcher::Respond respond) {
        const Json &a = req.args;
        std::string error;
        EditorController *c = MCTarget(weakHost, a, nil, error);
        if (!c) { respond(Result::failure(error)); return; }
        NSString *dir = a.has("cwd") ? NS(a["cwd"]) : c.rootPath;
        if (!MCIsDirectory(dir)) {
            respond(Result::failure(std::string("not a folder: ") + dir.UTF8String));
            return;
        }
        const std::string split = a["split"].asString();
        if (!split.empty() && split != "right" && split != "below") {
            respond(Result::failure("split is right or below"));
            return;
        }
        TerminalView *t = [c newTerminalForAgentIn:dir beside:split == "right"];
        if (!t) {
            respond(Result::failure("no room to split the terminal"));
            return;
        }
        if (a.has("title")) t.fixedTitle = NS(a["title"]);
        if (a.has("run")) [t runCommand:NS(a["run"])];
        Json r = MCWindowResult(c);
        r.set("terminal", (int)MCControlTerminalID(t));
        respond(Result::success(r));
    });

    d.on("diff", [weakHost](const Request &req, Dispatcher::Respond respond) {
        id<MCControlAppHost> host = weakHost;
        const Json &a = req.args;
        NSString *path = MCResolved(NS(a["path"]));
        std::string error;
        NSString *right = MCReadText(path, error);
        if (!right) { respond(Result::failure(error)); return; }
        EditorController *fallback = [host controlControllerContaining:path spelled:nil];
        EditorController *c = MCTarget(host, a, fallback, error);
        if (!c) { respond(Result::failure(error)); return; }
        const BOOL preview = !a["keep"].asBool();
        if (a.has("other")) {
            NSString *other = MCResolved(NS(a["other"]));
            NSString *otherText = MCReadText(other, error);
            if (!otherText) { respond(Result::failure(error)); return; }
            MCComparison *cmp = [MCComparison new];
            cmp.leftText = right;
            cmp.rightText = otherText;
            cmp.leftLabel = MCShortPath(c, path);
            cmp.rightLabel = MCShortPath(c, other);
            cmp.path = other;
            cmp.title = [NSString stringWithFormat:@"%@ ↔ %@", path.lastPathComponent,
                         other.lastPathComponent];
            [c showComparison:cmp preview:preview];
            Json r = MCWindowResult(c);
            r.set("changed", (int)cmp.changedRows);
            respond(Result::success(r));
            return;
        }
        // git runs off the main thread; the answer waits for it.
        NSString *ref = a.has("base") ? NS(a["base"]) : @"HEAD";
        __weak EditorController *weakC = c;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            BOOL missing = NO;
            NSString *gitError = nil;
            NSString *left = MCTextAtRef(path, ref, &missing, &gitError);
            dispatch_async(dispatch_get_main_queue(), ^{
                EditorController *win = weakC;
                if (!win) { respond(Result::failure("the window closed")); return; }
                if (!left && !missing) {
                    respond(Result::failure(gitError.UTF8String ?: "git failed"));
                    return;
                }
                NSString *shortPath = MCShortPath(win, path);
                MCComparison *cmp = [MCComparison new];
                cmp.leftText = left ?: @"";
                cmp.rightText = right;
                cmp.leftLabel = missing ? [NSString stringWithFormat:@"%@: (not in %@)", ref, ref]
                                        : [NSString stringWithFormat:@"%@: %@", ref, shortPath];
                cmp.rightLabel = [NSString stringWithFormat:@"%@ (working copy)", shortPath];
                cmp.path = path;
                cmp.title = [NSString stringWithFormat:@"%@ (%@ ↔ working copy)",
                             path.lastPathComponent, ref];
                [win showComparison:cmp preview:preview];
                Json r = MCWindowResult(win);
                r.set("changed", (int)cmp.changedRows);
                respond(Result::success(r));
            });
        });
    });

    d.on("state", [weakHost](const Request &, Dispatcher::Respond respond) {
        id<MCControlAppHost> host = weakHost;
        EditorController *front = [host controlFrontController];
        Json windows = Json::array();
        for (EditorController *c in [host controlControllers])
            windows.push(MCWindowState(c, c == front));
        Json r = Json::object();
        r.set("pid", (int)getpid());
        r.set("windows", windows);
        respond(Result::success(r));
    });

    d.on("selection", [weakHost](const Request &req, Dispatcher::Respond respond) {
        std::string error;
        EditorController *c = MCTarget(weakHost, req.args, nil, error);
        if (!c) { respond(Result::failure(error)); return; }
        NSDictionary *sel = [c controlSelection];
        Json r = sel ? JFromObject(sel) : Json::object();
        if (!r.has("text")) r.set("text", "");
        r.set("window", (int)MCControlWindowID(c));
        respond(Result::success(r));
    });

    d.on("term-output", [weakHost](const Request &req, Dispatcher::Respond respond) {
        id<MCControlAppHost> host = weakHost;
        const NSInteger want = (NSInteger)req.args["terminal"].asInt();
        const int64_t lines = req.args["lines"].asInt(200);
        if (lines < 1 || lines > 100000) {
            respond(Result::failure("lines is from 1 to 100000"));
            return;
        }
        for (EditorController *c in [host controlControllers])
            for (TerminalView *t in [c controlTerminals])
                if (MCControlTerminalID(t) == want) {
                    Json r = Json::object();
                    r.set("terminal", (int)want);
                    r.set("text", J([t recentText:(NSUInteger)lines]));
                    respond(Result::success(r));
                    return;
                }
        respond(Result::failure("there is no terminal " + std::to_string(want)));
    });

    d.on("tour", [weakHost](const Request &req, Dispatcher::Respond respond) {
        id<MCControlAppHost> host = weakHost;
        const Json &a = req.args;
        const Json &list = a["stops"];
        if (list.size() == 0) { respond(Result::failure("a tour needs stops")); return; }
        NSMutableArray<MCTourStop *> *stops = [NSMutableArray array];
        for (size_t i = 0; i < list.size(); i++) {
            NSString *p = MCResolved(NS(list[i]["path"]));
            if (!MCIsFile(p)) {
                respond(Result::failure("stop " + std::to_string(i + 1) + ": no such file: " +
                                        p.UTF8String));
                return;
            }
            MCTourStop *s = [MCTourStop new];
            s.path = p;
            s.line = (int)list[i]["line"].asInt(1);
            s.column = (int)list[i]["column"].asInt(0);
            s.toLine = (int)list[i]["to"].asInt(0);
            s.note = NS(list[i]["note"]);
            [stops addObject:s];
        }
        std::string error;
        EditorController *fallback = [host controlControllerContaining:stops[0].path spelled:nil];
        EditorController *c = MCTarget(host, a, fallback, error);
        if (!c) { respond(Result::failure(error)); return; }
        for (MCTourStop *s in stops) s.path = MCSpelledFrom(c, s.path);
        if (![c.agentNotes startTour:stops from:NS(a["from"])]) {
            respond(Result::failure("could not open the first stop"));
            return;
        }
        Json r = MCWindowResult(c);
        r.set("stops", (int)stops.count);
        respond(Result::success(r));
    });
}
