// TerminalAttention.h — "Waiting for you" on Linux (PARITY.md item 3.6; the
// Mac's TerminalAttention.mm). One AttentionBoard for every window, kept by
// terminal id (a window has one terminal here, so its id is the window's
// control id). `minicode notify` marks a terminal, typing in it or closing it
// clears the mark, and the app draws the board: a dot over the terminal with
// a tooltip, a count in the window titles, and the "Next Waiting Terminal"
// command. This file holds the board and nothing of GTK.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "AttentionBoard.h"
#include "Json.h"

namespace TerminalAttention {

// A notify request's arguments (state, message, session_id, ...) for the
// terminal they name. False, with `error`, for a state it does not know.
bool apply(int terminal, const Json& args, std::string* error);
// The user typed in the terminal, or it closed.
void typed(int terminal);
void forget(int terminal);

Attention::Mark mark(int terminal);
// The session the terminal's last notify named, and when; false when none.
bool sessionOf(int terminal, std::string* sessionId, std::string* transcript, double* time);
// The terminal a session runs in (by id, else transcript); 0 for none.
int terminalFor(const std::string& sessionId, const std::string& transcript);
// "waiting", "done" or "" (no mark).
const char* markName(int terminal);
// The dot's tooltip: Claude's message for a waiting terminal.
std::string tooltip(int terminal);
int waitingCount();
// The next waiting terminal after `current` in `order`, 0 for none.
int nextWaiting(const std::vector<int>& order, int current);

// Called whenever a mark or a message changed (the app redraws).
void setChangedHandler(std::function<void()> handler);

}  // namespace TerminalAttention
