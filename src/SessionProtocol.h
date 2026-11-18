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
// Terminals (stage 5): the host owns each terminal's pty and a headless
// TerminalScreen fed from it, and forwards the pty's bytes to the renderer
// whose emulator draws them; what the user types comes back as bytes.
//   renderer → host  TermOpen {term, json: the shell (TermSpec)}, then
//                    TermInput {term, bytes}, TermResize {term, cols, rows},
//                    TermAck {term, v: bytes taken}, TermClose {term};
//                    TermAttach {term}: the terminal as it is now (replay)
//   host → renderer  TermOpened {term, v: the shell's pid}, TermOutput
//                    {term, bytes, v: the cursor's line, flag 1: on the
//                    alternate screen}, TermShell {term, json: TermShell},
//                    TermMarks {term, json: the commands}, TermExited
//                    {term, flag: wait status, name: why it never started},
//                    TermReplay {term, bytes, v}.
// Output past kTermCredit bytes not yet acknowledged stops the host reading
// the pty until the renderer catches up (the program blocks), as the Mac's
// reader stops past 1 MB.
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
    Hello,       // name (the renderer), unit; flag 1: it only reads (a browser's view):
                 // the host refuses anything from it that would change the session;
                 // flag 2: a guest (a browser's view that may edit): its documents are
                 // view seats as a reader's, and it may edit and undo them and type into
                 // terminals, nothing else that changes the session
    DocOpen,     // doc, name (its path), text8 (UTF-8): the host's copy starts as it, saved.
                 // From a renderer that only reads, a view of the document as it is: it
                 // never makes one, and keeps none open (DocGone when there is none, or
                 // when the last renderer that edits it closes it)
    DocReset,    // doc, text8: the text set whole anew (a file read again), history gone
    DocClose,    // doc
    DocEdit,     // doc, base, seq, ops, step; flag 1: outside the undo history (no step:
                 // the history moves past it, as a change made with recording off)
    DocUndo,     // doc, req, redo
    DocSaved,    // doc: the text as the renderer has it now was written
    DocForget,   // doc: the undo history forgotten, the text kept
    DocWatch,    // doc, req, flag (on or off): ops16 with every DocEdited, the text now in DocText
    DocQuery,    // doc, req: answered by DocText (the host's text and state, for checks)
    WindowTabs,  // window, json: a window's tabs as its renderer holds them
    WindowClose, // window
    StateQuery,  // req: the whole session, answered by State
    Ping,        // req: answered by Pong once everything sent before it is done
    TermOpen,    // term, json (TermSpec's fields)
    TermInput,   // term, bytes
    TermResize,  // term, cols, rows
    TermAck,     // term, v: bytes of output taken
    TermClose,   // term
    TermAttach,  // term: answered by TermReplay, TermShell and TermMarks
    // host → renderer
    Welcome,     // client: the renderer's id; v: the base of the ids it makes (kIdRange);
                 // name: the folder
    DocOpened,   // doc, v (and text16 when it was open already: another renderer's)
    DocEdited,   // doc, v, by, seq, ops, ops16, flag (1 undo, 2 redo), select
    DocUndone,   // doc, req, outcome
    UndoState,   // doc, undo
    DocText,     // doc, req, v, text16, flag (dirty), undo
    State,       // req, json
    Pong,        // req
    Error,       // doc, name (what went wrong)
    TermOpened,  // term, v: the shell's pid
    TermOutput,  // term, bytes, v: the cursor's line (TermMark's count), flag 1: alternate screen
    TermShell,   // term, json (TermShell's fields)
    TermMarks,   // term, json: {"commands": [[prompt, output, end, status], ...]}
    TermExited,  // term, flag: the shell's wait status (-1 unknown), name: why it never started
    TermReplay,  // term, bytes, v: as TermOutput
    DocGone,     // doc: to a renderer that only reads: the document is closed, or never was
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

using TermId = std::uint64_t;

// The ids a renderer makes for what it opens on the host (documents,
// terminals, windows): its Welcome's `v` is a base, a multiple of
// kIdRange, and it takes base + 1, base + 2, ... (Client::newId), so two
// renderers on one host never make the same id. The host gives each
// connection a base no live renderer has and in whose range it holds no
// id (SessionHost.cpp, chooseIdBase); ids below kIdRange are never in a
// base's range (a renderer from before bases, or a check, may use them).
// Every id stays under 2^53, which a JSON number carries exactly (the
// browser page's, SessionWebPage.cpp; Json keeps numbers as doubles).
constexpr std::uint64_t kIdRange = std::uint64_t(1) << 32;
constexpr std::uint64_t kIdSlots = std::uint64_t(1) << 21;   // bases 1 ... kIdSlots - 1 times kIdRange
constexpr std::uint64_t kMaxId = kIdSlots * kIdRange - 1;    // 2^53 - 1
// Unacknowledged output at which the host stops reading a pty, and below
// which it reads again.
constexpr long long kTermCredit = 1 << 20;
constexpr long long kTermCreditLow = 256 << 10;

// The shell a terminal runs (TermOpen's json).
struct TermSpec {
    std::string program;              // a path, or a name in PATH
    std::vector<std::string> argv;    // argv[0] included
    std::vector<std::string> env;     // the whole environment, NAME=value
    std::string cwd;
    int cols = 80, rows = 24;
    int scrollback = 10000;           // the host's screen's, as the renderer's
    std::string nonce;                // the integration's marks carry it
};
Json termSpecJson(const TermSpec& s);
TermSpec termSpecFrom(const Json& j);

// What the host knows of a terminal's shell, from the shell integration's
// marks (OSC 133 A, B, C and D, 633;E and 7, with the nonce) and the pty.
struct TermShell {
    bool known = false;       // a mark with the nonce came
    bool busy = false;        // a command started (C) and has not finished (D)
    bool atPrompt = false;    // a prompt (A) and no command since
    bool inputReady = false;  // zsh's line editor reads (B) since the prompt
    long long prompts = 0, started = 0, done = 0;
    int status = -1;          // the last finished command's, -1 before one
    std::string command;      // the running command's text (633;E)
    std::string cwd;          // the last OSC 7
    std::string title;        // the last OSC 0 or 2
    int foreground = 0;       // the pty's foreground process group, 0 unknown
    bool raw = false;         // the pty is out of canonical mode
    bool echoOff = false;     // echo off in canonical mode: a password
    bool signalsOff = false;  // ISIG off: a raw program, not zsh's line editor
    bool operator==(const TermShell& o) const;
    bool operator!=(const TermShell& o) const { return !(*this == o); }
};
Json termShellJson(const TermShell& s);
TermShell termShellFrom(const Json& j);

// A command the marks place (TerminalScreen's TermCommand), by line: lines
// counted from the first that ever went into the scrollback (the screen's
// historyBase() + line), so a renderer maps them through the cursor's line
// in TermOutput. -1 for an output or end not reached.
struct TermMark {
    long long prompt = 0, output = -1, end = -1;
    int status = 0;
};
Json termMarksJson(const std::vector<TermMark>& marks);
std::vector<TermMark> termMarksFrom(const Json& j);

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
    TermId term = 0;
    std::string bytes;          // terminal bytes, any (base64 on the wire)
    int cols = 0, rows = 0;
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
