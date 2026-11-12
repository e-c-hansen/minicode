// GitChecks.h — what the Source Control suites share (GitSyncSuite.cpp,
// GitActionsSuite.cpp, GitHunksSuite.cpp, GitVerbsSuite.cpp; PARITY.md 2.9),
// the helpers of the Mac's tests/gui/GitPanelSuite.mm and GitSyncSuite.mm:
// a window of their own on a scratch repository, its panel, git run in the
// scratch world (no global config, as the harness sets it up), a Trash that
// is a folder of the scratch world, the questions, and the panel's rows and
// menus pressed for real.
#pragma once
#include <memory>

#include "Harness.h"

#include "Editor.h"
#include "GitDiffView.h"
#include "GitPanel.h"
#include "GitTrash.h"
#include "PalettePanel.h"

#include <algorithm>
#include <string>
#include <vector>

namespace gitchecks {

// The folder the suite's window is on; its window and panel.
inline std::string& Repo() {
    static std::string repo;
    return repo;
}
inline MCGuiAppWindow RepoWindow() {
    for (const MCGuiAppWindow& w : MCGuiAppWindows())
        if (w.rootDir == Repo()) return w;
    return MCGuiAppWindow();
}
inline GitPanel* Panel() { return RepoWindow().git; }
inline std::string In(const std::string& name) { return Repo() + "/" + name; }

inline std::string GitPath() {
    static std::string git;
    if (git.empty())
        if (char* p = g_find_program_in_path("git")) {
            git = p;
            g_free(p);
        }
    return git;
}
// git quoted for a shell line.
inline std::string G() { return "'" + GitPath() + "'"; }

// A shell line in the scratch world; a check fails if it does.
inline void Shell(const std::string& what, const std::string& line) {
    MCGuiAsync(what, 60, [what, line](MCGuiDone done) {
        MCGuiRunProcess({"/bin/sh", "-c", line}, {},
                        [what, done](int status, std::string out, std::string err) {
                            GCHECK(status == 0);
                            if (status) MCGuiNote("%s: %s%s", what.c_str(), out.c_str(), err.c_str());
                            done();
                        });
    });
}

// What git prints, run synchronously in the repository (a check's read).
inline std::string GitText(const std::vector<std::string>& args) {
    std::vector<const char*> argv = {GitPath().c_str()};
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GSubprocessLauncher* l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    g_subprocess_launcher_set_cwd(l, Repo().c_str());
    GSubprocess* p = g_subprocess_launcher_spawnv(l, argv.data(), nullptr);
    g_object_unref(l);
    if (!p) return "";
    GBytes* out = nullptr;
    g_subprocess_communicate(p, nullptr, nullptr, &out, nullptr, nullptr);
    std::string text;
    if (out) {
        gsize n = 0;
        const char* d = static_cast<const char*>(g_bytes_get_data(out, &n));
        text.assign(d ? d : "", n);
        g_bytes_unref(out);
    }
    g_object_unref(p);
    return text;
}

inline std::vector<std::string> RowsNow() {
    return Panel() ? Panel()->rowDescriptions() : std::vector<std::string>();
}
inline bool Rows(const std::vector<std::string>& want) {
    return Panel() && Panel()->inRepository() && RowsNow() == want;
}
inline int RowOf(const std::string& description) {
    const std::vector<std::string> r = RowsNow();
    auto it = std::find(r.begin(), r.end(), description);
    return it == r.end() ? -1 : (int)(it - r.begin());
}
// A row's widget, on screen to be clicked.
inline GtkWidget* RowWidget(const std::string& description) {
    const int row = RowOf(description);
    GtkWidget* w = row >= 0 && Panel() ? Panel()->rowWidget(row) : nullptr;
    return MCGuiOnScreen(w) ? w : nullptr;
}

// A wait for the widget a step is about to click to be on screen. GTK gives
// a widget its place on the frame clock, after the status that made it
// visible (the message box and the Sync button come with the first status
// of a repository) or the Ctrl+Shift+G that showed the panel; the waits on
// the rows' data can end before that, and a click aimed then found no
// widget to point at (in whole GUI runs at load 13 to 14). So every click
// on a widget of the panel or the editor waits for its widget first.
inline void WaitOnScreen(const std::string& what, std::function<GtkWidget*()> widget) {
    MCGuiWait(what + " on screen", 10, [widget] { return MCGuiOnScreen(widget()); });
}

// A click on the branch line, for real: the left button opens the branch
// picker, the right one the panel's own menu.
inline void ClickBranch(const std::string& what, int button = 1) {
    WaitOnScreen("the branch line", [] { return Panel()->branchLabel(); });
    MCGuiStep(what, [button] { MCGuiClick(Panel()->branchLabel(), -1, -1, button); });
}

// A click in the editor of the suite's window.
inline void ClickEditor(const std::string& what) {
    WaitOnScreen("the editor", [] { return RepoWindow().editor->textView(); });
    MCGuiStep(what, [] { MCGuiClick(RepoWindow().editor->textView()); });
}

// A click on a row, for real, with modifiers ("ctrl", "shift") or another
// button. A row's tooltip, shown below the pointer after it rested on the
// row above, took an XTest click sent at once (GTK hides it on the next
// motion, after the click had gone to it): a Ctrl click on b.txt right below
// a.txt reached no window. So the pointer rests on the message box, which
// has no tooltip, first.
inline void ClickRow(const std::string& row, const std::string& mods = "", int button = 1) {
    WaitOnScreen("the message box", [] { return Panel()->messageView(); });
    MCGuiStep("the pointer off the rows", [] { MCGuiMoveTo(Panel()->messageView()); });
    MCGuiPause(0.6);   // time is the condition: the last row's tooltip gone, no double-click
    MCGuiWait(row + " on screen", 10, [row] { return RowWidget(row) != nullptr; });
    MCGuiStep((mods.empty() ? std::string("a click") : mods + " click") +
                  (button == 3 ? " (right)" : "") + " on " + row,
              [row, mods, button] {
                  GtkWidget* w = RowWidget(row);
                  GCHECK(w != nullptr);
                  if (!w) return;
                  if (mods.empty()) MCGuiClick(w, -1, -1, button);
                  else MCGuiClickWith(mods, w, -1, -1, button);
              });
}

// ---- the Trash: a folder of the scratch world, as the Mac's suites swap
// trashItemAtURL for one.
inline std::string ScratchTrash() { return MCGuiScratch() + "/git-trash"; }
inline void UseScratchTrash(bool on) { GitTrash::useFolderForChecks(on ? ScratchTrash() : ""); }
inline std::vector<std::string> TrashedTexts() {
    std::vector<std::string> out;
    if (GDir* d = g_dir_open(ScratchTrash().c_str(), 0, nullptr)) {
        while (const char* name = g_dir_read_name(d))
            out.push_back(MCGuiReadFile(ScratchTrash() + "/" + name));
        g_dir_close(d);
    }
    return out;
}
inline bool Trashed(const std::string& text) {
    const std::vector<std::string> t = TrashedTexts();
    return std::find(t.begin(), t.end(), text) != t.end();
}

// ---- the questions (GtkAlertDialog windows)
inline bool QuestionUp() {
    GtkWindow* top = MCGuiTopWindow();
    if (top == MCGuiApp().window) return false;
    GtkWidget* cancel = MCGuiFindButton(GTK_WIDGET(top), "Cancel");
    return cancel && gtk_widget_get_width(cancel) > 0 && gtk_widget_get_height(cancel) > 0;
}
// Every label of the question, joined: its message and detail.
inline std::string QuestionText() {
    GtkWindow* top = MCGuiTopWindow();
    if (top == MCGuiApp().window) return "";
    std::string all;
    std::vector<GtkWidget*> todo = {GTK_WIDGET(top)};
    while (!todo.empty()) {
        GtkWidget* w = todo.back();
        todo.pop_back();
        if (GTK_IS_LABEL(w)) all += std::string(gtk_label_get_text(GTK_LABEL(w))) + "\n";
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            todo.push_back(c);
    }
    return all;
}
inline void Answer(const std::string& button) {
    MCGuiSetKeyWindow(MCGuiTopWindow());
    MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), button));
}
inline bool QuestionGone() { return MCGuiTopWindow() == MCGuiApp().window; }

// Whether `w` is what has the keyboard in the suite's window, active or not
// (gtk_widget_has_focus also wants the window to have the X focus, which the
// harness gives it only before a key).
inline bool Focused(GtkWidget* w) {
    GtkWindow* win = RepoWindow().window;
    GtkWidget* f = win ? gtk_root_get_focus(GTK_ROOT(win)) : nullptr;
    return f && w && (f == w || gtk_widget_is_ancestor(f, w));
}

// ---- menus: the labels of the panel's (or the diff's) popover, in order
inline std::vector<std::string> MenuLabels(GtkWidget* menu) {
    std::vector<std::string> out;
    if (!menu || !gtk_widget_get_mapped(menu)) return out;
    std::vector<GtkWidget*> todo = {menu};
    while (!todo.empty()) {
        GtkWidget* w = todo.front();
        todo.erase(todo.begin());
        if (GTK_IS_LABEL(w) && *gtk_label_get_text(GTK_LABEL(w)) && gtk_widget_get_mapped(w))
            out.push_back(gtk_label_get_text(GTK_LABEL(w)));
        std::vector<GtkWidget*> kids;
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            kids.push_back(c);
        todo.insert(todo.begin(), kids.begin(), kids.end());
    }
    return out;
}
inline bool MenuHas(GtkWidget* menu, const std::string& label) {
    return MCGuiFindLabel(menu, label) != nullptr;
}
// The item showing `label`, the widget a click and its tooltip are on.
inline GtkWidget* MenuItem(GtkWidget* menu, const std::string& label) {
    GtkWidget* l = MCGuiFindLabel(menu, label);
    for (GtkWidget* w = l; w && w != menu; w = gtk_widget_get_parent(w))
        if (std::string(G_OBJECT_TYPE_NAME(w)) == "GtkModelButton") return w;
    return l;
}

// ---- the palette's commands, typed and run for real
inline PalettePanel* Pal() { return RepoWindow().palette; }
inline std::string PaletteSelected() {
    const std::vector<std::string> l = Pal()->labels();
    const int i = Pal()->selected();
    return i >= 0 && i < (int)l.size() ? l[(size_t)i] : "";
}
// Ctrl+Shift+P, `query`, and Return once `label` is the row chosen.
inline void RunFromPalette(const std::string& query, const std::string& label) {
    MCGuiStep("Ctrl+Shift+P for " + label, [] { MCGuiKey("ctrl+shift+p"); });
    // A key sent while a popover menu that Escape just closed is still letting
    // go of the keyboard goes nowhere (seen only in full runs, a few times in
    // ten, whatever the View menu's length: the palette never opened and the
    // steps after it typed into the void). Pressed again while it has not
    // opened; on an open palette the key only takes the text again.
    auto polls = std::make_shared<int>(0);
    MCGuiWait("the palette", 10, [polls] {
        if (Pal() && Pal()->isOpen() && !Pal()->isPick()) return true;
        if (++*polls % 40 == 0) MCGuiKey("ctrl+shift+p");   // every 2 s
        return false;
    });
    MCGuiStep("\"" + query + "\"", [query] { MCGuiType(query); });
    MCGuiWait(label + " chosen", 10, [label] { return PaletteSelected() == label; });
    MCGuiStep("Return", [] { MCGuiKey("enter"); });
    MCGuiWait("the palette closes", 10, [] { return !Pal()->isOpen(); });
}

// ---- the diff on show and its hunk links
inline GitDiffView* Diff() {
    Editor* e = RepoWindow().editor;
    return e ? e->diffView() : nullptr;
}
// Whether GTK has laid the diff's text out: a diff just shown has its lines
// placed in idle slices, and until then a line not reached has no height,
// so a point worked out for a character (a link, "+k2") lands on another
// line. Under load a click on "+k2" selected the line below it, and the
// click before Shift+F10 put the caret outside the hunk. The last line is
// the last to get a height, and asking for it lays nothing out.
inline bool DiffLaidOut() {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(Diff()->buffer(), &end);
    int y = 0, h = 0;
    gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(Diff()->textView()), &end, &y, &h);
    return h > 0;
}
// The diff on show, on screen and laid out, so its points can be clicked.
inline bool DiffOnShow() {
    Editor* e = RepoWindow().editor;
    return e && e->isDiff() && Diff() && MCGuiOnScreen(Diff()->textView()) && DiffLaidOut();
}
inline size_t LinkCount() { return DiffOnShow() ? Diff()->links().size() : 0; }
// Where the `i`-th link's words are, in the text view's coordinates.
inline bool LinkPoint(size_t i, double* x, double* y) {
    if (!Diff() || i >= Diff()->links().size()) return false;
    GtkTextView* tv = GTK_TEXT_VIEW(Diff()->textView());
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(Diff()->buffer(), &it, Diff()->links()[i].start + 2);
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv, &it, &r);
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &wx, &wy);
    *x = wx + 1;
    *y = wy + r.height / 2.0;
    return true;
}
inline std::string LinkText(size_t i) {
    if (!Diff() || i >= Diff()->links().size()) return "";
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(Diff()->buffer(), &a, Diff()->links()[i].start);
    gtk_text_buffer_get_iter_at_offset(Diff()->buffer(), &b, Diff()->links()[i].end);
    char* t = gtk_text_buffer_get_text(Diff()->buffer(), &a, &b, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}
// A click on the `i`-th link, for real.
inline void ClickLink(size_t i) {
    double x = 0, y = 0;
    GCHECK(LinkPoint(i, &x, &y));
    MCGuiClick(Diff()->textView(), x, y);
}
// The point of a character of the diff's text, in the view's coordinates.
inline void PointOf(int offset, double* x, double* y) {
    GtkTextView* tv = GTK_TEXT_VIEW(Diff()->textView());
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(Diff()->buffer(), &it, offset);
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv, &it, &r);
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &wx, &wy);
    *x = wx + 1;
    *y = wy + r.height / 2.0;
}

// ---- the window
// A window of the suite's own on `repo`, the panel shown by Ctrl+Shift+G.
inline void OpenRepoWindow(const std::string& repo, bool showPanel) {
    MCGuiStep("a window on " + repo.substr(repo.find_last_of('/') + 1), [repo] {
        Repo() = repo;
        const size_t before = MCGuiAppWindows().size();
        MCGuiOpenWindow(repo);
        GCHECK(MCGuiAppWindows().size() == before + 1);
        MCGuiSetKeyWindow(nullptr);
        // Its panel exists before it is first shown (autofetch, the commands).
        GCHECK(Panel() != nullptr);
        GCHECK(!Panel()->active());
    });
    MCGuiWait("drawn", 10, [] { return gtk_widget_get_mapped(GTK_WIDGET(RepoWindow().window)); });
    if (!showPanel) return;
    MCGuiStep("Ctrl+Shift+G", [] { MCGuiKey("ctrl+shift+g"); });
    MCGuiWait("the panel shows", 10, [] { return Panel() && Panel()->active(); });
}

inline void CloseRepoWindow() {
    MCGuiStep("the window closes; the real Trash back", [] {
        UseScratchTrash(false);
        if (GtkWindow* w = RepoWindow().window) gtk_window_close(w);
    });
    MCGuiWait("gone", 10, [] { return RepoWindow().window == nullptr; });
    MCGuiStep("the first window has the keyboard again", [] { MCGuiSetKeyWindow(nullptr); });
}

}  // namespace gitchecks
