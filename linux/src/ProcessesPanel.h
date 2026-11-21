// ProcessesPanel.h — the Processes panel (PARITY.md item 3.8, part two; the
// Mac's MCProcessesPanel in mac/src/ProcessesPanel.mm). It takes the file tree's
// place in the sidebar as the Source Control panel does (main.cpp's
// act_toggle_processes). Its rows come from ProcessMonitor::view for this
// window, grouped as the core's buildView says: each terminal, orphans (their
// terminal closed), "not started here". A row has the program's name, its
// listening ports, footprint and CPU, the folder it runs in, and a tooltip
// with the whole command line. Stop is in the row's menu and on Delete, and
// asks first for anything the Mac asks about (not an orphan). Looks come
// every 2 s only while the panel shows (ProcessMonitor::setPanelShown).
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

#include "ProcessTable.h"
#include "Settings.h"

class ProcessesPanel {
public:
    ProcessesPanel();
    ~ProcessesPanel();

    GtkWidget* widget() const { return root_; }
    GtkWindow* hostWindow = nullptr;
    // This window's id (its terminal's id in the monitor) and folder, both as
    // the monitor was told them (the folder as realpath spells it).
    std::function<int()> windowId;
    std::function<std::string()> folder;
    std::function<void(const std::string&)> onNote;   // the status bar
    std::function<void(const std::string&)> onOpenUrl;      // the user's page in the browser pane
    std::function<bool(int)> onRevealTerminal;              // by terminal id; false when it is gone

    // Shown in the sidebar or not: only then do looks come every 2 s.
    void setActive(bool active);
    void focus();
    void refresh();                    // rebuild from the monitor's last look
    // The terminal's close question's Show in Processes (the Mac's
    // revealTerminal:job:): selects process `job` (a foreground process
    // group's leader; 0 for none), else the first process still running in
    // terminal `terminal`'s group, else its first, and scrolls to it. A job
    // not yet seen waits for a look that has it, up to 3 s, then falls back.
    void revealTerminal(int terminal, int job);
    // The pid of the selected process row, 0 when none (for the checks).
    int selectedPid() const;

    // What the rows say, for the checks.
    struct RowInfo {
        bool header = false;
        std::string text;      // a group's title, or the row's name
        std::string detail;    // command and folder line
        std::vector<int> portNumbers;
        std::string ports, memory, cpu, tooltip;
        std::string label, stopLabel;   // for a screen reader: the row, and its Stop
        int pid = 0;
        bool orphan = false, outside = false, alive = true;
        int terminal = 0;
    };
    std::vector<RowInfo> rows() const { return rows_; }
    // The row's right click menu for `pid` (pops up); the check then
    // activates "procs.stop" on actionGroup(), as the menu item would.
    void openMenuFor(int pid);
    // The label the row for `pid` carries for a screen reader, as set on its widget
    // (GTK has no getter for it; "" when no row), and its description (the Stop).
    std::string rowAccessibleLabel(int pid) const;
    std::string rowAccessibleDescription(int pid) const;
    GActionGroup* actionGroup() const { return G_ACTION_GROUP(actions_); }
    std::string menuLabels() const { return menuLabels_; }

private:
    GtkWidget* root_ = nullptr;
    GtkWidget* summary_ = nullptr;
    GtkWidget* list_ = nullptr;
    GtkWidget* menu_ = nullptr;
    GSimpleActionGroup* actions_ = nullptr;
    std::vector<RowInfo> rows_;
    std::vector<Procs::Key> keys_;     // per row, parallel to rows_
    int menuPid_ = 0;
    bool active_ = false;
    unsigned timer_ = 0;
    unsigned seen_ = 0;
    int revealTerminal_ = 0, revealJob_ = 0;
    gint64 revealUntil_ = 0;          // monotonic microseconds; 0 when none waits
    gint64 revealFallbackAt_ = 0;     // from then the group's first process will do
    std::string menuLabels_;

    void rebuild();
    void applyReveal();
    void stopPid(int pid);
    void revealOrigin(int pid);
    void popup(GMenu* menu, double x, double y);
    void dropMenu();
    static void onPressed(GtkGestureClick*, int, double, double, gpointer);
    static gboolean onKey(GtkEventControllerKey*, guint, guint, GdkModifierType, gpointer);
    static int tick(void*);
};
