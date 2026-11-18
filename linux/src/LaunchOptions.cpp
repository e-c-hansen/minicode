// LaunchOptions.cpp — see LaunchOptions.h.
#include "LaunchOptions.h"

#include <cstdio>

namespace LaunchOptions {

Parsed parse(const std::vector<std::string>& argv) {
    Parsed out;
    out.args.push_back(argv.empty() ? "mutible" : argv[0]);
    bool options = true;
    for (size_t i = 1; i < argv.size(); i++) {
        const std::string& a = argv[i];
        if (options && a == "--") {
            options = false;
            continue;
        }
        if (options && a.size() > 1 && a[0] == '-') {
            if (a == "-h" || a == "--help") out.help = true;
            else if (a == "-v" || a == "--version") out.version = true;
            else if (a == "--foreground") out.foreground = true;
            // GLib's, for a D-Bus activated start; the port's desktop entry
            // does not ask for one today, but GApplication still offers it.
            else if (a == "--gapplication-service") out.args.push_back(a);
            // The dock icon's Open Folder…: the running mutible, or this one,
            // shows the system's folder chooser (main.cpp, handleCommandLine).
            else if (a == "--open-folder") {
                out.openFolder = true;
                out.args.push_back(a);
            }
            // The dock icon's New Window: always a window of its own.
            else if (a == "--new-window") {
                out.newWindow = true;
                out.args.push_back(a);
            }
            else if (out.error.empty())
                out.error = "mutible: unknown option " + a + "\nTry 'mutible --help'.\n";
            continue;
        }
        out.args.push_back(a.size() > 1 && a[0] == '-' ? "./" + a : a);
    }
    return out;
}

bool shouldDetach(const Parsed& parsed, bool fromTerminal, const char* foregroundEnv) {
    if (parsed.help || parsed.version || !parsed.error.empty()) return false;
    if (parsed.foreground || !fromTerminal) return false;
    const std::string env = foregroundEnv ? foregroundEnv : "";
    return env.empty() || env == "0";
}

std::string helpText() {
    return "Usage: mutible [options] [path]\n"
           "\n"
           "Opens a folder or a file in mutible. With no path, opens the current folder.\n"
           "\n"
           "  mutible              open the current folder\n"
           "  mutible ~/project    open a folder, or bring forward the window showing it\n"
           "  mutible notes.md     open a file, in the window showing its folder, else in\n"
           "                        a new window on that folder\n"
           "  mutible https://example.com\n"
           "                        open the page in a browser tab of the bookmark window\n"
           "                        (several addresses, several tabs)\n"
           "\n"
           "A mutible that is already running opens the path, and the command returns\n"
           "at once. Started from a terminal, mutible returns as soon as its window is\n"
           "up and runs apart from the terminal: closing the terminal leaves it open, and\n"
           "what it prints goes nowhere, unless sent to a file.\n"
           "\n"
           "Options:\n"
           "      --open-folder  choose a folder in the system's chooser and open a window\n"
           "                     on it (the dock icon's Open Folder…)\n"
           "      --new-window   open a new window, never bringing one forward: on the\n"
           "                     path's folder, else on the front window's, else on the\n"
           "                     home folder (the dock icon's New Window)\n"
           "      --foreground   stay in the terminal until mutible quits, as any program\n"
           "                     run there does, with its output in the terminal; closing\n"
           "                     the terminal then closes mutible (MUTIBLE_FOREGROUND=1\n"
           "                     does the same)\n"
           "  -v, --version      print the version\n"
           "  -h, --help         print this help\n";
}

std::string versionText(const std::string& version) {
    return "mutible " + (version.empty() ? std::string("unknown") : version) + "\n";
}

std::string scopeName(const std::string& appId, long pid) {
    // systemd's unit name escaping: letters, digits, ':', '_' and '.' stay
    // (a '.' only past the first character), everything else is \xNN.
    std::string id;
    for (size_t i = 0; i < appId.size(); i++) {
        const unsigned char c = (unsigned char)appId[i];
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == ':' || c == '_' || (c == '.' && i > 0);
        if (plain) {
            id += (char)c;
        } else {
            char hex[8];
            std::snprintf(hex, sizeof hex, "\\x%02x", c);
            id += hex;
        }
    }
    return "app-mutible-" + id + "-" + std::to_string(pid) + ".scope";
}

}  // namespace LaunchOptions
