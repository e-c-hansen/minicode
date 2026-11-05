// ControlProtocol.cpp — see ControlProtocol.h.
#include "ControlProtocol.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>

namespace ControlChannel {

namespace {

ArgSpec arg(const char* name, ArgType type, bool required = false,
            std::vector<ArgSpec> fields = {}) {
    return ArgSpec{name, type, required, std::move(fields)};
}

// Window-targeted commands take one of these (R1.5).
void addTarget(std::vector<ArgSpec>& args) {
    args.push_back(arg("window", ArgType::Int));
    args.push_back(arg("folder", ArgType::String));
}

std::vector<CommandSpec> makeCommands() {
    std::vector<CommandSpec> c;
    c.push_back({"ping", "Is minicode there: its process id and version", {}});
    {
        CommandSpec s{"open", "Open files, at a line and column, with a note", {
            arg("files", ArgType::Objects, true,
                {arg("path", ArgType::String, true), arg("line", ArgType::Int),
                 arg("column", ArgType::Int)}),
            arg("to_line", ArgType::Int), arg("to_column", ArgType::Int),
            arg("note", ArgType::String), arg("from", ArgType::String),
            arg("keep", ArgType::Bool), arg("split", ArgType::String)}};
        addTarget(s.args);
        c.push_back(s);
    }
    c.push_back({"window", "Select or create a window",
                 {arg("folder", ArgType::String), arg("new", ArgType::Bool)}});
    {
        CommandSpec s{"browse", "Open an address in the window's browser pane",
                      {arg("url", ArgType::String, true), arg("split", ArgType::String)}};
        addTarget(s.args);
        c.push_back(s);
    }
    {
        CommandSpec s{"term", "A new terminal, optionally running a command",
                      {arg("cwd", ArgType::String), arg("title", ArgType::String),
                       arg("run", ArgType::String), arg("split", ArgType::String)}};
        addTarget(s.args);
        c.push_back(s);
    }
    {
        CommandSpec s{"diff", "Side-by-side diff of a file against a git ref, or of two files",
                      {arg("path", ArgType::String, true), arg("base", ArgType::String),
                       arg("other", ArgType::String), arg("keep", ArgType::Bool)}};
        addTarget(s.args);
        c.push_back(s);
    }
    c.push_back({"state", "Every window's folder, tabs, selection, terminals and browser", {}});
    {
        CommandSpec s{"selection", "The selected text in the window's editor", {}};
        addTarget(s.args);
        c.push_back(s);
    }
    c.push_back({"term-output", "The last lines a terminal shows, scrollback included",
                 {arg("terminal", ArgType::Int, true), arg("lines", ArgType::Int)}});
    {
        CommandSpec s{"tour", "A sequence of notes, stepped through with Option+] and Option+[",
                      {arg("stops", ArgType::Objects, true,
                           {arg("path", ArgType::String, true), arg("line", ArgType::Int, true),
                            arg("column", ArgType::Int), arg("to", ArgType::Int),
                            arg("note", ArgType::String, true)}),
                       arg("from", ArgType::String)}};
        addTarget(s.args);
        c.push_back(s);
    }
    return c;
}

const char* typeName(ArgType t) {
    switch (t) {
    case ArgType::String: return "a string";
    case ArgType::Int: return "a whole number";
    case ArgType::Bool: return "true or false";
    case ArgType::Objects: return "a list of objects";
    }
    return "?";
}

bool isWholeNumber(const Json& v) {
    if (!v.isNumber()) return false;
    double d = v.asNumber();
    return std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9.0e15;
}

bool checkFields(const std::vector<ArgSpec>& specs, const Json& obj, const std::string& where,
                 std::string& error) {
    if (!obj.isObject()) {
        error = where + " must be an object";
        return false;
    }
    for (const auto& kv : obj.items()) {
        const ArgSpec* spec = nullptr;
        for (const ArgSpec& s : specs)
            if (s.name == kv.first) spec = &s;
        if (!spec) {
            error = "unknown field \"" + kv.first + "\" in " + where;
            return false;
        }
        const Json& v = kv.second;
        bool ok = false;
        switch (spec->type) {
        case ArgType::String: ok = v.isString(); break;
        case ArgType::Int: ok = isWholeNumber(v); break;
        case ArgType::Bool: ok = v.isBool(); break;
        case ArgType::Objects:
            ok = v.isArray();
            if (ok) {
                for (size_t i = 0; i < v.size(); i++)
                    if (!checkFields(spec->fields, v[i],
                                     "\"" + spec->name + "\" item " + std::to_string(i + 1), error))
                        return false;
            }
            break;
        }
        if (!ok) {
            error = "\"" + kv.first + "\" in " + where + " must be " + typeName(spec->type);
            return false;
        }
    }
    for (const ArgSpec& s : specs)
        if (s.required && !obj.has(s.name)) {
            error = where + " needs \"" + s.name + "\"";
            return false;
        }
    return true;
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool allDigits(const std::string& s) {
    if (s.empty() || s.size() > 9) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

}  // namespace

const std::vector<CommandSpec>& commands() {
    static const std::vector<CommandSpec> table = makeCommands();
    return table;
}

const CommandSpec* findCommand(const std::string& name) {
    for (const CommandSpec& c : commands())
        if (c.name == name) return &c;
    return nullptr;
}

bool validateArgs(const CommandSpec& spec, const Json& args, std::string& error) {
    return checkFields(spec.args, args, "the arguments of " + spec.name, error);
}

bool parseRequest(const std::string& line, Request& out, std::string& error) {
    out = Request();
    Json j;
    std::string perr;
    if (!Json::parse(line, j, &perr)) {
        error = "not JSON: " + perr;
        return false;
    }
    if (!j.isObject()) {
        error = "a request is a JSON object";
        return false;
    }
    if (j["id"].isNumber() || j["id"].isString()) out.id = j["id"];
    for (const auto& kv : j.items())
        if (kv.first != "v" && kv.first != "id" && kv.first != "cmd" && kv.first != "args") {
            error = "unknown field \"" + kv.first + "\" in the request";
            return false;
        }
    if (!j.has("id") || !(j["id"].isNumber() || j["id"].isString())) {
        error = "the request needs an \"id\", a number or a string";
        return false;
    }
    if (!isWholeNumber(j["v"]) || j["v"].asInt() != kVersion) {
        error = "the request needs \"v\": " + std::to_string(kVersion);
        return false;
    }
    if (!j["cmd"].isString()) {
        error = "the request needs a \"cmd\"";
        return false;
    }
    out.cmd = j["cmd"].asString();
    const CommandSpec* spec = findCommand(out.cmd);
    if (!spec) {
        error = "unknown command \"" + out.cmd + "\"";
        return false;
    }
    out.args = j.has("args") ? j["args"] : Json::object();
    return validateArgs(*spec, out.args, error);
}

std::string okLine(const Json& id, const Json& result) {
    Json j = Json::object();
    j.set("id", id);
    j.set("ok", true);
    j.set("result", result);
    return j.dump() + "\n";
}

std::string errorLine(const Json& id, const std::string& error) {
    Json j = Json::object();
    j.set("id", id);
    j.set("ok", false);
    j.set("error", error);
    return j.dump() + "\n";
}

std::string requestLine(const Json& id, const std::string& cmd, const Json& args) {
    Json j = Json::object();
    j.set("v", kVersion);
    j.set("id", id);
    j.set("cmd", cmd);
    j.set("args", args);
    return j.dump() + "\n";
}

// ------------------------------------------------------------- dispatcher
void Dispatcher::on(const std::string& cmd, Handler handler) {
    for (auto& h : handlers_)
        if (h.first == cmd) { h.second = std::move(handler); return; }
    handlers_.emplace_back(cmd, std::move(handler));
}

bool Dispatcher::handles(const std::string& cmd) const {
    for (const auto& h : handlers_)
        if (h.first == cmd) return true;
    return false;
}

void Dispatcher::handle(const std::string& line,
                        std::function<void(const std::string&)> send) const {
    Request req;
    std::string error;
    if (!parseRequest(line, req, error)) {
        send(errorLine(req.id, error));
        return;
    }
    const Handler* handler = nullptr;
    for (const auto& h : handlers_)
        if (h.first == req.cmd) handler = &h.second;
    if (!handler) {
        send(errorLine(req.id, "\"" + req.cmd + "\" is not available here"));
        return;
    }
    auto answered = std::make_shared<bool>(false);
    Json id = req.id;
    Respond respond = [answered, id, send](const Result& r) {
        if (*answered) return;
        *answered = true;
        send(r.ok ? okLine(id, r.value) : errorLine(id, r.error.empty() ? "failed" : r.error));
    };
    try {
        (*handler)(req, respond);
    } catch (const std::exception& e) {
        respond(Result::failure(std::string("internal error: ") + e.what()));
    } catch (...) {
        respond(Result::failure("internal error"));
    }
}

// ----------------------------------------------------------- command line
PathSpec splitPathSpec(const std::string& spec,
                       const std::function<bool(const std::string&)>& exists) {
    PathSpec p;
    p.path = spec;
    if (exists && exists(spec)) return p;
    // path:line:col, then path:line.
    size_t c2 = spec.rfind(':');
    if (c2 == std::string::npos || c2 == 0) return p;
    std::string last = spec.substr(c2 + 1);
    if (!allDigits(last)) return p;
    size_t c1 = spec.rfind(':', c2 - 1);
    if (c1 != std::string::npos && c1 > 0 && allDigits(spec.substr(c1 + 1, c2 - c1 - 1))) {
        std::string path = spec.substr(0, c1);
        if (!exists || exists(path) || !exists(spec.substr(0, c2))) {
            p.path = path;
            p.line = std::atoi(spec.substr(c1 + 1, c2 - c1 - 1).c_str());
            p.column = std::atoi(last.c_str());
            return p;
        }
    }
    p.path = spec.substr(0, c2);
    p.line = std::atoi(last.c_str());
    return p;
}

std::string absolutePath(const std::string& path, const std::string& cwd,
                         const std::string& home) {
    std::string p = path;
    if (p == "~") p = home;
    else if (startsWith(p, "~/")) p = home + p.substr(1);
    if (p.empty() || p[0] != '/') p = cwd + "/" + p;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string part = p.substr(i, j - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::string out;
    for (const std::string& part : parts) out += "/" + part;
    return out.empty() ? "/" : out;
}

bool isSubcommand(const std::string& word) {
    static const char* words[] = {"open", "window", "browse", "term", "diff", "split",
                                  "state", "selection", "term-output", "tour", "help"};
    for (const char* w : words)
        if (word == w) return true;
    return false;
}

namespace {

struct Help { const char* name; const char* text; };

const Help kHelp[] = {
    {"open",
     "minicode open <path>[:line[:col]] ... [--to line[:col]] [--note TEXT]\n"
     "              [--from NAME] [--keep]\n"
     "    Opens files in the window whose folder holds them, at a line and\n"
     "    column. With --note the file opens in the window's agent preview tab\n"
     "    (one tab that each such open replaces; --keep opens a normal tab),\n"
     "    the lines up to --to (or the one line) are highlighted and scrolled\n"
     "    to the upper third, and TEXT shows above them, labelled NAME\n"
     "    (default Claude). Notes are plain text, up to 500 characters, never\n"
     "    saved, and close on Escape, their x, or an edit inside the lines.\n"},
    {"window",
     "minicode window [--folder DIR] [--new]\n"
     "    Brings the window showing DIR forward, opening one if none does\n"
     "    (always a new one with --new), and prints its id. Without --folder,\n"
     "    the frontmost window.\n"},
    {"browse",
     "minicode browse <url>\n"
     "    Opens the address in the window's browser pane, beside the editor.\n"},
    {"term",
     "minicode term [--cwd DIR] [--title TITLE] [--run COMMAND]\n"
     "    Opens a new terminal tab and prints its id. --run types COMMAND at\n"
     "    the prompt and presses Return, in plain sight, as if typed there.\n"},
    {"diff",
     "minicode diff <path> [--base REF]\n"
     "minicode diff <a> <b>\n"
     "    Shows a side-by-side diff of a file against a git ref (HEAD by\n"
     "    default), or of two files, in the agent preview tab (--keep for a\n"
     "    tab of its own).\n"},
    {"split",
     "minicode split [--right|--below] <open|browse|term> ...\n"
     "    Runs the command into a new split: term --right splits the active\n"
     "    terminal, term --below opens a terminal tab in the dock, browse\n"
     "    --right shows the browser beside the editor. The editor itself has\n"
     "    one pane, so open in a split is refused.\n"},
    {"state",
     "minicode state [--pretty]\n"
     "    Prints JSON: each window's id, folder, tabs (path, dirty, active),\n"
     "    selection (path, lines, text up to 64 KB), terminals (id, cwd,\n"
     "    title, foreground command) and browser (url, title).\n"},
    {"selection",
     "minicode selection\n"
     "    Prints the text selected in the window's editor.\n"},
    {"term-output",
     "minicode term-output <id> [--lines N]\n"
     "    Prints the last N lines (200 by default) of a terminal's screen and\n"
     "    scrollback, as plain text.\n"},
    {"tour",
     "minicode tour <file.json>\n"
     "    A tour of notes: the file holds a list of {\"path\", \"line\", \"to\",\n"
     "    \"note\"} stops (relative paths are taken from the current folder).\n"
     "    The first stop opens in the agent preview tab; Option+] and\n"
     "    Option+[ go to the next and previous stop. A tour replaces the\n"
     "    window's earlier one.\n"},
};

const char* kCommon =
    "Options for the commands above:\n"
    "  --window ID   the window to act in (ids come from `minicode state`)\n"
    "  --folder DIR  the window showing DIR, opened if none does\n"
    "  --pid PID     the minicode process to talk to (default: the newest)\n"
    "Commands act in the frontmost window unless told otherwise, and never\n"
    "move the keyboard or close, replace or reorder tabs and terminals the\n"
    "user has; they only add. When minicode is not running, it is started,\n"
    "and the command waits up to 10 seconds for it.\n"
    "Exit codes: 0 done, 1 the command failed (why is on stderr), 2 a usage\n"
    "error, 3 no minicode to talk to.\n";

}  // namespace

std::string usageText(const std::string& topic) {
    if (!topic.empty()) {
        for (const Help& h : kHelp)
            if (topic == h.name) return std::string(h.text);
        return "";
    }
    std::string out = "Commands:\n";
    for (const Help& h : kHelp) out += h.text;
    out += "\n";
    out += kCommon;
    return out;
}

namespace {

// --name value, or --name=value. Advances i past what it took.
bool takeValue(const std::vector<std::string>& a, size_t& i, const std::string& name,
               std::string& value, std::string& error) {
    const std::string& w = a[i];
    std::string flag = "--" + name;
    if (w == flag) {
        if (i + 1 >= a.size()) {
            error = flag + " needs a value";
            return true;
        }
        value = a[++i];
        return true;
    }
    if (startsWith(w, flag + "=")) {
        value = w.substr(flag.size() + 1);
        return true;
    }
    return false;
}

bool parsePositiveInt(const std::string& s, int& out) {
    if (!allDigits(s)) return false;
    out = std::atoi(s.c_str());
    return true;
}

}  // namespace

CliCommand parseCommandLine(const std::vector<std::string>& argvIn, const std::string& cwd,
                            const std::string& home,
                            const std::function<bool(const std::string&)>& exists) {
    CliCommand c;
    c.launchFolder = cwd;
    if (argvIn.empty()) {
        c.error = "no command given";
        return c;
    }
    std::string sub = argvIn[0];
    std::vector<std::string> a(argvIn.begin() + 1, argvIn.end());
    if (sub == "help" || sub == "-h" || sub == "--help") {
        c.help = true;
        if (!a.empty()) c.helpTopic = a[0];
        return c;
    }
    std::string split;
    if (sub == "split") {
        while (!a.empty() && startsWith(a[0], "--") &&
               (a[0] == "--right" || a[0] == "--below")) {
            split = a[0].substr(2);
            a.erase(a.begin());
        }
        if (a.empty()) {
            c.error = "split needs a command: open, browse or term";
            return c;
        }
        sub = a[0];
        a.erase(a.begin());
        if (sub != "open" && sub != "browse" && sub != "term") {
            c.error = "split runs open, browse or term, not " + sub;
            return c;
        }
        if (split.empty()) split = "right";
    }
    if (!isSubcommand(sub) || sub == "split") {
        c.error = "unknown command " + sub;
        return c;
    }
    c.cmd = sub;
    const CommandSpec* spec = findCommand(sub);
    auto accepts = [&](const char* name) {
        for (const ArgSpec& s : spec->args)
            if (s.name == name) return true;
        return false;
    };

    std::vector<std::string> positional;
    std::string toSpec;
    for (size_t i = 0; i < a.size() && c.error.empty(); i++) {
        const std::string& w = a[i];
        std::string v;
        if (w == "-h" || w == "--help") {
            c.help = true;
            c.helpTopic = sub;
            return c;
        }
        if (w == "--") {
            positional.insert(positional.end(), a.begin() + (long)i + 1, a.end());
            break;
        }
        if (takeValue(a, i, "pid", v, c.error)) {
            if (c.error.empty() && (!parsePositiveInt(v, c.pid) || c.pid <= 0))
                c.error = "--pid needs a process id";
        } else if (accepts("window") && takeValue(a, i, "window", v, c.error)) {
            int id = 0;
            if (c.error.empty() && !parsePositiveInt(v, id)) c.error = "--window needs a window id";
            c.args.set("window", id);
        } else if ((accepts("folder") || sub == "window") && takeValue(a, i, "folder", v, c.error)) {
            std::string dir = absolutePath(v, cwd, home);
            c.args.set("folder", dir);
            c.launchFolder = dir;
        } else if (sub == "open" && takeValue(a, i, "to", v, c.error)) {
            toSpec = v;
        } else if (sub == "open" && takeValue(a, i, "note", v, c.error)) {
            c.args.set("note", cleanNoteText(v));
        } else if ((sub == "open" || sub == "tour") && takeValue(a, i, "from", v, c.error)) {
            c.args.set("from", v);
        } else if ((sub == "open" || sub == "diff") && w == "--keep") {
            c.args.set("keep", true);
        } else if (sub == "window" && w == "--new") {
            c.args.set("new", true);
        } else if (sub == "term" && takeValue(a, i, "cwd", v, c.error)) {
            c.args.set("cwd", absolutePath(v, cwd, home));
        } else if (sub == "term" && takeValue(a, i, "title", v, c.error)) {
            c.args.set("title", v);
        } else if (sub == "term" && takeValue(a, i, "run", v, c.error)) {
            c.args.set("run", v);
        } else if (sub == "diff" && takeValue(a, i, "base", v, c.error)) {
            c.args.set("base", v);
        } else if (sub == "term-output" && takeValue(a, i, "lines", v, c.error)) {
            int n = 0;
            if (c.error.empty() && (!parsePositiveInt(v, n) || n <= 0))
                c.error = "--lines needs a number";
            c.args.set("lines", n);
        } else if (sub == "state" && w == "--pretty") {
            c.pretty = true;
        } else if (startsWith(w, "-") && w.size() > 1) {
            c.error = "unknown option " + w + " for " + sub;
        } else {
            positional.push_back(w);
        }
    }
    if (!c.error.empty()) return c;
    if (!split.empty()) c.args.set("split", split);

    if (sub == "open") {
        if (positional.empty()) { c.error = "open needs a path"; return c; }
        Json files = Json::array();
        for (const std::string& p : positional) {
            PathSpec s = splitPathSpec(absolutePath(p, cwd, home), exists);
            Json f = Json::object();
            f.set("path", s.path);
            if (s.line > 0) f.set("line", s.line);
            if (s.column > 0) f.set("column", s.column);
            files.push(f);
        }
        const bool single = positional.size() == 1;
        if (!single && (c.args.has("note") || !toSpec.empty())) {
            c.error = "--note and --to go with one path";
            return c;
        }
        if (!toSpec.empty()) {
            int line = 0, col = 0;
            size_t colon = toSpec.find(':');
            bool ok = parsePositiveInt(toSpec.substr(0, colon), line) && line > 0;
            if (ok && colon != std::string::npos) ok = parsePositiveInt(toSpec.substr(colon + 1), col);
            if (!ok) { c.error = "--to needs a line, or line:column"; return c; }
            if (files[0]["line"].isNull()) {
                c.error = "--to needs a starting line: give the path as path:line";
                return c;
            }
            c.args.set("to_line", line);
            if (col > 0) c.args.set("to_column", col);
        }
        c.args.set("files", files);
    } else if (sub == "browse") {
        if (positional.size() != 1) { c.error = "browse needs one address"; return c; }
        c.args.set("url", positional[0]);
    } else if (sub == "diff") {
        if (positional.empty() || positional.size() > 2) {
            c.error = "diff needs a path, or two";
            return c;
        }
        c.args.set("path", absolutePath(positional[0], cwd, home));
        if (positional.size() == 2) {
            if (c.args.has("base")) { c.error = "--base goes with one path"; return c; }
            c.args.set("other", absolutePath(positional[1], cwd, home));
        }
    } else if (sub == "term-output") {
        int id = 0;
        if (positional.size() != 1 || !parsePositiveInt(positional[0], id) || id <= 0) {
            c.error = "term-output needs a terminal id";
            return c;
        }
        c.args.set("terminal", id);
        c.output = Output::Text;
        c.outputField = "text";
    } else if (sub == "tour") {
        if (positional.size() != 1) { c.error = "tour needs a file"; return c; }
        c.tourFile = absolutePath(positional[0], cwd, home);
    } else if (!positional.empty()) {
        c.error = sub + " takes no " + (positional.size() == 1 ? "argument " : "arguments ") +
                  positional[0];
        return c;
    }
    if (sub == "window") { c.output = Output::Id; c.outputField = "window"; }
    if (sub == "term") { c.output = Output::Id; c.outputField = "terminal"; }
    if (sub == "state") c.output = Output::Json;
    if (sub == "selection") { c.output = Output::Text; c.outputField = "text"; }
    return c;
}

bool tourStops(const std::string& text, const std::string& cwd, const std::string& home,
               Json& stops, std::string& error) {
    Json j;
    std::string perr;
    if (!Json::parse(text, j, &perr)) {
        error = "the tour file is not JSON: " + perr;
        return false;
    }
    const Json& list = j.isObject() ? j["stops"] : j;
    if (!list.isArray() || list.size() == 0) {
        error = "a tour is a list of stops, each {\"path\", \"line\", \"to\", \"note\"}";
        return false;
    }
    stops = Json::array();
    for (size_t i = 0; i < list.size(); i++) {
        const Json& s = list[i];
        std::string where = "stop " + std::to_string(i + 1);
        if (!s.isObject() || !s["path"].isString() || !isWholeNumber(s["line"]) ||
            s["line"].asInt() < 1) {
            error = where + " needs a \"path\" and a \"line\"";
            return false;
        }
        Json out = Json::object();
        out.set("path", absolutePath(s["path"].asString(), cwd, home));
        out.set("line", (int)s["line"].asInt());
        if (isWholeNumber(s["column"]) && s["column"].asInt() > 0)
            out.set("column", (int)s["column"].asInt());
        if (isWholeNumber(s["to"]) && s["to"].asInt() >= s["line"].asInt())
            out.set("to", (int)s["to"].asInt());
        out.set("note", cleanNoteText(s["note"].asString()));
        stops.push(out);
    }
    return true;
}

std::string cleanNoteText(const std::string& text) {
    // The characters, with control characters and runs of spaces as one space.
    std::vector<std::string> chars;
    bool space = false;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = (unsigned char)text[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + len > text.size()) len = 1;
        if (c <= 0x20 || c == 0x7F) {
            space = !chars.empty();
            i++;
            continue;
        }
        if (space) chars.push_back(" ");
        space = false;
        chars.push_back(text.substr(i, len));
        i += len;
    }
    bool cut = chars.size() > kMaxNoteChars;
    if (cut) {
        chars.resize(kMaxNoteChars - 1);
        while (!chars.empty() && chars.back() == " ") chars.pop_back();
    }
    std::string out;
    for (const std::string& ch : chars) out += ch;
    if (cut) out += "\xE2\x80\xA6";
    return out;
}

std::string clipUtf8(const std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    size_t n = maxBytes;
    while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    return text.substr(0, n);
}

// ---------------------------------------------------------- socket folder
int pidFromSocketName(const std::string& name) {
    const std::string ext = ".sock";
    if (name.size() <= ext.size() || name.compare(name.size() - ext.size(), ext.size(), ext) != 0)
        return 0;
    std::string digits = name.substr(0, name.size() - ext.size());
    int pid = 0;
    if (!parsePositiveInt(digits, pid)) return 0;
    return pid;
}

std::string socketName(int pid) { return std::to_string(pid) + ".sock"; }

int chooseSocket(const std::vector<SocketEntry>& found, int wantPid,
                 const std::function<bool(int)>& alive) {
    if (wantPid > 0) {
        for (const SocketEntry& e : found)
            if (e.pid == wantPid && alive(e.pid)) return e.pid;
        return 0;
    }
    int best = 0;
    int64_t bestStarted = 0;
    for (const SocketEntry& e : found) {
        if (e.pid <= 0 || !alive(e.pid)) continue;
        if (!best || e.started > bestStarted || (e.started == bestStarted && e.pid > best)) {
            best = e.pid;
            bestStarted = e.started;
        }
    }
    return best;
}

// --------------------------------------------------------------- pretty JSON
namespace {

void prettyTo(const Json& v, std::string& out, int depth) {
    std::string pad((size_t)depth * 2, ' ');
    std::string inner((size_t)(depth + 1) * 2, ' ');
    if (v.isArray() && v.size() > 0) {
        out += "[\n";
        for (size_t i = 0; i < v.size(); i++) {
            out += inner;
            prettyTo(v[i], out, depth + 1);
            out += i + 1 < v.size() ? ",\n" : "\n";
        }
        out += pad + "]";
    } else if (v.isObject() && v.size() > 0) {
        out += "{\n";
        const auto& items = v.items();
        for (size_t i = 0; i < items.size(); i++) {
            out += inner + Json(items[i].first).dump() + ": ";
            prettyTo(items[i].second, out, depth + 1);
            out += i + 1 < items.size() ? ",\n" : "\n";
        }
        out += pad + "}";
    } else {
        out += v.dump();
    }
}

}  // namespace

std::string prettyJson(const Json& value) {
    std::string out;
    prettyTo(value, out, 0);
    return out;
}

}  // namespace ControlChannel
