// Notebook.cpp — see Notebook.h.
#include "Notebook.h"
#include "Json.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace Notebook {

namespace {

// nbformat writes multi-line text as a list of lines; either form is read.
std::string joined(const Json &v) {
    if (v.isString()) return v.asString();
    std::string out;
    if (v.isArray())
        for (const Json &line : v.elements()) out += line.asString();
    return out;
}

std::string withoutWhitespace(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t') out += c;
    return out;
}

std::string trimTrailingNewlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// One output of a code cell, from its "data" bundle when it has one: a
// picture first, then Markdown, then plain text, then HTML's text.
bool readBundle(const Json &data, Output &o) {
    for (const char *mime : {"image/png", "image/jpeg", "image/gif"}) {
        if (data.has(mime)) {
            o.kind = Output::Kind::Image;
            o.mime = mime;
            o.base64 = withoutWhitespace(joined(data[mime]));
            o.text = joined(data["text/plain"]);
            return !o.base64.empty();
        }
    }
    if (data.has("text/markdown")) {
        o.kind = Output::Kind::Markdown;
        o.text = joined(data["text/markdown"]);
        return true;
    }
    if (data.has("text/plain")) {
        o.kind = Output::Kind::Text;
        o.text = joined(data["text/plain"]);
        return true;
    }
    if (data.has("text/html")) {
        o.kind = Output::Kind::Text;
        o.text = htmlToText(joined(data["text/html"]));
        return true;
    }
    return false;
}

Output::Kind kindOfV3Picture(const Json &out, Output &o) {
    for (const char *key : {"png", "jpeg", "gif"}) {
        if (out.has(key)) {
            o.mime = std::string("image/") + key;
            o.base64 = withoutWhitespace(joined(out[key]));
            return Output::Kind::Image;
        }
    }
    return Output::Kind::Text;
}

void readOutputs(const Json &list, int nbformat, Cell &cell) {
    for (const Json &out : list.elements()) {
        const std::string type = out["output_type"].asString();
        Output o;
        if (type == "stream") {
            o.kind = Output::Kind::Stream;
            o.name = out["name"].asString(nbformat >= 4 ? "stdout" : out["stream"].asString("stdout"));
            o.text = joined(out["text"]);
        } else if (type == "error" || type == "pyerr") {
            o.kind = Output::Kind::Error;
            std::string tb;
            for (const Json &line : out["traceback"].elements()) tb += line.asString() + "\n";
            o.text = stripAnsi(tb.empty() ? out["ename"].asString() + ": " + out["evalue"].asString()
                                          : tb);
        } else if (nbformat >= 4) {
            if (!readBundle(out["data"], o)) continue;
        } else {   // nbformat 3: pyout and display_data with the data at the top
            o.kind = kindOfV3Picture(out, o);
            if (o.kind != Output::Kind::Image) {
                if (out.has("text")) o.text = joined(out["text"]);
                else if (out.has("html")) o.text = htmlToText(joined(out["html"]));
                else continue;
            }
        }
        cell.outputs.push_back(std::move(o));
    }
}

// Markdown cells name their own pictures "attachment:name"; they become
// data: images.
std::string inlineAttachments(std::string source, const Json &attachments) {
    if (!attachments.isObject()) return source;
    for (const auto &item : attachments.items()) {
        const Json &bundle = item.second;
        for (const auto &kind : bundle.items()) {
            if (kind.first.rfind("image/", 0) != 0) continue;
            const std::string ref = "attachment:" + item.first;
            const std::string uri = "data:" + kind.first + ";base64," +
                                    withoutWhitespace(joined(kind.second));
            for (size_t at = source.find(ref); at != std::string::npos;
                 at = source.find(ref, at + uri.size()))
                source.replace(at, ref.size(), uri);
            break;
        }
    }
    return source;
}

void appendBlock(std::string &md, const std::string &text, const std::string &lang) {
    const std::string body = trimTrailingNewlines(text);
    const std::string fence = fenceFor(body);
    md += fence + lang + "\n" + body + "\n" + fence + "\n\n";
}

}  // namespace

std::string stripAnsi(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7E)) i++;
            continue;
        }
        if (s[i] == '\x1b') { i++; continue; }
        out += s[i];
    }
    return out;
}

std::string htmlToText(const std::string &html) {
    std::string out;
    size_t i = 0;
    auto tagName = [&](size_t at, size_t end) {
        std::string name;
        size_t k = at + 1;
        if (k < end && html[k] == '/') k++;
        while (k < end && std::isalnum((unsigned char)html[k]))
            name += (char)std::tolower((unsigned char)html[k++]);
        return name;
    };
    bool skipping = false;   // inside <style> or <script>
    while (i < html.size()) {
        if (html[i] == '<') {
            size_t end = html.find('>', i);
            if (end == std::string::npos) break;
            const std::string name = tagName(i, end);
            const bool closing = i + 1 < html.size() && html[i + 1] == '/';
            if (name == "style" || name == "script") skipping = !closing;
            if (name == "br" || name == "p" || name == "div" || name == "tr" || name == "li" ||
                name == "h1" || name == "h2" || name == "h3" || name == "table") {
                if (!out.empty() && out.back() != '\n') out += '\n';
            } else if ((name == "td" || name == "th") && closing) {
                out += '\t';
            }
            i = end + 1;
            continue;
        }
        if (skipping) { i++; continue; }
        if (html[i] == '&') {
            static const std::pair<const char *, const char *> entities[] = {
                {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
                {"&#39;", "'"}, {"&nbsp;", " "}, {"&apos;", "'"}};
            bool done = false;
            for (const auto &e : entities) {
                const size_t n = std::char_traits<char>::length(e.first);
                if (html.compare(i, n, e.first) == 0) {
                    out += e.second;
                    i += n;
                    done = true;
                    break;
                }
            }
            if (done) continue;
        }
        const char c = html[i++];
        if (c == '\n' || c == '\r') {
            if (!out.empty() && out.back() != '\n' && out.back() != ' ') out += ' ';
            continue;
        }
        out += c;
    }
    // Tidy: no space or tab before a line break, no blank lines in a row.
    std::string tidy;
    for (char c : out) {
        if (c == '\n')
            while (!tidy.empty() && (tidy.back() == ' ' || tidy.back() == '\t')) tidy.pop_back();
        if (c == '\n' && tidy.size() >= 2 && tidy.back() == '\n' && tidy[tidy.size() - 2] == '\n')
            continue;
        tidy += c;
    }
    while (!tidy.empty() && (tidy.front() == '\n' || tidy.front() == ' ')) tidy.erase(0, 1);
    return trimTrailingNewlines(tidy);
}

std::string fenceFor(const std::string &text) {
    size_t longest = 0, run = 0;
    for (char c : text) {
        run = c == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return std::string(std::max<size_t>(3, longest + 1), '`');
}

bool parse(const std::string &json, Document &out, std::string *error) {
    Json root;
    std::string why;
    if (!Json::parse(json, root, &why)) {
        if (error) *error = "not valid JSON (" + why + ")";
        return false;
    }
    if (!root.isObject()) {
        if (error) *error = "not a notebook (the file is not a JSON object)";
        return false;
    }
    out = Document();
    out.nbformat = (int)root["nbformat"].asInt(4);
    const Json &meta = root["metadata"];
    out.language = meta["language_info"]["name"].asString(
        meta["kernelspec"]["language"].asString(meta["language"].asString("")));
    const Json *cells = &root["cells"];
    if (!cells->isArray() && root["worksheets"].isArray())   // nbformat 3
        cells = &root["worksheets"][0]["cells"];
    if (!cells->isArray()) {
        if (error) *error = "not a notebook (no cells)";
        return false;
    }
    for (const Json &c : cells->elements()) {
        Cell cell;
        const std::string type = c["cell_type"].asString();
        if (type == "markdown" || type == "heading") {
            cell.kind = Cell::Kind::Markdown;
            cell.source = inlineAttachments(joined(c["source"]), c["attachments"]);
            if (type == "heading")
                cell.source = std::string((size_t)std::max<int64_t>(1, c["level"].asInt(1)), '#') +
                              " " + cell.source;
        } else if (type == "code") {
            cell.kind = Cell::Kind::Code;
            cell.source = joined(c.has("source") ? c["source"] : c["input"]);
            const Json &count = c.has("execution_count") ? c["execution_count"] : c["prompt_number"];
            cell.executionCount = count.isNumber() ? (long)count.asInt() : -1;
            readOutputs(c["outputs"], out.nbformat, cell);
        } else {
            cell.kind = Cell::Kind::Raw;
            cell.source = joined(c["source"]);
        }
        out.cells.push_back(std::move(cell));
    }
    return true;
}

std::string toMarkdown(const Document &doc) {
    std::string md;
    if (doc.cells.empty()) return "*This notebook has no cells.*\n";
    for (const Cell &cell : doc.cells) {
        switch (cell.kind) {
        case Cell::Kind::Markdown:
            md += trimTrailingNewlines(cell.source) + "\n\n";
            break;
        case Cell::Kind::Raw:
            appendBlock(md, cell.source, "");
            break;
        case Cell::Kind::Code:
            appendBlock(md, cell.source, doc.language.empty() ? "python" : doc.language);
            for (const Output &o : cell.outputs) {
                switch (o.kind) {
                case Output::Kind::Image:
                    md += "![output](data:" + o.mime + ";base64," + o.base64 + ")\n\n";
                    break;
                case Output::Kind::Markdown:
                    md += trimTrailingNewlines(o.text) + "\n\n";
                    break;
                default:
                    if (!trimTrailingNewlines(o.text).empty()) appendBlock(md, o.text, "");
                    break;
                }
            }
            break;
        }
    }
    return md;
}

std::string markdownForFile(const std::string &json) {
    Document doc;
    std::string error;
    if (!parse(json, doc, &error))
        return "**This notebook cannot be read:** " + error +
               ".\n\nShift Command V shows its text.\n";
    return toMarkdown(doc);
}

}  // namespace Notebook
