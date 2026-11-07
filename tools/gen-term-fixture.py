#!/usr/bin/env python3
"""Write tests/fixtures/terminal/agent-session.bin.gz, the busy output the
terminal_draw GUI suite replays to time the grid's drawing.

A Claude Code session cannot be recorded here without writing to ~/.claude,
so this is a replay of its redraw pattern, made the way its renderer (Ink)
makes it, at 120 columns by 40 rows:

  - Each frame is a synchronized update (CSI ? 2026 h ... l). Ink erases the
    lines it drew last time (ESC [2K, then ESC [1A, once a line) and writes
    the whole live region again: the reply being streamed, the spinner line
    with its shimmering status text (each letter its own truecolor), the
    rounded input box, and the footer. Most of those lines come back
    exactly as they were; only the spinner and the newest words change.
  - When a reply or a tool's output is finished it is written above the
    live region once, as Ink's <Static> does, and the screen scrolls.
  - Then a build's colored output at full speed (the scrolling case), as
    `make` or `cargo build` in the same pane.

Seeded, so the file is the same on every run. From the repo root:

    python3 -I tools/gen-term-fixture.py [tests/fixtures/terminal/agent-session.bin.gz]

The bytes (about 1.2 MB) are kept gzipped (about 31 KB; the suite reads them
with the system zlib). The suite feeds them in pieces: a frame at a time up to the marker
line below, then 16 KB at a time, as Terminal.mm parses its backlog.
"""
import gzip
import random
import sys

COLS, ROWS = 120, 40
ESC = "\x1b"
SYNC_ON, SYNC_OFF = ESC + "[?2026h", ESC + "[?2026l"
# Between the agent's frames and the build output, so the suite can split.
MARKER = ESC + "]1337;minicode-fixture-build\x07"

rng = random.Random(2026)

WORDS = ("the grid draws only rows that changed since the last frame and "
         "keeps a copy of each row as drawn so a row written again with the "
         "same text costs nothing the colors are made once per style and "
         "each run of cells in one style is one line of glyphs set at its "
         "columns `TerminalScreen::takeDirty` `drawRect:` `NSColor` "
         "**bold** words and inline code come and go").split()

SPIN = ["·", "✢", "✳", "✶", "✻", "✽"]
STATUS = ["Thinking", "Pondering", "Compiling", "Reticulating", "Weaving"]


def rgb(r, g, b):
    return f"{ESC}[38;2;{r};{g};{b}m"


RESET = ESC + "[0m"
DIM = ESC + "[2m"
BOLD = ESC + "[1m"
GRAY = rgb(136, 136, 136)
ORANGE = rgb(215, 119, 87)
CODE = rgb(177, 185, 249)
GREEN = rgb(78, 186, 101)


def shimmer(text, t):
    """Each letter its own color, a bright band moving along (Claude Code's
    status text does this every frame)."""
    out = []
    for i, ch in enumerate(text):
        d = abs((i - t) % (len(text) + 6) - 3)
        k = max(0, 3 - d)
        out.append(rgb(215 + 13 * k, 119 + 40 * k, 87 + 40 * k) + ch)
    return "".join(out) + RESET


def box(inner):
    w = COLS - 2
    top = GRAY + "╭" + "─" * (w - 2) + "╮" + RESET
    mid = GRAY + "│" + RESET + " > " + inner.ljust(w - 5) + GRAY + "│" + RESET
    bot = GRAY + "╰" + "─" * (w - 2) + "╯" + RESET
    return [top, mid, bot]


def styled_words(words):
    out = []
    for w in words:
        if w.startswith("`"):
            out.append(CODE + w.strip("`") + RESET)
        elif w.startswith("**"):
            out.append(BOLD + w.strip("*") + RESET)
        else:
            out.append(w)
    return out


def wrap(words, width):
    """Lines of styled words, wrapped by visible width."""
    lines, cur, n = [], [], 0
    for w in words:
        vis = len(w.strip("`*"))
        if cur and n + 1 + vis > width:
            lines.append(cur)
            cur, n = [], 0
        cur.append(w)
        n += vis + (1 if n else 0)
    if cur:
        lines.append(cur)
    return [" ".join(styled_words(l)) for l in lines]


class Ink:
    def __init__(self):
        self.out = []
        self.prev = 0          # lines the live region took last frame

    def frame(self, live, static=()):
        s = [SYNC_ON]
        # eraseLines(prev): ESC[2K and up, the last without the up, then CR.
        for i in range(self.prev):
            s.append(ESC + "[2K")
            if i + 1 < self.prev:
                s.append(ESC + "[1A")
        if self.prev:
            s.append(ESC + "[G")
        for line in static:
            s.append(line + "\r\n")
        s.append("\r\n".join(live))
        s.append(SYNC_OFF)
        self.prev = len(live)
        self.out.append("".join(s))


def spinner(t, secs, tokens):
    word = STATUS[(t // 40) % len(STATUS)] + "…"
    return (ORANGE + SPIN[t % len(SPIN)] + RESET + " " + shimmer(word, t) +
            GRAY + f" ({secs}s · ↑ {tokens / 1000:.1f}k tokens · esc to interrupt)" + RESET)


def footer():
    return GRAY + "  ⏵⏵ accept edits on (shift+tab to cycle)" + RESET


def session():
    ink = Ink()
    head = [
        "% claude\r\n",
        ORANGE + "╭" + "─" * 46 + "╮" + RESET + "\r\n",
        ORANGE + "│" + RESET + " ✻ Welcome to the agent.".ljust(46) + ORANGE + "│" + RESET + "\r\n",
        ORANGE + "╰" + "─" * 46 + "╯" + RESET + "\r\n\r\n",
    ]
    ink.out.append("".join(head))
    t = 0
    tokens = 0
    for turn in range(4):
        prompt = GRAY + "> " + RESET + f"make the terminal draw less, step {turn + 1}"
        ink.frame([""] + box("") + [footer()], static=[prompt, ""])
        # Thinking: only the spinner changes.
        for _ in range(45):
            t += 1
            tokens += rng.randint(20, 90)
            ink.frame([spinner(t, t // 10, tokens), ""] + box("") + [footer()])
        # Streaming: the reply grows a few words a frame, the spinner below.
        words = []
        for _ in range(70):
            t += 1
            for _ in range(rng.randint(1, 3)):
                words.append(rng.choice(WORDS))
            tokens += rng.randint(5, 30)
            reply = wrap(words, COLS - 4)
            reply[0] = "⏺ " + reply[0]
            reply = [reply[0]] + ["  " + l for l in reply[1:]]
            ink.frame(reply + ["", spinner(t, t // 10, tokens), ""] + box("") + [footer()])
        # The reply is done: written once above, and the screen scrolls.
        reply = wrap(words, COLS - 4)
        static = ["⏺ " + reply[0]] + ["  " + l for l in reply[1:]] + [""]
        # A tool call and its output, also once.
        static.append(GREEN + "⏺" + RESET + BOLD + " Bash" + RESET + "(make test)")
        for i in range(12):
            static.append(GRAY + ("  ⎿  " if i == 0 else "     ") + RESET + DIM +
                          f"core: group {rng.randint(1, 900)} passed in {rng.randint(1, 99)} ms" + RESET)
        static.append("")
        ink.frame([""] + box("") + [footer()], static=static)
    return "".join(ink.out)


def build():
    out = []
    files = ["TerminalScreen", "TerminalGridView", "Terminal", "Settings", "LspClient",
             "SyntaxHighlighter", "MarkdownParser", "FolderSearch"]
    for i in range(5000):
        f = rng.choice(files)
        kind = rng.random()
        if kind < 0.75:
            out.append(f"{GREEN}[{i * 100 // 5000:3d}%]{RESET} Compiling "
                       f"{BOLD}src/{f}.cpp{RESET} -> build/obj/{f}.o\r\n")
        elif kind < 0.9:
            out.append(f"{BOLD}src/{f}.cpp:{rng.randint(1, 2000)}:{rng.randint(1, 80)}: "
                       f"{rgb(220, 180, 50)}warning:{RESET}{BOLD} unused variable "
                       f"'x{rng.randint(1, 99)}'{RESET}\r\n")
        else:
            out.append(DIM + "    " + "~" * rng.randint(4, 60) + "^" + RESET + "\r\n")
    return "".join(out)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/terminal/agent-session.bin.gz"
    data = (session() + MARKER + build()).encode("utf-8")
    with open(path, "wb") as f:
        # mtime 0 and no name, so the same bytes give the same file.
        with gzip.GzipFile(filename="", mode="wb", compresslevel=9, fileobj=f, mtime=0) as z:
            z.write(data)
    print(f"{path}: {len(data)} bytes")


if __name__ == "__main__":
    main()
