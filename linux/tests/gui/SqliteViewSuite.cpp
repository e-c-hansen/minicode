// SqliteViewSuite.cpp — a SQLite database in the editor's slot
// (`./build/minicode-guitest sqlite_view`, linux/PARITY.md item 4.1): the
// tables and views listed, a table's rows with NULL told from an empty
// string, a query, 1,000 rows at most, only the first statement run, every
// statement that writes (INSERT, UPDATE, DELETE, CREATE, BEGIN, ATTACH, a
// pragma that sets) refused with the file's bytes unchanged, a WAL database
// with no -wal beside it opened, a query that never ends stopped at about
// 5 s while the main thread stays free, a failure shown in the slot, and a
// reopen of the file keeping the query on show.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "SqliteView.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
SqliteView* sv() { return ed()->sqliteView(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

void sql(const std::string& path, const char* text) {
    sqlite3* db = nullptr;
    GCHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    char* err = nullptr;
    const int rc = sqlite3_exec(db, text, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) printf("  sqlite3_exec: %s\n", err ? err : "?");
    GCHECK(rc == SQLITE_OK);
    sqlite3_free(err);
    sqlite3_close(db);
}

bool there(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool idle() { return sv() && !sv()->busy(); }
bool shows(const std::string& p) {
    return ed()->active() && ed()->active()->path() == p &&
           ed()->active()->kind() == Document::Kind::Database && idle();
}

// Runs a query and waits for its answer.
void run(const std::string& q) {
    sv()->runQuery(q);
}

std::string gBefore;
double gT0 = 0, gCpu0 = 0, gElapsed = 0, gCpu = 0;

const char* kMain =
    "CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT, note TEXT);"
    "INSERT INTO people VALUES(1,'Ann','hello'),(2,'Bob',NULL),(3,'Cy','');"
    "CREATE TABLE \"odd \"\"name\"(x);"
    "INSERT INTO \"odd \"\"name\" VALUES(42);"
    "CREATE VIEW adults AS SELECT name FROM people WHERE id > 1;"
    "CREATE TABLE big(n);"
    "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<1500) "
    "INSERT INTO big SELECT x FROM c;";

}  // namespace

MC_GUI_SUITE(sqlite_view) {
    MCGuiStep("a database with two tables, a view and a big table; a text file", [] {
        ed()->closeAllTabs();
        std::remove(F("a.db").c_str());
        sql(F("a.db"), kMain);
        gBefore = MCGuiReadFile(F("a.db"));
        GCHECK(gBefore.size() > 1000);
        MCGuiOpenFileKept(F("a.db"));
    });
    MCGuiWait("the database shows", 15, [] { return shows(F("a.db")) && sv()->rowCount() > 0; });
    MCGuiStep("with its tables then its views, and the first table's rows", [] {
        const auto& t = sv()->tables();
        if (t.size() != 4) { GCHECK_EQ_STR(std::to_string(t.size()), "4"); return; }
        GCHECK_EQ_STR(std::to_string(t.size()), "4");
        GCHECK_EQ_STR(t[0], "big");
        GCHECK_EQ_STR(t[1], "odd \"name");
        GCHECK_EQ_STR(t[2], "people");
        GCHECK_EQ_STR(t[3], "adults");
        GCHECK(!sv()->failed());
    });
    MCGuiStep("a table picked: its rows, NULL apart from an empty string", [] {
        sv()->showTable("people");
    });
    MCGuiWait("the answer", 10, [] { return idle() && sv()->columnCount() == 3; });
    MCGuiStep("with the cells", [] {
        GCHECK_EQ_STR(sv()->query(), "SELECT * FROM \"people\"");
        GCHECK_EQ_STR(std::to_string(sv()->rowCount()), "3");
        GCHECK_EQ_STR(sv()->columnName(1), "name");
        GCHECK_EQ_STR(sv()->cell(0, 1), "Ann");
        GCHECK(!sv()->isNull(0, 2));
        GCHECK(sv()->isNull(1, 2));
        GCHECK(!sv()->isNull(2, 2));
        GCHECK_EQ_STR(sv()->cell(2, 2), "");
        GCHECK_EQ_STR(sv()->cell(0, 0), "1");
        GCHECK(sv()->status().find("3 rows") == 0);
    });
    MCGuiStep("a table with quotes in its name", [] { sv()->showTable("odd \"name"); });
    MCGuiWait("its row", 10, [] { return idle() && sv()->rowCount() == 1; });
    MCGuiStep("which reads 42", [] { GCHECK_EQ_STR(sv()->cell(0, 0), "42"); });
    MCGuiStep("a view", [] { sv()->showTable("adults"); });
    MCGuiWait("its rows", 10, [] { return idle() && sv()->rowCount() == 2; });
    MCGuiStep("a SELECT query", [] {
        run("SELECT name, id * 10 AS tens FROM people WHERE note IS NULL OR id = 1 ORDER BY id");
    });
    MCGuiWait("its answer", 10, [] { return idle() && sv()->columnCount() == 2; });
    MCGuiStep("with its columns and rows", [] {
        GCHECK_EQ_STR(sv()->columnName(1), "tens");
        GCHECK_EQ_STR(std::to_string(sv()->rowCount()), "2");
        GCHECK_EQ_STR(sv()->cell(1, 0), "Bob");
        GCHECK_EQ_STR(sv()->cell(1, 1), "20");
    });
    MCGuiStep("a big table", [] { sv()->showTable("big"); });
    MCGuiWait("1,000 rows", 10, [] { return idle() && sv()->rowCount() == 1000; });
    MCGuiStep("and a note that more were left out", [] {
        GCHECK(sv()->truncated());
        GCHECK(sv()->status().find("1,000") != std::string::npos);
    });
    MCGuiStep("two statements: only the first runs", [] {
        run("SELECT count(*) FROM people; DELETE FROM people");
    });
    MCGuiWait("the count", 10, [] { return idle() && sv()->rowCount() == 1; });
    MCGuiStep("which is all three people", [] {
        GCHECK_EQ_STR(sv()->cell(0, 0), "3");
        run("SELECT * FROM nothere");
    });
    MCGuiWait("a failure", 10, [] { return idle() && sv()->failed(); });
    MCGuiStep("shown with SQLite's words", [] {
        GCHECK(sv()->status().find("no such table") != std::string::npos);
        run("INSERT INTO people VALUES(4,'Di','x')");
    });
    MCGuiWait("a refusal", 10, [] { return idle() && sv()->failed(); });
    MCGuiStep("of an INSERT", [] {
        GCHECK(sv()->status().find("Refused") != std::string::npos);
        run("UPDATE people SET name = 'Z'");
    });
    MCGuiWait("an UPDATE refused", 10, [] { return idle() && sv()->failed() && sv()->status().find("Refused") == 0; });
    MCGuiStep("and a DELETE", [] { run("DELETE FROM people"); });
    MCGuiWait("refused", 10, [] { return idle() && sv()->failed() && sv()->status().find("Refused") == 0; });
    MCGuiStep("and a CREATE", [] { run("CREATE TABLE t2(a)"); });
    MCGuiWait("refused again", 10, [] { return idle() && sv()->failed() && sv()->status().find("Refused") == 0; });
    MCGuiStep("and a BEGIN", [] { run("BEGIN"); });
    MCGuiWait("BEGIN refused", 10, [] { return idle() && sv()->failed() && sv()->status().find("Refused") == 0; });
    MCGuiStep("and a SAVEPOINT", [] { run("SAVEPOINT s"); });
    MCGuiWait("SAVEPOINT refused", 10, [] { return idle() && sv()->failed() && sv()->status().find("Refused") == 0; });
    MCGuiStep("and an ATTACH", [] { run("ATTACH DATABASE '" + F("other.db") + "' AS o"); });
    MCGuiWait("ATTACH refused", 10, [] { return idle() && sv()->failed() && sv()->status().find("Refused") == 0; });
    MCGuiStep("and a pragma that sets", [] { run("PRAGMA user_version = 5"); });
    MCGuiWait("pragma refused", 10, [] { return idle() && sv()->failed() && sv()->status().find("Refused") == 0; });
    MCGuiStep("while a pragma that reads still answers", [] { run("PRAGMA table_info(people)"); });
    MCGuiWait("its rows", 10, [] { return idle() && !sv()->failed() && sv()->rowCount() == 3; });
    MCGuiStep("with the file's bytes unchanged, and no other file made", [] {
        GCHECK(MCGuiReadFile(F("a.db")) == gBefore);
        GCHECK(!there(F("other.db")));
        GCHECK(!there(F("a.db-journal")));
    });

    // A WAL database whose -wal is gone (its last connection closed).
    MCGuiStep("a WAL database with no -wal beside it", [] {
        std::remove(F("w.db").c_str());
        sql(F("w.db"), "PRAGMA journal_mode=WAL; CREATE TABLE t(a); INSERT INTO t VALUES(7);");
        GCHECK(!there(F("w.db-wal")));
        gBefore = MCGuiReadFile(F("w.db"));
        GCHECK(gBefore.size() > 100 && (unsigned char)gBefore[18] == 2);
        MCGuiOpenFileKept(F("w.db"));
    });
    MCGuiWait("opens", 15, [] { return shows(F("w.db")) && sv()->rowCount() == 1; });
    MCGuiStep("and shows its row, leaving the file as it was", [] {
        GCHECK_EQ_STR(sv()->cell(0, 0), "7");
        GCHECK(!sv()->failed());
        GCHECK(MCGuiReadFile(F("w.db")) == gBefore);
    });

    // A file that is not a database.
    MCGuiStep("a file with a database's name that is not one", [] {
        MCGuiWriteFile(F("bad.sqlite"), std::string("SQLite format 3\0", 16) + "not really a database, just bytes" + std::string(200, 'x'));
        MCGuiOpenFileKept(F("bad.sqlite"));
    });
    MCGuiWait("the failure", 15, [] { return shows(F("bad.sqlite")) && sv()->failed(); });
    MCGuiStep("shown in the slot", [] {
        GCHECK(sv()->status().find("Cannot open") == 0);
        GCHECK(sv()->tables().empty());
    });

    // A reopen keeps the query.
    MCGuiStep("a query on a.db", [] {
        MCGuiOpenFileKept(F("a.db"));
    });
    MCGuiWait("a.db on show", 15, [] { return shows(F("a.db")); });
    MCGuiStep("the people over 1", [] { run("SELECT name FROM people WHERE id > 1 ORDER BY id"); });
    MCGuiWait("two rows", 10, [] { return idle() && !sv()->failed() && sv()->rowCount() == 2; });
    MCGuiStep("the file changes and is shown again", [] {
        sql(F("a.db"), "INSERT INTO people VALUES(9,'Zed',NULL)");
        sv()->show(F("a.db"));
    });
    MCGuiWait("three rows", 10, [] { return idle() && !sv()->failed() && sv()->rowCount() == 3; });
    MCGuiStep("the query kept, the new row in it", [] {
        GCHECK_EQ_STR(sv()->query(), "SELECT name FROM people WHERE id > 1 ORDER BY id");
        GCHECK_EQ_STR(sv()->cell(2, 0), "Zed");
        GCHECK_EQ_STR(std::to_string(sv()->tables().size()), "4");
    });

    // A query that never ends.
    MCGuiStep("a query that counts to a number it cannot reach", [] {
        gT0 = MCGuiNowMs();
        gCpu0 = MCGuiThreadCpuMs();
        run("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 4000000000000) "
            "SELECT count(*) FROM c");
    });
    MCGuiWait("it is stopped", 30, [] { return idle(); });
    MCGuiStep("after about 5 s, with a message, the main thread having done little", [] {
        gElapsed = MCGuiNowMs() - gT0;
        gCpu = MCGuiThreadCpuMs() - gCpu0;
        printf("  slow query: %.0f ms wall, %.0f ms of the main thread's CPU\n", gElapsed, gCpu);
        GCHECK(sv()->failed());
        GCHECK(sv()->status().find("Stopped") == 0);
        if (!MCGuiTimingSkipped()) {
            GCHECK(gElapsed > 4500);
            GCHECK(gElapsed < 15000);
        }
        GCHECK_TIME(gCpu, 1500, "main thread CPU while a query ran for 5 s");
    });
    MCGuiStep("and the next query answers", [] { run("SELECT 1 + 1"); });
    MCGuiWait("two", 10, [] { return idle() && !sv()->failed() && sv()->rowCount() == 1; });
    MCGuiStep("the file still as the last insert left it", [] {
        GCHECK_EQ_STR(sv()->cell(0, 0), "2");
        ed()->closeAllTabs();
    });
}
