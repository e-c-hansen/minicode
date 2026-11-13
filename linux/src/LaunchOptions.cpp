// LaunchOptions.cpp — see LaunchOptions.h.
#include "LaunchOptions.h"

#include <cstdio>

namespace LaunchOptions {

Parsed parse(const std::vector<std::string>& argv) {
    Parsed out;
    out.args.push_back(argv.empty() ? "minicode" : argv[0]);
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
            else if (out.error.empty())
                out.error = "minicode: unknown option " + a + "\nTry 'minicode --help'.\n";
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
    return "Usage: minicode [options] [path]\n"
           "\n"
           "Opens a folder or a file in minicode. With no path, opens the current folder.\n"
           "\n"
           "  minicode              open the current folder\n"
           "  minicode ~/project    open a folder, or bring forward the window showing it\n"
           "  minicode notes.md     open a file, in the window showing its folder, else in\n"
           "                        a new window on that folder\n"
           "  minicode https://example.com\n"
           "                        open the page in a browser tab of the bookmark window\n"
           "                        (several addresses, several tabs)\n"
           "\n"
           "A minicode that is already running opens the path, and the command returns\n"
           "at once. Started from a terminal, minicode returns as soon as its window is\n"
           "up and runs apart from the terminal: closing the terminal leaves it open, and\n"
           "what it prints goes nowhere, unless sent to a file.\n"
           "\n"
           "Options:\n"
           "      --foreground   stay in the terminal until minicode quits, as any program\n"
           "                     run there does, with its output in the terminal; closing\n"
           "                     the terminal then closes minicode (MINICODE_FOREGROUND=1\n"
           "                     does the same)\n"
           "  -v, --version      print the version\n"
           "  -h, --help         print this help\n";
}

std::string versionText(const std::string& version) {
    return "minicode " + (version.empty() ? std::string("unknown") : version) + "\n";
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
    return "app-minicode-" + id + "-" + std::to_string(pid) + ".scope";
}

}  // namespace LaunchOptions
