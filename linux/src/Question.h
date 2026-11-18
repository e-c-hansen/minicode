// Question.h — a question or notice in a window of its own over the app's,
// for the ones whose text holds a path, a command or a list of files
// (GtkAlertDialog's place there). GtkAlertDialog wraps its text at word
// breaks only, so a path longer than a line was one word as wide as itself:
// with a build in a long worktree path the hooks question, which names the
// program four times, was 1,083 px wide on the harness's 1,024 px display
// and its Cancel was off the screen (claude_hooks_offer, 2026-11-18). Here
// the text wraps at word breaks and, where a word does not fit, inside it,
// at most about 60 characters a line; the message and detail, and the
// preview under them (the Mac's accessory box: monospaced, selectable, at
// most 260 points tall), scroll past heights that leave the buttons on the
// screen. Return answers the default button, Esc and
// closing the window the cancel button; the answer comes once, after the
// window has gone, and a window closed with its parent answers the cancel
// button on the next turn of the main loop.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace Question {

struct Spec {
    std::string message;               // the question, in bold
    std::string detail;                // under it; "" for none
    std::string preview;               // monospaced in a box under the detail; "" for none
    std::vector<std::string> buttons;  // left to right
    int defaultButton = 0;             // Return
    int cancelButton = -1;             // Esc and closing the window; -1 is the last button
};

// Puts the question up over `parent` (modal, closed with it) and calls
// `answered` with the index of the button chosen.
void ask(GtkWindow* parent, const Spec& spec, std::function<void(int)> answered);

// A notice: `message` and `detail` with one OK, answered (`then`, may be
// empty) however it is closed.
void tell(GtkWindow* parent, const std::string& message, const std::string& detail,
          std::function<void()> then = nullptr);

}  // namespace Question
