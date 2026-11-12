// SqliteView.cpp — see SqliteView.h.
#include "SqliteView.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>

namespace {

using Clock = std::chrono::steady_clock;

std::string percentEncode(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// A WAL database (file format versions 2 at bytes 18 and 19) with no -wal
// beside it: read-only mode cannot create the WAL files, so it opens
// immutable.
bool walWithoutLog(const std::string& path) {
    unsigned char h[100];
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    const size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    return n == sizeof h && h[18] == 2 && h[19] == 2 && !exists(path + "-wal");
}

// Pragmas that take an argument and only read.
bool readingPragma(const char* name) {
    static const std::set<std::string> ok = {
        "table_info", "table_xinfo", "table_list", "index_list", "index_info", "index_xinfo",
        "foreign_key_list", "foreign_key_check", "integrity_check", "quick_check",
        "function_list", "module_list", "pragma_list", "collation_list", "database_list"};
    std::string n = name ? name : "";
    for (char& c : n) c = (char)tolower((unsigned char)c);
    return ok.count(n) > 0;
}

int authorize(void*, int action, const char* a1, const char* a2, const char*, const char*) {
    switch (action) {
    case SQLITE_TRANSACTION:
    case SQLITE_SAVEPOINT:
    case SQLITE_ATTACH:
    case SQLITE_DETACH:
        return SQLITE_DENY;
    case SQLITE_PRAGMA:
        if (a2 && !readingPragma(a1)) return SQLITE_DENY;   // a pragma that sets
        return SQLITE_OK;
    default:
        return SQLITE_OK;
    }
}

struct Limit {
    std::atomic<bool>* cancel;
    Clock::time_point deadline;
    bool timedOut = false;
};

int progress(void* p) {
    Limit* l = static_cast<Limit*>(p);
    if (l->cancel->load()) return 1;
    if (Clock::now() > l->deadline) {
        l->timedOut = true;
        return 1;
    }
    return 0;
}

std::string quoteName(const std::string& name) {
    std::string q = "\"";
    for (char c : name) {
        if (c == '"') q += '"';
        q += c;
    }
    return q + "\"";
}

std::string selectAll(const std::string& name) { return "SELECT * FROM " + quoteName(name); }

std::string capText(const unsigned char* t, int n) {
    const int kMax = 4000;
    if (!t) return "";
    if (n <= kMax) return std::string((const char*)t, (size_t)n);
    int cut = kMax;
    while (cut > 0 && (t[cut] & 0xC0) == 0x80) cut--;
    return std::string((const char*)t, (size_t)cut) + "…";
}

std::string withCommas(size_t n) {
    std::string s = std::to_string(n), out;
    for (size_t i = 0; i < s.size(); i++) {
        if (i && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return out;
}

struct ColData {
    class SqliteView* view;
    size_t col;
};

}  // namespace

struct SqliteView::Link {
    SqliteView* view = nullptr;
};

SqliteView::SqliteView() {
    link_ = std::make_shared<Link>();
    link_->view = this;

    namesModel_ = gtk_string_list_new(nullptr);
    picked_ = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(namesModel_)));
    gtk_single_selection_set_autoselect(picked_, FALSE);
    gtk_single_selection_set_can_unselect(picked_, TRUE);
    gtk_single_selection_set_selected(picked_, GTK_INVALID_LIST_POSITION);
    GtkListItemFactory* nf = gtk_signal_list_item_factory_new();
    g_signal_connect(nf, "setup", G_CALLBACK(setupName), this);
    g_signal_connect(nf, "bind", G_CALLBACK(bindName), this);
    tableList_ = gtk_list_view_new(GTK_SELECTION_MODEL(picked_), nf);
    g_signal_connect(picked_, "notify::selected", G_CALLBACK(onPicked), this);
    GtkWidget* listScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(listScroll), tableList_);
    gtk_widget_set_size_request(listScroll, 160, -1);

    entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_), "SELECT * FROM table   (Enter runs it)");
    gtk_widget_add_css_class(entry_, "monospace");
    g_signal_connect(entry_, "activate", G_CALLBACK(onActivate), this);
    statusLabel_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(statusLabel_), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(statusLabel_), TRUE);
    gtk_widget_set_margin_start(statusLabel_, 6);
    gtk_widget_set_margin_top(statusLabel_, 3);
    gtk_widget_set_margin_bottom(statusLabel_, 3);
    grid_ = gtk_column_view_new(nullptr);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(grid_), TRUE);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(grid_), TRUE);
    GtkWidget* gridScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(gridScroll), grid_);
    gtk_widget_set_hexpand(gridScroll, TRUE);
    gtk_widget_set_vexpand(gridScroll, TRUE);

    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(right), entry_);
    gtk_box_append(GTK_BOX(right), statusLabel_);
    gtk_box_append(GTK_BOX(right), gridScroll);

    root_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(root_), listScroll);
    gtk_paned_set_end_child(GTK_PANED(root_), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(root_), FALSE);
    gtk_paned_set_position(GTK_PANED(root_), 180);
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_vexpand(root_, TRUE);
    g_object_ref_sink(root_);

    thread_ = std::thread([this] { work(); });
}

SqliteView::~SqliteView() {
    link_->view = nullptr;
    if (reloadTimer_) g_source_remove(reloadTimer_);
    if (monitor_) {
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        cancel_ = true;
        Job q;
        q.kind = Job::Quit;
        jobs_.push_back(q);
    }
    cv_.notify_one();
    thread_.join();
    g_object_unref(namesModel_);
    g_object_unref(root_);
}

// ---------------------------------------------------------------- the worker

void SqliteView::submit(Job j) {
    outstanding_++;
    {
        std::lock_guard<std::mutex> l(mu_);
        cancel_ = true;   // a running query gives way; the worker clears it per job
        jobs_.push_back(std::move(j));
    }
    cv_.notify_one();
}

void SqliteView::work() {
    sqlite3* db = nullptr;
    std::string openError;
    auto closeDb = [&] {
        if (db) sqlite3_close(db);
        db = nullptr;
    };
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return !jobs_.empty(); });
            j = std::move(jobs_.front());
            jobs_.pop_front();
            cancel_ = false;
        }
        if (j.kind == Job::Quit) break;
        Result* r = new Result;
        r->kind = j.kind;
        r->gen = j.gen;
        r->path = j.path;
        const auto t0 = Clock::now();
        if (j.kind == Job::Close) {
            closeDb();
        } else if (j.kind == Job::Open) {
            closeDb();
            openError.clear();
            const bool immutable = walWithoutLog(j.path);
            const std::string uri =
                "file:" + percentEncode(j.path) + "?mode=ro" + (immutable ? "&immutable=1" : "");
            int rc = sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI,
                                     nullptr);
            if (rc != SQLITE_OK) {
                openError = db ? sqlite3_errmsg(db) : "out of memory";
                closeDb();
            } else {
                sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
                sqlite3_busy_timeout(db, 1000);
                sqlite3_exec(db, "PRAGMA query_only=ON", nullptr, nullptr, nullptr);
                sqlite3_set_authorizer(db, authorize, nullptr);
                sqlite3_stmt* st = nullptr;
                rc = sqlite3_prepare_v2(
                    db,
                    "SELECT name, type FROM sqlite_master WHERE type IN ('table','view') "
                    "AND name NOT LIKE 'sqlite_%' ORDER BY type, name",
                    -1, &st, nullptr);
                while (rc == SQLITE_OK && (rc = sqlite3_step(st)) == SQLITE_ROW) {
                    r->tables.push_back((const char*)sqlite3_column_text(st, 0));
                    r->tableTypes.push_back((const char*)sqlite3_column_text(st, 1));
                    rc = SQLITE_OK;
                }
                if (st) sqlite3_finalize(st);
                if (rc != SQLITE_DONE && rc != SQLITE_OK) {
                    openError = sqlite3_errmsg(db);
                    r->tables.clear();
                    r->tableTypes.clear();
                    closeDb();
                }
            }
            r->error = openError;
        } else if (!db) {
            r->error = openError.empty() ? "The database is not open." : openError;
        } else {
            Limit lim{&cancel_, Clock::now() + std::chrono::milliseconds(kQueryLimitMs)};
            sqlite3_progress_handler(db, 1000, progress, &lim);
            sqlite3_stmt* st = nullptr;
            int rc = sqlite3_prepare_v2(db, j.sql.c_str(), -1, &st, nullptr);   // the first statement
            if (rc == SQLITE_AUTH || (rc == SQLITE_ERROR && lim.timedOut == false &&
                                      strstr(sqlite3_errmsg(db), "not authorized"))) {
                r->error = "Refused: this statement could change the database or hold a lock, "
                           "and the viewer only reads.";
            } else if (rc != SQLITE_OK) {
                r->error = sqlite3_errmsg(db);
            } else if (!st) {
                r->error = "There is no statement to run.";
            } else if (!sqlite3_stmt_readonly(st)) {
                r->error = "Refused: this statement would change the database, and the viewer "
                           "only reads.";
            } else {
                const int n = sqlite3_column_count(st);
                for (int c = 0; c < n; c++) {
                    const char* nm = sqlite3_column_name(st, c);
                    r->columns.push_back(nm ? nm : "");
                }
                while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                    if ((int)r->rows.size() >= kMaxRows) {
                        r->truncated = true;
                        break;
                    }
                    std::vector<Cell> row(n);
                    for (int c = 0; c < n; c++) {
                        const int t = sqlite3_column_type(st, c);
                        if (t == SQLITE_NULL) row[c].null = true;
                        else if (t == SQLITE_BLOB)
                            row[c].text = "(BLOB, " + std::to_string(sqlite3_column_bytes(st, c)) +
                                          " bytes)";
                        else {
                            const unsigned char* tx = sqlite3_column_text(st, c);
                            row[c].text = capText(tx, sqlite3_column_bytes(st, c));
                        }
                    }
                    r->rows.push_back(std::move(row));
                }
                if (rc == SQLITE_INTERRUPT || lim.timedOut) {
                    r->stopped = true;
                    r->error = lim.timedOut
                        ? "Stopped: the query ran for more than " +
                              std::to_string(kQueryLimitMs / 1000) + " seconds."
                        : "Stopped.";
                } else if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                    r->error = sqlite3_errmsg(db);
                }
            }
            if (st) sqlite3_finalize(st);
            sqlite3_progress_handler(db, 0, nullptr, nullptr);
        }
        r->ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        deliver(r);
    }
    closeDb();
}

void SqliteView::deliver(Result* r) {
    struct Hand {
        std::shared_ptr<Link> link;
        Result* r;
    };
    Hand* h = new Hand{link_, r};
    g_idle_add(+[](gpointer p) -> gboolean {
        Hand* hand = static_cast<Hand*>(p);
        if (SqliteView* v = hand->link->view) {
            v->outstanding_--;
            v->apply(*hand->r);
        }
        delete hand->r;
        delete hand;
        return G_SOURCE_REMOVE;
    }, h);
}

// ---------------------------------------------------------------- the view

void SqliteView::show(const std::string& path) {
    const bool keep = path == path_ && (!query_.empty() || !tableNames_.empty());
    watch(path);
    path_ = path;
    autoFirst_ = !keep || query_.empty();
    if (!keep) {
        query_.clear();
        tableNames_.clear();
        tableTypes_.clear();
        columns_.clear();
        rows_.clear();
        fillTables();
        fillRows();
        gtk_editable_set_text(GTK_EDITABLE(entry_), "");
        setStatus("Opening…", false);
    }
    Job o;
    o.kind = Job::Open;
    o.path = path;
    o.gen = ++openGen_;
    submit(o);
    if (!query_.empty()) {
        Job q;
        q.kind = Job::Query;
        q.sql = query_;
        q.gen = ++queryGen_;
        submit(q);
    }
}

void SqliteView::clear() {
    openGen_++;
    queryGen_++;
    Job c;
    c.kind = Job::Close;
    submit(c);
    watch("");
    path_.clear();
    query_.clear();
    tableNames_.clear();
    tableTypes_.clear();
    columns_.clear();
    rows_.clear();
    fillTables();
    fillRows();
    gtk_editable_set_text(GTK_EDITABLE(entry_), "");
    setStatus("", false);
}

void SqliteView::setPath(const std::string& path) {
    path_ = path;
    watch(path);
    // The connection keeps the file it has open; the next show reads the new name.
}

void SqliteView::runQuery(const std::string& sql) {
    autoFirst_ = false;
    query_ = sql;
    gtk_editable_set_text(GTK_EDITABLE(entry_), sql.c_str());
    filling_ = true;
    gtk_single_selection_set_selected(picked_, GTK_INVALID_LIST_POSITION);
    filling_ = false;
    Job q;
    q.kind = Job::Query;
    q.sql = sql;
    q.gen = ++queryGen_;
    submit(q);
    setStatus("Running…", false);
}

void SqliteView::showTable(const std::string& name) {
    runQuery(selectAll(name));
    for (size_t i = 0; i < tableNames_.size(); i++)
        if (tableNames_[i] == name) {
            filling_ = true;
            gtk_single_selection_set_selected(picked_, (guint)i);
            filling_ = false;
        }
}

void SqliteView::apply(const Result& r) {
    if (r.kind == Job::Open) {
        if (r.gen != openGen_) return;
        tableNames_ = r.tables;
        tableTypes_ = r.tableTypes;
        fillTables();
        if (!r.error.empty()) {
            columns_.clear();
            rows_.clear();
            fillRows();
            setStatus("Cannot open “" + path_ + "” as a database: " + r.error, true);
            return;
        }
        if (autoFirst_ && !tableNames_.empty()) {
            autoFirst_ = false;
            showTable(tableNames_[0]);
        } else if (tableNames_.empty() && query_.empty()) {
            setStatus("This database has no tables.", false);
        }
        return;
    }
    if (r.kind != Job::Query || r.gen != queryGen_) return;
    if (!r.error.empty() && !r.stopped) {
        columns_.clear();
        rows_.clear();
        fillRows();
        setStatus(r.error, true);
        return;
    }
    columns_ = r.columns;
    rows_ = r.rows;
    truncated_ = r.truncated;
    fillRows();
    char ms[32];
    snprintf(ms, sizeof ms, "%.0f", r.ms);
    if (r.stopped) setStatus(r.error, true);
    else if (r.truncated)
        setStatus("The first " + withCommas(rows_.size()) + " rows; the rest are left out. " +
                  std::string(ms) + " ms", false);
    else
        setStatus(withCommas(rows_.size()) + (rows_.size() == 1 ? " row, " : " rows, ") + ms +
                  " ms", false);
}

void SqliteView::setStatus(const std::string& text, bool failed) {
    status_ = text;
    failed_ = failed;
    gtk_label_set_text(GTK_LABEL(statusLabel_), text.c_str());
    if (failed) gtk_widget_add_css_class(statusLabel_, "error");
    else gtk_widget_remove_css_class(statusLabel_, "error");
}

void SqliteView::fillTables() {
    filling_ = true;
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(namesModel_));
    gtk_string_list_splice(namesModel_, 0, n, nullptr);
    for (size_t i = 0; i < tableNames_.size(); i++) {
        const std::string label = tableNames_[i] + (tableTypes_[i] == "view" ? "  (view)" : "");
        gtk_string_list_append(namesModel_, label.c_str());
    }
    guint sel = GTK_INVALID_LIST_POSITION;
    for (size_t i = 0; i < tableNames_.size(); i++)
        if (!query_.empty() && selectAll(tableNames_[i]) == query_) sel = (guint)i;
    gtk_single_selection_set_selected(picked_, sel);
    filling_ = false;
}

void SqliteView::fillRows() {
    GListModel* cols = gtk_column_view_get_columns(GTK_COLUMN_VIEW(grid_));
    while (g_list_model_get_n_items(cols) > 0) {
        GtkColumnViewColumn* c = GTK_COLUMN_VIEW_COLUMN(g_list_model_get_item(cols, 0));
        gtk_column_view_remove_column(GTK_COLUMN_VIEW(grid_), c);
        g_object_unref(c);
    }
    for (size_t c = 0; c < columns_.size(); c++) {
        GtkListItemFactory* f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(setupCell), this);
        g_signal_connect_data(f, "bind", G_CALLBACK(bindCell), new ColData{this, c},
                              +[](gpointer d, GClosure*) { delete static_cast<ColData*>(d); },
                              (GConnectFlags)0);
        GtkColumnViewColumn* col = gtk_column_view_column_new(columns_[c].c_str(), f);
        gtk_column_view_column_set_resizable(col, TRUE);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(grid_), col);
        g_object_unref(col);
    }
    GtkStringList* idx = gtk_string_list_new(nullptr);
    for (size_t i = 0; i < rows_.size(); i++) gtk_string_list_append(idx, std::to_string(i).c_str());
    GtkNoSelection* sel = gtk_no_selection_new(G_LIST_MODEL(idx));
    gtk_column_view_set_model(GTK_COLUMN_VIEW(grid_), GTK_SELECTION_MODEL(sel));
    g_object_unref(sel);
}

void SqliteView::setupCell(GtkListItemFactory*, GtkListItem* item, gpointer) {
    GtkWidget* l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    gtk_list_item_set_child(item, l);
}

void SqliteView::bindCell(GtkListItemFactory*, GtkListItem* item, gpointer datap) {
    ColData* d = static_cast<ColData*>(datap);
    SqliteView* v = d->view;
    GtkWidget* l = gtk_list_item_get_child(item);
    const char* s = gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item)));
    const size_t row = (size_t)atoi(s ? s : "0");
    if (row >= v->rows_.size() || d->col >= v->rows_[row].size()) {
        gtk_label_set_text(GTK_LABEL(l), "");
        return;
    }
    const Cell& c = v->rows_[row][d->col];
    if (c.null) {
        gtk_label_set_markup(GTK_LABEL(l), "<i>NULL</i>");
        gtk_widget_add_css_class(l, "dim-label");
    } else {
        gtk_label_set_text(GTK_LABEL(l), c.text.c_str());
        gtk_widget_remove_css_class(l, "dim-label");
    }
}

void SqliteView::setupName(GtkListItemFactory*, GtkListItem* item, gpointer) {
    GtkWidget* l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_margin_start(l, 6);
    gtk_widget_set_margin_end(l, 6);
    gtk_list_item_set_child(item, l);
}

void SqliteView::bindName(GtkListItemFactory*, GtkListItem* item, gpointer) {
    const char* s = gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item)));
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), s ? s : "");
}

void SqliteView::onPicked(GObject*, GParamSpec*, gpointer p) {
    SqliteView* v = static_cast<SqliteView*>(p);
    if (v->filling_) return;
    const guint i = gtk_single_selection_get_selected(v->picked_);
    if (i == GTK_INVALID_LIST_POSITION || i >= v->tableNames_.size()) return;
    v->showTable(v->tableNames_[i]);
}

void SqliteView::onActivate(GtkEntry* e, gpointer p) {
    const char* t = gtk_editable_get_text(GTK_EDITABLE(e));
    if (t && *t) static_cast<SqliteView*>(p)->runQuery(t);
}

// ---------------------------------------------------------------- watching

void SqliteView::watch(const std::string& path) {
    if (monitor_) {
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
        monitor_ = nullptr;
    }
    if (path.empty()) return;
    GFile* f = g_file_new_for_path(path.c_str());
    monitor_ = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(f);
    if (monitor_) g_signal_connect(monitor_, "changed", G_CALLBACK(onFileChanged), this);
}

void SqliteView::onFileChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev, gpointer p) {
    SqliteView* v = static_cast<SqliteView*>(p);
    switch (ev) {
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_RENAMED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
        break;
    default:
        return;   // a change is read once the writer is done
    }
    if (v->reloadTimer_) g_source_remove(v->reloadTimer_);
    v->reloadTimer_ = g_timeout_add(400, [](gpointer q) -> gboolean {
        SqliteView* s = static_cast<SqliteView*>(q);
        s->reloadTimer_ = 0;
        if (!s->path_.empty() && exists(s->path_)) s->show(s->path_);
        return G_SOURCE_REMOVE;
    }, v);
}
