// TabStrip.h — a row of tabs, the GTK form of the Mac's MCTabStrip
// (mac/src/TabStrip.h): the editor's (both groups), the terminal dock's and
// the browser's. It only shows the tabs and reports what the pointer did;
// its owner keeps the tabs (Editor.h, Tabs.cpp, and the rules in
// ../src/EditorTabs.h).
//
// Each tab is its title, in italics for the preview tab, then for files of
// one name the folders that tell them apart, dimmed and a little smaller
// (EditorTabs::tabNames), and a close button that is a dot while the file
// has unsaved changes. As VS Code's tabSizing "fit" and the Mac's strip, a
// tab keeps its natural width, 80 to 320 pixels (EditorTabs::layoutStrip),
// and is never squeezed: a title is cut in the middle only past the widest
// tab (EditorTabs::fitName). A row wider than the strip scrolls sideways
// instead, by the wheel (vertical too) or a touchpad (wheelScroll), with a
// thin bar while the pointer is over it and fades at the cut-off ends
// (clipped); the tab on show is scrolled into view (revealScroll) when it
// changes, never for a dot. A press on a tab selects it, as in a browser; a
// double-click on its title keeps a preview tab; its close button or a
// middle click closes it; a right click opens the menu its owner builds;
// dragging it along the row moves it (dropIndex), the row scrolling while
// the pointer is held near an end (dragAutoScroll).
#pragma once

#include <gtk/gtk.h>

#include "ClickCount.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class TabStrip {
public:
    struct Item {
        std::string title;
        std::string hint;       // folders after the title, dimmed; "" for none
        std::string tooltip;    // the whole path
        bool dirty = false;     // a dot in place of the close button
        bool preview = false;   // the preview tab: an italic title
    };

    TabStrip();
    // Takes its menu off the screen; called while the window still exists.
    ~TabStrip();
    TabStrip(const TabStrip&) = delete;
    TabStrip& operator=(const TabStrip&) = delete;

    GtkWidget* widget() const { return root_; }

    // The tabs as they are now, and which is active (-1 for none).
    void setItems(const std::vector<Item>& items, int active);
    int count() const { return (int)items_.size(); }
    int activeIndex() const { return active_; }
    const Item& item(int i) const { return items_[(size_t)i]; }

    // A tab's widget, its title, its hint and its close button, for the
    // checks to press; null out of range.
    GtkWidget* tabWidget(int i) const;
    GtkWidget* titleWidget(int i) const;
    GtkWidget* hintWidget(int i) const;
    GtkWidget* closeWidget(int i) const;
    // What tab `i` shows of its title and hint at its width (fitName): the
    // whole of them but past the widest tab.
    std::string displayedTitle(int i) const;
    std::string displayedHint(int i) const;
    // As last laid out: the row of tabs (the widget the tabs are in, as
    // wide as the strip), each tab's left edge and width in the whole row,
    // the row's width, and how far it is scrolled left (0 when it fits).
    GtkWidget* rowWidget() const { return row_; }
    double tabLeft(int i) const;
    double tabWidth(int i) const;
    double contentWidth() const { return content_; }
    double scrollOffset() const { return scroll_; }
    // The menu a right click opened, while it is up.
    GtkWidget* menu() const { return menu_; }

    // Each called once the press or click is over, from an idle, so the
    // owner may rebuild the strip.
    std::function<void(int index)> onSelect;
    std::function<void(int index)> onClose;
    std::function<void(int index)> onDoubleClick;
    // A right click on tab `index`: its menu, a new reference whose items
    // name actions widget() resolves; null for none.
    std::function<GMenuModel*(int index)> menuFor;
    // Dragging a tab along the row (EditorTabs::dropIndex): called once the
    // pointer is let go, with the tab's place now and the place it goes to
    // in the row after the move. Off unless set; the editor's strips, the
    // terminal dock's and the browser's set it.
    std::function<void(int from, int to)> onMove;
    // What a drop at `rowX` (in the whole row's coordinates) does to tab
    // `from`, as if the pointer had been let go there; for the checks.
    void dropAt(int from, double rowX);

private:
    struct Tab {
        GtkWidget* box = nullptr;
        GtkWidget* names = nullptr;   // the title and the hint
        GtkWidget* title = nullptr;
        GtkWidget* hint = nullptr;
        GtkWidget* close = nullptr;
    };
    // A tab's widths as GTK measures it now: what its whole title and hint
    // would take (`natural`), and what it takes besides them (`chrome`).
    struct Measured {
        double natural = 0, chrome = 0;
    };
    Tab makeTab();
    int indexOf(GtkWidget* box) const;
    void later(std::function<void(int)> cb, int index);
    void showMenu(int index, double x, double y);
    void dropMenu();
    Measured measure(int i) const;
    void measureRow();
    void placeScroll(int width);
    bool fitTitles(bool apply);
    void refitLater();
    void settle();
    void scrollTo(double scroll);
    void autoScroll();
    static void rowMeasure(GtkWidget* w, GtkOrientation o, int forSize, int* min, int* nat,
                           int* minBase, int* natBase);
    static void rowAllocate(GtkWidget* w, int width, int height, int baseline);
    static void onPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static void onReleased(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static gboolean onScroll(GtkEventControllerScroll* c, double dx, double dy, gpointer self);
    static void onMotion(GtkEventControllerMotion* c, double x, double y, gpointer self);

    GtkWidget* root_ = nullptr;     // box: the row
    GtkWidget* row_ = nullptr;      // the tabs at their places, clipped (rowAllocate)
    GtkWidget* fadeLeft_ = nullptr;    // over the tabs at a cut-off end
    GtkWidget* fadeRight_ = nullptr;
    GtkWidget* bar_ = nullptr;      // the scroll bar, while the pointer is over the row
    GtkWidget* menu_ = nullptr;
    std::vector<Tab> tabs_;
    std::vector<Item> items_;
    int active_ = -1;
    // The layout (EditorTabs::layoutStrip) and the scroll, as the Mac's.
    std::vector<Measured> measured_;
    std::vector<double> x_, w_;
    double content_ = 0;
    bool stale_ = true;             // a tab's size may have changed since measured_
    double scroll_ = 0;
    int view_ = 0;                  // the row's width when last laid out
    bool reveal_ = true;            // bring the tab on show into view at the next layout
    bool inside_ = false;           // the pointer is over the row: the bar shows
    guint refit_ = 0;               // an idle cutting titles anew (the font changed)
    int pressClose_ = -1;           // the close button a press went down on
    int pressMiddle_ = -1;          // the tab a middle press went down on
    ClickCount clicks_;             // double-clicks by event time
    int dragFrom_ = -1;             // the tab a primary press went down on
    double dragX_ = 0;              // where, in the whole row's coordinates
    bool dragging_ = false;         // moved past a click's slack since
    double dragViewX_ = 0;          // the pointer now, in the row widget's coordinates
    guint autoScroll_ = 0;          // the tick scrolling while a drag is held at an end
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};
