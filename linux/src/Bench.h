// Bench.h — the Linux side of scripts/membench-linux.sh (the Mac's is
// src/Bench.mm).
//
// Inert unless MINICODE_BENCH is set. Then, a step every couple of seconds as
// a person would click through, the first window opens MINICODE_BENCH_CPP,
// shows the terminal, and loads MINICODE_BENCH_URL in the browser pane; a
// second window opens MINICODE_BENCH_TEX, whose LaTeX preview typesets it
// (the Mac's bench does the same; one editor slot cannot show both). Last it
// prints "minicode bench: ready" on stderr.
#pragma once

#include <functional>
#include <string>

namespace Bench {

bool active();

struct Hooks {
    std::function<void(const std::string&)> openFile;       // in the first window
    std::function<void()> showTerminal;                      // first window
    std::function<void(const std::string&)> showAddress;     // browser pane, first window
    std::function<void(const std::string&)> openInNewWindow; // a second window on the file
};

// Called once, with the first window's hooks; later calls do nothing.
void start(const Hooks& hooks);

}  // namespace Bench
