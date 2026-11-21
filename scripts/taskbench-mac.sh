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
#   TASKBENCH_DIR=DIR           another scratch folder (its name must hold "taskbench")
#   KEEP_APP=1                  leave the last task's app running, to look at
#   TASKBENCH_TRACE=DIR         each step's CPU samples and every frame read from the screen
#   MUTIBLE_APP, VSCODE_APP, ZED_APP   other copies of the apps (installed copies only)
#
# How it works: the script makes a fixture project under the scratch folder,
# and for each task and app writes the app's steps to a file and runs
# build/taskbench-mac on it (tools/taskbench-mac.m has the step language).
# The tool starts the app on a scratch profile, sends each input (posted key
# events, clicks on elements found through the Accessibility API, a file
# opened with `open -a` as Finder opens it) and polls the step's result
# condition every 10 ms: a window title, an element's text, the keyboard's
# place, git's index, a file on disk, a port closing, text or brightness
# read from the screen. Then the script stops the app (only pids it
# started) and goes on.
#
# Measures, per task and app:
#   wall       the sum of each step's time from its last input event to when its
#              result happened: when a poll found it, or for a CPU-settled step
#              when the app last worked, or for a screen read the first frame
#              that showed it (the steps CSV says which: its observed column)
#   inputs     clicks + key chords + typed keystrokes + Finder opens
#   CPU        user + system time of the app's process tree during the steps
#              (children it reaped included; helpers launchd started for it
#              counted by responsible pid, as membench does)
#   peak MB    the tree's largest physical footprint from launch through settle
#              (a 10 ms sampler); the report fails if it is below the settled
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
CLAMP_LC="63:15"   # where app.py calls clamp (line:column); make_fixture sets it from the file
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
"definition|no|Go to the definition of a function used in app.py and defined in util.py, with the app's Python language server|util.py is the window's file (its title; in mutible and VS Code also the keyboard in its text, which Zed's AX does not show)"
"rename|no|Rename that function across the project with the language server, saved to disk|util.py, app.py, and report.py on disk call it bound_value, and clamp is gone"
"replace|no|Replace a string in three files across the project, saved to disk|no file on disk holds warehouse-a, and three places hold warehouse-b"
"history|no|Show app.py's git history and open its version from an older commit|a view shows the line only that commit had (TB_PAST_VERSION)"
"theme|no|Change the color theme to a light one and see it applied|the window's mean brightness, read from the screen, passes the middle"
"multicursor|no|Rename a variable used four times in one file with one cursor per use, saved to disk|orders.py on disk holds amount four times and qty nowhere"
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
# `quiet MS` (the app's CPU settled, the step ending when it last worked)
# stands in where neither AX nor the screen can read a result, and makes the
# result "unverified"; `screen TEXT` and `luma>X` read the screen, ending
# at the first frame that showed it; `sclick TEXT` clicks text read there;
# `text TEXT` finds a file's text through AX; `settle MS` waits for the
# whole tree after a launch. The observed column of the steps CSV says
# which ended each step (an `sh` check naming git reads as git). A function
# that prints `cannot: why` records the task as one the app cannot do.
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
    echo "press AXButton=Stage change 1 in app.py => sh $CHECKS/git-staged-one-only"
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
    echo "key cmd+k cmd+alt+s => sh $CHECKS/git-staged-one-only"
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
    echo "key cmd+y => sh $CHECKS/git-staged-one-only"
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


# The neutral tasks (2026-11-20, the owner's words: "tasks we did not
# choose, where VS Code and Zed are expected to be strong"). Each app's
# shortest documented path, with its own keys:
#   VS Code: code.visualstudio.com/docs/reference/default-keybindings
#     (F12 Go to Definition, F2 Rename Symbol, Ctrl+G Go to Line,
#     Shift+Cmd+H Replace in Files, Cmd+K Cmd+T Color Theme, Shift+Cmd+L
#     Select All Occurrences), docs/editing/refactoring ("Rename Symbol"),
#     docs/sourcecontrol/overview ("Timeline view"), docs/editing/codebasics
#     ("Multiple selections"); the Python extension
#     (marketplace ms-python.python, which brings Pylance) is setup.
#   Zed: its default keymap (assets/keymaps/default-macos.json: f12
#     editor::GoToDefinition, f2 editor::Rename, ctrl-g go_to_line::Toggle,
#     cmd-shift-h pane::DeploySearch with replace, cmd-enter
#     search::ReplaceAll, cmd-alt-s workspace::SaveAll, cmd-k cmd-t
#     theme_selector::Toggle, cmd-shift-l editor::SelectAllMatches) and
#     zed.dev/docs/git ("File History ... use the Command Palette and search
#     for file history"); Python is built in (basedpyright, downloaded by Zed
#     the first time, in the template's setup).
#   mutible: docs/REFERENCE.md (F12, F2, Ctrl+G, Option+Shift+Cmd+F Replace
#     across the whole folder, Ctrl+Cmd+T Choose a color theme, Shift+Cmd+L
#     Select every place, Option+Cmd+S Save every tab, Shift+Cmd+G Source
#     Control and its commit graph); its Python server is basedpyright from
#     the owner's install (~/.local/bin, found with nothing set up), the one Zed finds too.
# A language server starts with the first Python file. F12 and F2 are
# pressed as a person presses them, once the cursor is on the name, and the
# step lasts until the result shows, so a server still starting is in the
# task's time. Waiting for the tree to settle first was tried and dropped:
# VS Code's stayed busy 30 to 90 s while Pylance indexed, which F12 does
# not wait for.

# Cursor to the call of clamp in app.py: Ctrl+G with line:column in all three.
goto_clamp() {   # goto_clamp <app>
    case "$1" in
        mutible)
            # Go to Line is the palette with ":" in its field, as VS Code's.
            echo "key ctrl+g => focus AXTextField=: @5"
            echo "type $CLAMP_LC => focus AXTextField=:$CLAMP_LC || ax AXTextField=:$CLAMP_LC @5"
            echo "key return => focus AXTextArea @5" ;;
        vscode)
            # Go to Line is Quick Open with ":" in its field.
            echo "key ctrl+g => ax AXTextField=: @5"
            echo "type $CLAMP_LC => ax AXTextField=:$CLAMP_LC @5"
            echo "key return => focus AXTextArea @5" ;;
        zed)
            # No AX tree: each step waits for the CPU to settle.
            echo "key ctrl+g => ?quiet 150 @3"
            echo "type $CLAMP_LC => ?quiet 150 @3"
            echo "key return => ?quiet 150 @5" ;;
    esac
}

open_py() {   # open_py <app> <file>: Cmd+P to a Python file with the keyboard in it
    case "$1" in
        # REFERENCE.md: Quick Open puts the keyboard in the file it opens,
        # as VS Code's does (since 2026-11-20; 1.6.9 gave it back to the
        # file tree, watched with axdump: AXOutline focused).
        mutible) open_by_name "$2" "window~$2 && focus AXTextArea" "focus AXTextField=$2" ;;
        vscode) vscode_open_by_name "$2" "window~$2 && focus AXTextArea" ;;
        zed) open_by_name "$2" "window~$2" ;;
    esac
}

vscode_python() {
    # Through the GUI: Shift+Cmd+X, "python" typed, Install: 1 + 6 + 1.
    echo "setup 8: sh $(code_cli) --install-extension ms-python.python >/dev/null 2>&1 => none @600"
}

# 7. definition -------------------------------------------------------------

steps_definition_mutible() {
    open_py mutible app.py; goto_clamp mutible
    echo "key f12 => window~util.py && focus AXTextArea @30"
}
steps_definition_vscode() {
    vscode_python
    open_py vscode app.py; goto_clamp vscode
    echo "key f12 => window~util.py && focus AXTextArea @30"
}
steps_definition_zed() {
    open_py zed app.py; goto_clamp zed
    echo "key f12 => window~util.py @30"
}

# 8. rename -----------------------------------------------------------------

steps_rename_mutible() {
    open_py mutible app.py; goto_clamp mutible
    echo "key f2 => focus AXTextField=clamp || ax AXTextField=clamp @15"
    echo "type bound_value => focus AXTextField=bound_value || ax AXTextField=bound_value @5"
    echo "key return => focus AXTextArea @15"
    echo "key cmd+alt+s => sh $CHECKS/file-renamed @30"
}
steps_rename_vscode() {
    vscode_python
    open_py vscode app.py; goto_clamp vscode
    # The rename box over the name, its text selected; files a rename
    # changes are saved (files.refactoring.autoSave, on by default).
    echo "key f2 => ax AXTextField=clamp @15"
    echo "type bound_value => ax AXTextField=bound_value @5"
    echo "key return => sh $CHECKS/file-renamed @30"
}
steps_rename_zed() {
    open_py zed app.py; goto_clamp zed
    echo "key f2 => ?quiet 150 @5"
    echo "type bound_value => ?quiet 150 @5"
    echo "key return => ?quiet 300 @15"
    echo "key cmd+alt+s => sh $CHECKS/file-renamed @30"
}

# 9. replace ----------------------------------------------------------------

steps_replace_mutible() {
    # REFERENCE.md: Replace in Folder (Option+Shift+Cmd+F) opens the folder
    # search with its replacement, the keyboard in the search field when
    # nothing is selected, as VS Code's Replace in Files (since 2026-11-20;
    # 1.6.9 put it in the replacement field, so the search came first from
    # Shift+Cmd+F); Tab goes on to the replacement; Option+Cmd+Return is
    # Replace All, after its question ("Replace 3 matches in 3 files?").
    echo "key cmd+alt+shift+f => focus AXTextField @10"
    echo "type warehouse-a => ax AXTextField=warehouse-a @5"
    echo "key tab => focus AXTextField @5"
    echo "type warehouse-b => ax AXTextField=warehouse-a && ax AXTextField=warehouse-b @5"
    echo "key cmd+alt+return => ax *~Replace 3 matches @15"
    echo "press AXButton=Replace => sh $CHECKS/file-replaced @30"
}
steps_replace_vscode() {
    # Replace in Files (Shift+Cmd+H) opens Search with the keyboard in its
    # field; Tab goes on to the replace field; Option+Cmd+Return is Replace
    # All, after its question; the files are saved. VS Code reports the
    # keyboard's element wrongly (Traps), so the fields are read by value.
    echo "key cmd+shift+h => ax AXTextArea~Search: Type Search Term @10"
    echo "type warehouse-a => ax AXStaticText~3 results in 3 files @15"
    echo "key tab => focus AXTextArea~Replace @5"
    echo "type warehouse-b => text warehouse-b @5"
    echo "key cmd+alt+return => ax AXButton=Replace @15"
    echo "press AXButton=Replace => sh $CHECKS/file-replaced @30"
}
steps_replace_zed() {
    # Shift+Cmd+H: project search with its replace field; Tab to it;
    # Cmd+Return is Replace All, which leaves the files edited, unsaved;
    # Option+Cmd+S saves them all.
    echo "key cmd+shift+h => ?quiet 150 @5"
    echo "type warehouse-a => ?quiet 300 @15"
    echo "key tab => ?quiet 150 @5"
    echo "type warehouse-b => ?quiet 150 @5"
    echo "key cmd+return => ?quiet 300 @15"
    echo "key cmd+alt+s => sh $CHECKS/file-replaced @30"
}

# 10. history ---------------------------------------------------------------

steps_history_mutible() {
    # The commit graph is the branch's history, not the file's (mutible has
    # no history of one file); a click on a commit opens its page, every
    # file it changed as it changed them. REFERENCE.md: VoiceOver (so AX)
    # reads each graph row as its commit, the subject first, and pressing
    # it opens the commit as a click does (since 2026-11-20; 1.6.9's rows
    # had no AX text, so the commit was found on the screen).
    echo "key cmd+shift+g => ax AXCell~Raise the limit @10"
    echo "press AXCell~Raise the limit => text TB_PAST_VERSION || screen TB_PAST_VERSION @15"
}
steps_history_vscode() {
    open_py vscode app.py
    # The Explorer's Timeline section (collapsed), then the commit's row:
    # its diff against the commit before. Its editors show no text through
    # AX until one has the keyboard, so the line is read from the screen.
    echo "click AXButton~Timeline Section => ax AXRow~Raise the limit @15"
    echo "click AXRow~Raise the limit => screen TB_PAST_VERSION @15"
}
steps_history_zed() {
    # zed.dev/docs (its git page): File History from the command palette,
    # for the open file; a commit opens its diff for that file.
    open_py zed app.py
    echo "key cmd+shift+p => ?quiet 150 @3"
    echo "type file history => ?quiet 150 @3"
    echo "key return => screen Raise the limit @15"
    # A click selects the commit (its details beside the list); Return opens
    # its diff (watched 2026-11-20).
    echo "sclick Raise the limit => screen 1 Changed File @10"
    echo "key return => screen TB_PAST_VERSION @15"
}

# 11. theme -----------------------------------------------------------------
#
# All three start dark (mutible and VS Code use Dark Modern whatever the
# system, Zed follows the system, which is dark here); each is switched to
# its own light theme. The result is read from the screen for all three:
# the front window's mean brightness passes the middle.

light_theme_check() { if [ "$(defaults read -g AppleInterfaceStyle 2>/dev/null)" = Dark ]; then echo dark; else echo light; fi; }

steps_theme_mutible() {
    echo "key ctrl+cmd+t => focus AXTextField @5"
    echo "type light modern => focus AXTextField=light modern || ax AXTextField=light modern @5"
    echo "key return => luma>0.5 @10"
}
steps_theme_vscode() {
    echo "key cmd+k cmd+t => ax AXTextField~color theme @5"
    echo "type light modern => ax AXTextField=light modern @5"
    echo "key return => luma>0.5 @10"
}
steps_theme_zed() {
    if [ "$(light_theme_check)" = light ]; then
        echo "cannot: Zed follows the system's appearance, which is light here, so it starts light; run with the system dark"
        return
    fi
    echo "key cmd+k cmd+t => ?quiet 150 @5"
    echo "type one light => ?quiet 150 @5"
    echo "key return => luma>0.5 @10"
}

# 12. multicursor -----------------------------------------------------------
#
# orders.py opens with the cursor at its start, on qty. Select every
# occurrence of the word at the cursor (Shift+Cmd+L in all three), type the
# new name, save. The chord and the typing wait for nothing: keys reach the
# editor in order, and the saved file on disk is the result.

multicursor_steps() {   # multicursor_steps <app>
    open_py "$1" orders.py
    echo "key cmd+shift+l => none"
    echo "type amount => none"
    echo "key cmd+s => sh $CHECKS/file-multicursor @15"
}
steps_multicursor_mutible() { multicursor_steps mutible; }
steps_multicursor_vscode() { multicursor_steps vscode; }
steps_multicursor_zed() { multicursor_steps zed; }

# ------------------------------------------------------------------ setup once per app
#
# Run on a template profile that every task then copies, so what an app asks
# once (VS Code's folder trust) is answered once and counted as setup.

prep_mutible() {
    echo "launch $(open_mutible bg "$P") => window @60"
    echo "setup 0: none => ?settle 1000 @20"
}
prep_vscode() {
    echo "launch $(open_vscode bg "$P") => window @90"
    # "Do you trust the authors of the files in this folder?", once a folder.
    echo "setup 1: ?press AXButton~Yes, I trust => !ax AXButton~Yes, I trust @20"
    echo "setup 0: none => ?settle 1000 @20"
}
prep_zed() {
    # Zed opens an unknown folder behind a modal "Unrecognized Project"
    # question (Restricted Mode), which AX cannot read and which took every
    # key of the first run's tasks (Cmd+Y staged nothing). Return is its
    # "Trust and Continue"; Zed keeps the answer in the profile the tasks
    # copy (watched 2026-11-20 in a screenshot), so it is asked once, like
    # VS Code's folder trust.
    echo "launch $(open_zed bg "$P") => window @90"
    echo "setup 0: none => ?settle 1500 @30"
    echo "setup 1: key return => ?quiet 500 @10"
}

# ------------------------------------------------------------------ the fixture

make_checks() {
    mkdir -p "$CHECKS"
    cat >"$CHECKS/git-staged-one-only" <<EOF
#!/bin/sh
# Task 1 is done: app.py's first hunk is staged, and nothing else.
d="\$(git -C "$P" diff --cached)"
echo "\$d" | grep -q TB_HUNK_ONE || exit 1
echo "\$d" | grep -q -e TB_HUNK_TWO -e TB_HUNK_THREE -e TB_NEW_FILE && exit 1
exit 0
EOF
    chmod +x "$CHECKS/git-staged-one-only"
    cat >"$CHECKS/file-renamed" <<EOF
#!/bin/sh
# Task rename is done: the function is bound_value in its file and both
# callers' (one definition, two imports, two calls), on disk.
cd "$P" || exit 1
grep -qw clamp util.py app.py report.py && exit 1
[ "\$(cat util.py app.py report.py | grep -o bound_value | wc -l | tr -d ' ')" = 5 ]
EOF
    cat >"$CHECKS/file-replaced" <<EOF
#!/bin/sh
# Task replace is done: warehouse-a is nowhere on disk, warehouse-b in its
# three places.
cd "$P" || exit 1
grep -rq --exclude-dir=.git warehouse-a . && exit 1
[ "\$(grep -ro --exclude-dir=.git warehouse-b . | wc -l | tr -d ' ')" = 3 ]
EOF
    cat >"$CHECKS/file-multicursor" <<EOF
#!/bin/sh
# Task multicursor is done: orders.py on disk says amount four times, qty never.
cd "$P" || exit 1
grep -qw qty orders.py && exit 1
[ "\$(grep -o amount orders.py | wc -l | tr -d ' ')" = 4 ]
EOF
    chmod +x "$CHECKS"/file-*
}

make_fixture() {
    rm -rf "$F"
    mkdir -p "$P/media" "$F/lonely" "$P/site"
    {
        echo '"""Inventory service: the fixture for the task benchmark."""'
        echo 'import json'
        echo 'import sys'
        echo 'from util import clamp'
        echo 'LIMIT = 10'
        echo '# Stock for warehouse-a.'
        echo
        for i in $(seq 1 18); do
            echo "def item_$i(stock):"
            echo "    return stock - $i"
            echo
        done
        echo 'if __name__ == "__main__":'
        echo '    level = clamp(LIMIT, 0, 5)'
        echo '    print(json.dumps({"limit": LIMIT, "level": level}))'
    } >"$P/app.py"
    # Where the call of clamp is, as Ctrl+G takes it (line:column, a column
    # inside the name).
    CLAMP_LC="$(awk '/level = clamp\(/ {print NR ":" index($0, "clamp") + 2; exit}' "$P/app.py")"
    printf '"""Reports for the stock in warehouse-a."""\nfrom util import clamp\n\n\ndef report(level):\n    return clamp(level, 0, 100)\n' >"$P/report.py"
    printf 'qty = 0\nqty += 3\nprint(qty)\ntotal = qty * 2\n' >"$P/orders.py"
    printf 'def clamp(x, lo, hi):\n    return max(lo, min(x, hi))\n\n\ndef total(xs):\n    return sum(xs)\n' >"$P/util.py"
    printf '# Inventory\n\nA small service for the task benchmark. Stock is kept in warehouse-a.\n' >"$P/README.md"
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
    # app.py's history (task history): a commit with a line only it had, one
    # that touches another file, and one that takes the line back out.
    sed -i '' 's/^LIMIT = 10$/LIMIT = 11  # TB_PAST_VERSION: a trial limit/' "$P/app.py"
    GIT_CONFIG_GLOBAL=/dev/null GIT_AUTHOR_DATE="2026-11-02T10:00:00" GIT_COMMITTER_DATE="2026-11-02T10:00:00" \
        "${g[@]}" commit -q -am "Raise the limit for the sale"
    printf '\nRun it with python3 app.py.\n' >>"$P/README.md"
    GIT_CONFIG_GLOBAL=/dev/null GIT_AUTHOR_DATE="2026-11-03T10:00:00" GIT_COMMITTER_DATE="2026-11-03T10:00:00" \
        "${g[@]}" commit -q -am "Say how to run it"
    sed -i '' 's/^LIMIT = 11  # TB_PAST_VERSION: a trial limit$/LIMIT = 10/' "$P/app.py"
    GIT_CONFIG_GLOBAL=/dev/null GIT_AUTHOR_DATE="2026-11-04T10:00:00" GIT_COMMITTER_DATE="2026-11-04T10:00:00" \
        "${g[@]}" commit -q -am "Put the limit back"
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
        [ "${KEEP_APP:-}" = 1 ] && break
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

quit_app() {   # quit_app <app> <pid> (KEEP_APP=1 leaves the last task's app running, to look at)
    if [ "${KEEP_APP:-}" = 1 ] && [ "${3:-}" != always ]; then return 0; fi
    [ -n "$2" ] && "$TOOL" quit --exe "$(app_exe "$1")" "$2" || true
}

# A task on the project runs on a copy of the app's template profile (its
# setup done, its extensions); a cold task on a fresh profile with only the
# quiet settings, as a first open from Finder would meet it.
new_run() {   # new_run <task> <app>: a fresh scratch profile for this run
    RUNID="$1-$2-$$"
    RUN="$WORK/run/$1-$2"
    EXTDIR="$VSEXT/$1"   # a task's own: only the extensions its setup installs
    mkdir -p "$EXTDIR"
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
    "editor.accessibilitySupport": "on",
    "editor.cursorBlinking": "solid"
}
EOF
            ;;
        zed)
            # Quiet only, in both places Zed may read it (with
            # --user-data-dir, and under the scratch HOME).
            mkdir -p "$RUN/data/config" "$RUN/home/.config/zed"
            printf '{\n  "auto_update": false,\n  "cursor_blink": false,\n  "telemetry": {"diagnostics": false, "metrics": false}\n}\n' \
                | tee "$RUN/data/config/settings.json" >"$RUN/home/.config/zed/settings.json"
            ;;
    esac
    "prep_$app" >"$RUN/steps.txt"
    local rc=0
    run_steps setup "$app" "$RUN/steps.txt" || rc=$?
    quit_app "$app" "$LAST_PID" always
    if [ "$rc" = 3 ]; then aborted; fi
    printf 'setup\t%s\t%s\t%s\n' "$app" "$([ $rc = 0 ] && echo ok || echo failed)" "$LAST_SUMMARY" >>"$RESULTS"
}

# A locked screen keeps every app from coming to the front, so each task
# would fail with "would not come to the front" (2026-11-21, run 4 on
# 1.6.10: the screen locked partway and 22 of 36 rows failed). Before each
# task, wait while the console is locked, then for the owner to be idle again.
screen_locked() {
    ioreg -n Root -d1 2>/dev/null | grep -q '"IOConsoleLocked" = Yes'
}
wait_unlocked() {
    if screen_locked; then
        echo "   the screen is locked; waiting for it to be unlocked..."
        while screen_locked; do sleep 5; done
        "$TOOL" idle-wait 15 "$STATE"
    fi
}

run_task() {   # run_task <task> <app>
    local task="$1" app="$2" reason
    wait_unlocked
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
          echo "setup 0: none => ?settle 1000 @30"
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
        local why; why="$(awk -F'\t' '$1 == "step" && $10 != "ok" && $10 != "unverified" && $10 !~ /^skipped/ {print "step " $2 ": " $10 " (" $12 ")"}' "$RUN/out.txt" | head -1 || true)"
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

# The footprint is sampled from the launch through the settle, so the peak
# can never be below the settled footprint (the last sample). A row where it
# is would be a measuring fault: the report says so and the run fails.
check_peaks() {
    awk -F'\t' '$3 != "cannot" {
        peak = ""; set = ""
        for (i = 4; i <= NF; i++) {
            if ($i ~ /^peak_mb=/) peak = substr($i, 9) + 0
            if ($i ~ /^settled_mb=/) set = substr($i, 12) + 0
        }
        if (peak != "" && set != "" && peak < set) { print $1 " on " $2 ": peak " peak " MB < settled " set " MB"; bad = 1 }
    } END { exit bad }' "$RESULTS"
}

report() {
    [ -f "$RESULTS" ] || return 0
    local peaks_bad=""
    peaks_bad="$(check_peaks)" || true
    local apps_line="" a
    for a in $APPS; do apps_line="$apps_line${apps_line:+, }$(app_name "$a") $(app_version "$a")"; done
    {
        echo "mutible taskbench, $(date '+%Y-%m-%d %H:%M')"
        echo "$(sw_vers -productName) $(sw_vers -productVersion), $(sysctl -n machdep.cpu.brand_string), $(( $(sysctl -n hw.memsize) / 1073741824 )) GB"
        echo "$apps_line"
        echo "wall: from each step's last input event to when its result happened (a CPU-settled step at its last busy"
        echo "  sample, a screen read at the first frame that showed it), summed; inputs: clicks/chords/keys/opens;"
        echo "CPU: the app's process tree during the steps; MB: physical footprint of the tree, peak (sampled every"
        echo "  10 ms from launch through settle) and settled. Each step's end is observed by ax, file, git, port,"
        echo "  cpu-quiet, or screen (the steps CSV's observed column)."
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
        csv task app step kind inputs input_ms wall_ms poll_ms res_ms observed status seen step_text
        # awk, since read would merge the empty fields between tabs
        touch "$WORK/steps.tsv"
        awk -F'\t' '
            function c(s) { gsub(/"/, "\"\"", s); return "\"" s "\"" }
            { print c($1) "," c($2) "," c($4) "," c($5) "," c($6) "," c($7) "," c($8) "," c($9) "," c($10) "," c($11) "," c($12) "," c($13) "," c($14) }
        ' "$WORK/steps.tsv"
    } >"$OUT_STEPS"
    if [ -n "$peaks_bad" ]; then
        { echo; echo "MEASURING FAULT: a peak below its settled footprint (the peak is sampled from launch through settle):"
          echo "$peaks_bad"; } >>"$OUT_TXT"
    fi
    cat "$OUT_TXT"
    echo
    echo "Saved to build/taskbench-mac.txt, build/taskbench-mac.csv, and build/taskbench-mac-steps.csv"
    if [ -n "$peaks_bad" ]; then echo "taskbench: peak below settled; see the report" >&2; return 5; fi
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
    --report) APPS="mutible vscode zed"; report; exit $? ;;
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
    wait_unlocked
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
report || exit $?
