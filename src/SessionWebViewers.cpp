// SessionWebViewers.cpp — what the session's web page renders itself
// rather than the browser (R3's second slice, SessionWeb.h): a Jupyter
// notebook as a page of HTML, and a SQLite database's tables and rows. Both
// are read here, from the folder the server serves, never through the
// window, and confined as the page's other reads are (openLocalFile).
#include "SessionWeb.h"

#include "MarkdownHtml.h"
#include "Notebook.h"
#include "SqliteRead.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <unistd.h>

namespace Shared {
namespace Web {

namespace {

std::string nameOf(const std::string& rel) {
    const std::size_t slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

// Up to `max` bytes from `fd`, which is closed; false past `max`.
bool readAll(int fd, std::size_t max, std::string* out) {
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out->append(buf, (std::size_t)n);
        if (out->size() > max) break;
    }
    ::close(fd);
    return out->size() <= max;
}

}  // namespace

Json notebookJson(const std::string& folder, const std::string& rel, bool hidden) {
    Json j = Json::object();
    j.set("t", Json("notebook"));
    j.set("path", Json(rel));
    auto refuse = [&](const std::string& why) {
        j.set("error", Json(why));
        return j;
    };
    const std::string name = nameOf(rel);
    if (fileType(rel).view != "notebook") return refuse(name + " is not a notebook.");
    std::string abs, why, bytes;
    std::uint64_t size = 0;
    const int fd = openLocalFile(folder, rel, hidden, &abs, &size, &why);
    if (fd < 0) return refuse(why);
    const std::string tooLarge = name + " is over " + std::to_string(kMaxNotebookBytes >> 20) +
                                 " MB, too large to show on this page; it opens in the mutible window.";
    if (size > kMaxNotebookBytes) {
        ::close(fd);
        return refuse(tooLarge);
    }
    if (!readAll(fd, kMaxNotebookBytes, &bytes)) return refuse(tooLarge);
    Notebook::Document doc;
    std::string error;
    if (!Notebook::parse(bytes, doc, &error))
        return refuse(name + " cannot be read as a notebook: " + error + ".");
    // The window's preview renders this same Markdown; MarkdownHtml escapes
    // every text and keeps only data:image, https and relative pictures.
    // The frame it goes in runs no script and fetches nothing outside the
    // page's own CSP (data: pictures only).
    const std::string html =
        MarkdownHtml::page(Notebook::toMarkdown(doc), name);
    j.set("html", Json(SqliteRead::validUtf8(html)));
    j.set("cells", Json((int64_t)doc.cells.size()));
    return j;
}

Json databaseJson(const std::string& folder, const std::string& rel, bool hidden,
                  const std::string& table, std::int64_t offset) {
    Json j = Json::object();
    j.set("t", Json("db"));
    j.set("path", Json(rel));
    auto refuse = [&](const std::string& why) {
        j.set("error", Json(why));
        return j;
    };
    const std::string name = nameOf(rel);
    if (fileType(rel).view != "database") return refuse(name + " is not a database.");
    std::string abs, why;
    std::uint64_t size = 0;
    const int fd = openLocalFile(folder, rel, hidden, &abs, &size, &why);
    if (fd < 0) return refuse(why);
    // A SQLite file says so in its first 16 bytes; anything else named .db
    // is not opened at all.
    std::string head;
    readAll(fd, 100, &head);
    if (!SqliteRead::isDatabaseHeader(head))
        return refuse(size == 0 ? name + " is empty." : name + " is not a SQLite database.");
    // Opened at its real path (confinedPath held it inside the folder), so
    // SQLite has no link to follow.
    char real[PATH_MAX];
    if (!::realpath(abs.c_str(), real)) return refuse(name + " could not be read.");
    SqliteRead::Database db;
    if (!db.open(real, &why)) return refuse(name + " could not be opened: " + why + ".");
    std::vector<SqliteRead::Table> tables;
    std::size_t moreTables = 0;
    db.tables(&tables, &moreTables, nullptr);
    Json list = Json::array();
    for (const SqliteRead::Table& t : tables) {
        Json e = Json::object();
        e.set("name", Json(SqliteRead::validUtf8(t.name)));
        e.set("type", Json(t.type));
        list.push(std::move(e));
    }
    j.set("tables", std::move(list));
    if (moreTables) j.set("moreTables", Json((int64_t)moreTables));
    if (tables.empty()) {
        j.set("table", Json(""));
        return j;
    }
    // The table asked for, by its exact name; else the first.
    std::string shown = tables.front().name;
    if (!table.empty()) {
        bool known = false;
        for (const SqliteRead::Table& t : tables)
            if (SqliteRead::validUtf8(t.name) == table) {
                shown = t.name;
                known = true;
                break;
            }
        if (!known) return refuse(name + " has no table or view by that name.");
    }
    if (offset < 0) offset = 0;
    j.set("table", Json(SqliteRead::validUtf8(shown)));
    j.set("offset", Json((int64_t)offset));
    SqliteRead::Page page;
    if (!db.rows(shown, offset, kDbPageRows, &page, &why, kDbPageBytes))
        return refuse(SqliteRead::validUtf8(shown) + " could not be read: " + why + ".");
    Json columns = Json::array();
    for (const std::string& c : page.columns) columns.push(Json(c));
    Json rows = Json::array();
    for (const auto& row : page.rows) {
        Json r = Json::array();
        for (const SqliteRead::Cell& c : row) {
            switch (c.kind) {
            case SqliteRead::Cell::Kind::Null: r.push(Json()); break;
            case SqliteRead::Cell::Kind::Blob: {
                Json b = Json::object();
                b.set("blob", Json((int64_t)c.bytes));
                r.push(std::move(b));
                break;
            }
            default:
                if ((std::size_t)c.bytes > c.text.size()) {
                    Json t = Json::object();
                    t.set("text", Json(c.text));
                    t.set("bytes", Json((int64_t)c.bytes));
                    r.push(std::move(t));
                } else {
                    r.push(Json(c.text));
                }
            }
        }
        rows.push(std::move(r));
    }
    j.set("columns", std::move(columns));
    j.set("rows", std::move(rows));
    j.set("more", Json(page.more));
    return j;
}

}  // namespace Web
}  // namespace Shared
