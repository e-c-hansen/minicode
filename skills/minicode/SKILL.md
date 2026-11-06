---
name: minicode
description: Show work in minicode, the user's editor, instead of only describing it. Use when reporting finished changes (open the changed lines with a note, a diff against the base, test output in a terminal), when pointing at code ("where is X", "what does this file do"), when checking a local web page (a dev server: load it in minicode's browser pane and read its text, console and screenshots before reaching for Playwright), and when the user says "this", "here" or "what I'm looking at" (read minicode's state and selection). Also for writing a workspace setup script, only when the user asks for one.
---

# minicode

minicode is the user's editor. The `minicode` command drives the copy that
is running: it opens files at lines with a short note, shows diffs side by
side, opens terminals and pages, and reports what the user is looking at.
Commands never close, replace or reorder the user's tabs, terminals or
browser page; they only add. They leave the keyboard where it is: while the
user is typing in the editor, a file or diff opens in a tab beside theirs
without being shown (the command says so on stderr), so say in the reply
that it is there. `window` and `--folder` can bring another window forward.

If `minicode` is not on PATH, or a command exits with code 3 (no minicode
to talk to), give plain paths in the reply instead and do not retry.

## When to use it

- **Reporting finished work.** Show the changed files at the changed lines
  with a note, a diff against the base, and the test output in a terminal,
  rather than only listing them in the reply.
- **Pointing at code.** When answering "where is X" or "what does this file
  do", or naming code in a report, open it with `--note` rather than only
  quoting the path.
- **Reading context.** When the user says "this", "here" or "what I'm
  looking at", run `minicode state` and `minicode selection` first.

## When not to

- Never rearrange or close the user's tabs or terminals (no command can).
- Never `--run` a long-running or destructive command (servers, installs,
  deletions, git pushes) without asking first. Short checks such as
  `make test` are fine when showing results was the point.
- Never write a workspace script unless the user asks for one.

## Commands

| Command | Does | Prints |
|---|---|---|
| `minicode open <path>[:line[:col]] ...` | Opens files at a line, in the window whose folder holds them | |
| `minicode open <path>:<line> [--to L[:C]] --note "TEXT" [--from NAME] [--keep]` | Highlights the lines, scrolls them into view, shows TEXT above them in the agent preview tab (replaced by the next note; `--keep` for a tab of its own) | |
| `minicode tour <file.json>` | A sequence of notes; the user steps with Option+] and Option+[ | |
| `minicode diff <path> [--base REF]` | Side-by-side diff against a git ref (default `HEAD`), in the diff preview tab (its own, so a note's tab stays) | |
| `minicode diff <a> <b>` | Side-by-side diff of two files (each up to 16 MB) | |
| `minicode term [--cwd D] [--title T] [--run CMD]` | A new terminal tab; `--run` types CMD and presses Return, visibly | terminal id |
| `minicode term-output <id> [--lines N]` | The terminal's last N lines (200), scrollback included | text |
| `minicode browse <url> [--wait]` | The page in the browser pane's agent page, beside the user's; `--wait` returns once loaded (15 s at most) | `--wait`: JSON url, status, title |
| `minicode page --url \| --text \| --console [--since N] \| --screenshot OUT.png [--full] \| --reload [--wait]` | Reads the agent page (`--yours`: the user's); text, console and screenshots of local pages only | JSON, text, JSON lines, path |
| `minicode split [--right\|--below] term ...` | `term --right` splits the active terminal | |
| `minicode window [--folder D] [--new]` | Brings forward, or opens, the window on a folder | window id |
| `minicode state [--pretty]` | JSON: windows, folders, tabs, selection, terminals, browser, note, tour | JSON |
| `minicode selection` | The text selected in the editor | text |
| `minicode procs [--folder D] [--json]` | What the window's terminals started, what they left behind (orphans), and what else runs in its folder: pid, memory, CPU, ports, command | table, or JSON |

Add `--window ID` or `--folder DIR` to act in a given window (default: the
frontmost). Exit codes: 0 done, 1 the command failed (stderr says why), 2
usage error, 3 no minicode (`state`, `selection`, `term-output`, `procs` and
`page` never start one). `minicode help <command>` has the details.

## Cleaning up after yourself

Before reporting, run `minicode procs` to see what you left running (dev
servers, watchers, browsers). Stop only processes you started, and only
with `kill <pid>` after listing them; never anything the user started, and
never by name (`pkill`, `killall`). Leave a server running when the user
is meant to look at it, and say so in the reply.

## Checking a local page

For a page on this Mac (a dev server, a file), use `browse --wait` and
`page` before Playwright, a headless browser or an extension. Only local
pages (localhost, 127.0.0.1, ::1, *.localhost, file://) can be read; others
fail naming `control.page-any-origin`, the user's to set. Nothing runs your
JavaScript or clicks. `--yours` only when the user means the page they see;
screenshots go in the session's scratchpad, never in the project.

```sh
minicode browse localhost:5173 --wait   # {"url":...,"status":200,"title":...}
minicode page --text                    # visible text; --console: logs, uncaught errors
minicode page --screenshot <scratchpad>/home.png --full
```

## Notes and tours

- A note is one or two sentences saying what the reader is looking at and
  why it matters. It does not repeat the reply. Plain text, at most 500
  characters, no Markdown.
- Give the range the note is about: `file:120 --to 145`, not the whole file.
- A report of a change across several files is a tour of at most about 8
  stops, in the order the reader should see them.
- Tour file: a JSON list; relative paths are from the current folder.

```json
[
  {"path": "src/retry.py", "line": 40, "to": 62, "note": "The retry loop: backs off exponentially and gives up after five tries."},
  {"path": "src/client.py", "line": 118, "note": "Every request now goes through the retry loop."},
  {"path": "tests/test_retry.py", "line": 1, "to": 30, "note": "New tests for the give-up path and the backoff times."}
]
```

## Example: reporting a change

After fixing a bug in `src/parser.c` and adding a test:

```sh
minicode open src/parser.c:212 --to 230 \
  --note "The fix: a quoted field may now hold the delimiter; the old code split on it."
minicode diff src/parser.c            # against HEAD, in a tab of its own
minicode term --title Tests --run "make test"
```

The note and the diff end up in two tabs side by side (the diff on show),
and the test run in a terminal. Say in the reply what was changed and that
the note, the diff and the test run are open in minicode. For a change
across files, write the stops to a scratch file and run
`minicode tour /path/to/tour.json` instead of the `open` line.

## Example: a workspace script, when asked

The user asks for "a script that sets up this project in minicode". Write
`setup.sh` in the project and tell them to run it themselves; never run it
for them, and nothing in minicode runs it automatically.

```sh
#!/bin/sh
# Opens this project in minicode: the server, its page, and the README.
cd "$(dirname "$0")"
minicode window --folder .
minicode term --title Server --run "python serve.py"
minicode browse http://localhost:8000
minicode open README.md
```

## Reading what the user means

```sh
minicode selection                      # the selected text, as is
minicode state | jq '.windows[] | select(.front) | {folder, tabs, selection}'
minicode term-output 3 --lines 50       # what a terminal shows (ids in state)
```

`state` lists each window's `id`, `folder`, `front`, `tabs` (`path`,
`title`, `kind`, `dirty`, `active`, `preview`), `selection` (`path`,
`text` up to 64 KB, `start` and `end` with `line` and `column`),
`terminals` (`id`, `cwd`, `title`, `command`, `busy`), `browser` (`page`,
`url`, `title`, `status`, `shown`), and the `note` and `tour` on show.
