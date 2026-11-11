// TerminalAttention.cpp — see TerminalAttention.h.
#include "TerminalAttention.h"

#include <chrono>

#include "ControlProtocol.h"

namespace TerminalAttention {
namespace {
Attention::Board gBoard;
std::function<void()> gChanged;

void changed() {
    if (gChanged) gChanged();
}

double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

bool apply(int terminal, const Json& args, std::string* error) {
    const ClaudeHooks::Action action = ClaudeHooks::actionNamed(args["state"].asString());
    if (action == ClaudeHooks::Action::None) {
        if (error) *error = "state is waiting, done or clear";
        return false;
    }
    Attention::Session s;
    s.sessionId = args["session_id"].asString();
    s.transcriptPath = args["transcript_path"].asString();
    s.cwd = args["cwd"].asString();
    // Whoever sent it, a message is shown as a note's text is: one line.
    const std::string message = ControlChannel::cleanNoteText(args["message"].asString());
    if (gBoard.apply(terminal, action, message, s, now())) changed();
    return true;
}

void typed(int terminal) {
    if (gBoard.typed(terminal)) changed();
}

void forget(int terminal) {
    if (gBoard.forget(terminal)) changed();
}

Attention::Mark mark(int terminal) { return gBoard.mark(terminal); }

bool sessionOf(int terminal, std::string* sessionId, std::string* transcript, double* time) {
    const Attention::Session* s = gBoard.session(terminal);
    if (!s || s->empty()) return false;
    if (sessionId) *sessionId = s->sessionId;
    if (transcript) *transcript = s->transcriptPath;
    if (time) *time = gBoard.sessionTime(terminal);
    return true;
}

int terminalFor(const std::string& sessionId, const std::string& transcript) {
    return gBoard.terminalFor(sessionId, transcript);
}

const char* markName(int terminal) {
    switch (gBoard.mark(terminal)) {
    case Attention::Mark::Waiting: return "waiting";
    case Attention::Mark::Done: return "done";
    case Attention::Mark::None: break;
    }
    return "";
}

std::string tooltip(int terminal) {
    switch (gBoard.mark(terminal)) {
    case Attention::Mark::Waiting: {
        const std::string m = gBoard.message(terminal);
        return m.empty() ? "Claude Code is waiting for you" : "Waiting for you: " + m;
    }
    case Attention::Mark::Done: return "Claude Code has finished";
    case Attention::Mark::None: break;
    }
    return "";
}

int waitingCount() { return gBoard.waitingCount(); }

int nextWaiting(const std::vector<int>& order, int current) {
    return gBoard.nextWaiting(order, current);
}

void setChangedHandler(std::function<void()> handler) { gChanged = std::move(handler); }

}  // namespace TerminalAttention
