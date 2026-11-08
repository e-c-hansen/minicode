// LspCompletion.cpp — see LspClient.h. Completion results parsed,
// resolved, filtered and accepted (completionEdits, lateAdditionalEdits,
// the keys that accept), and signature help parsed.
#include "LspClient.h"
#include "LspInternal.h"
#include <algorithm>

namespace Lsp {

namespace {

// clangd marks items whose #include it would add with a leading bullet, and
// pads the others with a space so the labels line up. Neither is part of the
// name.
std::string cleanLabel(const std::string& label) {
    std::string s = trim(label);
    static const std::string bullet = "\xE2\x80\xA2";   // U+2022
    if (s.compare(0, bullet.size(), bullet) == 0) s = trim(s.substr(bullet.size()));
    return s;
}

}  // namespace

namespace {
// A TextEdit[] (additionalTextEdits), skipping anything malformed.
std::vector<TextEdit> parseTextEdits(const Json& list) {
    std::vector<TextEdit> out;
    for (const Json& e : list.elements()) {
        if (!e.isObject() || !e["range"].isObject() || !e["newText"].isString()) continue;
        TextEdit t;
        t.range = parseRange(e["range"]);
        if (positionBefore(t.range.end, t.range.start)) std::swap(t.range.start, t.range.end);
        t.newText = e["newText"].asString();
        out.push_back(t);
    }
    return out;
}
}  // namespace

std::vector<CompletionItem> parseCompletion(const Json& result, bool* incomplete) {
    if (incomplete) *incomplete = false;
    const Json* list = &result;
    int defaultFormat = 1, defaultMode = 0;
    if (result.isObject()) {
        if (incomplete) *incomplete = result["isIncomplete"].asBool();
        list = &result["items"];
        const Json& defaults = result["itemDefaults"];
        if (defaults.isObject()) {
            defaultFormat = (int)defaults["insertTextFormat"].asInt(1);
            defaultMode = (int)defaults["insertTextMode"].asInt(0);
        }
    }
    std::vector<CompletionItem> out;
    for (const Json& it : list->elements()) {
        CompletionItem c;
        c.label = cleanLabel(it["label"].asString());
        if (c.label.empty()) continue;
        c.detail = trim(it["detail"].asString());
        parseMarkup(it["documentation"], c.documentation, c.docIsMarkdown);
        c.raw = std::make_shared<const Json>(it);
        c.insertText = it["insertText"].asString();
        c.filterText = it["filterText"].asString();
        c.sortText = it["sortText"].asString();
        c.kind = (int)it["kind"].asInt();
        c.isSnippet = (it.has("insertTextFormat") ? (int)it["insertTextFormat"].asInt(1)
                                                  : defaultFormat) == 2;
        c.insertAsIs = (it.has("insertTextMode") ? (int)it["insertTextMode"].asInt(0)
                                                 : defaultMode) == 1;
        const Json& edit = it["textEdit"];
        if (edit.isObject()) {
            c.hasEdit = true;
            c.newText = edit["newText"].asString();
            if (edit.has("range")) c.editRange = parseRange(edit["range"]);
            else c.editRange = parseRange(edit["insert"]);   // InsertReplaceEdit
        }
        if (it["additionalTextEdits"].isArray()) {
            c.hasAdditionalEdits = true;
            c.additionalEdits = parseTextEdits(it["additionalTextEdits"]);
        }
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const CompletionItem& a,
                                                const CompletionItem& b) {
        const std::string& ka = a.sortText.empty() ? a.label : a.sortText;
        const std::string& kb = b.sortText.empty() ? b.label : b.sortText;
        return ka < kb;
    });
    for (size_t i = 0; i < out.size(); i++) out[i].index = (int)i;
    return out;
}

bool parseMarkup(const Json& j, std::string& text, bool& isMarkdown) {
    if (j.isString()) {
        text = j.asString();
        isMarkdown = false;
        return true;
    }
    if (j.isObject() && j["value"].isString()) {
        text = j["value"].asString();
        isMarkdown = j["kind"].asString() == "markdown";
        return true;
    }
    return false;
}

void applyResolved(CompletionItem& item, const Json& result) {
    item.resolved = true;
    if (!result.isObject()) return;
    std::string detail = trim(result["detail"].asString());
    if (!detail.empty()) item.detail = detail;
    std::string doc;
    bool md = false;
    if (parseMarkup(result["documentation"], doc, md) && !trim(doc).empty()) {
        item.documentation = doc;
        item.docIsMarkdown = md;
    }
    if (result["additionalTextEdits"].isArray()) {
        item.hasAdditionalEdits = true;
        item.additionalEdits = parseTextEdits(result["additionalTextEdits"]);
    }
}

bool completionWantsResolve(const CompletionItem& item, bool serverResolves) {
    return serverResolves && item.raw && !item.resolved && !item.hasAdditionalEdits;
}

CompletionEdits completionEdits(const std::u16string& text, size_t start, size_t end,
                                const std::u16string& insert,
                                const std::vector<TextEdit>& additional) {
    CompletionEdits out;
    start = std::min(start, text.size());
    end = std::min(std::max(end, start), text.size());
    std::vector<OffsetEdit> extra;
    for (const TextEdit& e : additional) {
        size_t a = offsetForPosition(text, e.range.start);
        size_t b = offsetForPosition(text, e.range.end);
        if (b < a) std::swap(a, b);
        extra.push_back(OffsetEdit{a, b - a, toUtf16(e.newText)});
    }
    std::stable_sort(extra.begin(), extra.end(),
                     [](const OffsetEdit& x, const OffsetEdit& y) { return x.start < y.start; });
    // Each one wholly before the main edit (ending at its start counts) or
    // wholly after it, and none overlapping the next.
    bool ok = true;
    for (size_t i = 0; i < extra.size() && ok; i++) {
        const size_t a = extra[i].start, b = a + extra[i].length;
        if (!(b <= start || a >= end)) ok = false;
        if (i > 0 && a < extra[i - 1].start + extra[i - 1].length) ok = false;
    }
    if (!ok) {
        extra.clear();
        out.droppedAdditional = true;
    }
    long shift = 0;   // what the edits before the main one add or remove
    bool placed = false;
    for (const OffsetEdit& e : extra) {
        const bool before = e.start + e.length <= start;
        if (!before && !placed) {
            out.edits.push_back(OffsetEdit{start, end - start, insert});
            placed = true;
        }
        if (before) shift += (long)e.text.size() - (long)e.length;
        out.edits.push_back(e);
    }
    if (!placed) out.edits.push_back(OffsetEdit{start, end - start, insert});
    out.caret = (size_t)((long)start + shift) + insert.size();
    return out;
}

bool lateAdditionalEdits(const std::u16string& now, const std::vector<TextEdit>& additional,
                         size_t limit, std::vector<OffsetEdit>& out) {
    out.clear();
    limit = std::min(limit, now.size());
    const PositionIndex index(now);
    std::vector<OffsetEdit> edits;
    for (const TextEdit& e : additional) {
        size_t a = index.offset(e.range.start), b = index.offset(e.range.end);
        if (b < a) std::swap(a, b);
        // A position on the line `limit` is on, past it, may be a column
        // that line had before; past the limit nothing is known.
        if (b > limit) return false;
        edits.push_back(OffsetEdit{a, b - a, toUtf16(e.newText)});
    }
    std::stable_sort(edits.begin(), edits.end(),
                     [](const OffsetEdit& x, const OffsetEdit& y) { return x.start < y.start; });
    for (size_t i = 1; i < edits.size(); i++)
        if (edits[i].start < edits[i - 1].start + edits[i - 1].length) return false;
    out = std::move(edits);
    return true;
}

SignatureHelp parseSignatureHelp(const Json& result) {
    SignatureHelp help;
    if (!result.isObject()) return help;
    for (const Json& s : result["signatures"].elements()) {
        SignatureInfo info;
        info.label = s["label"].asString();
        parseMarkup(s["documentation"], info.documentation, info.docIsMarkdown);
        size_t from = 0;   // a string label is looked for after the last one
        for (const Json& p : s["parameters"].elements()) {
            const Json& l = p["label"];
            std::pair<int, int> range{-1, -1};
            if (l.isArray() && l.size() == 2) {
                range = {(int)l[0].asInt(-1), (int)l[1].asInt(-1)};
            } else if (l.isString() && !l.asString().empty()) {
                size_t at = info.label.find(l.asString(), from);
                if (at == std::string::npos) at = info.label.find(l.asString());
                if (at != std::string::npos) {
                    int a = (int)toUtf16(info.label.substr(0, at)).size();
                    range = {a, a + (int)toUtf16(l.asString()).size()};
                    from = at + l.asString().size();
                }
            }
            info.params.push_back(range);
            std::string doc;
            bool md = false;
            parseMarkup(p["documentation"], doc, md);
            info.paramDocs.push_back(trim(doc));
        }
        help.signatures.push_back(info);
    }
    if (help.signatures.empty()) return help;
    int active = (int)result["activeSignature"].asInt(0);
    if (active < 0 || active >= (int)help.signatures.size()) active = 0;
    help.activeSignature = active;
    // A signature's own activeParameter wins over the result's.
    const Json& sig = result["signatures"][(size_t)active];
    const Json& param = sig.has("activeParameter") ? sig["activeParameter"]
                                                   : result["activeParameter"];
    int ap = param.isNumber() ? (int)param.asInt() : 0;
    if (ap < 0 || ap >= (int)help.signatures[(size_t)active].params.size()) ap = -1;
    help.activeParameter = ap;
    return help;
}

std::vector<CompletionItem> filterCompletions(const std::vector<CompletionItem>& items,
                                              const std::string& prefix) {
    if (prefix.empty()) return items;
    std::string p = lower(prefix);
    // exact, same-case prefix, any-case prefix, subsequence
    std::vector<CompletionItem> tiers[4];
    for (const CompletionItem& it : items) {
        const std::string& raw = it.filterKey();
        if (raw == prefix || it.textToInsert() == prefix) { tiers[0].push_back(it); continue; }
        if (raw.compare(0, prefix.size(), prefix) == 0) { tiers[1].push_back(it); continue; }
        std::string key = lower(raw);
        if (key.compare(0, p.size(), p) == 0) { tiers[2].push_back(it); continue; }
        size_t k = 0;
        for (char c : key)
            if (k < p.size() && c == p[k]) k++;
        if (k == p.size()) tiers[3].push_back(it);
    }
    std::vector<CompletionItem> out;
    for (auto& t : tiers) out.insert(out.end(), t.begin(), t.end());
    return out;
}

int exactCompletion(const std::vector<CompletionItem>& shown, const std::string& typed) {
    if (typed.empty()) return -1;
    for (size_t i = 0; i < shown.size(); i++)
        if (shown[i].filterKey() == typed || shown[i].textToInsert() == typed) return (int)i;
    return -1;
}

bool completionKeyAccepts(CompletionOrigin origin, bool navigated, CompletionKey key,
                          bool selectedEqualsTyped) {
    switch (key) {
    case CompletionKey::Tab:
        return true;
    case CompletionKey::ShiftTab:
        return false;
    case CompletionKey::Return:
        // A word typed out in full: Return is a new line whatever the list.
        if (selectedEqualsTyped) return false;
        return origin != CompletionOrigin::Typing || navigated;
    }
    return false;
}

}  // namespace Lsp
