// AgentEdits.cpp — see AgentEdits.h.
#include "AgentEdits.h"
#include "Json.h"

#include <map>

namespace AgentEdits {

using AgentTranscript::Block;
using AgentTranscript::Line;

bool isEditTool(const std::string &n) {
    return n == "Edit" || n == "MultiEdit" || n == "Write" || n == "NotebookEdit";
}

const char *toolName(Tool t) {
    switch (t) {
    case Tool::Edit: return "Edit";
    case Tool::MultiEdit: return "MultiEdit";
    case Tool::Write: return "Write";
    case Tool::NotebookEdit: return "NotebookEdit";
    }
    return "Edit";
}

static bool toolNamed(const std::string &n, Tool *t) {
    if (n == "Edit") *t = Tool::Edit;
    else if (n == "MultiEdit") *t = Tool::MultiEdit;
    else if (n == "Write") *t = Tool::Write;
    else if (n == "NotebookEdit") *t = Tool::NotebookEdit;
    else return false;
    return true;
}

std::vector<Change> changesFromInput(Tool tool, const std::string &inputJson) {
    std::vector<Change> out;
    Json in;
    if (!Json::parse(inputJson, in) || !in.isObject()) return out;
    auto edit = [&](const Json &e, int part, int parts) {
        if (!e["old_string"].isString() || !e["new_string"].isString()) return;
        Change c;
        c.tool = tool;
        c.oldText = e["old_string"].asString();
        c.newText = e["new_string"].asString();
        c.replaceAll = e["replace_all"].asBool(false);
        c.part = part;
        c.parts = parts;
        out.push_back(std::move(c));
    };
    switch (tool) {
    case Tool::Edit:
        edit(in, 0, 1);
        break;
    case Tool::MultiEdit: {
        const Json &list = in["edits"];
        if (!list.isArray()) break;
        const int n = (int)list.size();
        for (int i = 0; i < n; i++) edit(list[i], i, n);
        break;
    }
    case Tool::Write:
        if (in["content"].isString()) {
            Change c;
            c.tool = tool;
            c.newText = in["content"].asString();
            out.push_back(std::move(c));
        }
        break;
    case Tool::NotebookEdit: {
        Change c;
        c.tool = tool;
        c.newText = in["new_source"].asString();
        out.push_back(std::move(c));
        break;
    }
    }
    return out;
}

// /private/tmp/x and /tmp/x, /private/var/x and /var/x: one spelling.
static std::string withoutPrivate(const std::string &p) {
    static const std::string priv = "/private/";
    if (p.compare(0, priv.size(), priv) == 0) {
        const std::string rest = p.substr(priv.size() - 1);   // keeps its leading "/"
        if (rest.compare(0, 5, "/tmp/") == 0 || rest == "/tmp" ||
            rest.compare(0, 5, "/var/") == 0 || rest == "/var" ||
            rest.compare(0, 5, "/etc/") == 0)
            return rest;
    }
    return p;
}

bool samePath(const Fs &fs, const std::string &a, const std::string &b) {
    if (a == b) return true;
    if (withoutPrivate(a) == withoutPrivate(b)) return true;
    if (!fs.realpath) return false;
    std::string ra = fs.realpath(a), rb = fs.realpath(b);
    if (ra.empty()) ra = a;
    if (rb.empty()) rb = b;
    return withoutPrivate(ra) == withoutPrivate(rb);
}

FileHistory collect(const Fs &fs, const std::string &transcript, const std::string &file,
                    size_t maxInput) {
    FileHistory h;
    h.transcript = transcript;
    h.file = file;
    struct Call {
        Tool tool;
        std::string id;
        double time;
        AgentTranscript::Span input;
        size_t prompt;   // into prompts, or npos
    };
    std::vector<std::string> prompts;   // the prompts calls answered, each once
    std::vector<Call> calls;
    std::map<std::string, size_t> byId;   // tool_use id -> calls[]
    struct Answer {
        bool error;
        AgentTranscript::Span output;
    };
    std::map<std::string, Answer> answers;
    std::map<std::string, bool> pathMatches;   // a path seen before, and whether it is the file
    AgentTranscript::Tail tail;
    tail.path = transcript;
    std::string lastPrompt;
    size_t lastPromptAt = std::string::npos;   // lastPrompt's place in prompts, once kept
    auto onLine = [&](const Line &l) {
        if (l.prompt && !l.meta) {
            lastPrompt.clear();
            lastPromptAt = std::string::npos;
            for (const Block &b : l.blocks)
                if (b.kind == Block::Kind::Text) {
                    if (!lastPrompt.empty()) lastPrompt += "\n";
                    lastPrompt += b.text;
                }
        }
        for (const Block &b : l.blocks) {
            if (b.kind == Block::Kind::ToolUse) {
                Tool t;
                if (!toolNamed(b.name, &t) || b.filePath.empty()) continue;
                auto seen = pathMatches.find(b.filePath);
                if (seen == pathMatches.end())
                    seen = pathMatches.emplace(b.filePath, samePath(fs, b.filePath, file)).first;
                if (!seen->second) continue;
                if (!b.id.empty()) byId[b.id] = calls.size();
                if (lastPromptAt == std::string::npos && !lastPrompt.empty()) {
                    prompts.push_back(lastPrompt);
                    lastPromptAt = prompts.size() - 1;
                }
                calls.push_back(Call{t, b.id, l.time, b.input, lastPromptAt});
            } else if (b.kind == Block::Kind::ToolResult && !b.id.empty()) {
                if (byId.count(b.id)) answers[b.id] = Answer{b.isError, b.output};
            }
        }
        return true;
    };
    auto onReset = [&]() {
        calls.clear();
        byId.clear();
        answers.clear();
        prompts.clear();
        lastPrompt.clear();
        lastPromptAt = std::string::npos;
    };
    AgentTranscript::ReadStats st;
    tail.read(fs, AgentTranscript::claudeCode(), onLine, onReset, &st);
    for (const Call &call : calls) {
        if (call.input.empty() || call.input.length > maxInput) {
            h.unreadable++;
            continue;
        }
        std::string json;
        if (!fs.read(transcript, call.input.offset, (size_t)call.input.length, &json) ||
            json.size() != call.input.length) {
            h.unreadable++;
            continue;
        }
        std::vector<Change> changes = changesFromInput(call.tool, json);
        if (changes.empty()) {
            h.unreadable++;
            continue;
        }
        bool answered = false, refused = false, created = false;
        auto a = answers.find(call.id);
        if (a != answers.end()) {
            answered = true;
            refused = a->second.error;
            if (call.tool == Tool::Write && !a->second.output.empty()) {
                // "File created successfully at: ..." against "The file ...
                // has been updated successfully."
                const std::string said = AgentTranscript::readOutputText(fs, transcript,
                                                                         a->second.output, 512);
                created = said.find("created") != std::string::npos;
            }
        }
        for (Change &c : changes) {
            c.toolUseId = call.id;
            c.time = call.time;
            if (call.prompt != std::string::npos) c.prompt = prompts[call.prompt];
            c.answered = answered;
            c.refused = refused;
            c.created = created;
            h.changes.push_back(std::move(c));
        }
    }
    return h;
}

size_t occurrences(const std::string &hay, const std::string &needle, size_t limit) {
    if (needle.empty()) return 0;
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos && n < limit;
         at = hay.find(needle, at + needle.size()))
        n++;
    return n;
}

static std::string ordinal(size_t i, const Change &c) {
    std::string s = "edit " + std::to_string(i + 1);
    if (c.tool == Tool::Write) s = "the rewrite in step " + std::to_string(i + 1);
    return s;
}

Result analyze(std::vector<Change> changes, const std::string &current, bool exists) {
    Result r;
    std::string working = current;
    bool ok = exists, existed = true;
    if (!exists) r.why = "The file is not there now.";
    auto stop = [&](const std::string &why) {
        if (ok) r.why = why;
        ok = false;
    };
    for (size_t k = changes.size(); k-- > 0;) {
        Change &c = changes[k];
        if (c.refused) {
            c.standing = Standing::Unknown;
            continue;
        }
        // Once undoing has stopped, a change is held against the file as
        // it is now.
        const std::string &text = ok ? working : current;
        switch (c.tool) {
        case Tool::Edit:
        case Tool::MultiEdit: {
            if (c.newText.empty()) {
                c.standing = Standing::Unknown;
                stop("Step " + std::to_string(k + 1) +
                     " only removed text, which cannot be found again to put back.");
                break;
            }
            const size_t n = occurrences(text, c.newText, 2);
            if (n == 0) {
                c.standing = Standing::Gone;
                stop("Something changed the text " + ordinal(k, c) +
                     " put in since, so the file before the session cannot be worked out.");
                break;
            }
            c.standing = Standing::Stands;
            if (!ok) break;
            if (n > 1) {
                stop("The text " + ordinal(k, c) + " put in appears more than once, so " +
                     (c.replaceAll ? "its places" : "its place") + " cannot be told for certain.");
                break;
            }
            const size_t at = working.find(c.newText);
            working.replace(at, c.newText.size(), c.oldText);
            break;
        }
        case Tool::Write:
            if (ok) {
                if (working != c.newText) {
                    c.standing = Standing::Gone;
                    stop("Something changed the file since " + ordinal(k, c) +
                         ", so the file before the session cannot be worked out.");
                    break;
                }
                c.standing = Standing::Stands;
                if (c.created) {
                    working.clear();
                    existed = false;
                } else {
                    stop("Step " + std::to_string(k + 1) +
                         " rewrote the whole file, and the transcript does not hold what was "
                         "there before.");
                }
            } else {
                c.standing = exists && current == c.newText ? Standing::Stands : Standing::Unknown;
            }
            break;
        case Tool::NotebookEdit:
            c.standing = Standing::Unknown;
            stop("Notebook cell edits are listed but not undone.");
            break;
        }
    }
    for (const Change &c : changes) {
        if (c.standing == Standing::Stands) r.standing++;
        if (c.standing == Standing::Gone) r.gone++;
    }
    r.changes = std::move(changes);
    if (ok) {
        r.rebuilt = true;
        r.existedBefore = existed;
        r.before = std::move(working);
    }
    return r;
}

}  // namespace AgentEdits
