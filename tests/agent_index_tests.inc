// agent_index_tests.inc — the `agentindex:*` groups of run_tests.cpp
// (AgentTranscript.h, AgentIndex.h), kept in their own file and included
// there; agent_evidence_tests.inc uses its helpers. Fixtures are in
// tests/fixtures/claude: trimmed, sanitized excerpts of real Claude Code
// transcripts.

#include <chrono>

namespace agentidx {

namespace fsys = std::filesystem;

// Lines shaped like Claude Code's, for building transcripts in a scratch tree.
std::string q(const std::string &s) { return Json(s).dump(); }

std::string head(const std::string &type, const std::string &sid, const std::string &cwd,
                 const std::string &ts, const std::string &extra = "") {
    return "{\"parentUuid\":null,\"isSidechain\":false,\"type\":" + q(type) +
           ",\"timestamp\":" + q(ts) + ",\"cwd\":" + q(cwd) + ",\"sessionId\":" + q(sid) +
           ",\"version\":\"9.9.9\",\"gitBranch\":\"main\"" + extra;
}
std::string prompt(const std::string &sid, const std::string &cwd, const std::string &ts,
                   const std::string &text) {
    return head("user", sid, cwd, ts, ",\"origin\":{\"kind\":\"human\"}") +
           ",\"message\":{\"role\":\"user\",\"content\":" + q(text) + "}}";
}
std::string use(const std::string &sid, const std::string &cwd, const std::string &ts,
                const std::string &id, const std::string &name, const std::string &input,
                const std::string &msgId = "msg_1") {
    return head("assistant", sid, cwd, ts) + ",\"message\":{\"id\":" + q(msgId) +
           ",\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":" + q(id) +
           ",\"name\":" + q(name) + ",\"input\":" + input + "}]}}";
}
std::string bash(const std::string &sid, const std::string &cwd, const std::string &ts,
                 const std::string &id, const std::string &cmd, const std::string &desc) {
    return use(sid, cwd, ts, id, "Bash",
               "{\"command\":" + q(cmd) + ",\"description\":" + q(desc) + "}");
}
std::string write(const std::string &sid, const std::string &cwd, const std::string &ts,
                  const std::string &id, const std::string &path) {
    return use(sid, cwd, ts, id, "Write",
               "{\"file_path\":" + q(path) + ",\"content\":\"x\\n\"}");
}
std::string result(const std::string &sid, const std::string &cwd, const std::string &ts,
                   const std::string &id, const std::string &content, bool error = false) {
    return head("user", sid, cwd, ts) +
           ",\"message\":{\"role\":\"user\",\"content\":[{\"tool_use_id\":" + q(id) +
           ",\"type\":\"tool_result\",\"content\":" + q(content) +
           ",\"is_error\":" + (error ? "true" : "false") + "}]},\"toolUseResult\":{\"stdout\":" +
           q(content.substr(0, 64)) + "}}";
}
std::string text(const std::string &sid, const std::string &cwd, const std::string &ts,
                 const std::string &body, const std::string &msgId) {
    return head("assistant", sid, cwd, ts) + ",\"message\":{\"id\":" + q(msgId) +
           ",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":" + q(body) + "}]}}";
}

void put(const fsys::path &p, const std::string &data, bool append = false) {
    fsys::create_directories(p.parent_path());
    std::ofstream f(p, append ? std::ios::app | std::ios::binary : std::ios::binary);
    f << data;
}

// The real POSIX Fs, counting what it reads.
struct CountingFs {
    std::shared_ptr<uint64_t> bytes = std::make_shared<uint64_t>(0);
    AgentTranscript::Fs fs;
    CountingFs() {
        fs = AgentTranscript::posixFs();
        auto inner = fs.read;
        auto b = bytes;
        fs.read = [inner, b](const std::string &p, uint64_t off, size_t n, std::string *out) {
            bool ok = inner(p, off, n, out);
            *b += out->size();
            return ok;
        };
    }
};

fsys::path scratch(const char *name) {
    fsys::path p = fsys::temp_directory_path() /
                   ("minicode-agentidx-" + std::to_string((long)getpid()) + "-" + name);
    fsys::remove_all(p);
    fsys::create_directories(p);
    return p;
}

}  // namespace agentidx

static void testAgentTranscript() {
    using namespace AgentTranscript;
    using namespace agentidx;

    GROUP("agentindex:time");
    CHECK(parseTime("1970-01-01T00:00:00Z") == 0);
    CHECK(parseTime("2026-11-05T19:39:56.507Z") > 1793900000 &&
          std::fabs(parseTime("2026-11-05T19:39:56.507Z") - 1793907596.507) < 1e-3);
    CHECK(std::fabs(parseTime("2026-11-05T21:39:56+02:00") - 1793907596) < 1e-6);
    CHECK(parseTime("") == 0);
    CHECK(parseTime("yesterday") == 0);
    CHECK(parseTime("2026-13-05T00:00:00Z") == 0);

    GROUP("agentindex:lines");
    const Format &cc = claudeCode();
    CHECK(std::string(cc.name()) == "Claude Code");
    Line l;
    {
        std::string s = prompt("S1", "/w/p", "2026-11-05T10:00:00Z", "Fix the \"tree\"\n\\ caf\\u00e9");
        CHECK(cc.parseLine(s.data(), s.size(), 100, &l));
        CHECK(l.type == "user" && l.sessionId == "S1" && l.cwd == "/w/p" && l.version == "9.9.9");
        CHECK(l.gitBranch == "main" && l.prompt && !l.meta && l.span.offset == 100);
        CHECK(l.blocks.size() == 1 && l.blocks[0].kind == Block::Kind::Text);
        CHECK(l.blocks[0].text == "Fix the \"tree\"\n\\ caf\\u00e9");
        CHECK(l.time == parseTime("2026-11-05T10:00:00Z"));
    }
    {
        // Escapes as JSON writes them, a surrogate pair, unknown fields and
        // block types, nesting.
        std::string s =
            "{\"type\":\"assistant\",\"future\":{\"deep\":[[[{\"x\":[1,2.5e3,-4,true,null]}]]]},"
            "\"message\":{\"id\":\"m9\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"hm\"},"
            "{\"type\":\"text\",\"text\":\"caf\\u00e9 \\ud83d\\ude00 \\\"q\\\" a\\/b\\tc\"},"
            "{\"type\":\"hologram\",\"text\":\"ignored\"},"
            "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Edit\",\"input\":"
            "{\"file_path\":\"/w/p/a.py\",\"old_string\":\"x\",\"new_string\":\"y\"}},"
            "{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"NotebookEdit\",\"input\":"
            "{\"notebook_path\":\"/w/p/n.ipynb\"}}]},\"sessionId\":\"S1\"}";
        CHECK(cc.parseLine(s.data(), s.size(), 0, &l));
        CHECK(l.messageId == "m9" && l.blocks.size() == 3);
        CHECK(l.blocks[0].text == "caf\xC3\xA9 \xF0\x9F\x98\x80 \"q\" a/b\tc");
        CHECK(l.blocks[1].kind == Block::Kind::ToolUse && l.blocks[1].name == "Edit" &&
              l.blocks[1].filePath == "/w/p/a.py" && l.blocks[1].id == "t1");
        CHECK(l.blocks[2].filePath == "/w/p/n.ipynb");
        CHECK(!l.prompt);
    }
    {
        // A tool result: string content, array content, is_error, the
        // spawned agent from toolUseResult.
        std::string s = result("S1", "/w/p", "2026-11-05T10:00:01Z", "t1", "line 1\nline \"2\"", true);
        CHECK(cc.parseLine(s.data(), s.size(), 1000, &l));
        CHECK(l.blocks.size() == 1 && l.blocks[0].kind == Block::Kind::ToolResult);
        CHECK(l.blocks[0].id == "t1" && l.blocks[0].isError && !l.prompt);
        const Span sp = l.blocks[0].output;
        CHECK(sp.offset >= 1000 && s.compare(sp.offset - 1000, sp.length, q("line 1\nline \"2\"")) == 0);
        std::string a =
            "{\"type\":\"user\",\"message\":{\"content\":[{\"tool_use_id\":\"t3\",\"type\":"
            "\"tool_result\",\"content\":[{\"type\":\"image\"},{\"type\":\"text\",\"text\":\"ok\"}]}]},"
            "\"toolUseResult\":{\"agentId\":\"a77\",\"status\":\"async_launched\"}}";
        CHECK(cc.parseLine(a.data(), a.size(), 0, &l));
        CHECK(l.blocks.size() == 1 && l.spawnedAgent == "a77" &&
              a.compare(l.blocks[0].output.offset, l.blocks[0].output.length, "\"ok\"") == 0);
    }
    {
        // What is not a prompt: meta lines, notifications, tool results, tags.
        std::string s = "{\"type\":\"user\",\"isMeta\":true,\"message\":{\"content\":\"Caveat\"}}";
        CHECK(cc.parseLine(s.data(), s.size(), 0, &l) && !l.prompt && l.meta);
        s = "{\"type\":\"user\",\"origin\":{\"kind\":\"task-notification\"},\"message\":{\"content\":\"done\"}}";
        CHECK(cc.parseLine(s.data(), s.size(), 0, &l) && !l.prompt);
        s = "{\"type\":\"user\",\"message\":{\"content\":\"  <local-command-stdout>x</local-command-stdout>\"}}";
        CHECK(cc.parseLine(s.data(), s.size(), 0, &l) && !l.prompt);
        s = "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"You are an agent\"}]}}";
        CHECK(cc.parseLine(s.data(), s.size(), 0, &l) && l.prompt);   // a subagent's task
        s = "{\"type\":\"ai-title\",\"aiTitle\":\"Fix tabs\",\"sessionId\":\"S1\"}";
        CHECK(cc.parseLine(s.data(), s.size(), 0, &l) && l.aiTitle == "Fix tabs" && l.blocks.empty());
        // The copies of requests Claude Code logs are not the conversation.
        s = "{\"type\":\"api-request-blob\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":"
            "\"tool_use\",\"id\":\"t1\",\"name\":\"Bash\",\"input\":{\"command\":\"ls\"}}]}}";
        CHECK(cc.parseLine(s.data(), s.size(), 0, &l) && l.blocks.empty());
    }
    // Lines that are not one JSON object are refused, never a crash.
    for (const char *bad : {"", "[1,2]", "{\"type\":\"user\"", "{\"type\":\"user\"} x",
                            "{\"type\":\"us", "{\"a\":tru}", "{\"a\":1,}", "{\"a\" 1}", "nul",
                            "{\"a\":\"\\\"}", "{\"a\":-}", "\"just a string\""}) {
        std::string s = bad;
        CHECK(!cc.parseLine(s.data(), s.size(), 0, &l));
    }
    {
        std::string deep(300, '[');
        std::string s = "{\"a\":" + deep + std::string(300, ']') + "}";
        CHECK(!cc.parseLine(s.data(), s.size(), 0, &l));   // too deep: refused, not a stack overflow
    }
    {
        // A text over the cap is cut, at a character boundary.
        std::string big(kTextCap + 10, 'a');
        big[kTextCap - 1] = '\xC3';
        big[kTextCap] = '\xA9';
        std::string s = text("S", "/w", "2026-11-05T10:00:00Z", big, "m");
        CHECK(cc.parseLine(s.data(), s.size(), 0, &l));
        CHECK(l.blocks.size() == 1 && l.blocks[0].textCut && l.blocks[0].text.size() == kTextCap - 1);
    }

    GROUP("agentindex:tail");
    {
        fsys::path dir = scratch("tail");
        fsys::path f = dir / "t.jsonl";
        std::string l1 = prompt("S", "/w", "2026-11-05T10:00:00Z", "one");
        std::string l2 = bash("S", "/w", "2026-11-05T10:00:01Z", "b1", "ls", "List");
        std::string l3 = result("S", "/w", "2026-11-05T10:00:02Z", "b1", "a\nb");
        put(f, l1 + "\n" + l2 + "\r\n\n" + l3.substr(0, 20));
        Tail t;
        t.path = f.string();
        Fs fs = posixFs();
        std::vector<std::string> types;
        int resets = 0;
        auto on = [&](const Line &x) { types.push_back(x.type); return true; };
        auto reset = [&]() { resets++; };
        ReadStats st;
        CHECK(t.read(fs, claudeCode(), on, reset, &st));
        CHECK(types.size() == 2 && st.lines == 2 && st.badLines == 0 && !st.reset);
        CHECK(t.offset == l1.size() + 1 + l2.size() + 3);   // the partial line is held back
        put(f, l3.substr(20) + "\n{not json}\n", true);
        st = ReadStats();
        CHECK(t.read(fs, claudeCode(), on, reset, &st));
        CHECK(types.size() == 3 && types[2] == "user" && st.badLines == 1 && st.lines == 2);
        CHECK(st.bytesRead == l3.size() + 12);   // what was added, and the held line's start again
        st = ReadStats();
        CHECK(t.read(fs, claudeCode(), on, reset, &st) && st.bytesRead == 0 && st.lines == 0);
        // Replaced by a shorter file: read again from the start.
        put(f, l1 + "\n");
        st = ReadStats();
        types.clear();
        CHECK(t.read(fs, claudeCode(), on, reset, &st));
        CHECK(st.reset && resets == 1 && types.size() == 1);
        // Lines longer than the chunk, and a callback that stops.
        std::string longLine = result("S", "/w", "2026-11-05T10:00:03Z", "b2", std::string(20000, 'z'));
        put(f, l1 + "\n" + longLine + "\n" + l2 + "\n");
        Tail t2;
        t2.path = f.string();
        types.clear();
        st = ReadStats();
        CHECK(t2.read(fs, claudeCode(), on, reset, &st, 4096));
        CHECK(types.size() == 3 && st.badLines == 0);
        Tail t3;
        t3.path = f.string();
        int n = 0;
        t3.read(fs, claudeCode(), [&](const Line &) { return ++n < 2; }, reset, nullptr, 4096);
        CHECK(n == 2 && t3.offset == l1.size() + 1 + longLine.size() + 1);
        Tail gone;
        gone.path = (dir / "missing.jsonl").string();
        CHECK(!gone.read(fs, claudeCode(), on, reset, nullptr));
        fsys::remove_all(dir);
    }

    GROUP("agentindex:output");
    {
        fsys::path dir = scratch("output");
        fsys::path f = dir / "o.jsonl";
        // 30 lines, then one large output of 200,000 lines with escapes near
        // the window edges, then a persisted preview.
        std::string small;
        for (int i = 1; i <= 30; i++) small += "line " + std::to_string(i) + "\n";
        std::string big;
        for (int i = 1; i <= 200000; i++) big += "row \"" + std::to_string(i) + "\" \xC3\xA9\\\n";
        std::string persisted =
            "<persisted-output>\nOutput too large (36.4KB). Full output saved to: /x/tool-results/b1.txt\n\nPreview (first 2KB):\nabc";
        std::string a = result("S", "/w", "2026-11-05T10:00:00Z", "r1", small);
        std::string b = result("S", "/w", "2026-11-05T10:00:01Z", "r2", big);
        std::string c = result("S", "/w", "2026-11-05T10:00:02Z", "r3", persisted);
        put(f, a + "\n" + b + "\n" + c + "\n");
        std::vector<Span> spans;
        Tail t;
        t.path = f.string();
        CountingFs cfs;
        t.read(cfs.fs, claudeCode(), [&](const Line &x) {
            spans.push_back(x.blocks.at(0).output);
            return true;
        }, nullptr, nullptr);
        CHECK(spans.size() == 3);
        Output o = readOutput(cfs.fs, f.string(), spans[0], 20, 5);
        CHECK(o.head.size() == 20 && o.tail.size() == 5 && o.omitted == 5 && !o.whole);
        CHECK(o.head[0] == "line 1" && o.tail[4] == "line 30" && o.tail[0] == "line 26");
        o = readOutput(cfs.fs, f.string(), spans[0], 20, 20);
        CHECK(o.whole && o.head.size() == 20 && o.tail.size() == 10 && o.omitted == 0);
        *cfs.bytes = 0;
        o = readOutput(cfs.fs, f.string(), spans[1], 20, 20);
        CHECK(spans[1].length > 3 * 1000 * 1000);
        CHECK(*cfs.bytes <= 2 * 64 * 1024);   // the ends only
        CHECK(o.omitted == -1 && o.head.size() == 20 && o.tail.size() == 20);
        CHECK(o.head[0] == "row \"1\" \xC3\xA9\\" && o.head[19] == "row \"20\" \xC3\xA9\\");
        CHECK(o.tail[19] == "row \"200000\" \xC3\xA9\\" && o.tail[0] == "row \"199981\" \xC3\xA9\\");
        bool cut = false;
        std::string t100 = readOutputText(cfs.fs, f.string(), spans[1], 100, &cut);
        CHECK(cut && t100.size() <= 100 && t100.compare(0, 10, "row \"1\" \xC3\xA9") == 0);
        std::string whole = readOutputText(cfs.fs, f.string(), spans[0], 1 << 20, &cut);
        CHECK(!cut && whole == small);
        o = readOutput(cfs.fs, f.string(), spans[2]);
        CHECK(o.persistedPath == "/x/tool-results/b1.txt");
        CHECK(persistedOutputPath("plain") == "");
        CHECK(backgroundOutputPath("Command running in background with ID: b7. Output is being "
                                   "written to: /t/tasks/b7.output") == "/t/tasks/b7.output");
        CHECK(backgroundOutputPath("Output is being written to: /t/x.output.\nmore") == "/t/x.output");
        CHECK(backgroundOutputPath("nothing here") == "");
        // A tail window that starts inside an escape or a character.
        std::string raw = "\\u00e9xyz\\nsecond\\\\n\\\"third\\\"";
        CHECK(decodeJsonFragment(raw.data() + 3, raw.size() - 3, false) == "\nsecond\\n\"third\"");
        CHECK(decodeJsonFragment(raw.data(), raw.size(), true) == "\xC3\xA9xyz\nsecond\\n\"third\"");
        // A plain persisted file, both ways.
        put(dir / "p.txt", small);
        o = readTextFile(cfs.fs, (dir / "p.txt").string(), 2, 2);
        CHECK(o.head == (std::vector<std::string>{"line 1", "line 2"}) && o.tail.back() == "line 30");
        put(dir / "q.txt", big);
        o = readTextFile(cfs.fs, (dir / "q.txt").string(), 1, 1, 4096);
        CHECK(o.omitted == -1 && o.head[0] == "row \"1\" \xC3\xA9\\" &&
              o.tail[0] == "row \"200000\" \xC3\xA9\\");
        fsys::remove_all(dir);
    }
}

static void testAgentIndex() {
    using namespace AgentIndex;
    using namespace agentidx;
    using AgentTranscript::Fs;

    GROUP("agentindex:paths");
    CHECK(normalizePath("/a/./b//c/../d/") == "/a/b/d");
    CHECK(normalizePath("a/../../b") == "../b");
    CHECK(normalizePath("/..") == "/");
    CHECK(isInside("/w/p", "/w/p") && isInside("/w/p/x", "/w/p") && !isInside("/w/pq", "/w/p"));
    CHECK(classify("/w/p/src/a.cpp", "/w/p", "") == Place::Main);
    CHECK(classify("/w/p/.claude/worktrees/agent-x/a.cpp", "/w/p", "/w/p/.claude/worktrees/agent-x") ==
          Place::Worktree);
    CHECK(classify("/w/p/.claude/worktrees/agent-y/a.cpp", "/w/p", "") == Place::Worktree);
    CHECK(classify("/private/tmp/claude-1/x/scratchpad/r.md", "/w/p", "") == Place::Outside);
    CHECK(std::string(placeName(Place::Outside)) == "Outside the folder");

    GROUP("agentindex:describe");
    CHECK(describePrompt("  Fix   the\n\ttree  ") == "Fix the tree");
    CHECK(describePrompt(std::string(100, 'a')) == std::string(80, 'a') + "\xE2\x80\xA6");
    CHECK(describePrompt("<command-message>review</command-message>\n<command-name>/review</command-name>"
                         "\n<command-args>src/a.cpp</command-args>") == "/review src/a.cpp");
    CHECK(describePrompt("a < b and c > d") == "a < b and c > d");
    {
        std::string s;
        for (int i = 0; i < 90; i++) s += "\xC3\xA9";
        std::string d = describePrompt(s);
        CHECK(d.size() == 80 * 2 + 3);   // 80 characters, never half of one
    }

    GROUP("agentindex:worktrees");
    {
        const std::string porcelain =
            "worktree /w/p\nHEAD 1111\nbranch refs/heads/main\n\n"
            "worktree /w/p/.claude/worktrees/agent-a1ad4e3e0a7ec7fd2\nHEAD 2222\n"
            "branch refs/heads/worktree-agent-a1ad4e3e0a7ec7fd2\nlocked\n\n"
            "worktree /w/p/.claude/worktrees/agent-bbb\nHEAD 3333\ndetached\nprunable gitdir file points to non-existent location\n\n"
            "worktree /elsewhere/wt\nHEAD 4444\nbranch refs/heads/x\n";
        std::vector<Worktree> all = parseWorktreeList(porcelain);
        CHECK(all.size() == 4 && all[0].main && !all[1].main);
        CHECK(all[0].branch == "main" && all[1].branch == "worktree-agent-a1ad4e3e0a7ec7fd2");
        CHECK(all[1].locked && !all[1].detached && all[2].detached && all[2].prunable &&
              all[2].branch.empty() && all[3].head == "4444");
        std::string z = porcelain;
        std::replace(z.begin(), z.end(), '\n', '\0');
        CHECK(parseWorktreeList(z).size() == 4);
        std::vector<Worktree> under = worktreesUnder(all, "/w/p", nullptr, 0);
        CHECK(under.size() == 2 && under[0].agentId == "a1ad4e3e0a7ec7fd2" && under[1].agentId == "bbb");
        CHECK(parseWorktreeList("").empty());
    }

    // ------------------------------------------------ the fixtures
    std::ifstream probe("tests/fixtures/claude/projects/-Users-user-Code-public-minicode/"
                        "1e1f578c-45c6-4cb5-92e5-84e11a306454.jsonl");
    if (!probe) {
        std::printf("  (tests/fixtures/claude not found here; fixture checks skipped)\n");
        return;
    }
    probe.close();
    GROUP("agentindex:fixtures");
    const std::string folder = "/Users/user/Code/public/minicode";
    const std::string sid = "1e1f578c-45c6-4cb5-92e5-84e11a306454";
    const std::string wt = folder + "/.claude/worktrees/agent-a1ad4e3e0a7ec7fd2";
    fsys::path root = scratch("fixtures");
    fsys::path projects = root / "projects";
    fsys::copy("tests/fixtures/claude/projects", projects, fsys::copy_options::recursive);
    fsys::path tmpRoot = root / "tmp";
    fsys::create_directories(tmpRoot / "-Users-user-Code-public-minicode" / sid / "scratchpad");
    fsys::create_directories(tmpRoot / "-Users-user-Other" / "someone-else" / "scratchpad");
    // A session of another folder, which must never be read past its cwd.
    fsys::path other = projects / "-Users-user-Other" / "o1.jsonl";
    {
        std::string o = prompt("o1", "/Users/user/Other", "2026-11-05T09:00:00Z", "other");
        for (int i = 0; i < 2000; i++)
            o += "\n" + bash("o1", "/Users/user/Other", "2026-11-05T09:00:01Z", "x" + std::to_string(i),
                             "echo " + std::string(100, 'x'), "Pad");
        put(other, o + "\n");
    }
    const fsys::path mainT = projects / "-Users-user-Code-public-minicode" / (sid + ".jsonl");
    const fsys::path subT = projects / "-Users-user-Code-public-minicode" / sid / "subagents" /
                            "agent-a1ad4e3e0a7ec7fd2.jsonl";
    CountingFs cfs;
    Index index(cfs.fs, {projects.string(), tmpRoot.string()});
    CHECK(!index.refresh());   // no folder yet: nothing
    index.setFolder(folder + "/");
    RefreshStats rs;
    CHECK(index.refresh(&rs));
    CHECK(rs.transcripts == 3 && rs.badLines == 0);
    // The other folder's 250 KB: its first 64 KB chunk only.
    CHECK(fsys::file_size(other) > 200 * 1024);
    CHECK(rs.bytesRead < 26 * 1024 + 64 * 1024);
    std::vector<Index::Row> rows = index.rows();
    CHECK(rows.size() == 2);
    if (rows.size() == 2) {
        const Session &m = *rows[0].session;
        const Session &s = *rows[1].session;
        CHECK(rows[0].depth == 0 && rows[1].depth == 1);
        CHECK(m.transcript == mainT.string() && m.agentId.empty() && m.parent.empty());
        CHECK(m.description == "Placeholder request for the main session.");
        CHECK(m.title == "Placeholder title" && m.sessionId == sid && m.cwd == folder);
        CHECK(m.versions == std::vector<std::string>{"2.1.318"});
        CHECK(m.bashCommands == 1 && m.toolUses == 4 && m.prompts == 1 && m.badLines == 0);
        CHECK(m.agentCalls == std::vector<std::string>{"toolu_01MuAehcWya4Y9bULcWFJECM"});
        CHECK(m.spawned == std::vector<std::string>{"a1ad4e3e0a7ec7fd2"});
        CHECK(m.files.size() == 1 && m.filesTouched() == 1 &&
              m.files[0].path == folder + "/scripts/minicode-launcher" &&
              m.files[0].place == Place::Main && m.files[0].writes == 1 && m.files[0].edits == 1);
        CHECK(m.worktreePath.empty() && !m.hasMeta && !m.finished);
        CHECK(m.started == AgentTranscript::parseTime("2026-11-05T01:16:34.415Z"));
        CHECK(m.lastActivity == AgentTranscript::parseTime("2026-11-05T12:26:09.608Z"));
        CHECK(m.running(m.written + 60) && !m.running(m.written + 121));

        CHECK(s.agentId == "a1ad4e3e0a7ec7fd2" && s.parent == m.transcript && s.hasMeta);
        CHECK(s.description == "LSP rename, references, quick fixes");
        CHECK(s.parentToolUseId == "toolu_01MuAehcWya4Y9bULcWFJECM" && s.agentType == "general-purpose");
        CHECK(s.worktreePath == wt && s.worktreeBranch == "worktree-agent-a1ad4e3e0a7ec7fd2");
        CHECK(s.finished && !s.running(s.written + 1));
        CHECK(s.bashCommands == 3 && s.filesTouched() == 3);
        CHECK(s.firstPrompt == "Placeholder task for the subagent.");
        int inWt = 0, outside = 0;
        for (const FileTouch &f : s.files) {
            if (f.place == Place::Worktree) inWt++;
            if (f.place == Place::Outside) outside++;
        }
        CHECK(inWt == 2 && outside == 1);
        const std::string pad = (tmpRoot / "-Users-user-Code-public-minicode" / sid / "scratchpad").string();
        CHECK(m.scratchpad == pad && s.scratchpad == pad);
        CHECK(index.scratchpads() == std::vector<std::string>{pad});
        CHECK(index.worktrees() == std::vector<std::string>{wt});
    }
    CHECK(index.find((projects / "-Users-user-Other" / "o1.jsonl").string()) == nullptr);
    CHECK(index.memoryEstimate() < 64 * 1024);

    // Nothing changed: nothing read.
    rs = RefreshStats();
    CHECK(!index.refresh(&rs));
    CHECK(rs.bytesRead == 0 && rs.read == 0);

    // A write to the scratchpad appears, reading only the added lines.
    const std::string report = (tmpRoot / "-Users-user-Code-public-minicode" / sid / "scratchpad" / "report.md").string();
    {
        std::string add = write(sid, wt, "2026-11-05T13:00:00Z", "toolu_new", report) + "\n" +
                          result(sid, wt, "2026-11-05T13:00:01Z", "toolu_new", "File created") + "\n" +
                          write(sid, wt, "2026-11-05T13:00:02Z", "toolu_refused", folder + "/x.txt") + "\n" +
                          result(sid, wt, "2026-11-05T13:00:03Z", "toolu_refused", "denied", true) + "\n" +
                          "{\"type\":\"user\",\"broken";
        put(subT, add, true);
        rs = RefreshStats();
        CHECK(index.refresh(&rs));
        CHECK(rs.read == 1 && rs.bytesRead < 4096 && rs.badLines == 0);   // the partial line waits
        const Session *s = index.find(subT.string());
        CHECK(s && s->files.size() == 5 && s->filesTouched() == 4);
        if (s && s->files.size() == 5) {
            CHECK(s->files[3].path == report && s->files[3].place == Place::Outside);
            CHECK(s->files[4].failed == 1 && !s->files[4].touched());
        }
        CHECK(s && s->lastActivity == AgentTranscript::parseTime("2026-11-05T13:00:03Z"));
        // The bad line, once complete, is counted and skipped.
        put(subT, "\n" + bash(sid, wt, "2026-11-05T13:01:00Z", "toolu_b", "ls", "List") + "\n", true);
        CHECK(index.refresh(&rs));
        s = index.find(subT.string());
        CHECK(s && s->badLines == 1 && s->bashCommands == 4);
    }
    // The meta file says finished: a refresh sees it.
    {
        std::string meta;
        fsys::path mp = subT;
        mp.replace_extension(".meta.json");
        std::ifstream in(mp);
        std::stringstream ss;
        ss << in.rdbuf();
        meta = ss.str();
        size_t at = meta.find("\"finished\":true");
        CHECK(at != std::string::npos);
        meta.replace(at, 15, "\"finished\":false");
        put(mp, meta);
        CHECK(index.refresh());
        const Session *s = index.find(subT.string());
        CHECK(s && !s->finished && s->running(s->written + 5));
    }
    // A worktree list joined with the index.
    {
        std::vector<Worktree> all = parseWorktreeList(
            "worktree " + folder + "\nHEAD 1\nbranch refs/heads/main\n\nworktree " + wt +
            "\nHEAD 2\nbranch refs/heads/worktree-agent-a1ad4e3e0a7ec7fd2\n");
        std::vector<Worktree> under = worktreesUnder(all, folder, &index, 0);
        CHECK(under.size() == 1 && under[0].description == "LSP rename, references, quick fixes" &&
              under[0].transcript == subT.string());
    }
    // The window on the worktree: only the subagent, at the top.
    {
        Index w(cfs.fs, {projects.string(), tmpRoot.string()});
        w.setFolder(wt);
        CHECK(w.refresh());
        std::vector<Index::Row> r = w.rows();
        CHECK(r.size() == 1 && r[0].depth == 0 && r[0].session->agentId == "a1ad4e3e0a7ec7fd2");
        // ...and on a folder nobody worked in: nothing.
        w.setFolder("/Users/user/Nowhere");
        w.refresh();
        CHECK(w.rows().empty());
    }
    // A transcript replaced by a shorter one is read again from the start.
    {
        put(mainT, prompt(sid, folder, "2026-11-06T08:00:00Z", "A new start") + "\n");
        CHECK(index.refresh());
        const Session *m = index.find(mainT.string());
        CHECK(m && m->description == "A new start" && m->bashCommands == 0 && m->files.empty());
    }
    // The projects folder deleted while running: empty, no error.
    fsys::remove_all(projects);
    CHECK(index.refresh());
    CHECK(index.rows().empty() && index.scratchpads().empty());
    CHECK(!index.refresh());

    GROUP("agentindex:resolve");
    {
        fsys::path w = root / "repo";
        fsys::path wt2 = w / ".claude" / "worktrees" / "agent-q";
        fsys::path pad = root / "pad";
        put(w / "src" / "Terminal.mm", "main copy\n");
        put(wt2 / "src" / "Terminal.mm", "worktree copy\n");
        put(pad / "report.md", "r\n");
        put(w / "home" / "notes.txt", "n\n");
        // The worktree's copy is the newer.
        fsys::last_write_time(w / "src" / "Terminal.mm",
                              fsys::last_write_time(wt2 / "src" / "Terminal.mm") - std::chrono::hours(1));
        Fs fs = AgentTranscript::posixFs();
        Roots roots;
        roots.cwd = wt2.string();
        roots.folder = w.string();
        roots.worktrees = {wt2.string()};
        roots.scratchpads = {pad.string()};
        roots.home = (w / "home").string();
        Resolution r = resolvePath("src/Terminal.mm:1454", roots, fs);
        CHECK(r.target == "src/Terminal.mm" && r.line == 1454 && r.column == 0);
        CHECK(r.matches.size() == 2);
        if (r.matches.size() == 2) {
            CHECK(r.matches[0].path == (wt2 / "src" / "Terminal.mm").string());
            CHECK(r.matches[1].path == (w / "src" / "Terminal.mm").string());
            CHECK(r.matches[0].mtime > r.matches[1].mtime && r.matches[0].root == wt2.string());
        }
        CHECK(r.tried == (std::vector<std::string>{wt2.string(), w.string(), pad.string()}));
        r = resolvePath("  report.md:3:7 ", roots, fs);
        CHECK(r.matches.size() == 1 && r.matches[0].path == (pad / "report.md").string() &&
              r.line == 3 && r.column == 7);
        r = resolvePath("~/notes.txt", roots, fs);
        CHECK(r.matches.size() == 1 && r.matches[0].path == (w / "home" / "notes.txt").string());
        r = resolvePath((w / "src" / "Terminal.mm").string() + ":12:3", roots, fs);
        CHECK(r.matches.size() == 1 && r.line == 12 && r.column == 3 && r.tried.size() == 1);
        r = resolvePath("File \"src/Terminal.mm\", line 9", roots, fs);
        CHECK(r.matches.size() == 2 && r.line == 9);
        r = resolvePath("src/Missing.mm:4", roots, fs);
        CHECK(r.matches.empty() && r.tried.size() == 3 && r.line == 4);
        r = resolvePath("src", roots, fs);
        CHECK(r.matches.size() == 2 && r.matches[0].isDir);
        CHECK(resolvePath("", roots, fs).matches.empty());
    }
    fsys::remove_all(root);

    GROUP("agentindex:perf");
    {
        // 50 sessions (two of them with 3 subagents each), about 400 lines
        // each, with 1-2 KB outputs, plus 50 sessions of another folder.
        fsys::path proot = scratch("perf");
        fsys::path pp = proot / "projects";
        const std::string pf = "/Users/user/Code/big";
        for (int s = 0; s < 100; s++) {
            const bool mine = s < 50;
            const std::string cwd = mine ? pf : "/Users/user/Code/other";
            const std::string id = "sess-" + std::to_string(s);
            std::string data = prompt(id, cwd, "2026-11-05T08:00:00Z", "Task number " + std::to_string(s)) + "\n";
            for (int k = 0; k < 130; k++) {
                const std::string t = "toolu_" + std::to_string(k);
                const std::string ts = "2026-11-05T09:" + std::to_string(10 + k % 50) + ":00Z";
                if (k % 3 == 0) {
                    data += write(id, cwd, ts, t, cwd + "/src/file" + std::to_string(k % 40) + ".cpp") + "\n";
                    data += result(id, cwd, ts, t, "File created successfully") + "\n";
                } else {
                    data += bash(id, cwd, ts, t, "make test 2>&1 | tail -40", "Run the tests") + "\n";
                    data += result(id, cwd, ts, t, std::string(1500, 'o') + "\n4466 passed, 0 failed") + "\n";
                }
                data += text(id, cwd, ts, "Working on step " + std::to_string(k), "m" + std::to_string(k)) + "\n";
            }
            put(pp / (mine ? "-Users-user-Code-big" : "-Users-user-Code-other") / (id + ".jsonl"), data);
            if (s < 2) {
                for (int a = 0; a < 3; a++) {
                    const std::string ag = "agent-p" + std::to_string(s) + std::to_string(a);
                    fsys::path sd = pp / "-Users-user-Code-big" / id / "subagents";
                    put(sd / (ag + ".jsonl"), prompt(id, pf, "2026-11-05T08:30:00Z", "Sub task") + "\n");
                    put(sd / (ag + ".meta.json"), "{\"description\":\"Sub " + std::to_string(a) + "\",\"finished\":true}");
                }
            }
        }
        CountingFs pfs;
        Index big(pfs.fs, {pp.string(), ""});
        big.setFolder(pf);
        RefreshStats prs;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(big.refresh(&prs));
        auto t1 = std::chrono::steady_clock::now();
        CHECK(big.rows().size() == 56);
        const size_t mem = big.memoryEstimate();
        CHECK(mem < 10u * 1024 * 1024);
        std::printf("  agentindex: %d transcripts, %.1f MB read in %.0f ms, index %.0f KB\n",
                    prs.transcripts, prs.bytesRead / 1e6,
                    std::chrono::duration<double, std::milli>(t1 - t0).count(), mem / 1024.0);
        // One appended line: read alone.
        put(pp / "-Users-user-Code-big" / "sess-7.jsonl",
            bash("sess-7", pf, "2026-11-05T11:00:00Z", "toolu_late", "ls", "List") + "\n", true);
        prs = RefreshStats();
        t0 = std::chrono::steady_clock::now();
        CHECK(big.refresh(&prs));
        t1 = std::chrono::steady_clock::now();
        CHECK(prs.read == 1 && prs.bytesRead < 1024);
        CHECK(big.rows()[0].session->transcript == (pp / "-Users-user-Code-big" / "sess-7.jsonl").string());
        std::printf("  agentindex: an appended line re-read in %.2f ms (%llu bytes)\n",
                    std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    (unsigned long long)prs.bytesRead);
        fsys::remove_all(proot);
    }
}
