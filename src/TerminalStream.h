// TerminalStream.h — pure C++. Turns the raw bytes a shell writes to its pty
// into display text plus shell-integration events. Not a screen emulator:
// cursor movement and colors are dropped, but the stream is parsed properly
// (sequences split across reads, partial UTF-8) rather than regex-stripped.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

struct TermEvent {
    enum Kind {
        Text,            // printable UTF-8 text; newlines are "\n"
        CarriageReturn,  // a lone \r: later text overwrites the current line
        Backspace,       // \b: move back over one character on this line
        PromptStart,     // OSC 133;A — the shell is ready for a command
        CommandStart,    // OSC 133;C — a command began running
        CommandEnd,      // OSC 133;D[;status] — it finished
        Directory,       // OSC 7 — the shell's working directory changed
    };
    Kind kind;
    std::string text;    // Text: the characters. Directory: the decoded path.
    int status = 0;      // CommandEnd: exit status (0 if not reported)
};

class TerminalStream {
public:
    // Feed the next chunk of output. Anything incomplete at the end of the
    // chunk (an escape sequence, half a UTF-8 character) is held back and
    // finished by a later call.
    std::vector<TermEvent> feed(const char* data, size_t length);
    std::vector<TermEvent> feed(const std::string& s) {
        return feed(s.data(), s.size());
    }

private:
    enum class State { Ground, Escape, EscapeIntermediate, Csi, Osc, OscEscape,
                       String, StringEscape };
    State state_ = State::Ground;
    bool pendingCR_ = false;   // saw \r; decide once the next byte arrives
    std::string text_;         // printable bytes not yet emitted
    std::string osc_;          // body of the OSC sequence being read

    void flushText(std::vector<TermEvent>& out, bool final);
    void finishOsc(std::vector<TermEvent>& out);
};
