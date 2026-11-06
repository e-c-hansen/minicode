// attention_tests.inc — the `hooks:*` and `attention:*` groups of
// run_tests.cpp (ClaudeHooks.h, AttentionBoard.h: spec feature 7, "Waiting
// for you" on the right terminal tab), kept in their own file and included
// there.
#include "AttentionBoard.h"
#include "ClaudeHooks.h"

static void testClaudeHooks() {
    using namespace ClaudeHooks;

    GROUP("hooks:events");
    {
        CHECK(actionFor("Notification") == Action::Waiting);
        CHECK(actionFor("Stop") == Action::Done);
        CHECK(actionFor("UserPromptSubmit") == Action::Clear);
        CHECK(actionFor("PostToolUse") == Action::Clear);
        CHECK(actionFor("SubagentStop") == Action::None);
        CHECK(actionFor("PreToolUse") == Action::None);
        CHECK(actionFor("") == Action::None && actionFor("stop") == Action::None);
        for (Action a : {Action::Waiting, Action::Done, Action::Clear})
            CHECK(actionNamed(actionName(a)) == a);
        CHECK(actionNamed("Waiting") == Action::None && std::string(actionName(Action::None)).empty());
    }

    GROUP("hooks:input");
    {
        HookInput in;
        CHECK(parseHookInput(R"({"session_id":"abc","transcript_path":"/h/.claude/projects/x/abc.jsonl",
            "cwd":"/w","hook_event_name":"Notification","message":"Claude needs your permission to use Bash",
            "notification_type":"permission_prompt","something_new":{"x":[1]}})", in));
        CHECK(in.event == "Notification" && in.sessionId == "abc" && in.cwd == "/w" &&
              in.transcriptPath == "/h/.claude/projects/x/abc.jsonl" &&
              in.message == "Claude needs your permission to use Bash");
        CHECK(parseHookInput("{}", in) && in.event.empty());
        CHECK(parseHookInput(R"({"hook_event_name":7,"message":null})", in) && in.event.empty());
        CHECK(!parseHookInput("", in) && !parseHookInput("[]", in) && !parseHookInput("{", in));
        CHECK(!parseHookInput("\xff", in));
    }

    GROUP("hooks:notify-args");
    {
        Json a;
        int pid = 0;
        const std::string stop = R"({"hook_event_name":"Stop","session_id":"s1","cwd":"/w",
                                     "transcript_path":"/t/s1.jsonl"})";
        CHECK(notifyArgsFromHook(stop, "3", "4242", a, pid));
        CHECK(pid == 4242 && a["terminal"].asInt() == 3 && a["state"].asString() == "done" &&
              a["event"].asString() == "Stop" && a["session_id"].asString() == "s1" &&
              a["transcript_path"].asString() == "/t/s1.jsonl" && a["cwd"].asString() == "/w" &&
              !a.has("message"));
        // What a request carries is what the protocol accepts.
        std::string err;
        CHECK(ControlChannel::validateArgs(*ControlChannel::findCommand("notify"), a, err));
        // The variables missing or wrong: nothing to do, quietly.
        CHECK(!notifyArgsFromHook(stop, nullptr, "4242", a, pid));
        CHECK(!notifyArgsFromHook(stop, "3", nullptr, a, pid));
        CHECK(!notifyArgsFromHook(stop, "", "", a, pid));
        CHECK(!notifyArgsFromHook(stop, "x3", "4242", a, pid));
        CHECK(!notifyArgsFromHook(stop, "3", "-1", a, pid));
        CHECK(!notifyArgsFromHook(stop, "3", "12345678901", a, pid));
        CHECK(!notifyArgsFromHook(stop, "0", "4242", a, pid));
        // `echo '{}' | minicode notify --from-hook`, garbage, other events.
        CHECK(!notifyArgsFromHook("{}", "3", "4242", a, pid));
        CHECK(!notifyArgsFromHook("not json", "3", "4242", a, pid));
        CHECK(!notifyArgsFromHook(R"({"hook_event_name":"PreToolUse"})", "3", "4242", a, pid));
        // A message is cleaned as a note is: one line, at most 500 characters.
        std::string longMsg(900, 'x');
        CHECK(notifyArgsFromHook(R"({"hook_event_name":"Notification","message":"Allow\n\u202eBash?)" +
                                     longMsg + "\"}",
                                 "12", "7", a, pid));
        const std::string m = a["message"].asString();
        CHECK(a["state"].asString() == "waiting" && m.find('\n') == std::string::npos &&
              m.compare(0, 12, "Allow Bash?x") == 0 && m.size() < 520);
        CHECK(parsePositive("123456789") == 123456789 && parsePositive("1234567890") == 0 &&
              parsePositive("") == 0 && parsePositive(nullptr) == 0 && parsePositive("1 ") == 0);
    }

    GROUP("hooks:configuration");
    {
        Json c = configuration();
        CHECK(c.isObject() && c.size() == 1 && c["hooks"].size() == events().size());
        for (const std::string &ev : events()) {
            const Json &list = c["hooks"][ev];
            CHECK(list.isArray() && list.size() == 1 && runsOurCommand(list[0]));
            CHECK(list[0]["hooks"][0]["type"].asString() == "command" &&
                  list[0]["hooks"][0]["command"].asString() == kCommand);
            CHECK(actionFor(ev) != Action::None);
        }
        // A merge into nothing is the configuration itself.
        MergeResult r = merge("");
        CHECK(r.ok && r.changed && r.added == events());
        Json back;
        CHECK(Json::parse(r.text, back) && back == c);
    }

    GROUP("hooks:merge");
    {
        // The user's settings and hooks of their own, in an order of theirs.
        const std::string mine = R"({
  "model": "opus",
  "hooks": {
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "say done"
          }
        ]
      }
    ],
    "PreToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          {
            "type": "command",
            "command": "~/bin/check.sh",
            "timeout": 30
          }
        ]
      }
    ]
  },
  "cleanupPeriodDays": 30,
  "ratio": 0.1,
  "name": "Grüße \"quoted\" \\ / \u0001",
  "env": {}
})";
        MergeResult r = merge(mine);
        CHECK(r.ok && r.changed);
        CHECK((r.added == std::vector<std::string>{"Stop", "Notification", "UserPromptSubmit",
                                                   "PostToolUse"}));
        Json before, after;
        CHECK(Json::parse(mine, before) && Json::parse(r.text, after));
        // Every top-level key in its place, every value but hooks untouched.
        CHECK(after.size() == before.size());
        for (size_t i = 0; i < before.items().size(); i++) {
            CHECK(after.items()[i].first == before.items()[i].first);
            if (before.items()[i].first != "hooks")
                CHECK(after.items()[i].second == before.items()[i].second);
        }
        CHECK(after["ratio"].asNumber() == 0.1 && r.text.find("0.1,") != std::string::npos);
        // The user's hooks first and unchanged; ours after theirs.
        const Json &h = after["hooks"];
        CHECK(h.items()[0].first == "Stop" && h.items()[1].first == "PreToolUse");
        CHECK(h["Stop"].size() == 2 && h["Stop"][0] == before["hooks"]["Stop"][0] &&
              runsOurCommand(h["Stop"][1]));
        CHECK(h["PreToolUse"] == before["hooks"]["PreToolUse"]);
        for (const std::string &ev : events()) {
            int ours = 0;
            for (size_t i = 0; i < h[ev].size(); i++) ours += runsOurCommand(h[ev][i]);
            CHECK(ours == 1);
        }
        // Two-space indents, as Claude Code writes the file.
        CHECK(r.text.rfind("{\n  \"model\": \"opus\",\n  \"hooks\": {\n    \"Stop\": [", 0) == 0 && r.text.back() == '\n');
        // Twice changes nothing.
        MergeResult again = merge(r.text);
        CHECK(again.ok && !again.changed && again.added.empty() && again.text.empty());
        // A command spelled with the launcher's whole path counts as ours.
        MergeResult spelled = merge(R"({"hooks":{"Notification":[{"hooks":[{"type":"command",
            "command":"/opt/homebrew/bin/minicode notify --from-hook"}]}]}})");
        CHECK(spelled.ok && spelled.changed &&
              (spelled.added == std::vector<std::string>{"Stop", "UserPromptSubmit", "PostToolUse"}));
        // An event list that has ours among other entries keeps all of them.
        Json s;
        CHECK(Json::parse(spelled.text, s) && s["hooks"]["Notification"].size() == 1);
    }

    GROUP("hooks:merge-refuses");
    {
        // What cannot be merged without losing something is left alone.
        CHECK(!merge("{ \"model\": ").ok);
        CHECK(!merge("[]").ok);
        CHECK(!merge("\"x\"").ok);
        CHECK(!merge(R"({"hooks": []})").ok);
        CHECK(!merge(R"({"hooks": {"Stop": {}}})").ok);
        MergeResult r = merge(R"({"hooks": {"Stop": "say done"}})");
        CHECK(!r.ok && r.error.find("Stop") != std::string::npos && r.text.empty());
        // A list of another event's of the wrong type is not ours to judge.
        CHECK(merge(R"({"hooks": {"Custom": 3}})").ok);
        // Blank is a file not written yet.
        CHECK(merge(" \n\t").ok && merge(" \n\t").changed);
        CHECK(merge("{}").ok && merge("{}").changed);
    }
}

static void testNotifyCommandLine() {
    using namespace ControlChannel;
    GROUP("hooks:command-line");
    auto parse = [](std::vector<std::string> a) {
        return parseCommandLine(a, "/w", "/Users/me", [](const std::string &) { return false; });
    };
    CliCommand c = parse({"notify", "--from-hook"});
    CHECK(c.error.empty() && c.cmd == "notify" && c.fromHook && c.output == Output::None);
    c = parse({"notify", "--terminal", "3", "--state", "waiting", "--message", "Allow\n Bash?"});
    CHECK(c.error.empty() && !c.fromHook && c.args["terminal"].asInt() == 3 &&
          c.args["state"].asString() == "waiting" && c.args["message"].asString() == "Allow Bash?");
    std::string err;
    CHECK(validateArgs(*findCommand("notify"), c.args, err));
    CHECK(parse({"notify", "--terminal=2", "--state=clear", "--pid", "9"}).pid == 9);
    CHECK(!parse({"notify"}).error.empty());
    CHECK(!parse({"notify", "--terminal", "3"}).error.empty());
    CHECK(!parse({"notify", "--state", "done"}).error.empty());
    CHECK(!parse({"notify", "--terminal", "3", "--state", "busy"}).error.empty());
    CHECK(!parse({"notify", "--terminal", "x", "--state", "done"}).error.empty());
    CHECK(!parse({"notify", "--from-hook", "--terminal", "3"}).error.empty());
    CHECK(!parse({"notify", "--from-hook", "extra"}).error.empty());
    // A hook's command never starts minicode.
    CHECK(!launchesApp("notify"));
    c = parse({"hooks", "--print"});
    CHECK(c.error.empty() && c.hooksAction == "print" && c.cmd.empty());
    CHECK(parse({"hooks", "--install"}).hooksAction == "install");
    CHECK(!parse({"hooks"}).error.empty());
    CHECK(!parse({"hooks", "--print", "--install"}).error.empty());
    CHECK(!parse({"hooks", "--remove"}).error.empty());
    CHECK(!parse({"hooks", "--print", "x"}).error.empty());
    CHECK(isSubcommand("notify") && isSubcommand("hooks"));
    CHECK(parse({"hooks", "-h"}).help && usageText("hooks").find("--install") != std::string::npos);
    CHECK(usageText().find("minicode notify --from-hook") != std::string::npos);
}

static void testAttentionBoard() {
    using namespace Attention;
    using ClaudeHooks::Action;

    GROUP("attention:marks");
    {
        Board b;
        Session s1{"s1", "/t/s1.jsonl", "/w"};
        CHECK(b.mark(2) == Mark::None && b.waitingCount() == 0 && !b.session(2));
        CHECK(b.apply(2, Action::Waiting, "Claude needs your permission to use Bash", s1, 10));
        CHECK(b.mark(2) == Mark::Waiting && b.message(2) == "Claude needs your permission to use Bash");
        CHECK(b.waitingCount() == 1);
        // The same again changes nothing on show; a new message does.
        CHECK(!b.apply(2, Action::Waiting, "Claude needs your permission to use Bash", s1, 11));
        CHECK(b.apply(2, Action::Waiting, "Claude is waiting for your input", s1, 12));
        // Approving: PostToolUse clears it.
        CHECK(b.apply(2, Action::Clear, "", s1, 13) && b.mark(2) == Mark::None &&
              b.message(2).empty() && b.waitingCount() == 0);
        CHECK(!b.apply(2, Action::Clear, "", s1, 14));
        CHECK(b.apply(2, Action::Done, "", s1, 15) && b.mark(2) == Mark::Done);
        CHECK(b.waitingCount() == 0);
        // Typing clears either mark, once.
        CHECK(b.typed(2) && b.mark(2) == Mark::None && !b.typed(2));
        CHECK(!b.typed(99));
        // An event that means nothing changes nothing, but records the session.
        Session s2{"s2", "/t/s2.jsonl", "/w"};
        CHECK(!b.apply(5, Action::None, "", s2, 16) && b.session(5) &&
              b.session(5)->sessionId == "s2");
        CHECK(!b.apply(0, Action::Waiting, "x", s2, 17) && b.waitingCount() == 0);
        // A closed terminal is forgotten.
        b.apply(3, Action::Waiting, "", s2, 18);
        CHECK(b.waitingCount() == 1 && b.forget(3) && b.waitingCount() == 0 && !b.forget(3));
        CHECK(strongest(Mark::Done, Mark::Waiting) == Mark::Waiting &&
              strongest(Mark::None, Mark::Done) == Mark::Done &&
              strongest(Mark::None, Mark::None) == Mark::None);
    }

    GROUP("attention:sessions");
    {
        Board b;
        b.apply(1, Action::Clear, "", {"s1", "/t/s1.jsonl", "/w"}, 1);
        b.apply(2, Action::Done, "", {"s2", "/t/s2.jsonl", "/w"}, 2);
        CHECK(b.terminalFor("s1", "") == 1 && b.terminalFor("s2", "/t/s2.jsonl") == 2);
        CHECK(b.terminalFor("", "/t/s1.jsonl") == 1);
        CHECK(b.terminalFor("nope", "/t/s2.jsonl") == 2);   // by the transcript
        CHECK(b.terminalFor("nope", "") == 0 && b.terminalFor("", "") == 0);
        // Resumed in terminal 4: the newest naming wins.
        b.apply(4, Action::Clear, "", {"s1", "/t/s1.jsonl", "/w2"}, 3);
        CHECK(b.terminalFor("s1", "") == 4);
        // A notify without a session keeps the one known.
        b.apply(4, Action::Waiting, "m", {}, 4);
        CHECK(b.session(4) && b.session(4)->cwd == "/w2");
        // A new session in the same terminal replaces it.
        b.apply(4, Action::Clear, "", {"s9", "/t/s9.jsonl", "/w2"}, 5);
        CHECK(b.terminalFor("s1", "") == 1 && b.terminalFor("s9", "") == 4);
        b.forget(1);
        CHECK(b.terminalFor("s1", "") == 0);
    }

    GROUP("attention:next");
    {
        Board b;
        std::vector<int> order{1, 2, 3, 4, 5};
        CHECK(b.nextWaiting(order, 1) == 0);
        b.apply(2, Action::Waiting, "", {}, 1);
        b.apply(4, Action::Waiting, "", {}, 1);
        b.apply(5, Action::Done, "", {}, 1);
        CHECK(b.nextWaiting(order, 1) == 2);
        CHECK(b.nextWaiting(order, 2) == 4);
        CHECK(b.nextWaiting(order, 4) == 2);   // around
        CHECK(b.nextWaiting(order, 5) == 2);
        CHECK(b.nextWaiting(order, 0) == 2);   // from nowhere: the first
        CHECK(b.nextWaiting(order, 77) == 2);
        b.typed(4);
        CHECK(b.nextWaiting(order, 2) == 2);   // the only one, itself
        CHECK(b.nextWaiting({}, 2) == 0);
    }

    GROUP("attention:session-roots");
    {
        std::vector<std::string> m{"/r/src/a.mm", "/r/.claude/worktrees/agent-1/src/a.mm",
                                   "/tmp/claude-501/x/s1/scratchpad/src/a.mm"};
        std::vector<bool> k = preferSessionMatches(m, {"/r/.claude/worktrees/agent-1"});
        CHECK((k == std::vector<bool>{false, true, false}));
        k = preferSessionMatches(m, {"/tmp/claude-501/x/s1/scratchpad/", "/r/.claude/worktrees/agent-1"});
        CHECK((k == std::vector<bool>{false, true, true}));
        // Nothing under the session's roots: every match stays.
        k = preferSessionMatches(m, {"/elsewhere", ""});
        CHECK((k == std::vector<bool>{true, true, true}));
        CHECK((preferSessionMatches(m, {}) == std::vector<bool>{true, true, true}));
        // A root is a folder, not a prefix of a name.
        k = preferSessionMatches({"/r/.claude/worktrees/agent-10/a"}, {"/r/.claude/worktrees/agent-1"});
        CHECK((k == std::vector<bool>{true}));
    }
}
