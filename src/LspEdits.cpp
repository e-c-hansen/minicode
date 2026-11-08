// LspEdits.cpp — see LspClient.h. Results that change the code:
// WorkspaceEdits (parsed, checked against the versions they were asked
// for, as offsets, applied), formatting results as an edit, prepareRename,
// and code actions (parsed, merged across servers, organize imports).
#include "LspClient.h"
#include "LspInternal.h"
#include <algorithm>

namespace Lsp {

size_t WorkspaceEdit::editCount() const {
    size_t n = 0;
    for (const FileEdit& f : files) n += f.edits.size();
    return n;
}

WorkspaceEdit parseWorkspaceEdit(const Json& j, const CanonicalUri& canonical) {
    WorkspaceEdit we;
    if (!j.isObject()) return we;
    std::map<std::string, size_t> at;
    auto add = [&](const std::string& named, const Json& edits) {
        if (named.empty()) return;
        std::string uri = canonical ? canonical(named) : named;
        auto it = at.find(uri);
        if (it == at.end()) {
            it = at.emplace(uri, we.files.size()).first;
            we.files.push_back(FileEdit{uri, {}});
        }
        for (const Json& e : edits.elements()) {
            if (!e.isObject() || !e["range"].isObject() || !e["newText"].isString()) continue;
            TextEdit t;
            t.range = parseRange(e["range"]);
            if (positionBefore(t.range.end, t.range.start)) std::swap(t.range.start, t.range.end);
            t.newText = e["newText"].asString();
            we.files[it->second].edits.push_back(t);
        }
    };
    // documentChanges wins when a server sends both, as the spec says.
    const Json& dc = j["documentChanges"];
    if (dc.isArray()) {
        for (const Json& c : dc.elements()) {
            if (c["kind"].isString()) {   // create, rename or delete a file
                we.resourceOperations = true;
                continue;
            }
            const std::string named = c["textDocument"]["uri"].asString();
            add(named, c["edits"]);
            const Json& v = c["textDocument"]["version"];
            if (!named.empty() && v.isNumber()) {
                FileEdit& f = we.files[at[canonical ? canonical(named) : named]];
                // Two entries for one file at different versions: the edit
                // cannot be consistent with any one text.
                if (f.version >= 0 && f.version != (int)v.asInt())
                    we.problem = "The server's edits to " + pathFromUri(f.uri) +
                                 " are for different versions of it";
                f.version = (int)v.asInt();
            }
        }
    } else if (j["changes"].isObject()) {
        for (const auto& kv : j["changes"].items()) add(kv.first, kv.second);
    }
    std::vector<FileEdit> kept;
    for (FileEdit& f : we.files) {
        std::stable_sort(f.edits.begin(), f.edits.end(), [](const TextEdit& a, const TextEdit& b) {
            return positionBefore(a.range.start, b.range.start);
        });
        std::vector<TextEdit> unique;
        for (const TextEdit& e : f.edits) {
            bool dup = false;
            for (auto k = unique.rbegin(); k != unique.rend() && k->range.start == e.range.start; ++k)
                if (k->range.end == e.range.end && k->newText == e.newText) { dup = true; break; }
            if (!dup) unique.push_back(e);
        }
        f.edits.swap(unique);
        for (size_t i = 1; i < f.edits.size() && we.problem.empty(); i++)
            if (positionBefore(f.edits[i].range.start, f.edits[i - 1].range.end))
                we.problem = "The server's edits to " + pathFromUri(f.uri) + " overlap";
        if (!f.edits.empty()) kept.push_back(std::move(f));
    }
    we.files.swap(kept);
    if (we.resourceOperations)
        we.problem = "The edit creates, renames or deletes files, which minicode does not do";
    return we;
}

WorkspaceEdit formattingEdit(const std::string& uri, const Json& result,
                             const CanonicalUri& canonical) {
    if (!result.isArray()) return WorkspaceEdit();
    Json changes = Json::object();
    changes.set(uri, result);
    return parseWorkspaceEdit(Json::object({{"changes", changes}}), canonical);
}

namespace {
int versionIn(const DocVersions& v, const std::string& uri) {
    auto it = v.find(uri);
    if (it != v.end()) return it->second;
    const std::string path = pathFromUri(uri);
    if (!path.empty())
        for (const auto& kv : v)
            if (pathFromUri(kv.first) == path) return kv.second;
    return -1;   // not open
}
}  // namespace

std::string staleEditFile(const WorkspaceEdit& we, const DocVersions& asked,
                          const DocVersions& now) {
    for (const FileEdit& f : we.files) {
        const int current = versionIn(now, f.uri);
        bool stale = f.version >= 0 && f.version != current;
        const int then = versionIn(asked, f.uri);
        if (then >= 0 && then != current) stale = true;   // changed, or closed since
        if (stale) return f.uri;
    }
    return "";
}

std::vector<OffsetEdit> offsetEdits(const std::u16string& text,
                                    const std::vector<TextEdit>& edits) {
    std::vector<OffsetEdit> out;
    out.reserve(edits.size());
    const PositionIndex index(text);
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        size_t a = index.offset(it->range.start);
        size_t b = index.offset(it->range.end);
        if (b < a) b = a;
        out.push_back(OffsetEdit{a, b - a, toUtf16(it->newText)});
    }
    return out;
}

std::u16string applyTextEdits(const std::u16string& text, const std::vector<TextEdit>& edits) {
    std::u16string out = text;
    for (const OffsetEdit& e : offsetEdits(text, edits)) out.replace(e.start, e.length, e.text);
    return out;
}

PrepareRename parsePrepareRename(const Json& result) {
    PrepareRename p;
    if (!result.isObject()) return p;
    if (result.has("defaultBehavior")) {
        p.ok = result["defaultBehavior"].asBool();
    } else if (result["range"].isObject()) {
        p.ok = p.hasRange = true;
        p.range = parseRange(result["range"]);
        p.placeholder = result["placeholder"].asString();
    } else if (result["start"].isObject() && result["end"].isObject()) {
        p.ok = p.hasRange = true;
        p.range = parseRange(result);
    }
    return p;
}

CodeAction parseCodeAction(const Json& j) {
    CodeAction a;
    if (!j.isObject()) return a;
    a.title = trim(j["title"].asString());
    if (j["command"].isString()) {   // a bare Command
        a.command = j;
        return a;
    }
    a.kind = j["kind"].asString();
    a.preferred = j["isPreferred"].asBool();
    if (j["edit"].isObject()) a.edit = j["edit"];
    if (j["command"].isObject()) a.command = j["command"];
    if (j["disabled"].isObject()) {
        a.disabledReason = j["disabled"]["reason"].asString();
        if (a.disabledReason.empty()) a.disabledReason = "unavailable";
    }
    a.raw = std::make_shared<const Json>(j);
    return a;
}

std::vector<CodeAction> parseCodeActions(const Json& result, bool canResolve) {
    std::vector<CodeAction> out;
    for (const Json& j : result.elements()) {
        CodeAction a = parseCodeAction(j);
        if (a.title.empty() || !a.disabledReason.empty()) continue;
        if (!a.hasEdit() && !a.hasCommand() && !(canResolve && a.raw)) continue;
        out.push_back(std::move(a));
    }
    std::stable_partition(out.begin(), out.end(), [](const CodeAction& a) { return a.preferred; });
    return out;
}

bool kindUnder(const std::string& actionKind, const std::string& kind) {
    if (kind.empty() || actionKind.size() < kind.size()) return false;
    if (actionKind.compare(0, kind.size(), kind) != 0) return false;
    return actionKind.size() == kind.size() || actionKind[kind.size()] == '.';
}

std::vector<CodeAction> mergeCodeActions(const std::vector<std::vector<CodeAction>>& perServer) {
    // 0 preferred, 1 quick fixes, 2 refactorings, 3 the rest, 4 source.
    auto rank = [](const CodeAction& a) {
        if (a.preferred) return 0;
        if (kindUnder(a.kind, "quickfix")) return 1;
        if (kindUnder(a.kind, "refactor")) return 2;
        if (kindUnder(a.kind, "source")) return 4;
        return 3;
    };
    std::vector<CodeAction> all;
    for (size_t i = 0; i < perServer.size(); i++)
        for (const CodeAction& a : perServer[i]) {
            bool dup = false;
            for (const CodeAction& b : all)
                if (b.title == a.title && b.kind == a.kind) { dup = true; break; }
            if (dup) continue;
            all.push_back(a);
            all.back().origin = (int)i;
        }
    std::stable_sort(all.begin(), all.end(), [&](const CodeAction& a, const CodeAction& b) {
        return rank(a) < rank(b);
    });
    return all;
}

int pickOrganizeImports(const std::vector<CodeAction>& actions) {
    static const std::string kKind = "source.organizeImports";
    int bare = -1;
    for (size_t i = 0; i < actions.size(); i++) {
        const std::string& k = actions[i].kind;
        if (!kindUnder(k, kKind)) continue;
        if (k.size() > kKind.size()) return (int)i;
        if (bare < 0) bare = (int)i;
    }
    return bare;
}

}  // namespace Lsp
