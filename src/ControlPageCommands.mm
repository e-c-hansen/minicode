// ControlPageCommands.mm — `minicode browse` and `minicode page`: agents
// checking pages in the browser pane (R6.1-R6.5). Everything happens in the
// pane's agent page (Browser.h), never the user's, unless `yours` asks to
// read the user's page; reads of a page (text, console, screenshot) work
// only for local pages unless control.page-any-origin is set (PageCheck.h).
// No command runs JavaScript an agent sent: there is no such argument.
#import "ControlCommands.h"
#import "ControlServer.h"
#import "AppSettings.h"
#import "Browser.h"
#import "BrowserPages.h"
#import "EditorController.h"
#include "PageCheck.h"
#include <sys/stat.h>

using ControlChannel::Dispatcher;
using ControlChannel::Request;
using ControlChannel::Result;

namespace {

NSString *NS(const Json &j) {
    return [NSString stringWithUTF8String:j.asString().c_str()] ?: @"";
}

std::string U(NSString *s) { return s.UTF8String ?: ""; }

// Whether an agent may read the page at `url`.
bool mayRead(NSString *url, std::string &error) {
    if ([AppSettings shared].settings.pageAnyOrigin() || PageCheck::isLocalAddress(U(url)))
        return true;
    error = PageCheck::originError(U(url));
    return false;
}

Json pageResult(MCPageProbe *probe, BOOL agent) {
    Json r = [probe stateJson];
    r.set("page", agent ? "agent" : "yours");
    if (agent) r.set("console_seq", probe.consoleLog->lastSeq());
    return r;
}

// Answers once the load is done (with --wait), or at once.
void answerLoad(MCPageProbe *probe, WKNavigation *nav, BOOL wait, BOOL agent, Json extra,
                Dispatcher::Respond respond) {
    if (!wait) {
        respond(Result::success(extra));
        return;
    }
    [probe waitFor:nav done:^(NSString *error, BOOL timedOut) {
        if (error) {
            respond(Result::failure(U(error) + (probe.url.length ? " (" + U(probe.url) + ")" : "")));
            return;
        }
        Json r = pageResult(probe, agent);
        if (timedOut) r.set("timed_out", true);
        for (const auto &kv : extra.items())
            if (kv.first == "pane_busy") r.set(kv.first, kv.second);
        respond(Result::success(r));
    }];
}

}  // namespace

Json MCControlBrowserPages(EditorController *c) {
    Json list = Json::array();
    BrowserView *b = [c browserPane];
    if (!b) return list;
    NSDictionary *shown = [c controlBrowser];
    const bool visible = [shown[@"visible"] boolValue];
    for (int agent = 0; agent < 2; agent++) {
        MCPageProbe *p = agent ? b.agentProbe : b.userProbe;
        if (!p) continue;
        Json o = Json::object();
        o.set("page", agent ? "agent" : "yours");
        o.set("url", U(p.url));
        o.set("title", U(p.title));
        o.set("status", p.status > 0 ? Json(p.status) : Json());
        o.set("loading", (bool)p.loading);
        o.set("shown", (bool)(agent ? b.agentPageShown : !b.agentPageShown));
        o.set("visible", visible);
        list.push(o);
    }
    return list;
}

void MCControlRegisterPageCommands(Dispatcher &d, id<MCControlAppHost> hostIn) {
    __weak id<MCControlAppHost> weakHost = hostIn;

    d.on("browse", [weakHost](const Request &req, Dispatcher::Respond respond) {
        const Json &a = req.args;
        if (a["split"].asString() == "below") {
            respond(Result::failure("the browser opens beside the editor: use --right"));
            return;
        }
        std::string url, error;
        auto exists = [](const std::string &p) {
            struct stat st;
            return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        };
        if (!PageCheck::agentAddress(a["url"].asString(), exists, url, error)) {
            respond(Result::failure(error));
            return;
        }
        EditorController *c = MCControlTarget(weakHost, a, nil, error);
        if (!c) { respond(Result::failure(error)); return; }
        BrowserView *b = [c agentBrowserPane];
        MCPageProbe *probe = [b openAgentPage];
        WKNavigation *nav = [b loadInAgentPage:@(url.c_str())];
        if (!nav) {
            respond(Result::failure("could not load " + url));
            return;
        }
        Json extra = Json::object();
        extra.set("window", (int)MCControlWindowID(c));
        extra.set("page", "agent");
        if (![b showAgentPageQuietly]) extra.set("pane_busy", true);
        answerLoad(probe, nav, a["wait"].asBool(), YES, extra, respond);
    });

    d.on("page", [weakHost](const Request &req, Dispatcher::Respond respond) {
        const Json &a = req.args;
        const std::string action = a["action"].asString();
        if (action != "url" && action != "text" && action != "console" &&
            action != "screenshot" && action != "reload") {
            respond(Result::failure("page does url, text, console, screenshot or reload, not \"" +
                                    action + "\""));
            return;
        }
        std::string error;
        EditorController *c = MCControlTarget(weakHost, a, nil, error);
        if (!c) { respond(Result::failure(error)); return; }
        const BOOL yours = a["yours"].asBool();
        BrowserView *b = [c browserPane];
        if (!b || (!yours && !b.hasAgentPage)) {
            respond(Result::failure(yours ? "the browser pane is not open"
                                          : "there is no agent page: minicode browse opens one"));
            return;
        }
        MCPageProbe *probe = yours ? b.userProbe : b.agentProbe;
        __weak MCPageProbe *weakProbe = probe;

        if (action == "url") {
            respond(Result::success(pageResult(probe, !yours)));
            return;
        }
        if (action == "reload") {
            if (yours && !mayRead(probe.url, error)) { respond(Result::failure(error)); return; }
            WKNavigation *nav = yours ? [b reloadUserPage] : [b reloadAgentPage];
            if (!nav) { respond(Result::failure("there is no page to reload")); return; }
            Json extra = Json::object();
            extra.set("window", (int)MCControlWindowID(c));
            if (!yours && ![b showAgentPageQuietly]) extra.set("pane_busy", true);
            answerLoad(probe, nav, a["wait"].asBool(), !yours, extra, respond);
            return;
        }
        if (!mayRead(probe.url, error)) { respond(Result::failure(error)); return; }
        NSString *readFrom = probe.url;
        // The page may have moved on while it was read: what was read must
        // still be from a page the agent may read.
        auto stillAllowed = [weakProbe, readFrom](std::string &why) {
            MCPageProbe *p = weakProbe;
            if (!p) { why = "the page was closed"; return false; }
            if (![p.url isEqualToString:readFrom]) {
                if (!mayRead(p.url, why)) return false;
            }
            return true;
        };

        if (action == "console") {
            if (yours) {
                respond(Result::failure("the console is recorded for the agent page only"));
                return;
            }
            const int64_t since = a["since"].asInt(0);
            Json entries = Json::array();
            for (const PageCheck::ConsoleEntry &e : probe.consoleLog->since(since))
                entries.push(PageCheck::entryJson(e));
            Json r = Json::object();
            r.set("entries", entries);
            r.set("last", probe.consoleLog->lastSeq());
            if (probe.consoleLog->dropped()) r.set("dropped", probe.consoleLog->dropped());
            respond(Result::success(r));
            return;
        }
        if (action == "text") {
            [probe readText:^(NSString *text, BOOL cut, NSString *readError) {
                std::string why;
                if (readError) { respond(Result::failure("could not read the page: " + U(readError))); return; }
                if (!stillAllowed(why)) { respond(Result::failure(why)); return; }
                Json r = Json::object();
                r.set("text", U(text));
                if (cut) r.set("truncated", true);
                respond(Result::success(r));
            }];
            return;
        }
        // screenshot
        NSString *path = NS(a["path"]);
        const BOOL overwrite = a["overwrite"].asBool();
        struct stat st;
        PageCheck::PathAt at = PageCheck::PathAt::Nothing;
        if (path.length && lstat(path.fileSystemRepresentation, &st) == 0)
            at = S_ISDIR(st.st_mode)   ? PageCheck::PathAt::Folder
                 : S_ISREG(st.st_mode) ? PageCheck::PathAt::File
                                       : PageCheck::PathAt::Other;
        if (!PageCheck::screenshotPath(U(path), at, overwrite, error)) {
            respond(Result::failure(error));
            return;
        }
        [probe snapshotTo:path full:a["full"].asBool() overwrite:overwrite
                     done:^(NSDictionary *info, NSString *snapError) {
            std::string why;
            if (snapError) { respond(Result::failure("no screenshot: " + U(snapError))); return; }
            if (!stillAllowed(why)) {
                [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
                respond(Result::failure(why));
                return;
            }
            Json r = Json::object();
            r.set("path", U(path));
            r.set("width", (int)[info[@"width"] integerValue]);
            r.set("height", (int)[info[@"height"] integerValue]);
            if ([info[@"cut"] boolValue]) r.set("cut", true);
            respond(Result::success(r));
        }];
    });
}
