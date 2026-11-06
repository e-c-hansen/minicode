---
name: minicode
description: Show work in minicode, the user's editor, instead of only describing it. Use when reporting finished changes (open the changed lines with a note, a diff against the base, test output in a terminal), when pointing at code ("where is X", "what does this file do"), and when the user says "this", "here" or "what I'm looking at" (read minicode's state and selection). Also for writing a workspace setup script, only when the user asks for one.
---

# minicode

minicode is the user's editor. The `minicode` command drives the copy that
is running: it opens files at lines with a short note, shows diffs side by
side, opens terminals and pages, and reports what the user is looking at.
Commands never take the keyboard or close, replace or reorder the user's
tabs and terminals; they only add.

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
| `minicode diff <path> [--base REF]` | Side-by-side diff against a git ref (default `HEAD`) | |
| `minicode diff <a> <b>` | Side-by-side diff of two files | |
| `minicode term [--cwd D] [--title T] [--run CMD]` | A new terminal tab; `--run` types CMD and presses Return, visibly | terminal id |
| `minicode term-output <id> [--lines N]` | The terminal's last N lines (200), scrollback included | text |
| `minicode browse <url>` | The page in the browser pane beside the editor | |
| `minicode split [--right\|--below] term ...` | `term --right` splits the active terminal | |
| `minicode window [--folder D] [--new]` | Brings forward, or opens, the window on a folder | window id |
| `minicode state [--pretty]` | JSON: windows, folders, tabs, selection, terminals, browser, note, tour | JSON |
| `minicode selection` | The text selected in the editor | text |

Add `--window ID` or `--folder DIR` to act in a given window (default: the
frontmost). Exit codes: 0 done, 1 the command failed (stderr says why), 2
usage error, 3 no minicode. `minicode help <command>` has the details.

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
minicode diff src/parser.c            # against HEAD, side by side
minicode term --title Tests --run "make test"
```

Then say in the reply what was changed and that the diff and the test run
are open in minicode. For a change across files, write the stops to a
scratch file and run `minicode tour /path/to/tour.json` instead of the
`open` line.

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
`terminals` (`id`, `cwd`, `title`, `command`, `busy`), `browser` (`url`,
`title`), and the `note` and `tour` on show.
