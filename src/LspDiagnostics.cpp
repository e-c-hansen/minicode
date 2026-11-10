// LspDiagnostics.cpp — see LspClient.h. publishDiagnostics parsed,
// diagnostics placed on the text as it is now (diagnosticSpans), the
// DiagnosticStore that keeps each server's list per file, the Problems
// panel's rows, counts and stepping, and the diagnostics a code action
// request sends (diagnosticsInRange).
#include "LspClient.h"
#include <algorithm>
#include <climits>
#include <set>

namespace Lsp {

std::vector<Diagnostic> parseDiagnostics(const Json& params, std::string* uri, int* version) {
    if (uri) *uri = params["uri"].asString();
    if (version) *version = params["version"].isNumber() ? (int)params["version"].asInt() : -1;
    std::vector<Diagnostic> out;
    for (const Json& d : params["diagnostics"].elements()) {
        Diagnostic diag;
        diag.range = parseRange(d["range"]);
        int sev = (int)d["severity"].asInt(1);
        if (sev < 1 || sev > 4) sev = 1;   // "if omitted, the client decides"
        diag.severity = (Severity)sev;
        diag.message = d["message"].asString();
        diag.source = d["source"].asString();
        const Json& code = d["code"];
        if (code.isString()) diag.code = code.asString();
        else if (code.isNumber()) diag.code = std::to_string(code.asInt());
        for (const Json& tag : d["tags"].elements()) {
            if (tag.asInt() == 1) diag.unnecessary = true;
            if (tag.asInt() == 2) diag.deprecated = true;
        }
        diag.raw = std::make_shared<const Json>(d);
        out.push_back(diag);
    }
    return out;
}

std::vector<DiagnosticSpan> diagnosticSpans(const std::u16string* base,
                                            const std::u16string& current,
                                            const std::vector<Diagnostic>& diags) {
    std::vector<DiagnosticSpan> out;
    const std::u16string& from = base ? *base : current;
    // The part that changed since: what lies between the common head and
    // the common tail. Typing between two syncs is one place (several
    // carets make it one wider place), so this is all the diff needed.
    size_t head = 0, tail = 0;
    if (base && *base != current) {
        const size_t n = std::min(from.size(), current.size());
        while (head < n && from[head] == current[head]) head++;
        while (tail < n - head &&
               from[from.size() - 1 - tail] == current[current.size() - 1 - tail])
            tail++;
    }
    const bool changed = base && *base != current;
    const size_t oldEnd = from.size() - tail;           // [head, oldEnd) changed
    const long delta = (long)current.size() - (long)from.size();
    const PositionIndex index(from);
    for (size_t i = 0; i < diags.size(); i++) {
        size_t a = index.offset(diags[i].range.start);
        size_t b = index.offset(diags[i].range.end);
        if (b < a) std::swap(a, b);
        if (changed) {
            // Touching the changed part, at either end, counts: an error at
            // the end of a word being typed is about the word as it was.
            if (a <= oldEnd && b >= head) continue;
            if (a > oldEnd) {
                a = (size_t)((long)a + delta);
                b = (size_t)((long)b + delta);
            }
        }
        out.push_back(DiagnosticSpan{a, b, i});
    }
    return out;
}

// ------------------------------------------------------- DiagnosticStore
bool DiagnosticStore::publish(const std::string& uri, const std::string& server,
                              std::vector<Diagnostic> list,
                              std::shared_ptr<const std::u16string> base,
                              const std::string& sourceName) {
    if (list.empty()) return erase(uri, server);
    if (!sourceName.empty())
        for (Diagnostic& d : list)
            if (d.source.empty()) d.source = sourceName;
    std::vector<Set>& sets = files_[uri];
    generations_[uri] = ++counter_;
    for (Set& set : sets)
        if (set.server == server) {
            set.list = std::move(list);
            set.base = std::move(base);
            return true;
        }
    sets.push_back(Set{server, std::move(list), std::move(base)});
    return true;
}

uint64_t DiagnosticStore::generation(const std::string& uri) const {
    auto it = generations_.find(uri);
    return it == generations_.end() ? 0 : it->second;
}

bool DiagnosticStore::erase(const std::string& uri, const std::string& server) {
    auto it = files_.find(uri);
    if (it == files_.end()) return false;
    std::vector<Set>& sets = it->second;
    auto s = std::find_if(sets.begin(), sets.end(),
                          [&](const Set& x) { return x.server == server; });
    if (s == sets.end()) return false;
    sets.erase(s);
    generations_[uri] = ++counter_;
    if (sets.empty()) files_.erase(it);
    return true;
}

bool DiagnosticStore::eraseServer(const std::string& server) {
    bool any = false;
    for (const std::string& uri : uris()) any = erase(uri, server) || any;
    return any;
}

std::vector<const DiagnosticStore::Set*> DiagnosticStore::forUri(const std::string& uri) const {
    std::vector<const Set*> out;
    auto it = files_.find(uri);
    if (it != files_.end())
        for (const Set& s : it->second) out.push_back(&s);
    return out;
}

const DiagnosticStore::Set* DiagnosticStore::find(const std::string& uri,
                                                  const std::string& server) const {
    for (const Set* s : forUri(uri))
        if (s->server == server) return s;
    return nullptr;
}

std::vector<std::string> DiagnosticStore::uris() const {
    std::vector<std::string> out;
    for (const auto& [uri, sets] : files_) out.push_back(uri);
    return out;
}

std::vector<Diagnostic> DiagnosticStore::merged(const std::string& uri) const {
    std::vector<Diagnostic> out;
    for (const Set* s : forUri(uri)) out.insert(out.end(), s->list.begin(), s->list.end());
    return out;
}

std::vector<MergedSpan> mergedDiagnosticSpans(
    const std::vector<const DiagnosticStore::Set*>& sets, const std::u16string& current) {
    std::vector<MergedSpan> out;
    for (const DiagnosticStore::Set* set : sets)
        for (const DiagnosticSpan& sp : diagnosticSpans(set->base.get(), current, set->list))
            out.push_back(MergedSpan{sp.start, sp.end, &set->list[sp.index]});
    std::stable_sort(out.begin(), out.end(), [](const MergedSpan& a, const MergedSpan& b) {
        return a.start < b.start;
    });
    return out;
}

std::vector<ProblemRow> problemRows(const std::vector<Problem>& problems) {
    // Each file's problems, with its worst severity.
    std::map<std::string, std::vector<size_t>> byFile;
    for (size_t i = 0; i < problems.size(); i++) byFile[problems[i].path].push_back(i);
    struct File {
        const std::string* path;
        std::vector<size_t> list;
        int worst;
    };
    std::vector<File> files;
    for (auto& [path, list] : byFile) {
        int worst = 4;
        for (size_t i : list) worst = std::min(worst, (int)problems[i].severity);
        files.push_back(File{&path, list, worst});
    }
    std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) {
        int wa = std::min(a.worst, 3), wb = std::min(b.worst, 3);   // info and hint alike
        if (wa != wb) return wa < wb;
        return *a.path < *b.path;
    });
    std::vector<ProblemRow> rows;
    for (File& f : files) {
        std::stable_sort(f.list.begin(), f.list.end(), [&](size_t a, size_t b) {
            const Problem &p = problems[a], &q = problems[b];
            if (p.severity != q.severity) return (int)p.severity < (int)q.severity;
            if (p.line != q.line) return p.line < q.line;
            return p.column < q.column;
        });
        ProblemRow h;
        h.heading = true;
        h.path = *f.path;
        for (size_t i : f.list) {
            if (problems[i].severity == Severity::Error) h.errors++;
            else if (problems[i].severity == Severity::Warning) h.warnings++;
            else h.others++;
        }
        rows.push_back(h);
        for (size_t i : f.list) {
            ProblemRow r;
            r.path = *f.path;
            r.problem = i;
            rows.push_back(r);
        }
    }
    return rows;
}

ProblemCounts countProblems(const std::vector<Problem>& problems) {
    ProblemCounts c;
    std::set<std::string> files;
    for (const Problem& p : problems) {
        switch (p.severity) {
        case Severity::Error: c.errors++; break;
        case Severity::Warning: c.warnings++; break;
        case Severity::Information: c.infos++; break;
        case Severity::Hint: c.hints++; break;
        }
        files.insert(p.path);
    }
    c.files = (int)files.size();
    return c;
}

std::string problemCountsText(const ProblemCounts& c) {
    auto named = [](int n, const char* one, const char* many) {
        return std::to_string(n) + " " + (n == 1 ? one : many);
    };
    std::vector<std::string> parts;
    if (c.errors) parts.push_back(named(c.errors, "error", "errors"));
    if (c.warnings) parts.push_back(named(c.warnings, "warning", "warnings"));
    if (parts.empty()) {
        if (c.infos) parts.push_back(named(c.infos, "info", "infos"));
        if (c.hints) parts.push_back(named(c.hints, "hint", "hints"));
    }
    if (parts.empty()) return "No problems";
    std::string s = parts[0];
    for (size_t i = 1; i < parts.size(); i++) s += ", " + parts[i];
    return s;
}

std::string problemOneLine(const std::string& message) {
    std::string out;
    bool space = false;
    for (char ch : message) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += ch;
    }
    return out;
}

long stepProblem(const std::vector<std::pair<size_t, size_t>>& spans, size_t caret,
                 long last, bool forward) {
    const long n = (long)spans.size();
    if (n == 0) return -1;
    std::vector<long> order((size_t)n);
    for (long i = 0; i < n; i++) order[(size_t)i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](long a, long b) { return spans[(size_t)a] < spans[(size_t)b]; });
    if (last >= 0 && last < n && spans[(size_t)last].first == caret) {
        long at = std::find(order.begin(), order.end(), last) - order.begin();
        return order[(size_t)((at + (forward ? 1 : n - 1)) % n)];
    }
    if (forward) {
        for (long i : order)
            if (spans[(size_t)i].first > caret) return i;
        return order.front();
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (spans[(size_t)*it].first < caret) return *it;
    return order.back();
}

Json diagnosticsInRange(const std::vector<Diagnostic>& diags, Range range) {
    Json out = Json::array();
    for (const Diagnostic& d : diags) {
        if (positionBefore(d.range.end, range.start) || positionBefore(range.end, d.range.start))
            continue;
        if (d.raw) {
            out.push(*d.raw);
            continue;
        }
        Json j = Json::object({{"range", Json::object({{"start", positionJson(d.range.start)},
                                                       {"end", positionJson(d.range.end)}})},
                               {"severity", (int)d.severity},
                               {"message", d.message}});
        if (!d.source.empty()) j.set("source", d.source);
        if (!d.code.empty()) j.set("code", d.code);
        out.push(j);
    }
    return out;
}

Range quickFixRange(const std::vector<Diagnostic>& diags, Range selection) {
    if (!(selection.start == selection.end)) return selection;
    if (diagnosticsInRange(diags, selection).size() > 0) return selection;
    const Range line{{selection.start.line, 0}, {selection.start.line, INT_MAX}};
    const Json onLine = diagnosticsInRange(diags, line);
    if (onLine.size() > 0) return parseRange(onLine[0]["range"]);
    return selection;
}

}  // namespace Lsp
