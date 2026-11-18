// TermDockRecord.h — a window's terminal dock as its record on the session's
// host keeps it (stage 6; SessionProtocol.h's windowJson, SessionClient.h's
// reattachFrom): beside "terms", the flat list of the host terminals the
// dock shows, "termLayout" says how they sit, so a window reattaching puts
// split panes back as splits rather than one tab each. Both apps write and
// read it the same way; a record from an app before it has "terms" alone,
// and an app before it ignores the key.
//
//   "termLayout": {
//     "tabs": [
//       {"columns": [[11], [12, 13]],          // terminal ids: columns left to
//                                              // right, each top to bottom
//        "columnShares": [0.6, 0.4],           // each column's share of the width
//        "rowShares": [[1], [0.5, 0.5]],       // each pane's share of its column
//        "activeColumn": 1, "activeRow": 0},   // the pane with the keyboard
//       {"columns": [[14]], "columnShares": [1], "rowShares": [[1]],
//        "activeColumn": 0, "activeRow": 0}
//     ],
//     "activeTab": 0                           // the dock's tab on show
//   }
//
// The shape is TerminalLayout::Layout's, one level deep as both apps' docks
// are (a split to the right makes a column, a split down stacks a pane in
// its column), and the same keys as a terminal tab in the window list
// (TabSessions.h) with ids where that has panes. Shares are written to four
// places and read back normalized; ones that do not match their panes (or
// do not read) give equal shares.
//
// Reading (`termDockFrom`) keeps only the terminals the window took
// (Reattach::terms) and each once: a pane naming a terminal that is gone
// (its shell exited while no window showed it) or another window's drops
// out, its share given to its neighbour as closing it would
// (TerminalLayout::removePane), the others keeping their places; a tab left
// with none goes. The pane with the keyboard, if it went, is the one the
// keyboard would move to (the pane above it, else below; else the column
// before, else after). Taken terminals the layout does not place (a record
// with no layout, or none at all) follow as a tab each, in their order.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "Json.h"
#include "TerminalLayout.h"

namespace Shared {

struct TermDock {
    struct Tab {
        std::vector<std::vector<std::uint64_t>> columns;   // ids, [column][row]
        TerminalLayout::Layout layout;                     // shares, matching columns
        int activeColumn = 0, activeRow = 0;
    };
    std::vector<Tab> tabs;
    int activeTab = 0;

    // Every terminal in the order a dock makes them back: tab by tab, each
    // column left to right, top to bottom.
    std::vector<std::uint64_t> order() const;
    // Drops every pane whose id `keep` refuses, and a second pane of one
    // id, as reading does (above). Shares and active places follow.
    void prune(const std::function<bool(std::uint64_t)>& keep);
};

// The record's "termLayout": panes with id 0 (a terminal not on the
// window's host) dropped first. Null when no pane is left.
Json termLayoutJson(TermDock dock);

// The dock a window reattaching builds from its record's tabs (Reattach::
// tabs, null for none) and the terminals it took (Reattach::terms' ids, in
// their order): every one of `taken` placed exactly once, nothing else.
TermDock termDockFrom(const Json& recordTabs, const std::vector<std::uint64_t>& taken);

}  // namespace Shared
