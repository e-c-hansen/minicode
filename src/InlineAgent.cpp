// InlineAgent.cpp — see InlineAgent.h.
#include "InlineAgent.h"

#include "LineDiff.h"
#include "SideBySide.h"

#include <algorithm>
#include <cctype>

namespace InlineAgent {

const char* const kPrintFlags =
    "-p --tools Read,Grep,Glob --no-session-persistence --output-format stream-json --verbose "
    "--include-partial-messages";

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string rtrim(const std::string& s) {
    size_t b = s.size();
    while (b > 0 && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(0, b);
}

// Lines without their "\n" or "\r\n", and where each starts and ends (the
// end before the line break).
struct Lines {
    std::vector<std::string> text;
    std::vector<size_t> start, end;
};
Lines splitLines(const std::string& s) {
    Lines l;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        size_t e = nl == std::string::npos ? s.size() : nl;
        size_t content = e;
        if (content > pos && s[content - 1] == '\r') content--;
        l.text.push_back(s.substr(pos, content - pos));
        l.start.push_back(pos);
        l.end.push_back(content);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return l;
}

std::vector<std::string> linesOf(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (true) {
        size_t nl = s.find('\n', pos);
        std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& lines, const std::string& eol) {
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += eol;
        out += lines[i];
    }
    return out;
}

std::string earlierText(const std::vector<std::string>& earlier) {
    if (earlier.empty()) return "";
    std::string out = "Earlier in this exchange the user asked for these, in order, and "
                      "they are already done:\n";
    for (const std::string& e : earlier) out += "- " + trim(e) + "\n";
    return out + "\n";
}

// A fence that the text does not contain, so the text cannot end it early.
std::string fenceFor(const std::string& text) {
    std::string fence = "```";
    while (text.find(fence) != std::string::npos) fence += "`";
    return fence;
}

}  // namespace

int lineOf(const std::string& text, size_t offset) {
    offset = std::min(offset, text.size());
    return 1 + (int)std::count(text.begin(), text.begin() + offset, '\n');
}

std::string editPrompt(const EditRequest& r) {
    const std::string& t = r.text;
    const size_t selStart = std::min(r.selStart, t.size());
    const size_t selEnd = std::max(selStart, std::min(r.selEnd, t.size()));
    std::string p = "You are the inline agent of minicode, a code editor. The user pressed "
                    "Cmd+I in the file " + r.path + " and asked:\n\n" +
                    trim(r.request) + "\n\n" + earlierText(r.earlier);

    const int first = lineOf(t, selStart);
    if (selEnd > selStart) {
        int last = lineOf(t, selEnd);
        if (t[selEnd - 1] == '\n' && last > first) last--;
        const std::string sel = t.substr(selStart, selEnd - selStart);
        const std::string fence = fenceFor(sel);
        p += "The user selected " +
             (first == last ? "part of line " + std::to_string(first)
                            : "lines " + std::to_string(first) + " to " + std::to_string(last)) +
             ":\n" + fence + "\n" + sel + (sel.back() == '\n' ? "" : "\n") + fence + "\n\n";
    } else {
        const size_t ls = t.rfind('\n', selStart ? selStart - 1 : 0);
        const size_t lineStart = (selStart == 0 || ls == std::string::npos) ? 0 : ls + 1;
        size_t le = t.find('\n', selStart);
        if (le == std::string::npos) le = t.size();
        std::string line = t.substr(lineStart, le - lineStart);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        p += "The cursor is on line " + std::to_string(first) + ", column " +
             std::to_string(selStart - lineStart + 1) + (line.empty() ? ", an empty line.\n\n"
                                                                     : ", which reads:\n" +
                                                                           line + "\n\n");
    }

    // The buffer, or the lines around the cursor in a big one.
    std::string shown = t;
    std::string intro = "The file as it is in the editor (it may hold unsaved changes the copy "
                        "on disk lacks):\n";
    if (t.size() > kMaxFileBytes) {
        Lines l = splitLines(t);
        const int total = (int)l.text.size();
        int lo = std::max(1, first - 400), hi = std::min(total, lineOf(t, selEnd) + 400);
        // Shrink evenly until it fits.
        while (hi > lo && l.end[hi - 1] - l.start[lo - 1] > kMaxFileBytes) {
            if (first - lo > hi - first) lo++;
            else hi--;
        }
        shown = t.substr(l.start[lo - 1], l.end[hi - 1] - l.start[lo - 1]);
        intro = "Lines " + std::to_string(lo) + " to " + std::to_string(hi) + " of the " +
                std::to_string(total) + " in the file, as they are in the editor (it may hold "
                "unsaved changes the copy on disk lacks):\n";
    }
    const std::string fence = fenceFor(shown);
    p += intro + fence + "\n" + shown + (shown.empty() || shown.back() == '\n' ? "" : "\n") +
         fence + "\n\n";

    p += "Answer in one of two ways.\n\n"
         "To change the file, reply with edit blocks and nothing else, no Markdown fences "
         "and no explanation:\n\n"
         "<<<<<<< SEARCH\n"
         "lines copied exactly from the file, indentation included\n"
         "=======\n"
         "the lines to put in their place\n"
         ">>>>>>> REPLACE\n\n"
         "Each SEARCH part must match the file exactly and only once, so give it the lines "
         "being changed and a line or two around them if they are not unique, no more. Use as "
         "many blocks as the change needs, in file order. To add lines, put a neighbouring "
         "line in SEARCH and again in REPLACE beside the new ones. To insert at the cursor in "
         "an empty file, leave SEARCH empty. Change only what the request asks for, in the "
         "file's own style and indentation, and add no comments about the change. When there "
         "is a selection, work on it unless the request plainly needs more. You may read "
         "other files of the project for context, but change only this file, and only "
         "through these blocks.\n\n"
         "If the request is a question, or cannot be done as an edit to this file, reply with "
         "a short plain-text answer instead, with no edit blocks.\n";
    return p;
}

Reply parseReply(const std::string& output) {
    Reply r;
    const std::vector<std::string> lines = linesOf(output);
    auto marker = [](const std::string& line, char c, const char* word) {
        std::string t = rtrim(line);
        size_t n = 0;
        while (n < t.size() && t[n] == c) n++;
        if (n < 5 || n > 9) return false;
        std::string rest = trim(t.substr(n));
        return word ? rest == word : rest.empty();
    };
    for (size_t i = 0; i < lines.size(); i++) {
        if (!marker(lines[i], '<', "SEARCH")) continue;
        std::vector<std::string> search, replace;
        size_t j = i + 1;
        while (j < lines.size() && !marker(lines[j], '=', nullptr)) search.push_back(lines[j++]);
        if (j >= lines.size()) break;
        j++;
        while (j < lines.size() && !marker(lines[j], '>', "REPLACE")) replace.push_back(lines[j++]);
        if (j >= lines.size()) break;   // an unfinished block is not applied
        Edit e;
        e.hasSearch = !search.empty();
        e.hasReplace = !replace.empty();
        e.search = join(search, "\n");
        e.replace = join(replace, "\n");
        r.edits.push_back(e);
        i = j;
    }
    if (r.edits.empty()) r.answer = trim(output);
    return r;
}

namespace {

// Where `search` (lines) sits in the buffer's lines, compared by `same`:
// the starting line of every match.
template <class Same>
std::vector<size_t> findLines(const Lines& l, const std::vector<std::string>& search, Same same) {
    std::vector<size_t> hits;
    if (search.empty() || search.size() > l.text.size()) return hits;
    for (size_t i = 0; i + search.size() <= l.text.size(); i++) {
        bool ok = true;
        for (size_t j = 0; j < search.size() && ok; j++) ok = same(l.text[i + j], search[j]);
        if (ok) hits.push_back(i);
    }
    return hits;
}

// Of several places [from, to), the one the selection touches (a caret:
// holds), or -1 when none or more than one does.
long touched(const std::vector<std::pair<size_t, size_t>>& at, size_t selStart, size_t selEnd) {
    if (at.size() == 1) return 0;
    long found = -1;
    for (size_t i = 0; i < at.size(); i++) {
        const bool hit = selEnd > selStart ? at[i].first < selEnd && at[i].second > selStart
                                           : at[i].first <= selStart && selStart <= at[i].second;
        if (!hit) continue;
        if (found >= 0) return -1;
        found = (long)i;
    }
    return found;
}

std::string leading(const std::string& line) {
    size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) n++;
    return line.substr(0, n);
}
bool allOf(const std::string& s, char c) {
    return !s.empty() && s.find_first_not_of(c) == std::string::npos;
}

// The REPLACE lines moved from the SEARCH part's indentation `model` to the
// file's `file` (see applyEdits).
std::vector<std::string> reindent(std::vector<std::string> lines, const std::string& model,
                                  const std::string& file) {
    if (model == file) return lines;
    // One level as the model wrote it against one as the file has it, when
    // one side is all spaces and the other all tabs.
    size_t spacesPerTab = 0;
    bool toTabs = false;
    if (allOf(file, '\t') && allOf(model, ' ') && model.size() % file.size() == 0) {
        spacesPerTab = model.size() / file.size();
        toTabs = true;
    } else if (allOf(file, ' ') && allOf(model, '\t') && file.size() % model.size() == 0) {
        spacesPerTab = file.size() / model.size();
    }
    auto convert = [&](const std::string& extra) {
        if (!spacesPerTab) return extra;
        std::string out;
        size_t spaces = 0;
        for (char c : extra) {
            if (toTabs && c == ' ') {
                if (++spaces == spacesPerTab) { out += '\t'; spaces = 0; }
                continue;
            }
            out.append(spaces, ' ');
            spaces = 0;
            if (!toTabs && c == '\t') out.append(spacesPerTab, ' ');
            else out += c;
        }
        out.append(spaces, ' ');
        return out;
    };
    for (std::string& l : lines) {
        if (trim(l).empty()) continue;
        const std::string lead = leading(l);
        const std::string body = l.substr(lead.size());
        if (lead.compare(0, model.size(), model) == 0) {
            l = file + convert(lead.substr(model.size())) + body;
        } else if (model.compare(0, lead.size(), lead) == 0) {
            // Less indented than the SEARCH part: take as much off the file's.
            size_t less = model.size() - lead.size();
            if (spacesPerTab && toTabs) less = (less + spacesPerTab - 1) / spacesPerTab;
            else if (spacesPerTab) less *= spacesPerTab;
            l = file.substr(0, file.size() > less ? file.size() - less : 0) + body;
        }
    }
    return lines;
}

}  // namespace

Applied applyEdits(const std::string& text, const std::vector<Edit>& edits, size_t selStart,
                   size_t selEnd) {
    Applied a;
    std::string cur = text;
    const bool crlf = text.find("\r\n") != std::string::npos;
    const std::string eol = crlf ? "\r\n" : "\n";
    selStart = std::min(selStart, text.size());
    selEnd = std::max(selStart, std::min(selEnd, text.size()));
    auto which = [&](size_t k) {
        return edits.size() == 1 ? std::string("The agent's edit")
                                 : "The agent's edit " + std::to_string(k + 1) + " of " +
                                       std::to_string(edits.size());
    };
    // The selection follows each replacement, so later edits see it where
    // its text now is.
    auto replaced = [&](size_t from, size_t to, size_t length) {
        auto move = [&](size_t& p) {
            if (p >= to) p = p - (to - from) + length;
            else if (p > from) p = from + std::min(p - from, length);
        };
        move(selStart);
        move(selEnd);
    };
    for (size_t k = 0; k < edits.size(); k++) {
        const Edit& e = edits[k];
        std::vector<std::string> replace =
            e.hasReplace ? linesOf(e.replace) : std::vector<std::string>{};
        if (!e.hasSearch) {
            std::string ins = join(replace, eol);
            const size_t at = std::min(selStart, cur.size());
            // On an empty line or at a line's end, the lines go in as lines.
            const bool lineEnd = at == cur.size() || cur[at] == '\n' || cur[at] == '\r';
            if (lineEnd && at > 0 && cur[at - 1] != '\n') ins = eol + ins;
            cur.insert(at, ins);
            replaced(at, at, ins.size());
            continue;
        }
        const std::vector<std::string> search = linesOf(e.search);
        Lines l = splitLines(cur);
        auto lineSpans = [&](const std::vector<size_t>& hits) {
            std::vector<std::pair<size_t, size_t>> at;
            for (size_t h : hits) at.emplace_back(l.start[h], l.end[h + search.size() - 1]);
            return at;
        };
        auto replaceLines = [&](size_t i, const std::vector<std::string>& with) {
            const size_t n = search.size();
            size_t from = l.start[i], to = l.end[i + n - 1];
            std::string put;
            if (with.empty()) {
                // The lines go, with one line break.
                if (i + n < l.text.size()) to = l.start[i + n];
                else if (i > 0) from = l.end[i - 1];
            } else {
                put = join(with, eol);
            }
            cur.replace(from, to - from, put);
            replaced(from, to, put.size());
        };
        auto ambiguous = [&](size_t places) {
            a.error = which(k) + " matches " + std::to_string(places) +
                      " places in the file, so nothing was changed. Select the part to change "
                      "and ask again.";
            return a;
        };
        std::vector<size_t> hits = findLines(l, search, [](const std::string& x, const std::string& y) {
            return x == y;
        });
        if (!hits.empty()) {
            const long pick = touched(lineSpans(hits), selStart, selEnd);
            if (pick < 0) return ambiguous(hits.size());
            replaceLines(hits[(size_t)pick], replace);
            continue;
        }
        // As text anywhere, part of a line too.
        const std::string needle = join(search, eol);
        std::vector<std::pair<size_t, size_t>> at;
        for (size_t p = cur.find(needle); p != std::string::npos && !needle.empty();
             p = cur.find(needle, p + 1))
            at.emplace_back(p, p + needle.size());
        if (!at.empty()) {
            const long pick = touched(at, selStart, selEnd);
            if (pick < 0) return ambiguous(at.size());
            const size_t p = at[(size_t)pick].first;
            // Found after the indentation of its line: the lines after the
            // first go in at that line's indentation, as for lines matched
            // ignoring indentation below.
            const size_t lineStart = p == 0 ? 0 : (cur.rfind('\n', p - 1) + 1);
            const std::string before = cur.substr(lineStart, p - lineStart);
            if (!before.empty() && replace.size() > 1 && leading(before) == before) {
                const std::string model = leading(search[0]);
                replace = reindent(replace, model, before + model);
                if (replace[0].compare(0, before.size(), before) == 0)
                    replace[0] = replace[0].substr(before.size());
            }
            const std::string put = join(replace, eol);
            cur.replace(p, needle.size(), put);
            replaced(p, p + needle.size(), put.size());
            continue;
        }
        // Whitespace the model got wrong: trailing, then indentation too.
        int level = 0;
        for (; level < 2 && hits.empty(); level++)
            hits = findLines(l, search, [level](const std::string& x, const std::string& y) {
                return level == 0 ? rtrim(x) == rtrim(y) : trim(x) == trim(y);
            });
        if (hits.empty()) {
            a.error = which(k) + " did not match the file, so nothing was changed.";
            return a;
        }
        const long pick = touched(lineSpans(hits), selStart, selEnd);
        if (pick < 0) return ambiguous(hits.size());
        const size_t i = hits[(size_t)pick];
        if (level == 2) {
            // Matched ignoring indentation: the first line with text on it
            // says how far the model's lines are off.
            for (size_t j = 0; j < search.size(); j++) {
                if (trim(search[j]).empty()) continue;
                replace = reindent(replace, leading(search[j]), leading(l.text[i + j]));
                break;
            }
        }
        replaceLines(i, replace);
    }
    a.ok = true;
    a.text = cur;
    a.selStart = selStart;
    a.selEnd = selEnd;
    return a;
}

// ------------------------------------------------------------ the stream
namespace {

std::string fileName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string shortened(std::string s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return s.substr(0, n) + "…";
}

// A tool call as the bar names it.
std::string describeTool(const std::string& name, const Json& input) {
    const std::string path = input["file_path"].asString(), pattern = input["pattern"].asString();
    if (name == "Read") return path.empty() ? "Reading a file" : "Reading " + fileName(path);
    if (name == "Grep")
        return pattern.empty() ? "Searching the project"
                               : "Searching for " + shortened(pattern, 40);
    if (name == "Glob")
        return pattern.empty() ? "Looking for files" : "Looking for " + shortened(pattern, 40);
    return name.empty() ? "Using a tool" : "Using " + name;
}

}  // namespace

bool Stream::feed(const std::string& bytes) {
    changed_ = false;
    // The output as it came is the reply only while no line is JSON; once
    // one is, it is never read, and with --verbose it would hold every
    // tool result (whole files read) a second time.
    if (!sawJson_) raw_ += bytes;
    pending_ += bytes;
    size_t from = 0;
    for (size_t nl = pending_.find('\n'); nl != std::string::npos; nl = pending_.find('\n', from)) {
        line(pending_.substr(from, nl - from));
        from = nl + 1;
    }
    pending_.erase(0, from);
    if (sawJson_ && !raw_.empty()) std::string().swap(raw_);
    return changed_;
}

bool Stream::finish() {
    changed_ = false;
    if (!pending_.empty()) line(pending_);
    pending_.clear();
    return changed_;
}

void Stream::line(const std::string& l) {
    const std::string t = trim(l);
    if (t.empty() || t[0] != '{') return;
    Json j;
    if (!Json::parse(t, j) || !j.isObject() || !j["type"].isString()) return;
    sawJson_ = true;
    const std::string type = j["type"].asString();
    auto setText = [&](std::string v) {
        if (v != text_) { text_ = std::move(v); changed_ = true; }
    };
    auto setActivity = [&](std::string v) {
        if (v != activity_) { activity_ = std::move(v); changed_ = true; }
    };
    if (type == "stream_event") {
        const Json& ev = j["event"];
        const std::string et = ev["type"].asString();
        if (et == "message_start") {
            messageId_ = ev["message"]["id"].asString();
            partial_ = false;
            setText("");
        } else if (et == "content_block_start") {
            const Json& block = ev["content_block"];
            if (block["type"].asString() == "tool_use")
                setActivity(describeTool(block["name"].asString(), Json()));
        } else if (et == "content_block_delta") {
            const Json& d = ev["delta"];
            if (d["type"].asString() == "text_delta" && d["text"].isString()) {
                partial_ = true;
                setActivity("");
                setText(text_ + d["text"].asString());
            }
        }
    } else if (type == "assistant") {
        const Json& m = j["message"];
        std::string said;
        for (const Json& b : m["content"].elements()) {
            const std::string bt = b["type"].asString();
            if (bt == "text") said += b["text"].asString();
            else if (bt == "tool_use") setActivity(describeTool(b["name"].asString(), b["input"]));
        }
        // Claude Code may send a message a content block at a time; the
        // pieces of one message share its id. With partial messages on, the
        // deltas have already written its text.
        const std::string id = m["id"].asString();
        const bool same = !id.empty() && id == messageId_;
        if (!same) {
            messageId_ = id;
            partial_ = false;
            setText(said);
        } else if (!partial_ && !said.empty()) {
            setText(text_ + said);
        }
        if (!said.empty()) lastText_ = text_;
    } else if (type == "user") {
        setActivity("");
    } else if (type == "result") {
        done_ = true;
        failed_ = j["is_error"].asBool(false) || (j["subtype"].isString() &&
                                                  j["subtype"].asString() != "success");
        result_ = j["result"].asString();
        subtype_ = j["subtype"].asString();
        setActivity("");
        changed_ = true;
    }
}

std::string Stream::reply() const {
    if (!sawJson_) return raw_;
    if (done_ && !result_.empty()) return result_;
    if (!lastText_.empty()) return lastText_;
    return text_;
}

std::string Stream::error() const {
    if (failed_) {
        if (!trim(result_).empty()) return trim(result_);
        if (subtype_ == "error_max_turns") return "Claude Code stopped: it used all its turns.";
        if (!subtype_.empty() && subtype_ != "success")
            return "Claude Code stopped: " + subtype_ + ".";
        return "Claude Code reported an error.";
    }
    if (sawJson_ && !done_) return "Claude Code stopped before it finished its answer.";
    return "";
}

std::string preview(const std::string& partial, size_t maxLines) {
    std::vector<std::string> in = linesOf(partial);
    // A line still being written that starts a marker says nothing yet.
    if (!partial.empty() && partial.back() != '\n') {
        const std::string last = trim(in.back());
        if (!last.empty() && (last[0] == '<' || last[0] == '=' || last[0] == '>')) in.pop_back();
    }
    auto marker = [](const std::string& line, char c, const char* word) {
        std::string t = rtrim(line);
        size_t n = 0;
        while (n < t.size() && t[n] == c) n++;
        if (n < 5 || n > 9) return false;
        std::string rest = trim(t.substr(n));
        return word ? rest == word : rest.empty();
    };
    std::vector<std::string> out;
    enum { Outside, Search, Replace } state = Outside;
    int block = 0;
    size_t searchLines = 0, header = 0;
    for (const std::string& l : in) {
        if (state == Outside && marker(l, '<', "SEARCH")) {
            if (!out.empty() && !trim(out.back()).empty()) out.push_back("");
            block++;
            searchLines = 0;
            header = out.size();
            out.push_back("Edit " + std::to_string(block) + ":");
            state = Search;
        } else if (state == Search && marker(l, '=', nullptr)) {
            if (searchLines)
                out[header] = "Edit " + std::to_string(block) + ", in place of " +
                              std::to_string(searchLines) +
                              (searchLines == 1 ? " line:" : " lines:");
            else
                out[header] = "Edit " + std::to_string(block) + ", new lines:";
            state = Replace;
        } else if (state == Search) {
            searchLines++;
        } else if (state == Replace && marker(l, '>', "REPLACE")) {
            state = Outside;
        } else {
            out.push_back(l);
        }
    }
    while (!out.empty() && trim(out.back()).empty()) out.pop_back();
    while (!out.empty() && trim(out.front()).empty()) out.erase(out.begin());
    if (out.size() > maxLines) out.erase(out.begin(), out.end() - (long)maxLines);
    return join(out, "\n");
}

// ------------------------------------------------------- the pending diff
namespace {

std::vector<std::u16string> lines16(const std::u16string& s) {
    std::vector<std::u16string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find(u'\n', pos);
        size_t e = nl == std::u16string::npos ? s.size() : nl;
        std::u16string line = s.substr(pos, e - pos);
        if (!line.empty() && line.back() == u'\r') line.pop_back();
        out.push_back(line);
        if (nl == std::u16string::npos) break;
        pos = nl + 1;
    }
    return out;
}

}  // namespace

std::vector<DiffLine> pendingDiff(const std::u16string& before, const std::u16string& after,
                                  size_t context, size_t maxLines) {
    std::vector<DiffLine> out;
    const std::vector<SideBySide::Row> rows = SideBySide::align(before, after);
    const std::vector<SideBySide::Hunk> hunks = SideBySide::hunks(rows);
    if (hunks.empty()) return out;
    const std::vector<std::u16string> a = lines16(before), b = lines16(after);
    auto lineAt = [](const std::vector<std::u16string>& v, int i) {
        return i >= 0 && (size_t)i < v.size() ? v[(size_t)i] : std::u16string();
    };
    // Rows on show: each run of changes and `context` rows either side.
    std::vector<char> show(rows.size(), 0);
    for (const SideBySide::Hunk& h : hunks)
        for (size_t r = h.first > context ? h.first - context : 0;
             r < std::min(rows.size(), h.end + context); r++)
            show[r] = 1;
    size_t more = 0;
    auto push = [&](DiffLine d) {
        if (out.size() >= maxLines) { more++; return; }
        out.push_back(std::move(d));
    };
    for (size_t r = 0; r < rows.size();) {
        if (!show[r]) { r++; continue; }
        if (r > 0 && !show[r - 1] && !out.empty()) {
            DiffLine gap;
            gap.kind = DiffLine::Gap;
            push(gap);
        }
        if (rows[r].kind == SideBySide::Kind::Same) {
            DiffLine d;
            d.kind = DiffLine::Context;
            d.oldLine = rows[r].left + 1;
            d.newLine = rows[r].right + 1;
            d.text = lineAt(b, rows[r].right);
            push(d);
            r++;
            continue;
        }
        // A run of changes: its old lines, then its new ones, the words of
        // each changed pair marked.
        size_t e = r;
        while (e < rows.size() && rows[e].kind != SideBySide::Kind::Same) e++;
        std::vector<DiffLine> added;
        for (size_t k = r; k < e; k++) {
            const SideBySide::Row& row = rows[k];
            SideBySide::WordChanges w;
            if (row.kind == SideBySide::Kind::Changed)
                w = SideBySide::wordChanges(lineAt(a, row.left), lineAt(b, row.right));
            if (row.left >= 0) {
                DiffLine d;
                d.kind = DiffLine::Removed;
                d.oldLine = row.left + 1;
                d.text = lineAt(a, row.left);
                for (const SideBySide::Span& sp : w.left) d.words.emplace_back(sp.start, sp.length);
                push(d);
            }
            if (row.right >= 0) {
                DiffLine d;
                d.kind = DiffLine::Added;
                d.newLine = row.right + 1;
                d.text = lineAt(b, row.right);
                for (const SideBySide::Span& sp : w.right) d.words.emplace_back(sp.start, sp.length);
                added.push_back(d);
            }
        }
        for (DiffLine& d : added) push(std::move(d));
        r = e;
    }
    if (more) {
        DiffLine gap;
        gap.kind = DiffLine::Gap;
        gap.more = more;
        out.push_back(gap);
    }
    return out;
}

size_t changedLines(const std::u16string& before, const std::u16string& after) {
    const std::vector<SideBySide::Row> rows = SideBySide::align(before, after);
    size_t n = 0;
    for (const SideBySide::Hunk& h : SideBySide::hunks(rows)) {
        size_t removed = 0, added = 0;
        for (size_t r = h.first; r < h.end; r++) {
            if (rows[r].left >= 0) removed++;
            if (rows[r].right >= 0) added++;
        }
        n += std::max(removed, added);
    }
    return n;
}

size_t moveOffset(const std::u16string& base, const std::u16string& now, size_t pos) {
    if (base == now) return std::min(pos, now.size());
    return LineDiff::mapOffset(base, now, LineDiff::diff(base, now), std::min(pos, base.size()));
}

bool sameChange(const std::u16string& base, const std::u16string& pending,
                const std::u16string& now, const std::u16string& next) {
    const std::vector<LineDiff::Hunk> shown = LineDiff::diff(base, pending, 40000, 0.25, false);
    const std::vector<LineDiff::Hunk> made = LineDiff::diff(now, next, 40000, 0.25, false);
    if (shown.size() != made.size()) return false;
    const std::vector<LineDiff::Hunk> user = LineDiff::diff(base, now, 40000, 0.25, false);
    for (size_t i = 0; i < shown.size(); i++) {
        const LineDiff::Hunk &a = shown[i], &b = made[i];
        if (base.compare(a.oldStart, a.oldLength, now, b.oldStart, b.oldLength) != 0 ||
            pending.compare(a.newStart, a.newLength, next, b.newStart, b.newLength) != 0)
            return false;
        // The user changed none of the lines the run takes out (a run the
        // user deleted or rewrote may be "found" again elsewhere), so the
        // user's edits only moved it.
        const size_t from = a.oldStart, to = a.oldStart + a.oldLength;
        for (const LineDiff::Hunk &h : user) {
            const size_t hFrom = h.oldStart, hTo = h.oldStart + h.oldLength;
            if (hFrom == hTo ? hFrom > from && hFrom < to
                             : hFrom < std::max(to, from + 1) && hTo > from)
                return false;
        }
        // Where the user's edits moved the run: its start, or else its end
        // (a line the user added right above it maps its start before
        // that line).
        if (LineDiff::mapOffset(base, now, user, a.oldStart) != b.oldStart &&
            LineDiff::mapOffset(base, now, user, a.oldStart + a.oldLength) !=
                b.oldStart + b.oldLength)
            return false;
    }
    return true;
}

std::string commandPrompt(const CommandRequest& r) {
    std::string p = "You are the inline agent in minicode's terminal. The user pressed Cmd+I in "
                    "a terminal running zsh on macOS, in the folder " + r.directory +
                    ", and asked:\n\n" + trim(r.request) + "\n\n" + earlierText(r.earlier);
    if (!trim(r.screen).empty()) {
        const std::string fence = fenceFor(r.screen);
        p += "The last lines in the terminal:\n" + fence + "\n" + r.screen + "\n" + fence + "\n\n";
    }
    p += "Reply with only the shell command that does it: no explanation, no Markdown fence, "
         "no \"$\" before it. One line when it can be; join commands with && when there are "
         "several. It is typed at the prompt for the user to read and run, so it has not run "
         "yet. You may read the project's files to get it right. If no command can do it, "
         "reply with one line starting with # that says why.\n";
    return p;
}

std::string commandFrom(const std::string& output, std::string* note) {
    if (note) note->clear();
    std::vector<std::string> lines = linesOf(output);
    // A fenced block, if the model wrote one anyway: its content alone.
    for (size_t i = 0; i < lines.size(); i++) {
        if (trim(lines[i]).rfind("```", 0) != 0) continue;
        size_t j = i + 1;
        while (j < lines.size() && trim(lines[j]).rfind("```", 0) != 0) j++;
        if (j < lines.size()) {
            lines = std::vector<std::string>(lines.begin() + i + 1, lines.begin() + j);
            break;
        }
    }
    while (!lines.empty() && trim(lines.front()).empty()) lines.erase(lines.begin());
    while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
    if (lines.empty()) return "";
    if (trim(lines.front()).rfind("#", 0) == 0 && lines.size() == 1) {
        std::string n = trim(trim(lines.front()).substr(1));
        if (note) *note = n;
        return "";
    }
    for (std::string& l : lines) {
        std::string t = l;
        size_t i = 0;
        while (i < t.size() && t[i] == ' ') i++;
        if (t.compare(i, 2, "$ ") == 0 || t.compare(i, 2, "% ") == 0) l = t.substr(i + 2);
        l = rtrim(l);
    }
    return join(lines, "\n");
}

std::string taskPrompt(const TaskRequest& r) {
    const std::string req = trim(r.request);
    if (req.empty()) return "";
    std::string p = req + "\n\n(Asked from minicode with Cmd+I";
    if (!r.where.empty()) p += ", in " + r.where;
    p += ".";
    if (!r.path.empty() && r.where.find(r.path) == std::string::npos)
        p += " The file open in the editor is " + r.path + ".";
    p += ")\n";
    if (!r.selection.empty()) {
        const std::string fence = fenceFor(r.selection);
        p += "\nThe selected text, " +
             (r.firstLine == r.lastLine ? "on line " + std::to_string(r.firstLine)
                                        : "lines " + std::to_string(r.firstLine) + " to " +
                                              std::to_string(r.lastLine)) +
             (r.path.empty() ? "" : " of " + r.path) + ":\n" + fence + "\n" + r.selection +
             (r.selection.back() == '\n' ? "" : "\n") + fence + "\n";
    }
    if (!trim(r.screen).empty()) {
        const std::string fence = fenceFor(r.screen);
        p += "\nThe last lines in that terminal:\n" + fence + "\n" + r.screen + "\n" + fence + "\n";
    }
    return p;
}

}  // namespace InlineAgent
