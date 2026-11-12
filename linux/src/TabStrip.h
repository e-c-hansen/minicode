// TabStrip.h — the row of open files above the editor, the GTK form of the
// Mac's MCTabStrip (src/TabStrip.h). It only shows the tabs and reports what
// the pointer did; its owner keeps the tabs (Editor.h, Tabs.cpp, and the
// rules in ../src/EditorTabs.h).
//
// Each tab is a title, ellipsized in the middle and in italics for the
// preview tab, and a close button that is a dot while the file has unsaved
// changes. A press on a tab selects it, as in a browser; a double-click on
// its title keeps a preview tab; its close button or a middle click closes
// it; a right click opens the menu its owner builds. The row scrolls
// sideways when the tabs do not fit, keeping the active one in sight. Tabs
// are not dragged to reorder, as on the Mac, whose editor strip leaves that
// to its terminal tabs.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class TabStrip {
public:
    struct Item {
        std::string title;
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

    // A tab's widget, its title and its close button, for the checks to
    // press; null out of range.
    GtkWidget* tabWidget(int i) const;
    GtkWidget* titleWidget(int i) const;
    GtkWidget* closeWidget(int i) const;
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
    // in the row after the move. Off unless set; the editor's strip leaves
    // it unset, as the Mac's does, and the browser's sets it.
    std::function<void(int from, int to)> onMove;
    // What a drop at `rowX` (in the row's coordinates) does to tab `from`,
    // as if the pointer had been let go there; for the checks.
    void dropAt(int from, double rowX);

private:
    struct Tab {
        GtkWidget* box = nullptr;
        GtkWidget* title = nullptr;
        GtkWidget* close = nullptr;
    };
    Tab makeTab();
    int indexOf(GtkWidget* box) const;
    void later(std::function<void(int)> cb, int index);
    void showMenu(int index, double x, double y);
    void dropMenu();
    void keepActiveInSight();
    static void onPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static void onReleased(GtkGestureClick* g, int n, double x, double y, gpointer self);

    GtkWidget* root_ = nullptr;     // box: the scroller, and the menu while it is up
    GtkWidget* scroller_ = nullptr;
    GtkWidget* row_ = nullptr;      // the tabs, side by side
    GtkWidget* menu_ = nullptr;
    std::vector<Tab> tabs_;
    std::vector<Item> items_;
    int active_ = -1;
    int pressClose_ = -1;           // the close button a press went down on
    int pressMiddle_ = -1;          // the tab a middle press went down on
    int dragFrom_ = -1;             // the tab a primary press went down on
    double dragX_ = 0;              // where, in the row's coordinates
    guint sightIdle_ = 0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};
