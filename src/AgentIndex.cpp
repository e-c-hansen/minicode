// AgentIndex.cpp — see AgentIndex.h.
#include "AgentIndex.h"

#include "Json.h"
#include "TermLinks.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

namespace AgentIndex {

using AgentTranscript::Block;
using AgentTranscript::Line;
using AgentTranscript::Stat;

// ================================================================== paths
std::string normalizePath(const std::string &path) {
    if (path.empty()) return path;
    const bool abs = path[0] == '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string part = path.substr(i, j - i);
        if (part.empty() || part == ".") {
        } else if (part == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!abs) parts.push_back(part);
        } else {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::string out = abs ? "/" : "";
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) out += '/';
        out += parts[k];
    }
    if (out.empty()) out = ".";
    return out;
}

bool isInside(const std::string &path, const std::string &folder) {
    if (folder.empty()) return false;
    if (folder == "/") return !path.empty() && path[0] == '/';
    if (path.size() < folder.size() || path.compare(0, folder.size(), folder) != 0) return false;
    return path.size() == folder.size() || path[folder.size()] == '/';
}

static std::string joinPath(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

static bool endsWith(const std::string &s, const char *suffix) {
    size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static std::string baseName(const std::string &p) {
    size_t e = p.size();
    while (e > 1 && p[e - 1] == '/') e--;
    size_t s = p.rfind('/', e - 1);
    return p.substr(s == std::string::npos ? 0 : s + 1, e - (s == std::string::npos ? 0 : s + 1));
}

// The worktree a path is in, when it is under a ".claude/worktrees/<name>".
static std::string worktreeOf(const std::string &path) {
    static const std::string kMark = "/.claude/worktrees/";
    size_t m = path.find(kMark);
    if (m == std::string::npos) return "";
    size_t s = m + kMark.size();
    size_t e = path.find('/', s);
    if (e == s) return "";
    return path.substr(0, e == std::string::npos ? path.size() : e);
}

Place classify(const std::string &path, const std::string &folder, const std::string &worktree) {
    if (!worktree.empty() && isInside(path, worktree)) return Place::Worktree;
    if (isInside(path, folder)) {
        std::string wt = worktreeOf(path);
        if (!wt.empty() && isInside(wt, folder) && wt.size() > folder.size()) return Place::Worktree;
        return Place::Main;
    }
    return Place::Outside;
}

const char *placeName(Place p) {
    switch (p) {
    case Place::Main: return "In the folder";
    case Place::Worktree: return "In its worktree";
    case Place::Outside: return "Outside the folder";
    }
    return "";
}

std::string describePrompt(const std::string &text, size_t max) {
    // Slash commands arrive as <command-name>/x</command-name>
    // <command-args>y</command-args>: keep the name and the arguments, drop
    // every other tag and what <command-message> holds (the same name again).
    std::string plain;
    plain.reserve(std::min<size_t>(text.size(), 4096));
    size_t i = 0;
    while (i < text.size() && plain.size() < 4 * max + 64) {
        if (text[i] == '<') {
            size_t close = text.find('>', i);
            bool tag = close != std::string::npos && close - i < 64 && close > i + 1;
            if (tag) {
                for (size_t k = i + 1; k < close; k++) {
                    char c = text[k];
                    if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '/')) {
                        tag = false;
                        break;
                    }
                }
            }
            if (tag) {
                std::string name = text.substr(i + 1, close - i - 1);
                if (name == "command-message") {
                    size_t end = text.find("</command-message>", close);
                    i = end == std::string::npos ? close + 1 : end + 18;
                } else {
                    i = close + 1;
                }
                plain += ' ';
                continue;
            }
        }
        plain += text[i++];
    }
    // Collapse whitespace, cut at `max` characters.
    std::string out;
    size_t chars = 0;
    bool space = false, cut = false;
    for (size_t k = 0; k < plain.size(); k++) {
        unsigned char c = (unsigned char)plain[k];
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            space = !out.empty();
            continue;
        }
        bool lead = (c & 0xC0) != 0x80;
        if (lead) {
            if (chars + (space ? 1 : 0) >= max) {
                cut = true;
                break;
            }
            if (space) {
                out += ' ';
                chars++;
                space = false;
            }
            chars++;
        }
        out += (char)c;
    }
    if (cut || plain.size() < text.size()) {
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (cut) out += "\xE2\x80\xA6";   // …
    }
    return out;
}

// ================================================================ sessions
int Session::filesTouched() const {
    int n = 0;
    for (const FileTouch &f : files)
        if (f.touched()) n++;
    return n;
}

bool Session::running(double now, double window) const {
    return !finished && written > 0 && now - written <= window;
}

struct Index::File {
    AgentTranscript::Tail tail;
    Stat seen;                  // the transcript as last read
    Stat metaSeen;
    enum class Decision { Unknown, In, Out } decision = Decision::Unknown;
    bool subagent = false;
    bool needScratch = true;
    std::string metaDescription;
    Session s;
    std::map<std::string, size_t> fileIndex;               // path -> files[]
    std::vector<std::pair<std::string, size_t>> pending;   // file tool calls awaiting results

    void resetContent() {
        std::string transcript = s.transcript, metaPath = s.metaPath;
        Session fresh;
        fresh.transcript = transcript;
        fresh.metaPath = metaPath;
        s = std::move(fresh);
        fileIndex.clear();
        pending.clear();
        decision = Decision::Unknown;
        needScratch = true;
        metaSeen = Stat();   // read the meta again into the fresh session
    }
};

Index::Index(Fs fs, Options options, const AgentTranscript::Format &format)
    : fs_(std::move(fs)), opt_(std::move(options)), format_(format) {}

Index::~Index() = default;

void Index::setFolder(const std::string &folder) {
    std::string f = normalizePath(folder);
    if (f == folder_) return;
    folder_ = f;
    folderReal_ = fs_.realpath ? normalizePath(fs_.realpath(f)) : f;
    files_.clear();
    realCache_.clear();
}

bool Index::inFolder(const std::string &cwd) {
    if (folder_.empty() || cwd.empty()) return false;
    std::string c = normalizePath(cwd);
    if (isInside(c, folder_) || isInside(c, folderReal_)) return true;
    if (!fs_.realpath) return false;
    auto it = realCache_.find(c);
    if (it == realCache_.end()) it = realCache_.emplace(c, normalizePath(fs_.realpath(c))).first;
    return isInside(it->second, folder_) || isInside(it->second, folderReal_);
}

static void describe(Session &s, const std::string &metaDescription) {
    if (!metaDescription.empty()) s.description = metaDescription;
    else if (!s.firstPrompt.empty()) s.description = s.firstPrompt;
    else s.description = s.title;
}

bool Index::readMeta(File &f) {
    if (!f.subagent) return false;
    Stat st = fs_.stat(f.s.metaPath);
    if (st.exists == f.metaSeen.exists && st.size == f.metaSeen.size &&
        st.mtime == f.metaSeen.mtime && st.inode == f.metaSeen.inode)
        return false;
    f.metaSeen = st;
    std::string text;
    Json j;
    bool ok = st.exists && fs_.read(f.s.metaPath, 0, 256 * 1024, &text) && Json::parse(text, j) &&
              j.isObject();
    f.s.hasMeta = ok;
    if (!ok) {
        f.metaDescription.clear();
        f.s.finished = false;
        describe(f.s, f.metaDescription);
        return true;
    }
    f.metaDescription = describePrompt(j["description"].asString(), 200);
    f.s.agentType = j["agentType"].asString();
    f.s.finished = j["finished"].asBool(false);
    f.s.parentToolUseId = j["toolUseId"].asString();
    if (j["worktreePath"].isString()) f.s.worktreePath = normalizePath(j["worktreePath"].asString());
    if (j["worktreeBranch"].isString()) f.s.worktreeBranch = j["worktreeBranch"].asString();
    describe(f.s, f.metaDescription);
    return true;
}

bool Index::readFile(File &f, const Stat &st, RefreshStats &rs) {
    Session &s = f.s;
    const bool sub = f.subagent;
    AgentTranscript::ReadStats stats;
    bool any = false;
    f.tail.read(
        fs_, format_,
        [&](const Line &l) {
            any = true;
            s.lines++;
            if (l.time > 0) {
                if (s.started == 0 || l.time < s.started) s.started = l.time;
                if (l.time > s.lastActivity) s.lastActivity = l.time;
            }
            if (s.sessionId.empty() && !l.sessionId.empty()) s.sessionId = l.sessionId;
            if (sub && s.agentId.empty() && !l.agentId.empty()) s.agentId = l.agentId;
            if (!l.gitBranch.empty()) s.gitBranch = l.gitBranch;
            if (!l.version.empty() &&
                std::find(s.versions.begin(), s.versions.end(), l.version) == s.versions.end() &&
                s.versions.size() < 16)
                s.versions.push_back(l.version);
            if (!l.aiTitle.empty()) s.title = describePrompt(l.aiTitle, 200);
            if (s.cwd.empty() && !l.cwd.empty()) {
                s.cwd = normalizePath(l.cwd);
                if (f.decision == File::Decision::Unknown) {
                    bool in = inFolder(s.cwd) || (!s.worktreePath.empty() && inFolder(s.worktreePath));
                    f.decision = in ? File::Decision::In : File::Decision::Out;
                    if (!in) return false;   // never read further
                }
            }
            // A main transcript of an older version also holds its
            // subagents' lines, marked as a side chain: not its own work.
            if (!sub && l.sidechain) return true;
            if (l.prompt) {
                s.prompts++;
                if (s.firstPrompt.empty() && !l.blocks.empty())
                    s.firstPrompt = describePrompt(l.blocks.front().text, 80);
            }
            if (!l.spawnedAgent.empty() &&
                std::find(s.spawned.begin(), s.spawned.end(), l.spawnedAgent) == s.spawned.end())
                s.spawned.push_back(l.spawnedAgent);
            for (const Block &b : l.blocks) {
                if (b.kind == Block::Kind::ToolUse) {
                    s.toolUses++;
                    const std::string &n = b.name;
                    if (n == "Bash") s.bashCommands++;
                    else if (n == "Agent" || n == "Task") {
                        if (!b.id.empty()) s.agentCalls.push_back(b.id);
                    } else if ((n == "Write" || n == "Edit" || n == "MultiEdit" ||
                                n == "NotebookEdit") &&
                               !b.filePath.empty()) {
                        std::string p = b.filePath;
                        if (p[0] != '/') p = joinPath(l.cwd.empty() ? s.cwd : l.cwd, p);
                        p = normalizePath(p);
                        auto it = f.fileIndex.find(p);
                        if (it == f.fileIndex.end()) {
                            it = f.fileIndex.emplace(p, s.files.size()).first;
                            FileTouch t;
                            t.path = p;
                            s.files.push_back(std::move(t));
                        }
                        FileTouch &t = s.files[it->second];
                        if (n == "Write") t.writes++;
                        else t.edits++;
                        if (l.time > t.last) t.last = l.time;
                        if (!b.id.empty()) f.pending.emplace_back(b.id + (n == "Write" ? "W" : "E"),
                                                                  it->second);
                    }
                } else if (b.kind == Block::Kind::ToolResult && !f.pending.empty()) {
                    for (size_t k = 0; k < f.pending.size(); k++) {
                        const std::string &key = f.pending[k].first;
                        if (key.size() != b.id.size() + 1 || key.compare(0, b.id.size(), b.id) != 0)
                            continue;
                        if (b.isError) {
                            FileTouch &t = s.files[f.pending[k].second];
                            if (key.back() == 'W') t.writes--;
                            else t.edits--;
                            t.failed++;
                        }
                        f.pending.erase(f.pending.begin() + (long)k);
                        break;
                    }
                }
            }
            return true;
        },
        [&]() { f.resetContent(); readMeta(f); }, &stats,
        // Small reads until the first cwd says whether the file is wanted:
        // most transcripts are other folders', and their first lines say so.
        f.decision == File::Decision::Unknown ? 64 * 1024 : 1024 * 1024);
    rs.bytesRead += stats.bytesRead;
    rs.badLines += stats.badLines;
    s.badLines += stats.badLines;
    s.written = st.mtime;
    if (s.lastActivity == 0) s.lastActivity = st.mtime;
    if (f.decision == File::Decision::Out) {
        f.resetContent();
        f.decision = File::Decision::Out;
        f.metaSeen = Stat();
        return false;
    }
    if (s.worktreePath.empty()) s.worktreePath = worktreeOf(s.cwd);
    if (s.worktreeBranch.empty() && !s.worktreePath.empty()) s.worktreeBranch = s.gitBranch;
    describe(s, f.metaDescription);
    classify(s);
    if (any) f.needScratch = s.scratchpad.empty();
    return any || stats.reset || stats.badLines > 0;
}

void Index::classify(Session &s) const {
    for (FileTouch &t : s.files) {
        Place p = AgentIndex::classify(t.path, folder_, s.worktreePath);
        if (p == Place::Outside && folderReal_ != folder_)
            p = AgentIndex::classify(t.path, folderReal_, s.worktreePath);
        t.place = p;
    }
}

bool Index::refresh(RefreshStats *stats) {
    RefreshStats rs;
    bool changed = false;
    std::set<std::string> present;
    auto visit = [&](const std::string &path, bool sub, const std::string &metaPath) {
        Stat st = fs_.stat(path);
        if (!st.exists || st.isDir) return;
        present.insert(path);
        rs.transcripts++;
        auto it = files_.find(path);
        if (it == files_.end()) {
            auto f = std::make_unique<File>();
            f->tail.path = path;
            f->subagent = sub;
            f->s.transcript = path;
            f->s.metaPath = metaPath;
            it = files_.emplace(path, std::move(f)).first;
        }
        File &f = *it->second;
        const bool replaced = f.seen.exists && (st.inode != f.seen.inode || st.size < f.seen.size);
        if (f.decision == File::Decision::Out && !replaced) {
            f.seen = st;
            return;
        }
        if (replaced && f.decision == File::Decision::Out) {
            f.resetContent();
            f.tail.offset = 0;
            f.tail.inode = 0;
        }
        bool metaChanged = readMeta(f);
        const bool same = f.seen.exists && st.size == f.seen.size && st.mtime == f.seen.mtime &&
                          st.inode == f.seen.inode;
        f.seen = st;
        if (same) {
            if (metaChanged && f.decision == File::Decision::In) changed = true;
            return;
        }
        rs.read++;
        const bool wasIn = f.decision == File::Decision::In;
        const double written = f.s.written;
        bool grew = readFile(f, st, rs);
        const bool isIn = f.decision == File::Decision::In;
        if (isIn && (grew || metaChanged || !wasIn || f.s.written != written)) changed = true;
        if (wasIn && !isIn) changed = true;
    };
    if (!folder_.empty() && !opt_.projectsDir.empty()) {
        for (const std::string &proj : fs_.list(opt_.projectsDir)) {
            const std::string dir = joinPath(opt_.projectsDir, proj);
            for (const std::string &name : fs_.list(dir)) {
                if (endsWith(name, ".jsonl")) {
                    visit(joinPath(dir, name), false, "");
                    continue;
                }
                if (name.find('.') != std::string::npos) continue;
                const std::string subDir = joinPath(joinPath(dir, name), "subagents");
                for (const std::string &m : fs_.list(subDir)) {
                    if (m.compare(0, 6, "agent-") != 0 || !endsWith(m, ".jsonl")) continue;
                    const std::string meta =
                        joinPath(subDir, m.substr(0, m.size() - 6) + ".meta.json");
                    visit(joinPath(subDir, m), true, meta);
                }
            }
        }
    }
    for (auto it = files_.begin(); it != files_.end();) {
        if (present.count(it->first)) { ++it; continue; }
        if (it->second->decision == File::Decision::In) changed = true;
        it = files_.erase(it);
    }
    link();
    findScratchpads(false);
    if (stats) *stats = rs;
    return changed;
}

void Index::link() {
    std::map<std::string, std::string> byCall, bySpawn, byMain;
    for (auto &kv : files_) {
        const File &f = *kv.second;
        if (f.decision != File::Decision::In) continue;
        for (const std::string &id : f.s.agentCalls) byCall[id] = kv.first;
        for (const std::string &id : f.s.spawned) bySpawn[id] = kv.first;
        if (!f.subagent && !f.s.sessionId.empty()) byMain[f.s.sessionId] = kv.first;
    }
    for (auto &kv : files_) {
        File &f = *kv.second;
        if (f.decision != File::Decision::In) continue;
        std::string parent;
        if (f.subagent) {
            auto a = byCall.find(f.s.parentToolUseId);
            auto b = bySpawn.find(f.s.agentId);
            auto c = byMain.find(f.s.sessionId);
            if (!f.s.parentToolUseId.empty() && a != byCall.end()) parent = a->second;
            else if (!f.s.agentId.empty() && b != bySpawn.end()) parent = b->second;
            else if (c != byMain.end()) parent = c->second;
        }
        if (parent == kv.first) parent.clear();
        f.s.parent = parent;
    }
}

void Index::findScratchpads(bool all) {
    if (opt_.tmpRoot.empty()) return;
    std::vector<std::string> roots;
    bool listed = false;
    std::map<std::string, std::string> bySession;
    for (auto &kv : files_) {
        File &f = *kv.second;
        if (f.decision != File::Decision::In || f.s.sessionId.empty()) continue;
        if (!f.s.scratchpad.empty() || (!f.needScratch && !all)) continue;
        f.needScratch = false;
        auto known = bySession.find(f.s.sessionId);
        if (known != bySession.end()) {
            f.s.scratchpad = known->second;
            continue;
        }
        if (!listed) {
            roots = fs_.list(opt_.tmpRoot);
            listed = true;
        }
        for (const std::string &r : roots) {
            std::string p = joinPath(joinPath(joinPath(opt_.tmpRoot, r), f.s.sessionId), "scratchpad");
            Stat st = fs_.stat(p);
            if (st.exists && st.isDir) {
                f.s.scratchpad = p;
                bySession[f.s.sessionId] = p;
                break;
            }
        }
    }
}

std::vector<const Session *> Index::sessions() const {
    std::vector<const Session *> out;
    for (auto &kv : files_)
        if (kv.second->decision == File::Decision::In) out.push_back(&kv.second->s);
    return out;
}

const Session *Index::find(const std::string &transcript) const {
    auto it = files_.find(transcript);
    if (it == files_.end() || it->second->decision != File::Decision::In) return nullptr;
    return &it->second->s;
}

std::vector<Index::Row> Index::rows() const {
    std::vector<const Session *> all = sessions();
    auto newer = [](const Session *a, const Session *b) {
        if (a->lastActivity != b->lastActivity) return a->lastActivity > b->lastActivity;
        return a->transcript < b->transcript;
    };
    std::sort(all.begin(), all.end(), newer);
    std::map<std::string, std::vector<const Session *>> children;
    std::vector<const Session *> top;
    for (const Session *s : all) {
        if (!s->parent.empty() && find(s->parent)) children[s->parent].push_back(s);
        else top.push_back(s);
    }
    std::vector<Row> out;
    std::set<const Session *> done;
    std::function<void(const Session *, int)> add = [&](const Session *s, int depth) {
        if (!done.insert(s).second) return;
        out.push_back({s, depth});
        auto it = children.find(s->transcript);
        if (it == children.end()) return;
        for (const Session *c : it->second) add(c, depth + 1);
    };
    for (const Session *s : top) add(s, 0);
    for (const Session *s : all) add(s, 0);   // a cycle of parents, should one occur
    return out;
}

std::vector<std::string> Index::scratchpads() const {
    std::vector<std::string> out;
    for (const Row &r : rows())
        if (!r.session->scratchpad.empty() &&
            std::find(out.begin(), out.end(), r.session->scratchpad) == out.end())
            out.push_back(r.session->scratchpad);
    return out;
}

std::vector<std::string> Index::worktrees() const {
    std::vector<std::string> out;
    for (const Row &r : rows()) {
        const std::string &w = r.session->worktreePath;
        if (!w.empty() && std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
    }
    return out;
}

static size_t heap(const std::string &s) { return s.capacity() > 15 ? s.capacity() + 1 : 0; }
static size_t heap(const std::vector<std::string> &v) {
    size_t n = v.capacity() * sizeof(std::string);
    for (const std::string &s : v) n += heap(s);
    return n;
}

size_t Index::memoryEstimate() const {
    size_t n = sizeof(*this) + heap(folder_) + heap(folderReal_);
    for (auto &kv : realCache_) n += 64 + heap(kv.first) + heap(kv.second);
    for (auto &kv : files_) {
        const File &f = *kv.second;
        const Session &s = f.s;
        n += 64 + heap(kv.first) + sizeof(File) + heap(f.tail.path) + heap(f.metaDescription);
        for (const std::string *p : {&s.transcript, &s.metaPath, &s.sessionId, &s.agentId,
                                     &s.parent, &s.parentToolUseId, &s.description,
                                     &s.firstPrompt, &s.title, &s.agentType, &s.cwd,
                                     &s.gitBranch, &s.worktreePath, &s.worktreeBranch,
                                     &s.scratchpad})
            n += heap(*p);
        n += heap(s.versions) + heap(s.agentCalls) + heap(s.spawned);
        n += s.files.capacity() * sizeof(FileTouch);
        for (const FileTouch &t : s.files) n += heap(t.path);
        for (auto &fi : f.fileIndex) n += 64 + heap(fi.first);
        n += f.pending.capacity() * sizeof(f.pending[0]);
        for (auto &p : f.pending) n += heap(p.first);
    }
    return n;
}

// =============================================================== worktrees
std::vector<Worktree> parseWorktreeList(const std::string &outText) {
    std::string out = outText;
    std::replace(out.begin(), out.end(), '\0', '\n');
    std::vector<Worktree> list;
    size_t i = 0;
    while (i < out.size()) {
        size_t e = out.find('\n', i);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto value = [&](const char *key) -> const char * {
            size_t n = std::char_traits<char>::length(key);
            if (line.compare(0, n, key) != 0) return nullptr;
            if (line.size() == n) return "";
            if (line[n] != ' ') return nullptr;
            return line.c_str() + n + 1;
        };
        if (const char *v = value("worktree")) {
            Worktree w;
            w.path = normalizePath(v);
            w.main = list.empty();
            list.push_back(std::move(w));
            continue;
        }
        if (list.empty()) continue;
        Worktree &w = list.back();
        if (const char *v = value("HEAD")) w.head = v;
        else if (const char *v = value("branch")) {
            std::string b = v;
            if (b.compare(0, 11, "refs/heads/") == 0) b.erase(0, 11);
            w.branch = b;
        } else if (value("bare")) w.bare = true;
        else if (value("detached")) w.detached = true;
        else if (value("locked")) w.locked = true;
        else if (value("prunable")) w.prunable = true;
    }
    return list;
}

std::vector<Worktree> worktreesUnder(const std::vector<Worktree> &all, const std::string &folder,
                                     const Index *index, double now) {
    std::vector<Worktree> out;
    const std::string f = normalizePath(folder);
    std::map<std::string, const Session *> byPath, byAgent;
    if (index) {
        for (const Session *s : index->sessions()) {
            if (s->agentId.empty()) continue;
            if (!s->worktreePath.empty() && s->hasMeta) byPath[s->worktreePath] = s;
            byAgent[s->agentId] = s;
        }
    }
    for (const Worktree &w0 : all) {
        if (w0.main || w0.path == f || !isInside(w0.path, f)) continue;
        Worktree w = w0;
        std::string name = baseName(w.path);
        if (name.compare(0, 6, "agent-") == 0) w.agentId = name.substr(6);
        const Session *s = nullptr;
        auto a = byPath.find(w.path);
        if (a != byPath.end()) s = a->second;
        else if (!w.agentId.empty()) {
            auto b = byAgent.find(w.agentId);
            if (b != byAgent.end()) s = b->second;
        }
        if (s) {
            w.agentId = s->agentId;
            w.description = s->description;
            w.transcript = s->transcript;
            w.running = s->running(now);
        }
        out.push_back(std::move(w));
    }
    return out;
}

// ======================================================== path resolution
Resolution resolveTarget(const std::string &target, int line, int column, const Roots &roots,
                         const Fs &fs) {
    Resolution r;
    r.target = target;
    r.line = line;
    r.column = column;
    if (target.empty()) return r;
    auto add = [&](const std::string &path, const std::string &root) {
        std::string p = normalizePath(path);
        for (const Match &m : r.matches)
            if (m.path == p) return;
        Stat st = fs.stat(p);
        if (!st.exists) return;
        if (fs.realpath) {
            std::string real = fs.realpath(p);
            for (const Match &m : r.matches)
                if (fs.realpath(m.path) == real) return;
        }
        r.matches.push_back({p, root, st.mtime, st.isDir});
    };
    if (target[0] == '/') {
        r.tried.push_back(normalizePath(target));
        add(target, "");
    } else if (target[0] == '~' && (target.size() == 1 || target[1] == '/')) {
        if (!roots.home.empty()) {
            std::string p = roots.home + target.substr(1);
            r.tried.push_back(normalizePath(p));
            add(p, "");
        }
    } else {
        std::vector<std::string> order;
        auto push = [&](const std::string &root) {
            if (root.empty()) return;
            std::string n = normalizePath(root);
            if (std::find(order.begin(), order.end(), n) == order.end()) order.push_back(n);
        };
        push(roots.cwd);
        push(roots.folder);
        for (const std::string &w : roots.worktrees) push(w);
        for (const std::string &s : roots.scratchpads) push(s);
        for (const std::string &root : order) {
            r.tried.push_back(root);
            add(joinPath(root, target), root);
        }
    }
    std::stable_sort(r.matches.begin(), r.matches.end(),
                     [](const Match &a, const Match &b) { return a.mtime > b.mtime; });
    return r;
}

Resolution resolvePath(const std::string &text, const Roots &roots, const Fs &fs) {
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\n')) b--;
    const std::string t = text.substr(a, b - a);
    // The longest file reference TermLinks sees in it.
    const TermLinks::Link *best = nullptr;
    std::vector<TermLinks::Link> links = TermLinks::find(t);
    for (const TermLinks::Link &l : links)
        if (l.kind == TermLinks::Link::File && (!best || l.length > best->length)) best = &l;
    if (best && best->length * 2 >= t.size())
        return resolveTarget(best->target, best->line, best->column, roots, fs);
    // Otherwise the text itself, with a :line[:col] taken off the end.
    std::string target = t;
    int line = 0, column = 0;
    auto trailingNumber = [&](std::string &s, int *n) {
        size_t c = s.rfind(':');
        if (c == std::string::npos || c + 1 >= s.size() || c == 0) return false;
        for (size_t k = c + 1; k < s.size(); k++)
            if (s[k] < '0' || s[k] > '9') return false;
        if (s.size() - c > 9) return false;
        *n = std::stoi(s.substr(c + 1));
        s.resize(c);
        return true;
    };
    int n1 = 0, n2 = 0;
    if (trailingNumber(target, &n1)) {
        if (trailingNumber(target, &n2)) {
            line = n2;
            column = n1;
        } else {
            line = n1;
        }
    }
    return resolveTarget(target, line, column, roots, fs);
}

}  // namespace AgentIndex
