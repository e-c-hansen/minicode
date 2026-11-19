// terminal_exit_tests.inc — the `terminal:exit` group of run_tests.cpp:
// what a terminal does when its shell ends (TerminalExit.h): `exit` and
// Ctrl+D close it, or say "Process exited" with terminal.close-on-exit
// off; a signal, or an end before the first prompt, waits for Return.
#include "TerminalExit.h"

static void testTerminalExit() {
    using namespace TerminalExit;
    GROUP("terminal:exit");
    {
        // Wait statuses as the Mac, Linux and Android encode them.
        Ending zero = fromWaitStatus(0);
        CHECK(zero.known && !zero.signalled && zero.code == 0);
        Ending three = fromWaitStatus(3 << 8);
        CHECK(three.known && !three.signalled && three.code == 3);
        Ending killed = fromWaitStatus(9);
        CHECK(killed.known && killed.signalled && killed.signal == 9);
        Ending cored = fromWaitStatus(11 | 0x80);   // SIGSEGV, core dumped
        CHECK(cored.known && cored.signalled && cored.signal == 11);
        CHECK(!fromWaitStatus(-1).known);
        CHECK(!fromWaitStatus((19 << 8) | 0x7f).known);   // stopped: no ending

        // `exit` or Ctrl+D after a prompt: closed, or "Process exited".
        Outcome close = decide(zero, true, true);
        CHECK(close.action == Action::Close && close.note.empty() && !close.error);
        CHECK(decide(three, true, true).action == Action::Close);
        CHECK(decide(Ending(), true, true).action == Action::Close);   // status unknown
        Outcome kept = decide(zero, true, false);
        CHECK(kept.action == Action::Wait && !kept.error);
        CHECK(kept.note == "Process exited. Press Return to start a new shell.");
        CHECK(decide(three, true, false).note ==
              "Process exited (code 3). Press Return to start a new shell.");

        // Killed or crashed: never closed, whatever the setting.
        for (bool setting : {true, false}) {
            Outcome crash = decide(killed, true, setting);
            CHECK(crash.action == Action::Wait && crash.error);
            CHECK(crash.note == "The shell ended (signal 9). Press Return to start a new one.");
            CHECK(decide(killed, false, setting).action == Action::Wait);
        }

        // Ended before its first prompt: waits, so a broken startup file
        // neither closes the terminal it was opened in nor loops.
        Outcome early = decide(zero, false, true);
        CHECK(early.action == Action::Wait && early.error);
        CHECK(early.note == "The shell exited as it started. Press Return to try again.");
        CHECK(decide(fromWaitStatus(1 << 8), false, true).note ==
              "The shell exited as it started (code 1). Press Return to try again.");
    }
}
