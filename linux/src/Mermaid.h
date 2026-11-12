// Mermaid.h — ```mermaid blocks of the Markdown preview drawn by the user's
// mermaid-cli (`mmdc`, or $MINICODE_MMDC), as the Mac's MarkdownMermaid.mm
// does. The decisions (which runs, the key, the command line, the complaint's
// wording) are the core's MermaidDiagram; this is the running of mmdc and the
// keeping of its pictures.
//
// One run at a time, off the main thread, each in a process group of its own
// with 90 s, a transparent PNG at scale 2 in the preview's theme. Pictures
// are kept in memory and in $MINICODE_MERMAID_CACHE or
// $XDG_CACHE_HOME/minicode/mermaid (50 MB, trimmed to 40). A failure is
// remembered for 30 s. Nothing runs unless a page asks (want()).
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <string>

namespace Mermaid {

// mmdc is found ($MINICODE_MMDC, else the port's usual program lookup).
bool available();

struct State {
    enum Kind { Ready, Waiting, Failed } kind = Waiting;
    GdkTexture* texture = nullptr;   // Ready: owned by the service, do not unref
    std::string why;                 // Failed: the complaint, short
};

// The diagram for `source` in the light or dark theme: kept (Ready), being
// drawn or queued now (Waiting), or failed lately. A diagram neither kept nor
// failed nor on its way is queued.
State want(const std::string& source, bool dark);

// Called on the main thread after each run ends, whether it drew or not.
int subscribe(std::function<void()> fn);
void unsubscribe(int id);

// Checks: forget what is kept in memory and every failure (the disk cache
// stays), and how many times mmdc was started.
void forgetMemory();
int runs();

}  // namespace Mermaid
