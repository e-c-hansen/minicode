// SessionWebSuite.mm — `make guitest SUITES=session_web`: a browser's
// read-only view of a session's host (SessionWeb.h, stage 6b), loaded in
// WebKit as Safari would load it. A headless host (SessionServer.h) and its
// browser view run in this process on threads of their own, a renderer
// opens a Python file and a shell in it, and a WKWebView that is in no
// window loads the address with its token: the page draws the tabs, the
// file's lines with the highlighter's colors and the terminal's rows, and
// follows typing and output live, in the settings file's colors and font
// size, and recolors when the file changes. The view may edit: key events
// dispatched into the page type into the host's shell and the file. The
// same address with a wrong token shows only the refusal. Nothing here
// reaches the screen.
#import "Harness.h"
#import <WebKit/WebKit.h>

#include <memory>
#include <thread>

#include "SessionServer.h"
#include "SessionWeb.h"
#include "Text.h"

namespace {

// The renderer that edits: one document's text and a terminal's output
// taken (and acknowledged) as they come.
struct Editor : Shared::Client::Listener, Shared::Client::TermListener {
    Shared::Client* client = nullptr;
    std::u16string text;
    std::string out;
    void hostEdited(Shared::DocId, const Ot::Ops& ops, int, const Shared::Selection*) override {
        Ot::apply(text, ops, Model::Unit::Utf16);
    }
    void termOutput(Shared::TermId t, const std::string& b, long long, bool, bool replay) override {
        out += b;
        if (!replay) client->ackTerminal(t, b.size());
    }
    void type(const std::u16string& s) {
        const std::size_t at = text.size();
        client->openStep(1, {"Typing", true}, {{at, at}});
        Ot::Op o{at, 0, s};
        Ot::apply(text, Ot::Ops{o}, Model::Unit::Utf16);
        client->record(1, o);
        client->closeStep(1, {{text.size(), text.size()}});
    }
};

struct Rig {
    Shared::HostServer host;
    Shared::Web::Server web;
    std::thread hostThread, webThread;
    std::unique_ptr<Shared::HostLink> link;
    std::unique_ptr<Shared::Client> client;
    Editor editor;
};
Rig* gRig = nullptr;
WKWebView* gPage = nil;
NSDictionary* gSeen = nil;   // the page as last read
NSString* gSettings = nil;   // the settings file the page's colors come from

// What the page shows, read by a script in an isolated world.
NSString* const kRead =
    @"JSON.stringify({status: document.getElementById('status').textContent,"
    @" machine: document.getElementById('machine').textContent,"
    @" tabs: [...document.querySelectorAll('#tabs button')].map(b => b.textContent),"
    @" terms: document.querySelectorAll('#termtabs button').length,"
    @" lines: [...document.querySelectorAll('#code .l')].map(l => l.textContent),"
    @" first: [...document.querySelectorAll('#code .l:first-child span')].map(s =>"
    @"   s.className + ' ' + s.textContent + ' ' + getComputedStyle(s).color),"
    @" screen: [...document.querySelectorAll('#screen .r')].map(r => r.textContent).join('\\n'),"
    @" bg: getComputedStyle(document.body).backgroundColor,"
    @" font: getComputedStyle(document.getElementById('code')).fontSize,"
    @" body: document.body ? document.body.innerText.slice(0, 300) : ''})";

// The refusal page has none of the view's elements: only its text.
NSString* const kReadBody =
    @"JSON.stringify({body: document.body ? document.body.innerText.slice(0, 300) : ''})";

// Starts reading the page; the answer lands in gSeen.
void Read(WKWebView* w, NSString* script = kRead) {
    [w evaluateJavaScript:script inFrame:nil inContentWorld:WKContentWorld.defaultClientWorld
        completionHandler:^(id value, NSError* error) {
            if (![value isKindOfClass:NSString.class]) return;
            NSData* d = [(NSString*)value dataUsingEncoding:NSUTF8StringEncoding];
            id j = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
            if ([j isKindOfClass:NSDictionary.class]) gSeen = j;
            (void)error;
        }];
}

NSString* Joined(NSArray* lines) { return [lines componentsJoinedByString:@"\n"]; }

NSString* HostText() {
    std::u16string t = gRig->client->hostDocument(1).text;
    return [NSString stringWithUTF8String:Model::utf8FromUtf16(t).c_str()];
}

}  // namespace

MC_GUI_SUITE(session_web) {
    MCGuiStep(@"a headless host with a Python file and a shell, and its view for a browser", ^{
        NSString* dir = [MCGuiScratch() stringByAppendingPathComponent:@"session-web"];
        NSString* proj = [dir stringByAppendingPathComponent:@"proj"];
        [NSFileManager.defaultManager createDirectoryAtPath:proj withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
        gRig = new Rig();
        Shared::HostOptions o;
        o.folder = Shared::resolveFolder(proj.fileSystemRepresentation);
        o.socketDir = std::string(dir.fileSystemRepresentation) + "/s";
        std::string error;
        GCHECK(gRig->host.start(o, &error) == Shared::HostServer::Start::Ok);
        if (!error.empty()) MCGuiNote(@"host: %s", error.c_str());
        gRig->hostThread = std::thread([] { gRig->host.run(); });
        gRig->link = Shared::HostLink::open(gRig->host.socketPath(), &error);
        GCHECK(gRig->link != nullptr);
        gRig->client = std::make_unique<Shared::Client>(Model::Unit::Utf16, "editor");
        gRig->editor.client = gRig->client.get();
        if (gRig->link) gRig->link->connect(*gRig->client);
        gRig->editor.text = u"def total(xs):\n    return sum(xs)  # all\n";
        gRig->client->open(1, o.folder + "/calc.py", Model::utf8FromUtf16(gRig->editor.text),
                           &gRig->editor);
        Shared::TermSpec t;
        t.program = "/bin/sh";
        t.argv = {"sh"};
        t.env = {"PATH=/usr/bin:/bin", "TERM=xterm-256color", "PS1=$ ", "LANG=C", "ENV=",
                 "HOME=" + o.folder};
        t.cwd = o.folder;
        t.cols = 60;
        t.rows = 12;
        gRig->client->openTerminal(7, t, &gRig->editor);
        GCHECK(gRig->client->settle());

        Shared::Web::Options w;
        w.hostSocket = gRig->host.socketPath();
        w.folder = o.folder;
        w.machine = "guitest-host";
        w.pollMs = 150;
        w.editable = true;
        gSettings = [dir stringByAppendingPathComponent:@"settings.conf"];
        MCGuiWriteFile(gSettings, @"theme = dark-modern\neditor.background = #102030\n");
        w.settingsPath = gSettings.fileSystemRepresentation;
        GCHECK(gRig->web.start(w, &error));
        if (!error.empty()) MCGuiNote(@"web: %s", error.c_str());
        gRig->webThread = std::thread([] { gRig->web.run(); });

        WKWebViewConfiguration* config = [WKWebViewConfiguration new];
        config.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
        gPage = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 900, 700) configuration:config];
        NSURL* url = [NSURL URLWithString:@(gRig->web.url().c_str())];
        [gPage loadRequest:[NSURLRequest requestWithURL:url]];
    });
    MCGuiWait(@"the page draws the file with the highlighter's colors", 30, ^BOOL {
        gRig->client->dispatch();
        Read(gPage);
        NSArray* first = gSeen[@"first"];
        return [gSeen[@"status"] hasPrefix:@"live"] && [gSeen[@"lines"] count] == 3 && first.count > 0;
    });
    MCGuiStep(@"the header, the tab, the lines and their spans", ^{
        GCHECK_EQ_STR(gSeen[@"machine"], @"guitest-host");
        GCHECK([gSeen[@"tabs"] containsObject:@"calc.py"]);
        GCHECK([gSeen[@"terms"] intValue] == 1);
        GCHECK_EQ_STR(Joined(gSeen[@"lines"]), @"def total(xs):\n    return sum(xs)  # all\n");
        NSString* def = [gSeen[@"first"] firstObject];
        MCGuiNote(@"first span: %@", def);
        GCHECK([def hasPrefix:@"s1 def rgb("]);   // TokenStyle::Keyword, colored
        GCHECK(![def hasSuffix:@"rgb(0, 0, 0)"]);
        // The settings file's colors and the editor's font size.
        MCGuiNote(@"background %@, font %@", gSeen[@"bg"], gSeen[@"font"]);
        GCHECK_EQ_STR(gSeen[@"bg"], @"rgb(16, 32, 48)");
        GCHECK_EQ_STR(gSeen[@"font"], @"13px");
        MCGuiWriteFile(gSettings, @"theme = light-plus\neditor.background = #FAFAF0\n"
                                  @"editor.font-size = 16\nsyntax.keyword = #AA0011\n");
    });
    MCGuiWait(@"a change to the settings file recolors the page as it is", 20, ^BOOL {
        Read(gPage);
        return [gSeen[@"bg"] isEqualToString:@"rgb(250, 250, 240)"];
    });
    MCGuiStep(@"its colors, its size and the syntax colors, the text untouched", ^{
        GCHECK_EQ_STR(gSeen[@"font"], @"16px");
        GCHECK([[gSeen[@"first"] firstObject] hasPrefix:@"s1 def rgb(170, 0, 17)"]);
        GCHECK([gSeen[@"lines"] count] == 3);
    });
    MCGuiStep(@"typing in the editor", ^{
        for (int i = 0; i < 12; i++) gRig->editor.type(i % 4 == 3 ? u"\n" : u"x");
        GCHECK(gRig->client->settle());
        gRig->client->terminalInput(7, "echo browser-$((6*7))\n");
    });
    MCGuiWait(@"the page follows the typing", 20, ^BOOL {
        gRig->client->dispatch();
        Read(gPage);
        return [Joined(gSeen[@"lines"]) isEqualToString:HostText()];
    });
    MCGuiWait(@"and the terminal's output", 20, ^BOOL {
        gRig->client->dispatch();
        Read(gPage);
        return [gSeen[@"screen"] containsString:@"browser-42"];
    });
    MCGuiStep(@"keys typed into the page's terminal and its file", ^{
        NSString* js =
            @"(() => { const key = (el, k) => el.dispatchEvent(new KeyboardEvent('keydown',"
            @" {key: k, bubbles: true, cancelable: true}));"
            @" const t = document.getElementById('term');"
            @" for (const c of 'echo page-$((7*6))') key(t, c); key(t, 'Enter');"
            @" const f = document.getElementById('input');"
            @" for (const c of '# page') key(f, c); key(f, 'Enter'); return 1; })()";
        [gPage evaluateJavaScript:js inFrame:nil inContentWorld:WKContentWorld.pageWorld
                completionHandler:^(id, NSError* error) {
                    if (error) MCGuiNote(@"keys: %@", error.localizedDescription);
                }];
    });
    MCGuiWait(@"the shell ran what the page typed", 20, ^BOOL {
        gRig->client->dispatch();
        Read(gPage);
        return [gSeen[@"screen"] containsString:@"page-42\n"] || [gSeen[@"screen"] hasSuffix:@"page-42"];
    });
    MCGuiWait(@"and the file has the page's line, in the page and on the host", 20, ^BOOL {
        gRig->client->dispatch();
        Read(gPage);
        return [HostText() hasPrefix:@"# page\n"] && [Joined(gSeen[@"lines"]) isEqualToString:HostText()];
    });
    MCGuiStep(@"the page drew what the host has", ^{
        GCHECK_EQ_STR(Joined(gSeen[@"lines"]), HostText());
        GCHECK([gSeen[@"screen"] containsString:@"$ echo browser-$((6*7))\nbrowser-42"]);
        GCHECK([gSeen[@"screen"] containsString:@"$ echo page-$((7*6))\npage-42"]);
        // The editor's renderer has the page's line too.
        GCHECK([[NSString stringWithUTF8String:Model::utf8FromUtf16(gRig->editor.text).c_str()]
            isEqualToString:HostText()]);
        MCGuiNote(@"browsers attached: %d", gRig->web.browsers());
        GCHECK(gRig->web.browsers() == 1);
        // A wrong token: only the refusal.
        NSString* bad = [NSString stringWithFormat:@"http://127.0.0.1:%d/?t=%@", gRig->web.port(),
                                                   [@"" stringByPaddingToLength:48 withString:@"0"
                                                                    startingAtIndex:0]];
        gSeen = nil;
        [gPage loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:bad]]];
    });
    MCGuiWait(@"a wrong token shows the refusal and nothing else", 20, ^BOOL {
        Read(gPage, kReadBody);
        return [gSeen[@"body"] containsString:@"wrong token"];
    });
    MCGuiWait(@"and the view lets the host go once no browser looks", 20, ^BOOL {
        return gRig->web.browsers() == 0 && gRig->host.renderers() == 1;
    });
    MCGuiStep(@"everything stops", ^{
        [gPage stopLoading];
        gPage = nil;
        gRig->client->closeTerminal(7);
        gRig->client->settle();
        gRig->link->close();
        gRig->web.stop();
        gRig->webThread.join();
        gRig->host.stop();
        gRig->hostThread.join();
        delete gRig;   // the link's reader is joined: nothing delivers to the client now
        gRig = nullptr;
    });
}
