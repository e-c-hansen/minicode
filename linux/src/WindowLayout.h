// WindowLayout.h — the panes of an editor window, the GTK form of the Mac's
// layout (EditorController+Layout.mm and +Panels.mm; PARITY.md item 1.13).
//
//   hpaned (GtkPaned)
//     [start] the sidebar: the file tree, or a panel in its place
//     [end]   the right side, laid out by hand from the core's
//             RightAreaLayout (src/RightAreaLayout.h), as the Mac's
//             relayoutRightArea lays out its PanelHost:
//               the editor's side (its find bar, tab strip and slot) on the
//               left of the top, the browser on its right with a bar between
//               them, and the terminal below both, across the whole width,
//               with a bar above it; the Problems list (PARITY.md 2.2)
//               between the top and the terminal while it shows
//
// The right side is a plain container with a custom layout manager, not
// nested GtkPaneds: a paned draws no handle once a child is hidden, so the
// terminal could not be dragged back down from the top, and a side hidden
// by a drag could not come back in the same drag, as the Mac's do. The
// rules (each side's share and minimum, the smallest top, a bar dragged
// within 16 px of an edge hiding that side) are the core's, with tests; this
// class shows them. One gesture on the container, in the capture phase as
// GtkPaned's own, takes a press on a bar and leaves every other press alone.
//
// Each toggle hides or shows its own pane. With the editor, the browser and
// the terminal all hidden the right side has nothing to show, and the
// sidebar's divider goes to the right edge, as the Mac's
// collapseRightAreaIfEmpty does: the file tree has the window, and nothing
// the user hid comes back by itself. Anything that puts a file in front of
// the user (opening one, Focus Editor, the tabs' keys) calls revealEditor.
#pragma once

#include <gtk/gtk.h>

#include <functional>

#include "RightAreaLayout.h"

class WindowLayout {
public:
    // The panes, which the layout holds from now on: the sidebar, the
    // editor's side, the tab strip inside it (for the smallest top), and
    // the browser and the terminal, null in a build without them.
    WindowLayout(GtkWidget* sidebar, GtkWidget* editorPane, GtkWidget* tabStrip,
                 GtkWidget* browserPane, GtkWidget* terminalPane);
    // Called while the window's widgets still exist (main.cpp's
    // onWindowRemoved): the widgets stop calling into this object.
    ~WindowLayout();
    WindowLayout(const WindowLayout&) = delete;
    WindowLayout& operator=(const WindowLayout&) = delete;

    // What goes in the window.
    GtkWidget* widget() const { return hpaned_; }

    // Where the keyboard goes when the pane it was in hides, and what to do
    // when a pane shows or hides (the hints follow); set by the window.
    std::function<void()> focusEditor, focusBrowser, focusTerminal, focusTree;
    std::function<void()> onChanged;

    bool editorShown() const { return !state_.editorHidden; }
    bool browserShown() const { return RightArea::shows(state_, RightArea::Pane::Browser, parts_); }
    bool terminalShown() const { return RightArea::shows(state_, RightArea::Pane::Terminal, parts_); }
    bool sidebarShown() const;
    // The sidebar's divider at the right edge: the right side has no room.
    bool rightCollapsed() const;

    // Toggle Editor, Toggle Browser, Toggle Terminal and Toggle Sidebar,
    // each its own pane only, as the Mac's toggleEditor:, toggleBrowser:,
    // toggleTerminal: and toggleSidebarVisibility:.
    void toggleEditor();
    void toggleBrowser();
    void toggleTerminal();
    void toggleSidebar();
    // The editor shown, and the right side opened if it was collapsed.
    void revealEditor();
    // The browser shown, for a link opened in it; the keyboard stays.
    void showBrowser();
    // The browser hidden, for Close Tab with the browser's keyboard; the
    // keyboard goes to what is left.
    void hideBrowser();
    // The terminal hidden, for the last terminal tab closing.
    void hideTerminal();
    // The right side opened again if it was dragged or collapsed shut.
    void showRightArea();
    // The keyboard to what is left after a pane closed (the Mac's
    // focusAfterClosingPanel).
    void focusAfterClosingPanel();

    // Maximize Pane (PARITY.md 1.11, the Mac's Ctrl+Cmd+M): the pane with
    // the keyboard (the browser or the terminal; anywhere else the editor
    // if it shows, else whichever does) takes the whole right side, with
    // no bars; again, and everything is back where it was. False when no
    // pane shows. Showing another pane ends it (each toggle, revealEditor,
    // showBrowser), and a toggle of a pane hidden only by it shows that
    // pane rather than closing it, as on the Mac.
    bool toggleMaximized();
    bool maximized() const { return RightArea::maximizedNow(state_, nullptr, parts_); }
    // `pane` was shown or focused: Maximize Pane ends if it holds another.
    bool leaveMaximizedFor(RightArea::Pane pane);

    // The Problems list (ProblemsPanel.h), which the layout holds from now
    // on: between the top and the terminal at a fixed share of the height,
    // as the Mac's relayoutRightArea places it; alone at the bottom it takes
    // it all. Hidden while a pane is maximized.
    void setProblemsPane(GtkWidget* problems);
    void setProblemsShown(bool shown);
    bool problemsShown() const { return problemsShown_; }

    // The terminal's bar moved `delta` points up (negative: down), within the
    // limits, as a drag of it (the Mac's onResizeDock); false with no bar.
    bool nudgeTerminalBar(double delta);

    // For the checks (tests/gui/AppSeam.h).
    GtkWidget* area() const { return area_; }
    GtkWidget* sidebar() const { return sidebar_; }
    GtkWidget* editorPane() const { return editor_; }
    GtkWidget* browserPane() const { return browser_; }
    GtkWidget* terminalPane() const { return terminal_; }
    GtkWidget* browserBar() const { return browserBar_; }
    GtkWidget* terminalBar() const { return terminalBar_; }
    const RightArea::State& state() const { return state_; }
    const RightArea::Limits& limits() const { return limits_; }

    // How far a bar's grab area reaches past its drawn line on each side.
    static constexpr int kGrab = 4;
    // The gap a bar keeps between two panes: the port's 4 px divider.
    static constexpr int kBar = 4;

private:
    enum class Dragging { None, Browser, Terminal };

    // Visibility from the state, a new layout, and the hints told.
    void apply();
    // The right side empty: the sidebar's divider to the right edge.
    void collapseRightAreaIfEmpty();
    // Each pane's smallest size, measured, and the limits drawn from them.
    RightArea::Minimums measureMinimums() const;
    RightArea::Limits limitsFor(const RightArea::Minimums& m) const;
    void allocateArea(int width, int height);
    bool focusInside(GtkWidget* pane) const;
    // The Problems list's height in a right side `height` tall (0 when it
    // does not show), the Mac's rule.
    double problemsHeight(double height) const;
    // A bar's rectangle grown by its grab area, in the right side's
    // coordinates, or false when it does not show.
    bool grabRect(Dragging which, GdkRectangle* out) const;
    // A drag moved the bar to `pos`; the keyboard leaves what it hid.
    void dragTo(double pos);

    static void areaMeasure(GtkWidget* w, GtkOrientation o, int forSize, int* min, int* nat,
                            int* minBase, int* natBase);
    static void areaAllocate(GtkWidget* w, int width, int height, int baseline);
    static void onDragBegin(GtkGestureDrag* g, double x, double y, gpointer self);
    static void onDragUpdate(GtkGestureDrag* g, double dx, double dy, gpointer self);
    static void onDragEnd(GtkGestureDrag* g, double dx, double dy, gpointer self);
    static void onHpanedPosition(GObject*, GParamSpec*, gpointer self);
    static void onHpanedPressed(GtkGestureClick*, int, double, double, gpointer self);

    GtkWidget* hpaned_ = nullptr;
    GtkWidget* sidebar_ = nullptr;
    GtkWidget* area_ = nullptr;
    GtkWidget* editor_ = nullptr;
    GtkWidget* strip_ = nullptr;
    GtkWidget* browser_ = nullptr;
    GtkWidget* terminal_ = nullptr;
    GtkWidget* browserBar_ = nullptr;
    GtkWidget* terminalBar_ = nullptr;
    GtkWidget* problems_ = nullptr;
    bool problemsShown_ = false;
    double problemsH_ = 0;   // as of the last layout
    GtkGesture* drag_ = nullptr;

    RightArea::State state_;
    RightArea::Parts parts_;
    RightArea::Limits limits_;   // as of the last layout
    RightArea::Frames frames_;   // the last layout

    Dragging dragging_ = Dragging::None;
    double dragStart_ = 0;   // the press, along the bar's axis
    double dragGrab_ = 0;    // the press minus the bar's middle

    int sidebarWidth_ = 240;      // the sidebar's width before the right side collapsed
    bool sidebarVisible_ = true;
    bool adjustingPaned_ = false;   // a snap is moving the divider; ignore its notify
};
