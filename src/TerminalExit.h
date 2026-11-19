// TerminalExit.h — pure C++. What a terminal does when its shell ends.
//
// `exit`, or Ctrl+D at an empty prompt, ends the shell the way the user
// meant it to: the terminal closes, as in Terminal.app, VS Code and Zed
// (terminal.close-on-exit, on by default), or with the setting off it says
// "Process exited" and waits for Return to start a new shell. A shell that
// was killed or crashed (a signal) never closes its terminal on its own:
// it says so, with the signal, and waits for Return, so a crash is not
// hidden behind a tab that vanished. Nor does a shell that ended before it
// ever showed a prompt (a broken startup file), which waits for Return
// rather than closing the terminal it was opened in or starting in a loop.
#pragma once
#include <string>

namespace TerminalExit {

// How a shell ended, from its wait status.
struct Ending {
    bool known = false;      // false: someone else reaped it, or no status came
    bool signalled = false;  // killed by a signal (SIGKILL, a crash)
    int code = 0;            // its exit code, when not signalled
    int signal = 0;          // the signal, when signalled
};

// A waitpid status (-1 when unknown), decoded as <sys/wait.h> does on the
// Mac, Linux and Android alike: the low 7 bits are the signal, 0 for an
// exit, whose code is the next byte; 0x7f is a stop, which is no ending.
inline Ending fromWaitStatus(int status) {
    Ending e;
    if (status < 0) return e;
    const int low = status & 0x7f;
    if (low == 0x7f) return e;
    e.known = true;
    if (low == 0) {
        e.code = (status >> 8) & 0xff;
    } else {
        e.signalled = true;
        e.signal = low;
    }
    return e;
}

enum class Action {
    Close,   // the terminal closes (the last one: the panel goes)
    Wait,    // it stays, with `note`, until Return starts a new shell
};

struct Outcome {
    Action action = Action::Wait;
    std::string note;    // the line the terminal shows (Wait only)
    bool error = false;  // shown as an error: a signal, or a startup failure
};

// `prompted`: the shell showed a prompt, or ran long enough that it must
// have (a shell without the integration's marks). `closeOnExit`:
// terminal.close-on-exit.
inline Outcome decide(const Ending& e, bool prompted, bool closeOnExit) {
    Outcome o;
    if (e.known && e.signalled) {
        o.note = "The shell ended (signal " + std::to_string(e.signal) +
                 "). Press Return to start a new one.";
        o.error = true;
        return o;
    }
    if (!prompted) {
        o.note = e.known && e.code != 0
                     ? "The shell exited as it started (code " + std::to_string(e.code) +
                           "). Press Return to try again."
                     : "The shell exited as it started. Press Return to try again.";
        o.error = true;
        return o;
    }
    if (closeOnExit) {
        o.action = Action::Close;
        return o;
    }
    o.note = e.known && e.code != 0
                 ? "Process exited (code " + std::to_string(e.code) +
                       "). Press Return to start a new shell."
                 : "Process exited. Press Return to start a new shell.";
    return o;
}

}  // namespace TerminalExit
