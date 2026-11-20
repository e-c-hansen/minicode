#!/bin/bash
# Time the same tasks in mutible, VS Code, and Zed, driven by script, and
# compare them by numbers: wall time, inputs (clicks, key chords, typed
# keystrokes), CPU time of each app's whole process tree, and its memory.
#
#   scripts/taskbench-mac.sh            run every task on every app (`make taskbench-mac`)
#   scripts/taskbench-mac.sh --list     the task table and each app's path; drives nothing
#   scripts/taskbench-mac.sh --show T   the steps each app runs for task T; drives nothing
#   scripts/taskbench-mac.sh --check    Accessibility permission, the apps found; drives nothing
#   scripts/taskbench-mac.sh --fixture  make the fixture project and say where; drives nothing
#   scripts/taskbench-mac.sh --report   write the report again from a run kept with KEEP=1
#
#   APPS="mutible vscode zed"   the apps to run, in order
#   TASKS="review viewers ..."  the tasks to run (default: all, in the table's order)
#   LOG_MB=500                  the huge log's size (made on demand, deleted after)
#   KEEP=1                      keep the scratch folder (/private/tmp/mutible-taskbench)
#   MUTIBLE_APP, VSCODE_APP, ZED_APP   other copies of the apps (installed copies only)
#
# How it works: the script makes a fixture project under the scratch folder,
# and for each task and app writes the app's steps to a file and runs
# build/taskbench-mac on it (tools/taskbench-mac.m has the step language).
# The tool starts the app on a scratch profile, sends each input (posted key
# events, clicks on elements found through the Accessibility API, a file
# opened with `open -a` as Finder opens it) and polls the step's result
# condition every 10 ms: a window title, an element's text, the keyboard's
# place, git's index, a port closing. Then the script stops the app (only
# pids it started) and goes on.
#
# Measures, per task and app:
#   wall       the sum of each step's time from its input sent to its condition true
#   inputs     clicks + key chords + typed keystrokes + Finder opens
#   CPU        user + system time of the app's process tree during the steps
#              (children it reaped included; helpers launchd started for it
#              counted by responsible pid, as membench does)
#   peak MB    the tree's largest physical footprint during the steps (100 ms samples)
#   settled MB its footprint once the tree has been quiet for a second
#   setup      inputs an app needs once before it can do the task (an
#              extension, a trust prompt), counted apart and not timed
#
# Fairness: each app takes its shortest documented path, with its own
# default keys; the steps are in one table per app below, with the doc for
# any step that is not obvious. A task an app cannot do is "cannot", with the
# reason. Settings that only keep an app quiet (no update checks, no
# telemetry) are not counted; settings or extensions that let it do the task
# are setup inputs.
#
# Safety: nothing of yours is touched. Every app runs on a scratch profile
# (VS Code --user-data-dir and --extensions-dir, Zed --user-data-dir and a
# scratch HOME, mutible a scratch HOME as the GUI checks use), only installed
# apps are run, only pids this script started are stopped, and Zed is
# skipped while your own Zed runs (one Zed takes another's requests). The run
# waits until the keyboard and mouse have been idle for 15 s, and stops at
# once (exit 3) if they are touched while it runs: it posts keys and clicks,
# so leave the Mac alone until it says it is done. It needs Accessibility
# permission for the terminal it runs in.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TOOL="$REPO/build/taskbench-mac"
WORK="${TASKBENCH_DIR:-/private/tmp/mutible-taskbench}"
case "$(basename "$WORK")" in   # it is emptied with rm -rf: never a folder of yours
    *taskbench*) ;;
    *) echo "taskbench: TASKBENCH_DIR must be a folder named for the benchmark (*taskbench*)" >&2; exit 2 ;;
esac
F="$WORK/fixture"
P="$F/proj"
CHECKS="$WORK/bin"
STATE="$WORK/guard"
MUTIBLE_APP="${MUTIBLE_APP:-/Applications/Mutible.app}"
VSCODE_APP="${VSCODE_APP:-/Applications/Visual Studio Code.app}"
ZED_APP="${ZED_APP:-/Applications/Zed.app}"
APPS="${APPS:-mutible vscode zed}"
LOG_MB="${LOG_MB:-500}"
BIGLOG="$WORK/big/huge.log"
NEEDLE="NEEDLE-4242"
SRV_PORT=18765    # task 3's server, left running
PAGE_PORT=18766   # task 4's page
PAGE_LOG="$WORK/page-server.log"
VSEXT="$WORK/vscode-extensions"   # shared by every VS Code run, so extensions install once
EXTDIR="$VSEXT"                    # the run's (a cold task gets an empty one)
OUT_TXT="$REPO/build/taskbench-mac.txt"
OUT_CSV="$REPO/build/taskbench-mac.csv"
OUT_STEPS="$REPO/build/taskbench-mac-steps.csv"

# ------------------------------------------------------------------ the tasks
#
# One row a task: id | cold | what the task is | when it is done. A cold task
# starts with the app not running (the task's first input starts it); the
# others start the app on the fixture project first, which is timed apart
# as its launch. Add a task here and a steps_<id>_<app> function per app.
TASK_TABLE=(
"review|no|Review the uncommitted changes across files and stage one hunk|git diff --cached holds app.py's first hunk and nothing else"
"viewers|no|Open a PDF, PNG, MP4, CSV, notebook, and SQLite file and show each rendered|each file's window shows a view that is not its text (read through AX, or from the screen where an app has none), else 'opens as text' or 'refused'"
"server|no|Find the server left running on port $SRV_PORT and stop it from the app|nothing accepts on port $SRV_PORT"
"page|no|Open the page the fixture serves on localhost inside the app|the fixture's server logs the request (and the page's title shows, where AX can read it)"
"biglog|yes|Open a ${LOG_MB} MB log and find a string near its end|the find count reads 1 of 1"
"finder|yes|Open one file from Finder (open -a) to a window with the keyboard in the text|the window is titled for the file and its focused element is a text area"
)

task_field() {   # task_field <id> <n>: 2 cold, 3 title, 4 done-when
    local row
    for row in "${TASK_TABLE[@]}"; do
        if [ "${row%%|*}" = "$1" ]; then echo "$row" | cut -d'|' -f"$2"; return; fi
    done
}
task_ids() { local row; for row in "${TASK_TABLE[@]}"; do echo "${row%%|*}"; done; }

# ------------------------------------------------------------------ the apps

q() { printf '%q ' "$@"; }

app_path() {
    case "$1" in mutible) echo "$MUTIBLE_APP" ;; vscode) echo "$VSCODE_APP" ;; zed) echo "$ZED_APP" ;; esac
}
app_name() {
    case "$1" in mutible) echo "mutible" ;; vscode) echo "VS Code" ;; zed) echo "Zed" ;; esac
}
plist() { /usr/libexec/PlistBuddy -c "Print :$2" "$1/Contents/Info.plist" 2>/dev/null || true; }
app_exe() { local a; a="$(app_path "$1")"; echo "$a/Contents/MacOS/$(plist "$a" CFBundleExecutable)"; }
app_version() { plist "$(app_path "$1")" CFBundleShortVersionString; }
code_cli() { q "HOME=$RUN/home" "$VSCODE_APP/Contents/Resources/app/bin/code" --user-data-dir "$RUN/data" --extensions-dir "$EXTDIR"; }

# The command that opens PATH in a new copy of the app on this run's scratch
# profile: `open_<app> bg PATH` starts it in the background (a launch),
# `open_<app> fg PATH` brings it forward as Finder's Open does.
open_mutible() {
    local g=""; if [ "$1" = bg ]; then g="-g"; fi
    local h="$RUN/home"
    # shellcheck disable=SC2086
    q open -n $g -a "$MUTIBLE_APP" \
        --env "HOME=$h" --env "CFFIXED_USER_HOME=$h" --env "ZDOTDIR=$h" --env "TMPDIR=$RUN/t/" \
        --env "MUTIBLE_SETTINGS=$h/settings.conf" --env "MUTIBLE_DEFAULTS_SUITE=ai.ible.mutible.taskbench.$RUNID" \
        --env "MUTIBLE_BACKUP_DIR=$RUN/backups" --env "CLAUDE_CONFIG_DIR=$RUN/claude" \
        --env "MUTIBLE_MATH_CACHE=$RUN/math" "$2"
}
# Two Chromium switches (Electron passes them on), neither a step:
#   --use-mock-keychain: VS Code's safeStorage asks the keychain for "Code
#     Safe Storage" at startup; under a scratch HOME there is no login
#     keychain, so macOS put up "Keychain Not Found" (one dialog a launch,
#     on the owner's screen) and VS Code's main thread blocked on it: the
#     first run's 90 s launch timeouts. It also keeps VS Code off the
#     owner's keychain.
#   --force-renderer-accessibility: Chromium builds no AX tree until asked,
#     and asking through AXManualAccessibility once the app runs was lost
#     or took seconds (tools/taskbench-mac.m, Adopt), so the tree is on
#     from the first window, as for a person with VoiceOver running.
open_vscode() {
    local g=""; if [ "$1" = bg ]; then g="-g"; fi
    # shellcheck disable=SC2086
    q open -n $g -a "$VSCODE_APP" --env "HOME=$RUN/home" "$2" \
        --args --user-data-dir "$RUN/data" --extensions-dir "$EXTDIR" \
        --use-mock-keychain --force-renderer-accessibility
}
open_zed() {
    local g=""; if [ "$1" = bg ]; then g="-g"; fi
    # shellcheck disable=SC2086
    q open -n $g -a "$ZED_APP" --env "HOME=$RUN/home" "$2" --args --user-data-dir "$RUN/data"
}

# ------------------------------------------------------------------ each app's steps
#
# The step language is in tools/taskbench-mac.m's opening comment. In short:
# `INPUT => CONDITION @TIMEOUT`, inputs `key`, `type`, `press`, `click`,
# `menu`, `finder`, `sh`, `none`; `setup N:` counts N setup inputs; a
# condition led by `?` is "unverified" rather than failed when it times out;
# `quiet MS` (the app's CPU settled) stands in where AX cannot read a result,
# and makes the result "unverified". A function that prints `cannot: why`
# records the task as one the app cannot do.
#
# Docs cited: mutible's docs/REFERENCE.md; VS Code's keyboard shortcuts
# reference (code.visualstudio.com/docs/reference/default-keybindings) and
# source control docs (code.visualstudio.com/docs/sourcecontrol/overview);
# Zed's default key bindings (zed.dev/docs/key-bindings) and git docs
# (zed.dev/docs/git).

# Opening a file by name: every app's Go to File / Quick Open / file finder
# is Cmd+P. A click on the file in the tree is shorter (one input) in all
# three, but Zed's tree cannot be reached through AX, so all three use Cmd+P
# and the comparison stays even.
open_by_name() {   # open_by_name <name> <condition> [typed condition]
    echo "key cmd+p => focus AXTextField || quiet 250"
    echo "type $1 => ${3:-?quiet 150} @3"
    echo "key return => $2 @30"
}

# 1. review ----------------------------------------------------------------

steps_review_mutible() {
    # REFERENCE.md: Shift+Cmd+G opens Source Control; its Review Changes
    # button shows every changed file in one view, each change with Stage
    # and Discard beside its first line, which VoiceOver (so AX) reads as
    # "Stage change 1 in app.py"; the page opens at its top, where it shows.
    echo "key cmd+shift+g => ax AXButton=Review Changes @10"
    echo "press AXButton=Review Changes => ax AXButton=Stage change 1 in app.py @15"
    echo "press AXButton=Stage change 1 in app.py => sh $CHECKS/staged-one-only"
}
steps_review_vscode() {
    # Source control docs: Ctrl+Shift+G opens the Source Control view; a
    # file opens in its diff editor, a double click with the keyboard in it
    # (a single click opens a preview and leaves the keyboard in the list,
    # where the stage key does nothing); the diff editor opens with the
    # cursor on its first change (Option+F5, Go to Next Change, went on to
    # the second, watched 2026-11-20); Cmd+K Cmd+Option+S (Git: Stage
    # Selected Ranges, the git extension's keybinding, "when":
    # editorTextFocus) stages the change at the cursor.
    echo "key ctrl+shift+g => ax AXRow~app.py, Modified @15"
    echo "dclick AXRow~app.py, Modified => window~app.py && focus AXTextArea"
    echo "key cmd+k cmd+alt+s => sh $CHECKS/staged-one-only"
}
steps_review_zed() {
    # Git docs (zed.dev/docs/git): "git: diff" opens the Project Diff, every
    # changed file in one view; "focus on each hunk and stage them
    # individually ... via the keybindings git: stage and next" (Cmd+Y).
    # Watched 2026-11-20: the diff opens with the cursor on line 2, above
    # the first hunk, so the first Cmd+Y only moves to that hunk (its Stage
    # and Restore buttons appear) and the second stages it. Ctrl-G D, the
    # docs' key for git: diff, did nothing with no file open, so the palette.
    # Zed shows no AX tree, so the steps before the last wait for its CPU to
    # settle; the last is read from git's index.
    echo "key cmd+shift+p => ?quiet 250 @3"
    echo "type git: diff => ?quiet 150 @3"
    echo "key return => window~Uncommitted Changes || quiet 400 @15"
    echo "key cmd+y => ?quiet 150 @3"
    echo "key cmd+y => sh $CHECKS/staged-one-only"
}

# 2. viewers ---------------------------------------------------------------

viewer_steps() {   # viewer_steps [verified]: the palette's typed name read back from its field
    # The magic after each name is what the file's text starts with, so a
    # text view of it reads as "opens as text". The PNG's is its first
    # chunk's name, IHDR, not "PNG": Zed's image viewer prints "PNG" in its
    # size line, read from the screen.
    typed() { if [ "${V:-}" = verified ]; then echo "focus AXTextField=$1"; fi; }
    local V="${1:-}"
    open_by_name report.pdf "[report.pdf] view report.pdf %PDF" "$(typed report.pdf)"
    open_by_name picture.png "[picture.png] view picture.png IHDR" "$(typed picture.png)"
    open_by_name clip.mp4 "[clip.mp4] view clip.mp4 ftyp" "$(typed clip.mp4)"
    open_by_name cities.csv "[cities.csv] view cities.csv city,country" "$(typed cities.csv)"
    open_by_name analysis.ipynb "[analysis.ipynb] view analysis.ipynb \"cells\"" "$(typed analysis.ipynb)"
    open_by_name shop.sqlite "[shop.sqlite] view shop.sqlite SQLite format" "$(typed shop.sqlite)"
}
# mutible's palette field is read back once the name is typed (as VS Code's
# is), not waited on for 150 ms of quiet; its rows are drawn, so a Return
# before the list filtered would open another file and fail the view's title.
steps_viewers_mutible() { viewer_steps verified; }
# VS Code's Quick Open, read through AX: its field (an AXTextField described
# "Search files by name ...") is up, then holds the typed name with its list
# filtered to one ("1 Results"), so no wait for quiet is needed. The keyboard's
# element is no help here: VS Code reported an AXStaticText of the page
# behind as focused while Quick Open was up.
vscode_open_by_name() {   # vscode_open_by_name <name> <condition> [menu | click]
    if [ "${3:-}" = menu ]; then
        echo "menu Go>Go to File… => ax AXTextField~Search files by name"
    else
        echo "key cmd+p => ax AXTextField~Search files by name"
    fi
    if [ "${3:-}" = click ]; then
        echo "click AXTextField~Search files by name => none"
    fi
    # Filtered: the field holds the name and the list's count reads one.
    echo "type $1 => ax AXTextField=$1 && ax AXStaticText=1 Results @5"
    echo "key return => $2 @30"
}
steps_viewers_vscode() {
    # VS Code shows images, video, and notebooks itself; a PDF and a SQLite
    # file need an extension each (its PDF opens as text, its SQLite file is
    # refused). Through the GUI each is Shift+Cmd+X, the name typed, Install,
    # and the publisher-trust button: 1 + name + 2 inputs. The CSV stays text
    # (Rainbow CSV only colours it), so no extension is added for it.
    echo "setup 13: sh $(code_cli) --install-extension tomoki1207.pdf >/dev/null 2>&1 => none @300"
    echo "setup 16: sh $(code_cli) --install-extension qwtel.sqlite-viewer >/dev/null 2>&1 => none @300"
    vscode_open_by_name report.pdf "[report.pdf] view report.pdf %PDF"
    # The PDF viewer (pdf.js in a webview) keeps the keyboard and takes Cmd+P
    # for its own print, so Quick Open never came (watched 2026-11-20); the
    # next file is opened from the Go menu, as a person would once Cmd+P did
    # nothing: Go > Go to File…, two clicks.
    vscode_open_by_name picture.png "[picture.png] view picture.png IHDR" menu
    vscode_open_by_name clip.mp4 "[clip.mp4] view clip.mp4 ftyp"
    # From the video preview (a webview with the keyboard in its player),
    # Cmd+P brings Quick Open up but leaves the keyboard in the player, so
    # the name typed never reached the field (watched 2026-11-20: the field
    # stayed up and empty): a click in the field first.
    vscode_open_by_name cities.csv "[cities.csv] view cities.csv city,country" click
    vscode_open_by_name analysis.ipynb "[analysis.ipynb] view analysis.ipynb \"cells\""
    vscode_open_by_name shop.sqlite "[shop.sqlite] view shop.sqlite SQLite format"
}
steps_viewers_zed() { viewer_steps; }

# 3. server ----------------------------------------------------------------

steps_server_mutible() {
    # REFERENCE.md: Shift+Cmd+J lists the processes whose working folder is
    # in the window's (the server was started there, outside mutible); a
    # row's Stop button, which VoiceOver (so AX) finds by its port ("Stop
    # python3 on port N") without a hover, asks first, then stops it.
    echo "key cmd+shift+j => ax AXButton~on port $SRV_PORT @15"
    echo "press AXButton~on port $SRV_PORT => ax AXButton=Stop @5"
    echo "press AXButton=Stop => port-closed $SRV_PORT @10"
}
steps_server_vscode() {
    echo "cannot: no view lists local processes or listening ports; the Ports view only forwards ports (code.visualstudio.com/docs/debugtest/port-forwarding), so stopping it takes a terminal command or Activity Monitor"
}
steps_server_zed() {
    echo "cannot: no view lists processes or ports (none in zed.dev/docs or the command palette), so stopping it takes a terminal command or Activity Monitor"
}

# 4. page ------------------------------------------------------------------

steps_page_mutible() {
    # REFERENCE.md: Shift+Cmd+B shows the browser pane with the keyboard in
    # its address bar.
    # The typed address is read back from the field, as in VS Code: waiting
    # for 100 ms of quiet timed out at 3 s in every run on 2026-11-20 (the
    # new pane's WebKit processes were still busy), which put 3 s of the
    # benchmark's waiting into mutible's wall time.
    echo "key cmd+shift+b => focus AXTextField @10"
    echo "type http://127.0.0.1:$PAGE_PORT/ => focus AXTextField=http://127.0.0.1:$PAGE_PORT/ || ax AXTextField=http://127.0.0.1:$PAGE_PORT/ @5"
    echo "key return => sh grep -qE 'GET /[ ?]' $PAGE_LOG @15"
    echo "none => ?ax *~taskbench page @10"
}
steps_page_vscode() {
    # VS Code's built-in Simple Browser (the simple-browser extension that
    # ships with it): Command Palette, "Simple Browser: Show", the address.
    # Each field read through AX: the palette's (its ">" prefilled), filtered
    # to the one command, then the command's own "Enter url to visit" box
    # (described by its placeholder, https://example.com) holding the URL.
    echo "key cmd+shift+p => ax AXTextField~Type the name of a command"
    echo "type simple browser: show => ax AXTextField=>simple browser: show && ax AXStaticText=1 Results @5"
    echo "key return => ax AXTextField~https://example.com"
    echo "type http://127.0.0.1:$PAGE_PORT/ => ax AXTextField=http://127.0.0.1:$PAGE_PORT/ @5"
    echo "key return => sh grep -qE 'GET /[ ?]' $PAGE_LOG @15"
    echo "none => ?ax *~taskbench page @10"
}
steps_page_zed() {
    echo "cannot: Zed has no browser view (none in zed.dev/docs or the command palette); a link opens in the system browser, outside the app"
}

# 5. biglog (cold) ---------------------------------------------------------

biglog_steps() {   # biglog_steps <app> <shown condition> <find bar condition> <count condition>
    echo "finder $("open_$1" fg "$BIGLOG") => $2 @180"
    echo "key cmd+f => $3"
    echo "type $NEEDLE => $4 @120"
}
# Each app's find count: "1 of 1" in mutible and VS Code (exact, since "1 of
# 10000" holds "1 of 1" while the first letters are searched). VS Code's
# window is titled for the log about a second before its text is loaded
# (Cmd+F then did nothing), so it waits for the editor (a text area
# described "huge.log"); its find field is a text area described "Find"
# (it opens holding the word at the cursor, selected, so the needle typed
# replaces it).
steps_biglog_mutible() { biglog_steps mutible "window~huge.log" "focus AXTextField || quiet 300" "ax *=1 of 1"; }
steps_biglog_vscode() {
    biglog_steps vscode "window~huge.log && ax AXTextArea=huge.log" "ax AXTextArea=Find" "ax *=1 of 1"
}
# Zed, read from the screen (it has no AX tree): a file opened cold comes up
# behind the modal "Unrecognized Project" question (Restricted Mode), which
# holds the keyboard, so Cmd+F and the needle did nothing until it was
# answered (watched 2026-11-20); Return is its "Trust and Continue". Its find
# bar then reads "0/0", and "1/1" once the needle is found.
zed_trust_steps() {   # zed_trust_steps <file name>: the cold open and the trust question
    echo "finder $(open_zed fg "$2") => window~$1 && screen Unrecognized Project @180"
    echo "key return => !screen Unrecognized Project @60"
}
steps_biglog_zed() {
    zed_trust_steps huge.log "$BIGLOG"
    echo "key cmd+f => screen 0/0 @60"
    echo "type $NEEDLE => screen 1/1 @120"
}

# 6. finder (cold) ---------------------------------------------------------

steps_finder_mutible() { echo "finder $(open_mutible fg "$F/lonely/lonely.py") => window~lonely.py && focus AXTextArea @60"; }
steps_finder_vscode() { echo "finder $(open_vscode fg "$F/lonely/lonely.py") => window~lonely.py && focus AXTextArea @60"; }
steps_finder_zed() {
    # The first run read Zed as done once its window settled, but the
    # keyboard was in the trust question (above), not the text. Now the
    # question is answered and the file's text read from the screen; where
    # the keyboard is cannot be read, so the result is unverified.
    zed_trust_steps lonely.py "$F/lonely/lonely.py" | sed '$d'
    echo "key return => [text shown; keyboard place unverified] !screen Unrecognized Project && screen print(\"lonely\" @60"
}

# ------------------------------------------------------------------ setup once per app
#
# Run on a template profile that every task then copies, so what an app asks
# once (VS Code's folder trust) is answered once and counted as setup.

prep_mutible() {
    echo "launch $(open_mutible bg "$P") => window @60"
    echo "setup 0: none => ?quiet 1000 @20"
}
prep_vscode() {
    echo "launch $(open_vscode bg "$P") => window @90"
    # "Do you trust the authors of the files in this folder?", once a folder.
    echo "setup 1: ?press AXButton~Yes, I trust => !ax AXButton~Yes, I trust @20"
    echo "setup 0: none => ?quiet 1000 @20"
}
prep_zed() {
    # Zed opens an unknown folder behind a modal "Unrecognized Project"
    # question (Restricted Mode), which AX cannot read and which took every
    # key of the first run's tasks (Cmd+Y staged nothing). Return is its
    # "Trust and Continue"; Zed keeps the answer in the profile the tasks
    # copy (watched 2026-11-20 in a screenshot), so it is asked once, like
    # VS Code's folder trust.
    echo "launch $(open_zed bg "$P") => window @90"
    echo "setup 0: none => ?quiet 1500 @30"
    echo "setup 1: key return => ?quiet 500 @10"
}

# ------------------------------------------------------------------ the fixture

make_checks() {
    mkdir -p "$CHECKS"
    cat >"$CHECKS/staged-one-only" <<EOF
#!/bin/sh
# Task 1 is done: app.py's first hunk is staged, and nothing else.
d="\$(git -C "$P" diff --cached)"
echo "\$d" | grep -q TB_HUNK_ONE || exit 1
echo "\$d" | grep -q -e TB_HUNK_TWO -e TB_HUNK_THREE -e TB_NEW_FILE && exit 1
exit 0
EOF
    chmod +x "$CHECKS/staged-one-only"
}

make_fixture() {
    rm -rf "$F"
    mkdir -p "$P/media" "$F/lonely" "$P/site"
    {
        echo '"""Inventory service: the fixture for the task benchmark."""'
        echo 'import json'
        echo 'import sys'
        echo 'LIMIT = 10'
        echo
        for i in $(seq 1 18); do
            echo "def item_$i(stock):"
            echo "    return stock - $i"
            echo
        done
        echo 'if __name__ == "__main__":'
        echo '    print(json.dumps({"limit": LIMIT}))'
    } >"$P/app.py"
    printf 'def clamp(x, lo, hi):\n    return max(lo, min(x, hi))\n\n\ndef total(xs):\n    return sum(xs)\n' >"$P/util.py"
    printf '# Inventory\n\nA small service for the task benchmark.\n' >"$P/README.md"
    printf 'city,country,population,area\nOslo,Norway,709000,454\nLima,Peru,10092000,2672\nPerth,Australia,2192000,6418\nPune,India,7764000,331\nQuito,Ecuador,2011000,372\n' \
        >"$P/media/cities.csv"
    cat >"$P/media/analysis.ipynb" <<'EOF'
{"cells": [
 {"cell_type": "markdown", "metadata": {}, "source": ["# Analysis\n", "Numbers for the **fixture**."]},
 {"cell_type": "code", "execution_count": 1, "metadata": {}, "source": ["sum(range(10))"],
  "outputs": [{"output_type": "execute_result", "execution_count": 1, "metadata": {}, "data": {"text/plain": ["45"]}}]}
],
 "metadata": {"kernelspec": {"display_name": "Python 3", "language": "python", "name": "python3"},
  "language_info": {"name": "python"}},
 "nbformat": 4, "nbformat_minor": 5}
EOF
    /usr/bin/sqlite3 "$P/media/shop.sqlite" \
        "create table products(id integer primary key, name text, price real);
         create table orders(id integer primary key, product integer, qty integer);
         insert into products values (1,'pen',1.5),(2,'pad',3.25),(3,'ink',7.0);
         insert into orders values (1,1,10),(2,3,2);"
    "$TOOL" media "$P/media" >/dev/null
    printf '<!doctype html>\n<title>taskbench page</title>\n<h1>taskbench page</h1>\n<p>Served by the fixture.</p>\n' \
        >"$P/site/index.html"
    printf 'import sys\n\n\ndef main():\n    print("lonely", sys.argv[1:])\n\n\nmain()\n' >"$F/lonely/lonely.py"

    local g=(git -C "$P" -c user.name=taskbench -c user.email=taskbench@example.invalid
             -c commit.gpgsign=false -c init.defaultBranch=main)
    GIT_CONFIG_GLOBAL=/dev/null "${g[@]}" init -q
    GIT_CONFIG_GLOBAL=/dev/null "${g[@]}" add -A
    GIT_CONFIG_GLOBAL=/dev/null GIT_AUTHOR_DATE="2026-11-01T10:00:00" GIT_COMMITTER_DATE="2026-11-01T10:00:00" \
        "${g[@]}" commit -q -m "Inventory service"
    # The uncommitted changes, as an agent might leave them: two hunks in
    # app.py far apart, one in util.py, and a new file.
    sed -i '' 's/^LIMIT = 10$/LIMIT = 12  # TB_HUNK_ONE: raised for the sale/' "$P/app.py"
    sed -i '' 's/^    return stock - 17$/    return max(0, stock - 17)  # TB_HUNK_TWO/' "$P/app.py"
    sed -i '' 's/^    return sum(xs)$/    return sum(xs)  # TB_HUNK_THREE/' "$P/util.py"
    printf '# TB_NEW_FILE\n- check the sale price\n' >"$P/TODO.md"
}

free_mb() { df -m "$(dirname "$WORK")" | awk 'NR == 2 {print $4}'; }
# Copies of an executable running, counting one Gatekeeper runs from an
# AppTranslocation copy (a quarantined app it did not see Finder move).
running() {
    local inner; inner="$(echo "$1" | sed -E 's#^.*(/[^/]+\.app/Contents/MacOS/[^/]+)$#\1#')"
    ps -axo comm= | awk -v exe="$1" -v inner="$inner" '
        $0 == exe || (index($0, "/AppTranslocation/") && substr($0, length($0) - length(inner) + 1) == inner) { n++ }
        END { print n + 0 }'
}

# ------------------------------------------------------------------ servers and pids

STARTED_APPS=()     # "exe|pid" of every app this script started
STARTED_PIDS=()     # servers and caffeinate
cleanup() {
    local e
    for e in "${STARTED_APPS[@]:-}"; do
        [ -n "$e" ] && "$TOOL" quit --exe "${e%|*}" "${e##*|}" >/dev/null 2>&1 || true
    done
    for e in "${STARTED_PIDS[@]:-}"; do [ -n "$e" ] && kill "$e" 2>/dev/null || true; done
    if [ "${KEEP:-}" != 1 ] && [ -d "$WORK" ] && [ "${RAN:-}" = 1 ]; then rm -rf "$WORK"; fi
}

port_open() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }

start_server() {   # start_server <port> <log>: a server with its working folder in the project
    if port_open "$1"; then echo "taskbench: port $1 is in use; set another in the script" >&2; exit 2; fi
    (cd "$P" && exec nohup /usr/bin/python3 -m http.server "$1" --bind 127.0.0.1 --directory site >"$2" 2>&1) &
    local pid=$!
    disown "$pid" 2>/dev/null || true   # no "Terminated" line when the app under test stops it
    STARTED_PIDS+=("$pid")
    local i
    for ((i = 0; i < 500; i++)); do port_open "$1" && break; sleep 0.01; done
    SERVER_PID=$pid
}

stop_server() { kill "$1" 2>/dev/null || true; }

# ------------------------------------------------------------------ one task, one app

field() { echo "$2" | tr '\t' '\n' | sed -n "s/^$1=//p" | head -1; }

csv() {   # csv fields...: one CSV row, each field quoted
    local out="" f
    for f in "$@"; do f="${f//\"/\"\"}"; out="$out${out:+,}\"$f\""; done
    echo "$out"
}

RESULTS="$WORK/results.tsv"   # task \t app \t status \t summary-or-reason

# Run a steps file; record its summary line. Returns the tool's exit code.
run_steps() {   # run_steps <task> <app> <steps file>
    local task="$1" app="$2" steps="$3" out="$RUN/out.txt" rc=0
    local exe; exe="$(app_exe "$app")"
    "$TOOL" run --exe "$exe" --steps "$steps" --state "$STATE" --out "$out" >"$RUN/run.log" 2>&1 || rc=$?
    local pid; pid="$(awk -F'\t' '$1 == "pid" {p = $2} END {print p}' "$out" 2>/dev/null || true)"
    if [ -n "$pid" ]; then STARTED_APPS+=("$exe|$pid"); fi
    awk -F'\t' -v t="$task" -v a="$app" '$1 == "step" {print t "\t" a "\t" $0}' "$out" >>"$WORK/steps.tsv" 2>/dev/null || true
    LAST_SUMMARY="$(grep '^summary' "$out" 2>/dev/null | tail -1 || true)"
    LAST_PID="$pid"
    return $rc
}

quit_app() {   # quit_app <app> <pid>
    [ -n "$2" ] && "$TOOL" quit --exe "$(app_exe "$1")" "$2" || true
}

# A task on the project runs on a copy of the app's template profile (its
# setup done, its extensions); a cold task on a fresh profile with only the
# quiet settings, as a first open from Finder would meet it.
new_run() {   # new_run <task> <app>: a fresh scratch profile for this run
    RUNID="$1-$2-$$"
    RUN="$WORK/run/$1-$2"
    EXTDIR="$VSEXT"
    rm -rf "$RUN"
    mkdir -p "$RUN/home" "$RUN/t" "$RUN/backups" "$RUN/claude" "$RUN/math"
    local tpl="$WORK/template-$2"
    [ -d "$tpl" ] || return 0
    if [ "$(task_field "$1" 2)" = yes ]; then
        EXTDIR="$RUN/ext"
        mkdir -p "$EXTDIR"
        case "$2" in
            vscode) mkdir -p "$RUN/data/User"; cp "$tpl/data/User/settings.json" "$RUN/data/User/" ;;
            zed) mkdir -p "$RUN/data/config" "$RUN/home/.config/zed"
                 cp "$tpl/data/config/settings.json" "$RUN/data/config/"
                 cp "$tpl/home/.config/zed/settings.json" "$RUN/home/.config/zed/" ;;
        esac
        return 0
    fi
    case "$2" in
        vscode|zed) rm -rf "$RUN/data" "$RUN/home"; cp -R "$tpl/data" "$RUN/data"; cp -R "$tpl/home" "$RUN/home" ;;
    esac
}

prep_app() {   # prep_app <app>: the one-time setup, on the template profile
    local app="$1"
    RUNID="prep-$app-$$"
    RUN="$WORK/template-$app"
    EXTDIR="$VSEXT"
    rm -rf "$RUN"
    mkdir -p "$RUN/home" "$RUN/data/User" "$RUN/t" "$RUN/backups" "$RUN/claude" "$RUN/math"
    case "$app" in
        vscode)
            # Quiet only: no update checks, no telemetry (not counted).
            # editor.accessibilitySupport on: VS Code takes the AX tree the
            # benchmark reads for a screen reader and switches its editor to
            # Screen Reader Optimized on its own ("auto"), asking in a
            # notification; "on" is that mode without the notification. Off
            # would hide the editor's text from AX ("The editor is not
            # accessible at this time"), so a file that opens as text could
            # not be told from one shown.
            cat >"$RUN/data/User/settings.json" <<'EOF'
{
    "update.mode": "none",
    "telemetry.telemetryLevel": "off",
    "extensions.autoUpdate": false,
    "extensions.autoCheckUpdates": false,
    "editor.accessibilitySupport": "on"
}
EOF
            ;;
        zed)
            # Quiet only, in both places Zed may read it (with
            # --user-data-dir, and under the scratch HOME).
            mkdir -p "$RUN/data/config" "$RUN/home/.config/zed"
            printf '{\n  "auto_update": false,\n  "telemetry": {"diagnostics": false, "metrics": false}\n}\n' \
                | tee "$RUN/data/config/settings.json" >"$RUN/home/.config/zed/settings.json"
            ;;
    esac
    "prep_$app" >"$RUN/steps.txt"
    local rc=0
    run_steps setup "$app" "$RUN/steps.txt" || rc=$?
    quit_app "$app" "$LAST_PID"
    if [ "$rc" = 3 ]; then aborted; fi
    printf 'setup\t%s\t%s\t%s\n' "$app" "$([ $rc = 0 ] && echo ok || echo failed)" "$LAST_SUMMARY" >>"$RESULTS"
}

run_task() {   # run_task <task> <app>
    local task="$1" app="$2" reason
    new_run "$task" "$app"
    "steps_${task}_$app" >"$RUN/steps.txt"
    if reason="$(sed -n 's/^cannot: //p' "$RUN/steps.txt")" && [ -n "$reason" ]; then
        printf '%s\t%s\tcannot\t%s\n' "$task" "$app" "$reason" >>"$RESULTS"
        echo "   $(app_name "$app"): cannot"
        return
    fi
    make_fixture
    local server=""
    case "$task" in
        server) start_server "$SRV_PORT" "$WORK/server.log"; server=$SERVER_PID ;;
        page) start_server "$PAGE_PORT" "$PAGE_LOG"; server=$SERVER_PID ;;
    esac
    if [ "$(task_field "$task" 2)" = no ]; then
        # Setup done from the shell (installing an extension) comes before
        # the launch, so the app starts with it.
        { grep '^setup[ 0-9]*: sh ' "$RUN/steps.txt" || true
          echo "launch $("open_$app" bg "$P") => window @90"
          echo "setup 0: none => ?quiet 1000 @30"
          grep -v '^setup[ 0-9]*: sh ' "$RUN/steps.txt" || true; } >"$RUN/steps.full"
    else
        cp "$RUN/steps.txt" "$RUN/steps.full"
    fi
    local rc=0
    run_steps "$task" "$app" "$RUN/steps.full" || rc=$?
    quit_app "$app" "$LAST_PID"
    if [ -n "$server" ]; then stop_server "$server"; fi
    if [ "$rc" = 3 ]; then aborted; fi
    local status; status="$(field status "$LAST_SUMMARY")"
    [ -n "$status" ] || status="failed"
    if [ "$status" = failed ]; then
        local why; why="$(awk -F'\t' '$1 == "step" && $8 != "ok" && $8 != "unverified" && $8 !~ /^skipped/ {print "step " $2 ": " $8 " (" $10 ")"}' "$RUN/out.txt" | head -1 || true)"
        LAST_SUMMARY="$LAST_SUMMARY	why=${why:-the tool failed: $(tail -1 "$RUN/run.log")}"
    fi
    printf '%s\t%s\t%s\t%s\n' "$task" "$app" "$status" "$LAST_SUMMARY" >>"$RESULTS"
    echo "   $(app_name "$app"): $status, $(field wall_ms "$LAST_SUMMARY") ms, $(field inputs "$LAST_SUMMARY") inputs"
}

aborted() {
    echo
    echo "taskbench: stopped: the keyboard or mouse was used while it ran. Nothing more was driven."
    report || true
    exit 3
}

# ------------------------------------------------------------------ report

report() {
    [ -f "$RESULTS" ] || return 0
    local apps_line="" a
    for a in $APPS; do apps_line="$apps_line${apps_line:+, }$(app_name "$a") $(app_version "$a")"; done
    {
        echo "mutible taskbench, $(date '+%Y-%m-%d %H:%M')"
        echo "$(sw_vers -productName) $(sw_vers -productVersion), $(sysctl -n machdep.cpu.brand_string), $(( $(sysctl -n hw.memsize) / 1073741824 )) GB"
        echo "$apps_line"
        echo "wall: input sent to result, summed over the task's steps; inputs: clicks/chords/keys/opens;"
        echo "CPU: the app's process tree during the steps; MB: physical footprint, peak and settled."
        echo "A result marked * could not be read from what the app shows (unverified: only its CPU settling, or part of it)."
        local row id
        echo
        echo "Setup, once per app (not in the task rows)"
        awk -F'\t' '$1 == "setup"' "$RESULTS" | while IFS=$'\t' read -r _ app status summary; do
            printf '  %-8s %-6s %3s inputs, %6s ms, launch %s ms\n' "$(app_name "$app")" "$status" \
                "$(field setup_inputs "$summary")" "$(field setup_ms "$summary")" "$(field launch_ms "$summary")"
        done
        for row in "${TASK_TABLE[@]}"; do
            id="${row%%|*}"
            grep -q "^$id	" "$RESULTS" || continue
            echo
            echo "$id: $(task_field "$id" 3)"
            echo "  done when: $(task_field "$id" 4)"
            printf '  %-8s %-11s %8s %7s %-13s %8s %8s %8s %6s %8s\n' app result "wall s" inputs "clk/chd/key/op" "CPU ms" "peak MB" "set. MB" setup "launch s"
            awk -F'\t' -v t="$id" '$1 == t' "$RESULTS" | while IFS=$'\t' read -r _ app status summary; do
                if [ "$status" = cannot ]; then
                    printf '  %-8s cannot: %s\n' "$(app_name "$app")" "$summary"
                    continue
                fi
                local mark=""; if [ "$status" = unverified ]; then mark="*"; fi
                printf '  %-8s %-11s %8s %7s %-13s %8s %8s %8s %6s %8s\n' "$(app_name "$app")" "$status$mark" \
                    "$(awk -v m="$(field wall_ms "$summary")" 'BEGIN {printf "%.2f", m / 1000}')" \
                    "$(field inputs "$summary")" \
                    "$(field clicks "$summary")/$(field chords "$summary")/$(field keys "$summary")/$(field opens "$summary")" \
                    "$(field cpu_ms "$summary")" "$(field peak_mb "$summary")" "$(field settled_mb "$summary")" \
                    "$(field setup_inputs "$summary")" \
                    "$(awk -v m="$(field launch_ms "$summary")" 'BEGIN {printf "%.2f", m / 1000}')"
                local m w; m="$(field matched "$summary")"; w="$(field why "$summary")"
                if [ -n "$m" ]; then echo "             seen: $m"; fi
                if [ -n "$w" ]; then echo "             failed at $w"; fi
            done
        done
    } >"$OUT_TXT"
    {
        csv task app version status wall_ms inputs clicks chords keys opens setup_inputs setup_ms launch_ms cpu_ms peak_mb settled_mb seen note
        while IFS=$'\t' read -r task app status summary; do
            if [ "$status" = cannot ]; then
                csv "$task" "$app" "$(app_version "$app")" cannot "" "" "" "" "" "" "" "" "" "" "" "" "" "$summary"
            else
                csv "$task" "$app" "$(app_version "$app")" "$status" "$(field wall_ms "$summary")" "$(field inputs "$summary")" \
                    "$(field clicks "$summary")" "$(field chords "$summary")" "$(field keys "$summary")" "$(field opens "$summary")" \
                    "$(field setup_inputs "$summary")" "$(field setup_ms "$summary")" "$(field launch_ms "$summary")" \
                    "$(field cpu_ms "$summary")" "$(field peak_mb "$summary")" "$(field settled_mb "$summary")" \
                    "$(field matched "$summary")" "$(field why "$summary")"
            fi
        done <"$RESULTS"
    } >"$OUT_CSV"
    {
        csv task app step kind inputs input_ms wall_ms poll_ms status seen step_text
        # awk, since read would merge the empty fields between tabs
        touch "$WORK/steps.tsv"
        awk -F'\t' '
            function c(s) { gsub(/"/, "\"\"", s); return "\"" s "\"" }
            { print c($1) "," c($2) "," c($4) "," c($5) "," c($6) "," c($7) "," c($8) "," c($9) "," c($10) "," c($11) "," c($12) }
        ' "$WORK/steps.tsv"
    } >"$OUT_STEPS"
    cat "$OUT_TXT"
    echo
    echo "Saved to build/taskbench-mac.txt, build/taskbench-mac.csv, and build/taskbench-mac-steps.csv"
}

# ------------------------------------------------------------------ modes

selected_tasks() { if [ -n "${TASKS:-}" ]; then echo "$TASKS"; else task_ids; fi; }

list() {
    local row id app n
    echo "Tasks (scripts/taskbench-mac.sh, TASK_TABLE):"
    for row in "${TASK_TABLE[@]}"; do
        id="${row%%|*}"
        printf '\n  %-8s %s%s\n' "$id" "$(task_field "$id" 3)" "$(if [ "$(task_field "$id" 2)" = yes ]; then echo " (cold start)"; fi)"
        printf '           done when: %s\n' "$(task_field "$id" 4)"
        for app in mutible vscode zed; do
            RUN="$WORK/run/$id-$app"; RUNID="$id-$app"; EXTDIR="$VSEXT"
            if [ "$(task_field "$id" 2)" = yes ]; then EXTDIR="$RUN/ext"; fi
            local s; s="$("steps_${id}_$app")"
            if echo "$s" | grep -q '^cannot: '; then
                printf '           %-8s cannot: %s\n' "$(app_name "$app")" "$(echo "$s" | sed -n 's/^cannot: //p')"
            else
                n="$(echo "$s" | grep -vc '^setup')"
                printf '           %-8s %s step%s: %s\n' "$(app_name "$app")" "$n" "$([ "$n" = 1 ] || echo s)" \
                    "$(echo "$s" | grep -v '^setup' | sed -e 's/ =>.*//' -e 's/^finder .*/finder open/' | paste -sd ';' - | sed 's/;/; /g')"
            fi
        done
    done
}

show() {
    local id="$1" app
    [ -n "$(task_field "$id" 3)" ] || { echo "taskbench: no task \"$id\" (--list)" >&2; exit 2; }
    for app in mutible vscode zed; do
        RUN="$WORK/run/$id-$app"; RUNID="$id-$app"; EXTDIR="$VSEXT"
        if [ "$(task_field "$id" 2)" = yes ]; then EXTDIR="$RUN/ext"; fi
        echo "== $(app_name "$app")"
        "steps_${id}_$app"
        echo
    done
}

check() {
    local ok=0 a
    if [ -x "$TOOL" ]; then "$TOOL" check || ok=1; else echo "missing $TOOL (make build/taskbench-mac)"; ok=1; fi
    for a in mutible vscode zed; do
        local path; path="$(app_path "$a")"
        if [ -d "$path" ]; then
            local n; n="$(running "$(app_exe "$a")")"
            printf '%-8s %s %s (%s running now)\n' "$(app_name "$a")" "$(app_version "$a")" "$path" "$n"
            if [ "$a" = zed ] && [ "$n" != 0 ]; then echo "         your Zed is running: the Zed tasks are skipped until it quits"; fi
        else
            printf '%-8s not installed at %s (skipped)\n' "$(app_name "$a")" "$path"
        fi
    done
    if [ -x "$ZED_APP/Contents/MacOS/cli" ]; then
        if "$ZED_APP/Contents/MacOS/cli" --help 2>/dev/null | grep -q -- '--user-data-dir'; then
            echo "Zed has --user-data-dir: its profile is scratch, and HOME is scratch for ~/.config/zed"
        else
            echo "Zed has no --user-data-dir: only a scratch HOME keeps it off your settings"
        fi
    fi
    local need; need=$((LOG_MB + 1024))
    echo "disk: $(free_mb) MB free in $(dirname "$WORK"); the biglog task needs about $need MB"
    for a in git /usr/bin/python3 /usr/bin/sqlite3; do command -v "$a" >/dev/null || { echo "missing $a"; ok=1; }; done
    return $ok
}

case "${1:-}" in
    --list) list; exit 0 ;;
    --show) show "${2:-}"; exit 0 ;;
    --check) check; exit $? ;;
    --fixture)
        [ -x "$TOOL" ] || { echo "taskbench: make build/taskbench-mac first" >&2; exit 2; }
        mkdir -p "$WORK"; make_checks; make_fixture; echo "$F"; exit 0 ;;
    --report) APPS="mutible vscode zed"; report; exit 0 ;;
    --help|-h) sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; exit 0 ;;
    "") ;;
    *) echo "usage: $0 [--list | --show TASK | --check | --fixture | --report]" >&2; exit 2 ;;
esac

# ------------------------------------------------------------------ the run

[ -x "$TOOL" ] || { echo "taskbench: build the tool first: make build/taskbench-mac" >&2; exit 2; }
"$TOOL" check >/dev/null || { "$TOOL" check; exit 4; }
case "$MUTIBLE_APP" in "$REPO"/*)
    echo "taskbench: MUTIBLE_APP is inside the repo; opening it would register that copy with Launch Services (CLAUDE.md). Use an installed copy." >&2
    exit 2 ;;
esac
RUN_APPS=""
for a in $APPS; do
    path="$(app_path "$a")"
    if [ -z "$path" ] || [ ! -d "$path" ]; then echo "taskbench: $a is not installed; skipped"; continue; fi
    if [ "$a" = zed ] && [ "$(running "$(app_exe zed)")" != 0 ]; then
        echo "taskbench: your Zed is running, and a second Zed hands its requests to it; Zed skipped (quit Zed to include it)"
        continue
    fi
    RUN_APPS="$RUN_APPS $a"
done
[ -n "$RUN_APPS" ] || { echo "taskbench: no app to run" >&2; exit 2; }
APPS="$RUN_APPS"

trap cleanup EXIT
rm -rf "$WORK"
mkdir -p "$WORK" "$VSEXT"
RAN=1
: >"$RESULTS"
: >"$WORK/steps.tsv"
make_checks

echo "taskbench: this drives$(for a in $APPS; do printf ' %s,' "$(app_name "$a")"; done | sed 's/,$//') with posted keys and clicks."
echo "Leave the keyboard and mouse alone until it says it is done; touching them stops it."
"$TOOL" idle-wait 15 "$STATE"
caffeinate -d -i -w $$ &
STARTED_PIDS+=("$!")

for a in $APPS; do
    echo "== setup: $(app_name "$a")"
    make_fixture
    prep_app "$a"
done
for task in $(selected_tasks); do
    [ -n "$(task_field "$task" 3)" ] || { echo "taskbench: no task \"$task\"; skipped"; continue; }
    echo "== $task: $(task_field "$task" 3)"
    if [ "$task" = biglog ]; then
        if [ "$(free_mb)" -lt $((LOG_MB + 1024)) ]; then
            for a in $APPS; do printf '%s\t%s\tcannot\t%s\n' biglog "$a" "skipped: under $((LOG_MB + 1024)) MB free for the log" >>"$RESULTS"; done
            echo "   skipped: not enough disk for a $LOG_MB MB log"
            continue
        fi
        mkdir -p "$(dirname "$BIGLOG")"
        "$TOOL" biglog "$BIGLOG" "$LOG_MB" "$NEEDLE" >/dev/null
    fi
    for a in $APPS; do run_task "$task" "$a"; done
    if [ "$task" = biglog ]; then rm -f "$BIGLOG"; fi
done
echo
report
