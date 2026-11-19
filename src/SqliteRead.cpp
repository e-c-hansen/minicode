// SqliteRead.cpp — see SqliteRead.h.
#include "SqliteRead.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace SqliteRead {

namespace {

// Only reads run: the statements this file prepares, and what a view they
// read reads. Pragmas (query_only is set before this is installed),
// transactions, savepoints, ATTACH and DETACH, and anything that writes
// are refused. Schema parsing is not asked (SQLite skips the authorizer
// while it reads its schema).
int authorize(void*, int action, const char*, const char*, const char*, const char*) {
    switch (action) {
    case SQLITE_READ:
    case SQLITE_SELECT:
    case SQLITE_FUNCTION:
#ifdef SQLITE_RECURSIVE
    case SQLITE_RECURSIVE:
#endif
        return SQLITE_OK;
    default:
        return SQLITE_DENY;
    }
}

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() { sqlite3_finalize(st); }
};

std::string hexByte(unsigned char c) {
    static const char* hex = "0123456789ABCDEF";
    return std::string{'%', hex[c >> 4], hex[c & 15]};
}

}  // namespace

bool isDatabaseHeader(const std::string& head) {
    static const char magic[16] = {'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f',
                                   'o', 'r', 'm', 'a', 't', ' ', '3', '\0'};
    return head.size() >= 16 && std::memcmp(head.data(), magic, 16) == 0;
}

bool isWalHeader(const std::string& head) {
    return isDatabaseHeader(head) && head.size() >= 20 && head[18] == 2 && head[19] == 2;
}

std::string quoteName(const std::string& name) {
    std::string out = "\"";
    for (char c : name) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

std::string uriFor(const std::string& path, bool immutable) {
    std::string out = "file:";
    for (unsigned char c : path) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else
            out += hexByte(c);
    }
    out += "?mode=ro";
    if (immutable) out += "&immutable=1";
    return out;
}

std::string validUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        std::size_t len = 0;
        unsigned lo = 0x80, hi = 0xBF;   // the second byte's range
        if (c < 0x80) len = 1;
        else if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c == 0xE0) { len = 3; lo = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) len = 3;
        else if (c == 0xED) { len = 3; hi = 0x9F; }   // no surrogates
        else if (c >= 0xEE && c <= 0xEF) len = 3;
        else if (c == 0xF0) { len = 4; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) len = 4;
        else if (c == 0xF4) { len = 4; hi = 0x8F; }
        bool ok = len > 0 && i + len <= n;
        for (std::size_t k = 1; ok && k < len; k++) {
            const unsigned char b = (unsigned char)s[i + k];
            ok = k == 1 ? (b >= lo && b <= hi) : (b >= 0x80 && b <= 0xBF);
        }
        if (ok) {
            out.append(s, i, len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            i += 1;
        }
    }
    return out;
}

std::string cutUtf8(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    std::size_t at = n;
    while (at > 0 && ((unsigned char)s[at] & 0xC0) == 0x80) at--;
    return s.substr(0, at);
}

int Database::onProgress(void* self) {
    const Database* d = static_cast<const Database*>(self);
    return std::chrono::steady_clock::now() > d->deadline_ ? 1 : 0;
}

Database::~Database() {
    if (db_) sqlite3_close_v2(db_);
}

std::string Database::error(int rc) const {
    switch (rc & 0xFF) {
    case SQLITE_INTERRUPT:
        return "reading it took longer than " +
               (limit_.count() % 1000 == 0 ? std::to_string(limit_.count() / 1000) +
                                                 (limit_.count() == 1000 ? " second" : " seconds")
                                           : std::to_string(limit_.count()) + " ms") +
               " and was stopped";
    case SQLITE_TOOBIG: return "a value in it is over " + std::to_string(kMaxValueBytes >> 20) + " MB";
    case SQLITE_NOTADB: return "it is not a SQLite database";
    case SQLITE_BUSY:
    case SQLITE_LOCKED: return "a writer holds the database just now; try again";
    case SQLITE_AUTH: return "this viewer only reads";
    default: return db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
    }
}

bool Database::openWith(const std::string& path, bool immutable, std::string* why, bool* cantOpen) {
    *cantOpen = false;
    int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(uriFor(path, immutable).c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        *cantOpen = !db || (sqlite3_errcode(db) & 0xFF) == SQLITE_CANTOPEN;
        if (why) *why = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        sqlite3_close_v2(db);
        return false;
    }
    sqlite3_extended_result_codes(db, 1);
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, kMaxValueBytes);
    sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
    sqlite3_busy_timeout(db, 250);
#ifdef SQLITE_DBCONFIG_DEFENSIVE
    sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
#endif
#ifdef SQLITE_DBCONFIG_TRUSTED_SCHEMA
    sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
#endif
#ifdef SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION
    sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
#endif
    sqlite3_exec(db, "PRAGMA query_only = ON", nullptr, nullptr, nullptr);
    sqlite3_set_authorizer(db, authorize, nullptr);
    sqlite3_progress_handler(db, 1000, &Database::onProgress, this);
    db_ = db;
    immutable_ = immutable;

    // The schema, which is where a file that is not a database, or a WAL
    // database with no -shm to make, first fails.
    tables_.clear();
    more_ = 0;
    Statement s;
    int r = sqlite3_prepare_v2(db_,
                               "SELECT name, type FROM sqlite_master WHERE type IN ('table', 'view') "
                               "ORDER BY type = 'view', name",
                               -1, &s.st, nullptr);
    if (r == SQLITE_OK) {
        while ((r = sqlite3_step(s.st)) == SQLITE_ROW) {
            const char* name = (const char*)sqlite3_column_text(s.st, 0);
            const char* type = (const char*)sqlite3_column_text(s.st, 1);
            if (!name) continue;
            if (tables_.size() >= kMaxTables) {
                more_++;
                continue;
            }
            tables_.push_back({name, type ? type : "table"});
        }
        if (r == SQLITE_DONE) r = SQLITE_OK;
    }
    if (r != SQLITE_OK) {
        *cantOpen = (r & 0xFF) == SQLITE_CANTOPEN;
        if (why) *why = error(r);
        sqlite3_finalize(s.st);
        s.st = nullptr;
        sqlite3_close_v2(db_);
        db_ = nullptr;
        tables_.clear();
        return false;
    }
    return true;
}

bool Database::open(const std::string& path, std::string* why) {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
    deadline_ = std::chrono::steady_clock::now() + limit_;
    // A WAL database its last writer closed has no -shm (and usually no
    // -wal) beside it. A read-only connection would make them when the
    // folder lets it, which is changing the folder; with nothing in use
    // there is nothing outside the main file to read, so it opens
    // immutable, taking no locks and making nothing.
    std::string head(100, '\0');
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        head.resize(std::fread(&head[0], 1, head.size(), f));
        std::fclose(f);
    } else {
        head.clear();
    }
    struct stat st;
    const bool closedWal = isWalHeader(head) && (::stat((path + "-shm").c_str(), &st) != 0 ||
                                                 ::stat((path + "-wal").c_str(), &st) != 0);
    bool cantOpen = false;
    return openWith(path, closedWal, why, &cantOpen);
}

bool Database::tables(std::vector<Table>* out, std::size_t* more, std::string* why) {
    if (!db_) {
        if (why) *why = "the database is not open";
        return false;
    }
    if (out) *out = tables_;
    if (more) *more = more_;
    return true;
}

bool Database::rows(const std::string& name, std::int64_t offset, std::size_t limit, Page* out,
                    std::string* why, std::size_t maxBytes) {
    *out = Page();
    if (!db_) {
        if (why) *why = "the database is not open";
        return false;
    }
    bool known = false;
    for (const Table& t : tables_)
        if (t.name == name) known = true;
    if (!known) {
        if (why) *why = "it has no table or view by that name";
        return false;
    }
    if (offset < 0) offset = 0;
    deadline_ = std::chrono::steady_clock::now() + limit_;
    Statement s;
    const std::string sql = "SELECT * FROM " + quoteName(name) + " LIMIT ? OFFSET ?";
    int r = sqlite3_prepare_v2(db_, sql.c_str(), -1, &s.st, nullptr);
    if (r != SQLITE_OK || !s.st) {
        if (why) *why = error(r == SQLITE_OK ? SQLITE_ERROR : r);
        return false;
    }
    if (!sqlite3_stmt_readonly(s.st)) {
        if (why) *why = "this viewer only reads";
        return false;
    }
    sqlite3_bind_int64(s.st, 1, (sqlite3_int64)limit + 1);
    sqlite3_bind_int64(s.st, 2, (sqlite3_int64)offset);
    const int n = sqlite3_column_count(s.st);
    for (int i = 0; i < n; i++) {
        const char* c = sqlite3_column_name(s.st, i);
        out->columns.push_back(validUtf8(c ? c : ""));
    }
    std::size_t bytes = 0;
    while ((r = sqlite3_step(s.st)) == SQLITE_ROW) {
        if (out->rows.size() >= limit || bytes > maxBytes) {
            out->more = true;
            break;
        }
        std::vector<Cell> row((std::size_t)n);
        for (int i = 0; i < n; i++) {
            Cell& c = row[(std::size_t)i];
            switch (sqlite3_column_type(s.st, i)) {
            case SQLITE_NULL: c.kind = Cell::Kind::Null; break;
            case SQLITE_BLOB:
                c.kind = Cell::Kind::Blob;
                c.bytes = sqlite3_column_bytes(s.st, i);
                break;
            default: {
                const int type = sqlite3_column_type(s.st, i);
                c.kind = type == SQLITE_INTEGER ? Cell::Kind::Integer
                         : type == SQLITE_FLOAT ? Cell::Kind::Real
                                                : Cell::Kind::Text;
                const unsigned char* t = sqlite3_column_text(s.st, i);
                const int len = sqlite3_column_bytes(s.st, i);
                c.bytes = len;
                if (t) c.text = validUtf8(cutUtf8(std::string((const char*)t, (std::size_t)len), kMaxCellBytes));
                bytes += c.text.size();
                break;
            }
            }
        }
        out->rows.push_back(std::move(row));
    }
    if (r != SQLITE_ROW && r != SQLITE_DONE) {
        if (why) *why = error(r);
        *out = Page();
        return false;
    }
    return true;
}

}  // namespace SqliteRead
