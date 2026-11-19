// SqliteRead.h — a SQLite database read for showing, never changed: its
// tables and views, and a page of one's rows. Uses the system libsqlite3
// (the only library the core links besides the C++ library). The session
// web page's database viewer reads through it (SessionWeb.h,
// databaseJson); the Mac's own viewer (mac/src/SqliteView.mm) follows the
// same rules with a query box of its own.
//
// Read-only every way SQLite offers: SQLITE_OPEN_READONLY with "mode=ro" in
// the URI (and SQLITE_OPEN_NOFOLLOW where the library has it), query_only,
// the defensive flag, an untrusted schema (a view or trigger cannot call a
// function with side effects), no extensions, and an authorizer that lets
// only reads run (no BEGIN, SAVEPOINT, ATTACH, DETACH, setting pragmas or
// writes): a read takes a shared lock only while one statement runs, and
// statements run one at a time, so a live database is never held or
// changed. A WAL database whose writer has closed has no -shm (and
// usually no -wal) beside it, which a read-only connection would make
// where the folder lets it (SQLite 3.22 and later); with nothing in use
// there is nothing outside the main file to miss, so it opens with
// immutable=1 (no locks, nothing made) instead of trying first, as the
// Mac's viewer does after a failure.
//
// Every call stops at a deadline (sqlite3_progress_handler), so a hostile
// view (a recursive CTE, a cross join) or a huge OFFSET gives way; a single
// value is held to kMaxValueBytes (SQLITE_LIMIT_LENGTH), so one row cannot
// take all the memory; text is cut at kMaxCellBytes on a character and made
// valid UTF-8; a blob is only its size.
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct sqlite3;

namespace SqliteRead {

constexpr std::size_t kMaxTables = 1000;           // listed; the rest counted
constexpr std::size_t kMaxCellBytes = 2000;        // of a text value shown
constexpr int kMaxValueBytes = 16 << 20;           // a larger value stops the read
constexpr std::chrono::milliseconds kTimeLimit{2000};   // each call

struct Table {
    std::string name, type;   // type "table" or "view"
};

struct Cell {
    enum class Kind { Null, Integer, Real, Text, Blob };
    Kind kind = Kind::Null;
    std::string text;          // Integer, Real: SQLite's own spelling; Text: cut
    std::int64_t bytes = 0;    // Text, Blob: the whole value's size
};

struct Page {
    std::vector<std::string> columns;
    std::vector<std::vector<Cell>> rows;
    bool more = false;         // rows past the page
};

// The first 16 bytes of every SQLite 3 database.
bool isDatabaseHeader(const std::string& head);
// A database's header (its first 100 bytes, or fewer) says WAL.
bool isWalHeader(const std::string& head);
// `name` as an SQL identifier: in double quotes, its own doubled.
std::string quoteName(const std::string& name);
// The file: URI SQLite opens `path` (absolute) with: the path's bytes
// that a URI cannot hold percent-encoded, then "?mode=ro", and
// "&immutable=1" when asked.
std::string uriFor(const std::string& path, bool immutable);
// `s` with every byte that does not begin or continue a well-formed UTF-8
// character made U+FFFD, so it can go into JSON and a WebSocket text frame.
std::string validUtf8(const std::string& s);
// `s` cut to at most `n` bytes, at a character's start.
std::string cutUtf8(const std::string& s, std::size_t n);

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens `path` (absolute, already checked by the caller) read-only, and
    // reads its schema; false with `why` when it is not a database or
    // cannot be read.
    bool open(const std::string& path, std::string* why);
    // Its tables, then its views, each by name; at most kMaxTables
    // (`more` counts the rest).
    bool tables(std::vector<Table>* out, std::size_t* more, std::string* why);
    // Rows `offset` .. `offset + limit - 1` of a table or view the schema
    // names (`name` exactly as tables() gave it; anything else is
    // refused), in its own order; fewer when the text read passes
    // `maxBytes` (the row that passed it kept).
    bool rows(const std::string& name, std::int64_t offset, std::size_t limit, Page* out,
              std::string* why, std::size_t maxBytes = SIZE_MAX);

    // For tests: each call's deadline.
    void setTimeLimit(std::chrono::milliseconds limit) { limit_ = limit; }
    bool immutable() const { return immutable_; }

private:
    bool openWith(const std::string& path, bool immutable, std::string* why, bool* cantOpen);
    std::string error(int rc) const;

    sqlite3* db_ = nullptr;
    bool immutable_ = false;
    std::chrono::milliseconds limit_ = kTimeLimit;
    std::chrono::steady_clock::time_point deadline_{};
    std::vector<Table> tables_;
    std::size_t more_ = 0;

    static int onProgress(void* self);
};

}  // namespace SqliteRead
