// RightAreaLayout.h — how the right side of an editor window is shared
// between the editor's slot, the browser and the terminal dock, rule for
// rule as the Mac lays it out by hand (EditorController+Layout.mm:
// relayoutRightArea, terminalBarDraggedTo:, browserBarDraggedTo:,
// toggleEditor:, collapseRightAreaIfEmpty).
//
// The top holds the editor (or whatever has its slot) on the left and the
// browser on the right, the browser at its remembered share of the width
// and neither below its minimum while both fit. The terminal docks below
// both, across the whole width, at its remembered height, never so tall
// that the top is under its smallest height (the tab strip and 40 points).
// Either part of the top can be hidden and the other takes the top's whole
// width; with the top empty the terminal takes the whole height, its bar
// still at the top edge so it can be dragged back down. A bar dragged
// within `snap` points of an edge hides the side it went into, and dragging
// back out in the same drag brings that side back. Each toggle hides or
// shows its own pane and nothing else: with all three hidden there is
// nothing to show (`empty`), and the window then collapses the right side
// so the file tree has the window; nothing comes back by itself.
//
// Plain C++ with no I/O, unit-tested (`rightarea:*`). The GTK port lays its
// window out from it (linux/src/WindowLayout.cpp), with a gap of `bar`
// points between panes for its bars; the Mac keeps its own code, whose bars
// overlap the panes (bar 0), and which this follows.
#pragma once

namespace RightArea {

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;   // y from the top
};

// What the user asked to see, and the sizes remembered for when it shows.
struct State {
    bool editorHidden = false;      // Toggle Editor, or a bar dragged over it
    bool browserVisible = false;
    bool terminalVisible = false;
    double browserFraction = 0.5;   // the browser's share of the width
    double terminalHeight = 220;    // the dock's height while the top shows
};

struct Limits {
    double minTop = 70;              // the smallest top: the tab strip and 40
    double minEditorWidth = 150;     // beside the browser
    double minBrowserWidth = 150;    // beside the editor
    double minTerminal = 80;
    double snap = 16;                // a bar this close to an edge hides that side
    double bar = 0;                  // the gap a bar keeps between two panes
};

// The panes this window has at all: a build without the browser or the
// terminal has neither to show, whatever the state says.
struct Parts {
    bool browser = true;
    bool terminal = true;
};

enum class Pane { Editor, Browser, Terminal };

// Whether a pane shows.
bool shows(const State &s, Pane p, const Parts &parts = {});
// The editor or the browser shows.
bool topShown(const State &s, const Parts &parts = {});
// Nothing shows: the window collapses its right side.
bool empty(const State &s, const Parts &parts = {});

// Toggle Editor, Toggle Browser or Toggle Terminal: that pane alone hides
// or shows. Whether it shows now.
bool toggle(State &s, Pane p);

// Where everything goes in a right side `width` by `height` points.
// Rectangles of panes that do not show are empty.
struct Frames {
    bool editor = false, browser = false, terminal = false;
    bool browserBar = false;    // between the editor and the browser
    bool terminalBar = false;   // above the terminal
    Rect editorRect, browserRect, terminalRect, browserBarRect, terminalBarRect;
};
Frames layout(const State &s, double width, double height, const Limits &lim,
              const Parts &parts = {});

// The smallest size at which every pane on show gets its minimum, given
// each pane's own smallest size as measured (0 where it has none).
struct Minimums {
    double editorWidth = 0, editorHeight = 0;
    double browserWidth = 0, browserHeight = 0;
    double terminalWidth = 0, terminalHeight = 0;
};
void minimumSize(const State &s, const Limits &lim, const Minimums &m, double *width,
                 double *height, const Parts &parts = {});

// What a drag hid, so that the caller can move the keyboard to what is left.
enum class Hid {
    Nothing,
    Editor,    // the browser bar went to the left edge
    Browser,   // the browser bar went to the right edge
    Top,       // the terminal bar went to the top: the editor and the browser
};

// The bar between the editor and the browser dragged so that its middle is
// `x` points from the right side's left edge.
Hid dragBrowserBar(State &s, double x, double width, const Limits &lim, const Parts &parts = {});
// The terminal's bar dragged so that its middle is `y` points from the
// right side's top edge (the Mac measures from the bottom: its y is
// height - y here).
Hid dragTerminalBar(State &s, double y, double height, const Limits &lim,
                    const Parts &parts = {});

}  // namespace RightArea
