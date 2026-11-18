// DesktopMode.h — mutible as the whole desktop (linux/desktop/README.md):
// the session that linux/desktop/mutible-desktop-setup.sh installs runs
// `cage -s -- mutible --foreground` on tty1 with MUTIBLE_DESKTOP=1, and with
// no desktop environment around it the app takes on a little of one:
//   - its windows have no title bar (adjustWindow), cage showing one at a
//     time, full screen;
//   - closing the last window opens a fresh one on the home folder rather
//     than quitting (main.cpp's onWindowRemoved asks keepsLastWindow);
//   - the File menu ends with Suspend…, Restart…, Power Off… and Log Out…,
//     each asking first. Suspend goes to logind at once. Restart and Power
//     Off quit as File > Quit does, every window asking about its unsaved
//     tabs, and logind is asked only once the app is really going
//     (atShutdown, from GApplication's shutdown); a window kept open drops
//     the request (quitCancelled). Log Out only quits: the session ends with
//     cage, and systemd starts it again.
// logind is reached on the system bus (org.freedesktop.login1.Manager), never
// interactively: polkit's defaults let the active local session suspend,
// restart and power off with no password while no other user is logged in.
// Can<Method> is asked first, so a "challenge" or "na" is said in the status
// bar rather than after the windows closed.
//
// The GUI checks (the desktop_mode suite) set MUTIBLE_DESKTOP through
// EnvLock and replace the bus call with setLogindCall; in the checks' build
// the real call is never compiled in, so no run can suspend the machine.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace DesktopMode {

// MUTIBLE_DESKTOP=1 in the environment (read each time: the checks set it).
bool active();

struct Host {
    std::function<GtkWindow*()> front;                    // the window to ask over
    std::function<void()> quit;                           // File > Quit
    std::function<void(const std::string&)> note;         // the status bar
};

// The actions app.desktop-suspend, -restart, -poweroff and -logout, enabled
// while active(). Once, with the application's other actions.
void install(GtkApplication* app, Host host);
// A new window: without a title bar in desktop mode, since cage shows one
// window at a time, full screen, and has nothing to minimize it to.
void adjustWindow(GtkWindow* window);
// A section at the end of the File menu holding the four items while
// active(), empty otherwise.
void extendFileMenu(GMenu* file);
// The actions and the section follow active() again.
void refresh();

// Closing the last window keeps the app (and opens a window on the home
// folder) when in desktop mode and not quitting.
bool keepsLastWindow(bool quitting);
// A quit stopped at a window kept open.
void quitCancelled();
// The application is going: a Restart or Power Off asked for is sent now,
// synchronously, since the main loop has stopped.
void atShutdown();

// The call to logind: `method` is Suspend, Reboot, PowerOff or one of
// CanSuspend, CanReboot, CanPowerOff; `done` gets the answer of a Can call
// ("yes", "no", "challenge", "na") or "" and the error, "" when it worked.
// A null `done` makes the call synchronous. Tests set their own; an empty
// function puts the real one back.
using Done = std::function<void(const std::string& answer, const std::string& error)>;
using Call = std::function<void(const std::string& method, Done done)>;
void setLogindCall(Call call);

// What is waiting for the app to quit: "", "Reboot", "PowerOff" or "LogOut".
std::string pending();

}  // namespace DesktopMode
