// previews_tests.inc — the `sniff:*`, `csv:*` and `jsonoutline:*` groups
// of run_tests.cpp (FileSniff.h, Csv.h, JsonOutline.h): how a file is
// shown, and the cores of the table and outline views. Included there.

static void testFileSniff() {
    using namespace FileSniff;
    GROUP("sniff:binary");
    {
        CHECK(!looksBinary(std::string("plain text\n")));
        CHECK(!looksBinary(std::string()));
        CHECK(looksBinary(std::string("ab\0cd", 5)));
        // Only the first 8 KB count.
        std::string late(kHeadBytes, 'x');
        late += '\0';
        CHECK(!looksBinary(late));
        late[100] = '\0';
        CHECK(looksBinary(late));
        // UTF-8 beyond ASCII is text.
        CHECK(!looksBinary(std::string("caf\xC3\xA9 \xE2\x9C\x93\n")));
        // A PNG's signature is followed by NULs in its first chunk.
        CHECK(looksBinary(std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16)));
    }
    GROUP("sniff:sqlite");
    {
        const std::string header("SQLite format 3\0\x10\0\x01\x01", 20);
        CHECK(isSqlite(header));
        CHECK(!isSqlite(std::string("SQLite format 3")));          // too short
        CHECK(!isSqlite(std::string("SQLite format 2\0xxxx", 20)));
        CHECK(classify("/x/data.bin", header) == Kind::Sqlite);   // by content
        CHECK(classify("/x/notes.db", header) == Kind::Sqlite);
        CHECK(classify("/x/empty.sqlite", std::string()) == Kind::Sqlite);
        CHECK(classify("/x/empty.db", std::string()) == Kind::Text);
    }
    GROUP("sniff:classify");
    {
        CHECK(extensionOf("/a/b/Report.DOCX") == "docx");
        CHECK(extensionOf("/a/b.d/Makefile") == "");
        CHECK(extensionOf("/a/.env") == "");
        CHECK(extensionOf("x.tar.gz") == "gz");
        CHECK(classify("/a/r.docx", std::string("PK\x03\x04", 4)) == Kind::QuickLook);
        CHECK(classify("/a/r.pages", std::string()) == Kind::QuickLook);
        CHECK(classify("/a/data.csv", std::string("a,b\n1,2\n")) == Kind::Csv);
        CHECK(classify("/a/data.TSV", std::string("a\tb\n")) == Kind::Tsv);
        CHECK(classify("/a/x.json", std::string("{}")) == Kind::Json);
        CHECK(classify("/a/n.ipynb", std::string("{\"cells\": []}")) == Kind::Notebook);
        CHECK(classify("/a/main.c", std::string("int main;\n")) == Kind::Text);
        // Binary by content whatever the name says: never opened as text.
        CHECK(classify("/a/main.c", std::string("\x7f" "ELF\x02\x01\x01\0", 8)) == Kind::QuickLook);
        CHECK(classify("/a/data.csv", std::string("a,b\0", 4)) == Kind::QuickLook);
        CHECK(classify("/a/noext", std::string("\xCA\xFE\xBA\xBE\0\0", 6)) == Kind::QuickLook);
        CHECK(std::string(kindName(Kind::QuickLook)) == "quicklook");
    }
}

namespace {

// Every cell of a document, row by row, as "a|b|c" lines.
std::string cellsOf(const Csv::Document &d) {
    std::string out;
    for (size_t r = 0; r < d.rows(); r++) {
        for (size_t c = 0; c < d.fieldsInRow(r); c++) out += (c ? "|" : "") + d.cell(r, c);
        out += "\n";
    }
    return out;
}

// The lines that differ between two texts (split at \n), counted.
size_t changedLines(const std::string &a, const std::string &b) {
    auto split = [](const std::string &s) {
        std::vector<std::string> v;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); i++)
            if (i == s.size() || s[i] == '\n') {
                v.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        return v;
    };
    std::vector<std::string> x = split(a), y = split(b);
    size_t n = 0;
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++)
        if (i >= x.size() || i >= y.size() || x[i] != y[i]) n++;
    return n;
}

}  // namespace

static void testCsv() {
    using Csv::Document;
    GROUP("csv:parse");
    {
        Document d = Document::parse("name,age\nAda,36\n\"Lovelace, A\",\"say \"\"hi\"\"\"\n", ',');
        CHECK(d.rows() == 3);
        CHECK(d.columns() == 2);
        CHECK(d.cell(0, 0) == "name");
        CHECK(d.cell(2, 0) == "Lovelace, A");
        CHECK(d.cell(2, 1) == "say \"hi\"");
        CHECK(d.quoted(2, 0) && !d.quoted(1, 0));
        CHECK(d.cell(5, 5) == "");
        CHECK(!d.truncated());
        // A quoted field holding a line break is one field of one row.
        Document m = Document::parse("id,note\r\n1,\"two\r\nlines\"\r\n2,x\r\n", ',');
        CHECK(m.rows() == 3);
        CHECK(m.cell(1, 1) == "two\r\nlines");
        CHECK(m.lineEnding() == "\r\n");
        // Lone CRs end rows too; empty fields and a trailing delimiter count.
        Document cr = Document::parse("a,,c,\rb", ',');
        CHECK(cr.rows() == 2);
        CHECK(cr.fieldsInRow(0) == 4);
        CHECK(cr.cell(0, 1) == "" && cr.cell(0, 3) == "");
        CHECK(cr.lineEnding() == "\r");
        // Ragged rows: the table is as wide as the widest.
        Document rg = Document::parse("a\nb,c,d\ne,f\n", ',');
        CHECK(rg.columns() == 3);
        CHECK(cellsOf(rg) == "a\nb|c|d\ne|f\n");
        // An unterminated quote runs to the end, and is kept as it was.
        Document un = Document::parse("x,\"open\nmore", ',');
        CHECK(un.rows() == 1);
        CHECK(un.cell(0, 1) == "open\nmore");
        CHECK(un.serialize() == "x,\"open\nmore");
        // Text after a closing quote stays part of the field.
        Document junk = Document::parse("\"ab\"cd,e\n", ',');
        CHECK(junk.cell(0, 0) == "abcd");
        // Tabs.
        Document t = Document::parse("a\tb, c\n1\t2\n", '\t');
        CHECK(t.cell(0, 1) == "b, c");
        CHECK(Document::parse("", ',').rows() == 0);
    }
    GROUP("csv:round-trip");
    {
        const char *cases[] = {
            "a,b,c\n1,2,3\n",
            "a,b,c\n1,2,3",                                   // no newline at the end
            "a,b\r\n\"x, y\",\"multi\r\nline\"\r\n",            // CRLF, quoted
            "a,b\n\n\nc,d\n\n",                               // blank lines
            "\xEF\xBB\xBFh1,h2\n\"q\"\"uote\",z\n",             // BOM, doubled quote
            "  spaced , fields ,\n\"a\" ,b\n",                 // spaces, junk after quotes
            "only\n",
            "\n",
            ",,,\n,,,",
            "x;y;z\r1;2;3\r",                                  // lone CRs
            "a,\"unterminated\n",
            "mixed\nendings\r\nhere\rok",
        };
        for (const char *c : cases) {
            const std::string text = c;
            CHECK(Document::parse(text, ',').serialize() == text);
            CHECK(Document::parse(text, ';').serialize() == text);
            CHECK(Document::parse(text, '\t').serialize() == text);
        }
        Document bom = Document::parse("\xEF\xBB\xBFh1,h2\n", ',');
        CHECK(bom.cell(0, 0) == "h1");   // the mark is not shown
    }
    GROUP("csv:edit");
    {
        // The acceptance check: quoted fields with commas and line breaks;
        // one cell edit changes one line.
        const std::string text =
            "id,name,notes\n"
            "1,\"Smith, J\",\"line one\nline two\"\n"
            "2,Doe,plain\n"
            "3,\"Roe, R\",\"a, b, c\"\n";
        Document d = Document::parse(text, ',');
        CHECK(d.rows() == 4);
        CHECK(d.setCell(2, 2, "edited"));
        CHECK(!d.setCell(2, 2, "edited"));   // no change
        const std::string out = d.serialize();
        CHECK(out == "id,name,notes\n1,\"Smith, J\",\"line one\nline two\"\n2,Doe,edited\n"
                     "3,\"Roe, R\",\"a, b, c\"\n");
        CHECK(changedLines(text, out) == 1);
        // A quoted field stays quoted, others in the row are untouched.
        Document q = Document::parse("\"a\", b ,\"c\"\n", ',');
        q.setCell(0, 0, "plain");
        CHECK(q.serialize() == "\"plain\", b ,\"c\"\n");
        // A new value that needs quotes gets them, quotes inside doubled.
        Document n = Document::parse("a,b\n", ',');
        n.setCell(0, 1, "x,\"y\"\nz");
        CHECK(n.serialize() == "a,\"x,\"\"y\"\"\nz\"\n");
        CHECK(Document::parse(n.serialize(), ',').cell(0, 1) == "x,\"y\"\nz");
        // Past a row's end, the fields in between are added.
        Document p = Document::parse("a\nb,c,d\n", ',');
        p.setCell(0, 2, "z");
        CHECK(p.serialize() == "a,,z\nb,c,d\n");
        // CRLF rows keep CRLF.
        Document w = Document::parse("a,b\r\nc,d\r\n", ',');
        w.setCell(1, 0, "C");
        CHECK(w.serialize() == "a,b\r\nC,d\r\n");
        // Tab-separated: a tab in a value is quoted.
        Document t = Document::parse("a\tb\n", '\t');
        t.setCell(0, 0, "x\ty");
        CHECK(t.serialize() == "\"x\ty\"\tb\n");
        // Encoding.
        CHECK(Document::encode("plain", ',', false) == "plain");
        CHECK(Document::encode("plain", ',', true) == "\"plain\"");
        CHECK(Document::encode("a;b", ';', false) == "\"a;b\"");
        CHECK(Document::encode("a;b", ',', false) == "a;b");
    }
    GROUP("csv:rows-columns");
    {
        Document d = Document::parse("a,b\nc,d\n", ',');
        CHECK(d.insertRow(1));
        CHECK(d.serialize() == "a,b\n,\nc,d\n");
        d.setCell(1, 0, "x");
        CHECK(d.serialize() == "a,b\nx,\nc,d\n");
        CHECK(d.deleteRow(0));
        CHECK(d.serialize() == "x,\nc,d\n");
        CHECK(!d.deleteRow(9));
        CHECK(d.insertColumn(1));
        CHECK(d.serialize() == "x,,\nc,,d\n");
        CHECK(d.deleteColumn(0));
        CHECK(d.serialize() == ",\n,d\n");
        CHECK(!d.deleteColumn(5));
        // Appending to a file with no final newline keeps it without one.
        Document e = Document::parse("a,b\nc,d", ',');
        e.insertRow(2);
        CHECK(e.serialize() == "a,b\nc,d\n,");
        e.deleteRow(2);
        CHECK(e.serialize() == "a,b\nc,d");
        // New rows take the file's line ending (and in one column are "",
        // not a blank line).
        Document crlf = Document::parse("a\r\nb\r\n", ',');
        crlf.insertRow(1);
        CHECK(crlf.serialize() == "a\r\n\"\"\r\nb\r\n");
        // Quote-everything files get quoted new fields.
        Document all = Document::parse("\"a\",\"b\"\n\"c\",\"d\"\n", ',');
        all.insertColumn(2);
        CHECK(all.serialize() == "\"a\",\"b\",\"\"\n\"c\",\"d\",\"\"\n");
        // Blank lines stay blank when a column comes or goes.
        Document bl = Document::parse("a,b\n\nc,d\n", ',');
        bl.insertColumn(0);
        CHECK(bl.serialize() == ",a,b\n\n,c,d\n");
        bl.deleteColumn(0);
        CHECK(bl.serialize() == "a,b\n\nc,d\n");
        // Edits that are taken back by hand leave the text as it was.
        Document back = Document::parse("x,\"y\"\n", ',');
        back.setCell(0, 1, "z");
        back.setCell(0, 1, "y");
        CHECK(back.serialize() == "x,\"y\"\n");
    }
    GROUP("csv:empty-row");
    {
        // A one-column file with a cell emptied: "" keeps the row a row,
        // where a blank line is skipped by most readers and, after a lone
        // CR, would join it into a CRLF.
        Document ids = Document::parse("id\n17\n18\n", ',');
        CHECK(ids.setCell(1, 0, ""));
        CHECK(ids.serialize() == "id\n\"\"\n18\n");
        Document back = Document::parse(ids.serialize(), ',');
        CHECK(back.rows() == 3 && back.cell(1, 0) == "" && back.cell(2, 0) == "18");
        Document cr = Document::parse("a\rb\rc", ',');
        cr.setCell(2, 0, "");
        CHECK(cr.serialize() == "a\rb\r\"\"");
        CHECK(Document::parse(cr.serialize(), ',').rows() == 3);
        // A column deleted down to one empty field is "" too.
        Document two = Document::parse("x,y\n,1\n", ',');
        two.deleteColumn(1);
        CHECK(two.serialize() == "x\n\"\"\n");
        // Rows nobody touched stay as they were, blank lines included.
        Document blank = Document::parse("a\n\nb\n", ',');
        blank.setCell(2, 0, "c");
        CHECK(blank.serialize() == "a\n\nc\n");
    }
    GROUP("csv:limits");
    {
        std::string big;
        for (int i = 0; i < 1000; i++) big += std::to_string(i) + ",row\n";
        Document d = Document::parse(big, ',', 100);
        CHECK(d.rows() == 100);
        CHECK(d.truncated());
        CHECK(d.cell(99, 0) == "99");
        // Only the start of a file: a row cut short at the end is dropped.
        Document cut = Document::parse("a,b\nc,d\ne,\"f", ',', SIZE_MAX, false);
        CHECK(cut.rows() == 2);
        CHECK(cut.truncated());
        Document whole = Document::parse("a,b\nc,d\n", ',', SIZE_MAX, false);
        CHECK(whole.rows() == 2);
        CHECK(!whole.truncated());
    }
    GROUP("csv:delimiter");
    {
        CHECK(Csv::detectDelimiter("a,b,c\n1,2,3\n") == ',');
        CHECK(Csv::detectDelimiter("a;b;c\n1;2;3\n") == ';');
        CHECK(Csv::detectDelimiter("a\tb\n1\t2\n") == '\t');
        CHECK(Csv::detectDelimiter("a|b|c\n1|2|3\n") == '|');
        // Commas inside quotes do not count.
        CHECK(Csv::detectDelimiter("\"a,b,c\";d\n\"1,2\";3\n") == ';');
        // European decimals: semicolons split consistently.
        CHECK(Csv::detectDelimiter("x;y\n1,5;2,25\n3,0;4\n") == ';');
        CHECK(Csv::detectDelimiter("single\ncolumn\n") == ',');
        // One column whose first line happens to hold a ';' or '|': the
        // lines below do not split that way, so it is not the separator.
        CHECK(Csv::detectDelimiter("note; first\nplain\nmore\nrows\n") == ',');
        CHECK(Csv::detectDelimiter("a|b\nc\nd\n") == ',');
        // A ragged line or two still leaves the separator found.
        CHECK(Csv::detectDelimiter("a;b;c\n1;2;3\n4;5\n6;7;8\n") == ';');
        CHECK(Csv::detectDelimiter("") == ',');
    }
    GROUP("csv:random-edits");
    {
        // Random edits against a plain model: the text written back always
        // reads back as the model, and rows nobody edited keep their bytes.
        std::mt19937 rng(7);
        const char *pieces[] = {"a", "b c", "x,y", "q\"t", "l1\nl2", "", " s ", "\r\n"};
        for (int round = 0; round < 300; round++) {
            std::vector<std::vector<std::string>> model;
            const int rows = 1 + (int)(rng() % 6), cols = 1 + (int)(rng() % 4);
            for (int r = 0; r < rows; r++) {
                model.emplace_back();
                for (int c = 0; c < cols; c++) model.back().push_back(pieces[rng() % 8]);
            }
            std::string text;
            for (auto &row : model) {
                for (size_t c = 0; c < row.size(); c++)
                    text += (c ? "," : "") + Document::encode(row[c], ',', rng() % 3 == 0);
                text += (rng() % 2) ? "\r\n" : "\n";
            }
            Document d = Document::parse(text, ',');
            CHECK(d.serialize() == text);
            for (int e = 0; e < 4; e++) {
                const size_t r = rng() % model.size(), c = rng() % model[r].size();
                const std::string v = pieces[rng() % 8];
                d.setCell(r, c, v);
                model[r][c] = v;
            }
            Document back = Document::parse(d.serialize(), ',');
            bool same = back.rows() == model.size();
            for (size_t r = 0; same && r < model.size(); r++)
                for (size_t c = 0; c < model[r].size(); c++)
                    same = same && back.cell(r, c) == model[r][c];
            CHECK(same);
        }
    }
}

static void testJsonOutline() {
    using namespace JsonOutline;
    GROUP("jsonoutline:tree");
    {
        const std::string text = "{\n  \"name\": \"minicode\",\n  \"tags\": [1, true, null],\n"
                                 "  \"nested\": {\"k\": -2.5e3}\n}\n";
        JsonOutline::Outline o = parse(text);
        CHECK(o.ok);
        CHECK(o.nodes.size() == 8);
        const Node &root = o.nodes[0];
        CHECK(root.kind == Kind::Object);
        CHECK(root.children.size() == 3);
        CHECK(label(root) == "root");
        CHECK(summary(root) == "{3}");
        const Node &name = o.nodes[root.children[0]];
        CHECK(name.key == "name" && name.isMember);
        CHECK(summary(name) == "\"minicode\"");
        CHECK(text.substr(name.start, name.end - name.start) == "\"minicode\"");
        CHECK(text.substr(name.memberStart, name.end - name.memberStart) == "\"name\": \"minicode\"");
        const Node &tags = o.nodes[root.children[1]];
        CHECK(tags.kind == Kind::Array && tags.children.size() == 3);
        CHECK(summary(tags) == "[3]");
        CHECK(label(o.nodes[tags.children[2]]) == "[2]");
        CHECK(summary(o.nodes[tags.children[1]]) == "true");
        const Node &nested = o.nodes[root.children[2]];
        CHECK(summary(o.nodes[nested.children[0]]) == "-2.5e3");
        CHECK(root.start == 0 && root.end == text.size() - 1);
        // ASCII: UTF-16 offsets equal the byte offsets.
        CHECK(name.start16 == name.start && name.end16 == name.end);
    }
    GROUP("jsonoutline:utf16");
    {
        // é is 2 bytes and 1 unit; 😀 is 4 bytes and 2 units.
        const std::string text = "{\"\xC3\xA9\": \"\xF0\x9F\x98\x80\", \"b\": 1}";
        JsonOutline::Outline o = parse(text);
        CHECK(o.ok);
        const Node &e = o.nodes[o.nodes[0].children[0]];
        CHECK(e.key == "\xC3\xA9");
        CHECK(e.memberStart == 1 && e.memberStart16 == 1);
        CHECK(e.start == 7 && e.start16 == 6);
        CHECK(e.end == 13 && e.end16 == 10);
        const Node &b = o.nodes[o.nodes[0].children[1]];
        CHECK(b.memberStart == 15 && b.memberStart16 == 12);
        CHECK(nodeAt16(o, 0) == 0);
        CHECK(nodeAt16(o, 7) == (long)o.nodes[0].children[0]);
        CHECK(nodeAt16(o, 12) == (long)o.nodes[0].children[1]);
        CHECK(nodeAt16(o, 99) == -1);
    }
    GROUP("jsonoutline:strings");
    {
        JsonOutline::Outline o = parse("[\"a\\n\\u00e9\\ud83d\\ude00\\\"\"]");
        CHECK(o.ok);
        CHECK(o.nodes[1].preview == "a\n\xC3\xA9\xF0\x9F\x98\x80\"");
        // Long strings are cut for the outline, never mid-character.
        std::string longText = "[\"";
        for (int i = 0; i < 400; i++) longText += "\xC3\xA9";
        longText += "\"]";
        JsonOutline::Outline l = parse(longText);
        CHECK(l.ok);
        const std::string &p = l.nodes[1].preview;
        CHECK(p.size() < 600);
        CHECK(p.size() >= 3 && p.substr(p.size() - 3) == "\xE2\x80\xA6");
        CHECK((p.size() - 3) % 2 == 0);
    }
    GROUP("jsonoutline:errors");
    {
        JsonOutline::Outline o = parse("{\n  \"a\": 1,\n  \"b\" 2\n}");
        CHECK(!o.ok);
        CHECK(o.error == "Expected ':'");
        CHECK(o.errorLine == 3 && o.errorColumn == 7);
        // What was read before the error is kept.
        CHECK(o.nodes.size() >= 2);
        CHECK(o.nodes[o.nodes[0].children[0]].key == "a");
        CHECK(!parse("[1,]").ok);
        CHECK(!parse("01").ok);
        CHECK(!parse("{} x").ok);
        CHECK(!parse("").ok);
        CHECK(!parse("\"tab\there\"").ok);
        CHECK(parse("  [ ]  ").ok);
        std::string deep(600, '[');
        deep += std::string(600, ']');
        CHECK(!parse(deep).ok);
        JsonOutline::Outline cap = parse("[1,2,3,4,5]", 3);
        CHECK(!cap.ok && cap.truncated);
    }
    GROUP("jsonoutline:jsonc");
    {
        const std::string text = "// tsconfig\n{\n  /* the target */ \"target\": \"es2022\", // why\n"
                                 "  \"lib\": [\"dom\", \"es2022\",],\n}\n";
        CHECK(!parse(text).ok);                      // strict JSON refuses it
        JsonOutline::Outline o = parse(text, 2000000, true);
        CHECK(o.ok);
        CHECK(o.nodes.size() == 5);
        const Node &root = o.nodes[0];
        CHECK(root.children.size() == 2);
        CHECK(o.nodes[root.children[0]].key == "target");
        CHECK(o.nodes[root.children[1]].children.size() == 2);
        // The node's range is the value's, comments around it left out.
        const Node &target = o.nodes[root.children[0]];
        CHECK(text.substr(target.memberStart, target.end - target.memberStart) ==
              "\"target\": \"es2022\"");
        CHECK(text.substr(root.start, 1) == "{");
        // A comment's slashes inside a string are the string's.
        JsonOutline::Outline url = parse("{\"u\": \"http://x/*y*/\"}", 2000000, true);
        CHECK(url.ok && url.nodes.size() == 2 && url.nodes[1].preview == "http://x/*y*/");
        // Still refused: an unclosed comment, a lone comma, two commas.
        CHECK(!parse("{\"a\": 1 /* open", 2000000, true).ok);
        CHECK(parse("{\"a\": 1 /* open", 2000000, true).error == "Unterminated comment");
        CHECK(!parse("[,]", 2000000, true).ok);
        CHECK(!parse("{,}", 2000000, true).ok);
        CHECK(!parse("[1,,]", 2000000, true).ok);
        CHECK(parse("[] // end", 2000000, true).ok);
        // Which files: by name and folder.
        using JsonOutline::allowsComments;
        CHECK(allowsComments("/p/a.jsonc"));
        CHECK(allowsComments("/p/tsconfig.json"));
        CHECK(allowsComments("/p/tsconfig.base.json"));
        CHECK(allowsComments("/p/jsconfig.json"));
        CHECK(allowsComments("/p/.vscode/settings.json"));
        CHECK(allowsComments("/p/.devcontainer/devcontainer.json"));
        CHECK(allowsComments("/p/x.code-workspace"));
        CHECK(!allowsComments("/p/package.json"));
        CHECK(!allowsComments("/p/.vscode/notes.txt"));
        CHECK(!allowsComments("/p/vscode/settings.json"));
    }
}

static void testNotebook() {
    GROUP("notebook:parse");
    {
        const std::string nb = R"nb({
 "cells": [
  {"cell_type": "markdown", "metadata": {}, "source": ["# Title\n", "Some *text*."]},
  {"cell_type": "code", "execution_count": 3, "metadata": {}, "source": "print('hi')\nx = 1",
   "outputs": [
     {"output_type": "stream", "name": "stdout", "text": ["hi\n"]},
     {"output_type": "execute_result", "execution_count": 3,
      "data": {"text/plain": ["1"], "text/html": ["<b>1</b>"]}, "metadata": {}},
     {"output_type": "display_data", "metadata": {},
      "data": {"image/png": "iVBORw0K\nGgo=\n", "text/plain": ["<Figure>"]}},
     {"output_type": "error", "ename": "ValueError", "evalue": "bad",
      "traceback": ["\u001b[0;31mValueError\u001b[0m: bad"]}
   ]},
  {"cell_type": "raw", "metadata": {}, "source": "raw text"},
  {"cell_type": "code", "execution_count": null, "metadata": {}, "source": [], "outputs": []}
 ],
 "metadata": {"kernelspec": {"language": "python", "name": "python3"},
              "language_info": {"name": "python"}},
 "nbformat": 4, "nbformat_minor": 5
})nb";
        Notebook::Document d;
        std::string err;
        CHECK(Notebook::parse(nb, d, &err));
        CHECK(d.language == "python");
        CHECK(d.cells.size() == 4);
        CHECK(d.cells[0].kind == Notebook::Cell::Kind::Markdown);
        CHECK(d.cells[0].source == "# Title\nSome *text*.");
        CHECK(d.cells[1].executionCount == 3);
        CHECK(d.cells[1].outputs.size() == 4);
        CHECK(d.cells[1].outputs[0].kind == Notebook::Output::Kind::Stream);
        CHECK(d.cells[1].outputs[0].text == "hi\n");
        CHECK(d.cells[1].outputs[1].kind == Notebook::Output::Kind::Text);
        CHECK(d.cells[1].outputs[1].text == "1");   // plain text before HTML
        CHECK(d.cells[1].outputs[2].kind == Notebook::Output::Kind::Image);
        CHECK(d.cells[1].outputs[2].base64 == "iVBORw0KGgo=");
        CHECK(d.cells[1].outputs[3].text == "ValueError: bad\n");   // colors stripped
        CHECK(d.cells[2].kind == Notebook::Cell::Kind::Raw);
        CHECK(d.cells[3].executionCount == -1 && d.cells[3].source.empty());

        const std::string md = Notebook::toMarkdown(d);
        CHECK(md.find("# Title\nSome *text*.\n\n") == 0);
        CHECK(md.find("```python\nprint('hi')\nx = 1\n```\n") != std::string::npos);
        CHECK(md.find("```\nhi\n```\n") != std::string::npos);
        CHECK(md.find("![output](data:image/png;base64,iVBORw0KGgo=)") != std::string::npos);
        CHECK(md.find("```\nValueError: bad\n```") != std::string::npos);
        // The preview reads it as intended: the code is a python block.
        bool sawPython = false, sawImage = false;
        for (const MdRun &r : MarkdownParser::parse(md)) {
            if (r.codeBlock && r.lang == "python" && r.text == "print('hi')\n") sawPython = true;
            if (r.image && r.src.rfind("data:image/png;base64,", 0) == 0) sawImage = true;
        }
        CHECK(sawPython);
        CHECK(sawImage);
    }
    GROUP("notebook:edges");
    {
        Notebook::Document d;
        std::string err;
        CHECK(!Notebook::parse("[1, 2]", d, &err));
        CHECK(err.find("not a notebook") == 0);
        CHECK(!Notebook::parse("{", d, &err));
        CHECK(err.find("not valid JSON") == 0);
        CHECK(Notebook::markdownForFile("{").find("cannot be read") != std::string::npos);
        CHECK(Notebook::markdownForFile("{\"cells\": [], \"nbformat\": 4}") ==
              "*This notebook has no cells.*\n");
        // Backticks in a cell lengthen its fence.
        CHECK(Notebook::fenceFor("a ``` b") == "````");
        CHECK(Notebook::fenceFor("plain") == "```");
        // An attached picture is inlined.
        const std::string att = R"nb({"cells": [{"cell_type": "markdown",
            "source": "![x](attachment:p.png)",
            "attachments": {"p.png": {"image/png": "AAAA"}}}], "nbformat": 4})nb";
        CHECK(Notebook::markdownForFile(att).find("![x](data:image/png;base64,AAAA)") == 0);
        // HTML with no plain text is read as text; a table's cells by tabs.
        CHECK(Notebook::htmlToText("<table><tr><th>a</th><th>b</th></tr>"
                                   "<tr><td>1 &amp; 2</td><td>&lt;3</td></tr></table>") ==
              "a\tb\n1 & 2\t<3");
        CHECK(Notebook::htmlToText("<style>p{}</style><p>Hi<br>there</p>") == "Hi\nthere");
        CHECK(Notebook::stripAnsi("\x1b[1;32mok\x1b[0m done") == "ok done");
        // nbformat 3: worksheets, input, prompt_number, pyout.
        const std::string v3 = R"nb({"nbformat": 3, "metadata": {"language": "python"},
            "worksheets": [{"cells": [
              {"cell_type": "heading", "level": 2, "source": "Old"},
              {"cell_type": "code", "input": "1+1", "prompt_number": 1, "language": "python",
               "outputs": [{"output_type": "pyout", "text": ["2"]},
                           {"output_type": "stream", "stream": "stdout", "text": "x"}]}]}]})nb";
        CHECK(Notebook::parse(v3, d, &err));
        CHECK(d.cells.size() == 2);
        CHECK(d.cells[0].source == "## Old");
        CHECK(d.cells[1].source == "1+1" && d.cells[1].executionCount == 1);
        CHECK(d.cells[1].outputs.size() == 2 && d.cells[1].outputs[0].text == "2");
    }
    GROUP("notebook:svg-and-long-output");
    {
        // An SVG plot (stored as text) becomes a base64 data: picture.
        const std::string svgNb = R"nb({"nbformat": 4, "cells": [{"cell_type": "code",
            "source": "plot()", "outputs": [{"output_type": "display_data", "metadata": {},
            "data": {"image/svg+xml": ["<svg xmlns=\"http://www.w3.org/2000/svg\">", "</svg>\n"],
                     "text/plain": ["<Figure>"]}}]}]})nb";
        Notebook::Document d;
        CHECK(Notebook::parse(svgNb, d));
        CHECK(d.cells.size() == 1 && d.cells[0].outputs.size() == 1);
        const Notebook::Output &o = d.cells[0].outputs[0];
        CHECK(o.kind == Notebook::Output::Kind::Image && o.mime == "image/svg+xml");
        // base64 of '<svg xmlns="http://www.w3.org/2000/svg"></svg>\n'
        CHECK(o.base64 == "PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciPjwvc3ZnPgo=");
        CHECK(Notebook::toMarkdown(d).find("![output](data:image/svg+xml;base64,PHN2Zy") !=
              std::string::npos);
        // A long output keeps its first and last lines.
        std::string log;
        for (int i = 0; i < 20000; i++) log += "epoch " + std::to_string(i) + "\n";
        const std::string cut = Notebook::shortenOutput(log);
        CHECK(cut.size() < log.size() / 5);
        CHECK(cut.rfind("epoch 0\nepoch 1\n", 0) == 0);
        CHECK(cut.find("epoch 999\n") != std::string::npos);
        CHECK(cut.find("epoch 1000\n") == std::string::npos);
        CHECK(cut.find("18000 lines of output left out here") != std::string::npos);
        CHECK(cut.find("epoch 19000\n") != std::string::npos);
        CHECK(cut.size() >= 12 && cut.substr(cut.size() - 12) == "epoch 19999\n");
        CHECK(Notebook::shortenOutput("short\ntext\n") == "short\ntext\n");
        // One enormous line is cut by bytes, never inside a character.
        std::string wide;
        for (int i = 0; i < 300000; i++) wide += "\xC3\xA9";
        const std::string w = Notebook::shortenOutput(wide);
        CHECK(w.size() < wide.size() / 2);
        bool whole = true;
        for (size_t i = 0; i + 1 < w.size(); i++)
            if ((unsigned char)w[i] == 0xC3 && (unsigned char)w[i + 1] != 0xA9) whole = false;
        CHECK(whole);
        CHECK(w.find("Part of a long line of output left out here") != std::string::npos);
    }
    GROUP("md:fence-lang");
    {
        std::vector<MdRun> runs = MarkdownParser::parse("```Python {.x}\na = 1\n```\n\n```\nb\n```\n");
        std::string langs;
        for (const MdRun &r : runs)
            if (r.codeBlock) langs += "[" + r.lang + "]";
        CHECK(langs == "[python][]");
        CHECK(MarkdownParser::extensionForLanguage("Python") == "py");
        CHECK(MarkdownParser::extensionForLanguage("c++") == "cpp");
        CHECK(MarkdownParser::extensionForLanguage("bash") == "sh");
        CHECK(MarkdownParser::extensionForLanguage("klingon") == "");
        for (const char *l : {"python", "js", "typescript", "cpp", "go", "rust", "bash", "json", "yaml"})
            CHECK(SyntaxHighlighter::supports(MarkdownParser::extensionForLanguage(l)));
    }
}
