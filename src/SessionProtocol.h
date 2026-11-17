// SessionProtocol.h — the messages between a session's host and its
// renderers (SESSION-DESIGN.md, sections 2 and 3, stage 4). In one process
// they pass as these structs, never encoded; MINICODE_HOST=loopback (the
// GTK port's) and the protocol tests put every one through encode() and
// decode(), one JSON line each, so the wire format is exercised before any
// network carries it.
//
// The edit loop (SessionClient.h, SessionHost.h):
//   renderer → host  DocEdit {doc, base, seq, ops, step}: ops made on the
//                    host's version `base` (plus nothing unconfirmed: a
//                    renderer has one edit in flight at a time, as ot.js);
//   host → everyone  DocEdited {doc, v, by, seq, ops}: applied as version v;
//                    the sender's copy carries its seq and no ops (it has
//                    them), the others' carry the ops moved past whatever
//                    of theirs crossed it in flight.
// Undo: DocUndo goes to the host, whose answer is an ordinary DocEdited
// (with the selection to show) followed by DocUndone with the outcome.
//
// Offsets are in the receiving renderer's unit, named in its Hello (code
// points for GTK, UTF-16 for the Mac); `ops16` is the same edit in UTF-16
// for a renderer keeping a copy of the host's text (DocWatch: the GTK
// port's language server sync reads it). Pure C++17, no I/O.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Json.h"
#include "Ot.h"
#include "UndoHistory.h"

namespace Model {
class Window;
}

namespace Shared {

using DocId = std::uint64_t;
using ClientId = std::uint64_t;
using Selection = Model::UndoHistory::Selection;

enum class Kind {
    // renderer → host
    Hello,       // name (the renderer), unit
    DocOpen,     // doc, name (its path), text8 (UTF-8): the host's copy starts as it, saved
    DocReset,    // doc, text8: the text set whole anew (a file read again), history gone
    DocClose,    // doc
    DocEdit,     // doc, base, seq, ops, step
    DocUndo,     // doc, req, redo
    DocSaved,    // doc: the text as the renderer has it now was written
    DocWatch,    // doc, req, flag (on or off): ops16 with every DocEdited, the text now in DocText
    DocQuery,    // doc, req: answered by DocText (the host's text and state, for checks)
    WindowTabs,  // window, json: a window's tabs as its renderer holds them
    WindowClose, // window
    StateQuery,  // req: the whole session, answered by State
    Ping,        // req: answered by Pong once everything sent before it is done
    // host → renderer
    Welcome,     // client: the renderer's id
    DocOpened,   // doc, v (and text16 when it was open already: another renderer's)
    DocEdited,   // doc, v, by, seq, ops, ops16, flag (1 undo, 2 redo), select
    DocUndone,   // doc, req, outcome
    UndoState,   // doc, undo
    DocText,     // doc, req, v, text16, flag (dirty), undo
    State,       // req, json
    Pong,        // req
    Error,       // doc, name (what went wrong)
};

const char* kindName(Kind k);

// A renderer's undo step around edits: `id` names it (one GTK user action,
// one replaceRanges: call); the first message of a step opens it with the
// selection before, the last closes it with the selection after. `fresh`:
// the renderer saw the caret move by hand, or coalescing broken, since its
// last step, so typing starts a new step here (UndoHistory's
// breakCoalescing); the host checks the rest of the rule itself.
struct Step {
    std::uint64_t id = 0;
    std::string name = "Typing";
    bool coalesce = false;
    bool fresh = false;
    bool open = false, close = false;
    Selection before, after;
};

struct UndoInfo {
    bool canUndo = false, canRedo = false;
    std::string undoName, redoName;
    bool operator==(const UndoInfo& o) const {
        return canUndo == o.canUndo && canRedo == o.canRedo && undoName == o.undoName &&
               redoName == o.redoName;
    }
    bool operator!=(const UndoInfo& o) const { return !(*this == o); }
};

enum class Outcome { Done = 0, Nothing = 1, Refused = 2 };

struct Message {
    Kind kind = Kind::Ping;
    ClientId client = 0;        // Welcome: the renderer's id; DocEdited: who made it (0 the host)
    DocId doc = 0;
    std::uint64_t window = 0;
    std::uint64_t req = 0;
    std::uint64_t base = 0, v = 0, seq = 0;
    Model::Unit unit = Model::Unit::Utf16;
    int flag = 0;
    std::string name;
    std::string text8;          // DocOpen, DocReset
    std::u16string text16;      // DocOpened, DocText
    Ot::Ops ops, ops16;
    Step step;
    bool hasSelect = false;
    Selection select;
    Outcome outcome = Outcome::Done;
    UndoInfo undo;
    Json json;
};

// One line of JSON (no newline), and back. Texts with a surrogate missing
// its other half, which UTF-8 cannot carry, go as arrays of units.
std::string encode(const Message& m);
bool decode(const std::string& line, Message* out, std::string* error = nullptr);

// Through the encoding and back: what loopback does to every message.
Message roundTrip(const Message& m);

// A window's tabs as its renderer reports them (WindowTabs) and the host
// keeps them: its folder, the focused group, and each group's tabs in order
// (document, file, preview kind) with the one on show. When each tab was
// last used and the groups' share are left out: they move without the tabs
// changing (stage 6, which gives them to a second screen, adds them).
Json windowJson(const Model::Window& w);

}  // namespace Shared
