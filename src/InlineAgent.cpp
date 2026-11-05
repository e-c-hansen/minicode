// InlineAgent.cpp — see InlineAgent.h.
#include "InlineAgent.h"

#include <algorithm>
#include <cctype>

namespace InlineAgent {

const char* const kPrintFlags =
    "-p --tools Read,Grep,Glob --no-session-persistence --output-format text";

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

size_t nearest(const std::vector<size_t>& starts, size_t near) {
    size_t best = starts[0];
    auto dist = [&](size_t a) { return a > near ? a - near : near - a; };
    for (size_t s : starts)
        if (dist(s) < dist(best)) best = s;
    return best;
}

}  // namespace

Applied applyEdits(const std::string& text, const std::vector<Edit>& edits, size_t near) {
    Applied a;
    std::string cur = text;
    const bool crlf = text.find("\r\n") != std::string::npos;
    const std::string eol = crlf ? "\r\n" : "\n";
    near = std::min(near, text.size());
    for (size_t k = 0; k < edits.size(); k++) {
        const Edit& e = edits[k];
        const std::vector<std::string> replace =
            e.hasReplace ? linesOf(e.replace) : std::vector<std::string>{};
        if (!e.hasSearch) {
            std::string ins = join(replace, eol);
            const size_t at = std::min(near, cur.size());
            // On an empty line or at a line's end, the lines go in as lines.
            const bool lineEnd = at == cur.size() || cur[at] == '\n' || cur[at] == '\r';
            if (lineEnd && at > 0 && cur[at - 1] != '\n') ins = eol + ins;
            cur.insert(at, ins);
            continue;
        }
        const std::vector<std::string> search = linesOf(e.search);
        Lines l = splitLines(cur);
        auto replaceLines = [&](size_t i) {
            const size_t n = search.size();
            size_t from = l.start[i], to = l.end[i + n - 1];
            if (replace.empty()) {
                // The lines go, with one line break.
                if (i + n < l.text.size()) to = l.start[i + n];
                else if (i > 0) from = l.end[i - 1];
                cur.replace(from, to - from, "");
            } else {
                cur.replace(from, to - from, join(replace, eol));
            }
        };
        std::vector<size_t> hits = findLines(l, search, [](const std::string& a, const std::string& b) {
            return a == b;
        });
        if (!hits.empty()) {
            std::vector<size_t> starts;
            for (size_t h : hits) starts.push_back(l.start[h]);
            replaceLines(hits[std::find(starts.begin(), starts.end(), nearest(starts, near)) -
                              starts.begin()]);
            continue;
        }
        // As text anywhere, part of a line too.
        const std::string needle = join(search, eol);
        std::vector<size_t> at;
        for (size_t p = cur.find(needle); p != std::string::npos && !needle.empty();
             p = cur.find(needle, p + 1))
            at.push_back(p);
        if (!at.empty()) {
            const size_t p = nearest(at, near);
            cur.replace(p, needle.size(), join(replace, eol));
            continue;
        }
        // Whitespace the model got wrong.
        for (int level = 0; level < 2 && hits.empty(); level++)
            hits = findLines(l, search, [level](const std::string& a, const std::string& b) {
                return level == 0 ? rtrim(a) == rtrim(b) : trim(a) == trim(b);
            });
        if (hits.empty()) {
            a.error = edits.size() == 1
                          ? "The agent's edit did not match the file, so nothing was changed."
                          : "The agent's edit " + std::to_string(k + 1) + " of " +
                                std::to_string(edits.size()) +
                                " did not match the file, so nothing was changed.";
            return a;
        }
        std::vector<size_t> starts;
        for (size_t h : hits) starts.push_back(l.start[h]);
        replaceLines(hits[std::find(starts.begin(), starts.end(), nearest(starts, near)) -
                          starts.begin()]);
    }
    a.ok = true;
    a.text = cur;
    return a;
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
