// Detach.h — `minicode` started from a terminal gives the terminal back and
// is not tied to it (PARITY.md R2), as the Mac's launcher and VS Code's
// `code` are. Before, the first `minicode` ran the app in the foreground:
// the shell waited until it quit, GNOME Terminal warned that closing the
// window would kill a running process, and closing it did, by the hang-up's
// SIGHUP.
//
// How. Called first thing from main, before GLib or GTK start a thread or
// connect to anything, so the fork is safe. Started from a terminal (a
// controlling terminal, or a terminal on a standard stream: `minicode
// >/dev/null 2>&1 </dev/null &` is still the shell's job, and was closed
// with it) and with no --foreground (or MINICODE_FOREGROUND), the process
// forks once:
//   - The child calls setsid(): a session of its own and no controlling
//     terminal, so the terminal's hang-up never reaches it. It goes on to
//     run the app. When it is the primary instance and its first window is
//     made (GtkApplication's window-added, after the command line was
//     handled), it lets go of the terminal, putting /dev/null on every
//     standard stream that is not a regular file the user sent it to (a
//     pipe would hold the shell, or end the app with SIGPIPE once its
//     reader went), and tells the parent through a pipe. It then asks
//     systemd for a transient scope of its own, app-minicode-
//     org.minicode.Editor-<pid>.scope, through the user manager's
//     StartTransientUnit on the session bus, as GNOME Shell does for apps
//     it starts; otherwise it would stay in the terminal's vte-spawn scope,
//     which VTE never stops but which then lives as long as minicode does
//     and is judged with it by systemd-oomd. A failure there is not fatal.
//   - The parent waits until the child says its window is made, then exits
//     0 and prints nothing; or until the child ends without saying so (a
//     second `minicode <path>` handed to the running one, or a failure such
//     as no display, whose message was printed to the terminal) and exits
//     with its status. After 30 seconds without either it stops waiting.
//
// GNOME Terminal (VTE 0.84) puts each shell in a vte-spawn-<uuid>.scope
// with KillMode=process and never stops it (no StopUnit or KillUnit in
// libvte or gnome-terminal-server); the scope ends when its last process
// does. What closed minicode with its terminal was only the SIGHUP, which a
// process in another session does not get.
//
// The GUI checks' build never calls this (main.cpp's hook is left out under
// MINICODE_GUITEST): the harness's app must stay its child. The `detach`
// suite runs the real build/minicode on a pseudo-terminal instead.
#pragma once

namespace Detach {

// Reads the command line (LaunchOptions.h). Prints the help, the version or
// a usage error and returns the exit status for main to return; leaves the
// terminal as above and returns the parent's exit status, in the process
// that stays in the terminal. Returns -1 where the app should run: the
// detached child, or a run with no terminal or asked to stay. `*argc` and
// `*argv` then hold what GApplication should get, without the options.
int start(int* argc, char*** argv);

}  // namespace Detach
