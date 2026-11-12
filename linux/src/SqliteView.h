// SqliteView.h — a SQLite database in the editor's slot, the GTK side of the
// Mac's SqliteView.mm (linux/PARITY.md item 4.1).
//
// The file is never changed. It is opened SQLITE_OPEN_READONLY through a
// `mode=ro` URI (`immutable=1` too for a WAL database with no -wal file
// beside it, which read-only mode cannot open), with query_only and the
// defensive flag on. A statement that writes (sqlite3_stmt_readonly), and
// BEGIN, SAVEPOINT, ATTACH, DETACH and a pragma that sets something, are
// refused by an authorizer before they run; only a query's first statement
// runs; a progress handler stops it at 5 s; at most 1,000 rows are read.
//
// Everything that touches the database, opening included, runs on one worker
// thread that owns the connection; the results are handed to the main
// thread in an idle callback, and an answer for a request that a newer one
// replaced is dropped. Showing the same file again (the editor does when the
// file changes on disk) opens it again and runs the query that was on show.
#pragma once

#include <gtk/gtk.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SqliteView {
public:
    static constexpr int kMaxRows = 1000;
    static constexpr int kQueryLimitMs = 5000;

    SqliteView();
    ~SqliteView();
    SqliteView(const SqliteView&) = delete;
    SqliteView& operator=(const SqliteView&) = delete;

    GtkWidget* widget() const { return root_; }

    // Opens `path` on the worker. The same file again keeps the query or
    // table that was on show and runs it again; another file starts over
    // with its first table.
    void show(const std::string& path);
    // Lets the file go (the tab leaves); the query is forgotten.
    void clear();
    // Changes the file's name without losing the query (a rename).
    void setPath(const std::string& path);

    // Runs `sql` (its first statement) and shows the rows.
    void runQuery(const std::string& sql);
    // SELECT * FROM "name".
    void showTable(const std::string& name);

    // What is on show, for the checks.
    const std::vector<std::string>& tables() const { return tableNames_; }
    const std::string& query() const { return query_; }
    const std::string& status() const { return status_; }
    bool failed() const { return failed_; }
    bool busy() const { return outstanding_ > 0; }
    size_t rowCount() const { return rows_.size(); }
    size_t columnCount() const { return columns_.size(); }
    const std::string& columnName(size_t c) const { return columns_[c]; }
    bool isNull(size_t r, size_t c) const { return rows_[r][c].null; }
    const std::string& cell(size_t r, size_t c) const { return rows_[r][c].text; }
    bool truncated() const { return truncated_; }

private:
    struct Cell {
        bool null = false;
        std::string text;
    };
    struct Job {
        enum Kind { Open, Query, Close, Quit } kind = Open;
        std::string path, sql;
        unsigned gen = 0;
    };
    struct Result {
        Job::Kind kind = Job::Open;
        unsigned gen = 0;
        std::string path, error;
        std::vector<std::string> tables, tableTypes;
        std::vector<std::string> columns;
        std::vector<std::vector<Cell>> rows;
        bool truncated = false, stopped = false;
        double ms = 0;
    };
    struct Link;   // lets a result find the view, or know it is gone

    void submit(Job j);
    void work();
    void deliver(Result* r);
    void apply(const Result& r);
    void fillTables();
    void fillRows();
    void setStatus(const std::string& text, bool failed);
    void watch(const std::string& path);
    static void onFileChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer);
    static void onPicked(GObject*, GParamSpec*, gpointer);
    static void onActivate(GtkEntry*, gpointer);
    static void setupCell(GtkListItemFactory*, GtkListItem*, gpointer);
    static void bindCell(GtkListItemFactory*, GtkListItem*, gpointer);
    static void setupName(GtkListItemFactory*, GtkListItem*, gpointer);
    static void bindName(GtkListItemFactory*, GtkListItem*, gpointer);

    GtkWidget* root_ = nullptr;
    GtkWidget* entry_ = nullptr;
    GtkWidget* statusLabel_ = nullptr;
    GtkWidget* tableList_ = nullptr;
    GtkWidget* grid_ = nullptr;
    GtkStringList* namesModel_ = nullptr;
    GtkStringList* rowsModel_ = nullptr;
    GtkSingleSelection* picked_ = nullptr;
    GFileMonitor* monitor_ = nullptr;
    guint reloadTimer_ = 0;
    bool filling_ = false;

    std::string path_, query_, status_;
    bool failed_ = false, truncated_ = false, autoFirst_ = false;
    std::vector<std::string> tableNames_, tableTypes_, columns_;
    std::vector<std::vector<Cell>> rows_;
    unsigned openGen_ = 0, queryGen_ = 0;
    int outstanding_ = 0;

    std::shared_ptr<Link> link_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::atomic<bool> cancel_{false};
};
