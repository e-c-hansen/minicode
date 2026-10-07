# android/ — the Android port

MiniCode for Android, built on the same portable C++ core as the macOS app
(`../src`) and the GTK port (`../linux`). CMake compiles that core straight
from `../src` through the NDK; nothing is copied here, so highlighting,
Markdown, the terminal screen and the rest behave the same everywhere and
stay covered by `../tests/run_tests.cpp`.

## Install

1. On the phone, open the [latest release](https://github.com/e-c-hansen/minicode/releases/latest)
   and download `MiniCode-<version>.apk`.
2. Open the download. Android asks once whether your browser may install
   apps; allow it, then install.
3. Open MiniCode and tap "Open a folder on phone storage" on the start screen.

To keep it updated without checking by hand, add
`https://github.com/e-c-hansen/minicode` to
[Obtainium](https://github.com/ImranR98/Obtainium), which installs from a
repository's releases. MiniCode is not on the Play Store: Google allows the
"All files access" the terminal uses only for some kinds of app, and new
developer accounts need a two-week closed test before publishing anything.

Every release is signed with the same key, so updates install over the old
version. The APK is built by `scripts/release.sh` from the tagged source.

### Termux: python, git, language servers and LaTeX (optional)

These run in [Termux](https://f-droid.org/packages/com.termux/), installed
from F-Droid (the Play Store copy is years out of date). Once it is set up,
MiniCode's terminal is Termux's bash, so everything installed there runs in
the terminal too:

1. In Termux:

       pkg install git clang tectonic python
       pip install python-lsp-server
       echo allow-external-apps=true >> ~/.termux/termux.properties
       termux-reload-settings
       termux-setup-storage

2. In MiniCode, ⋮ → Termux tools → Allow. It then lists which language
   servers, tectonic and git it found. The next terminal you open is
   Termux's bash.
3. Keep projects in phone storage (Termux sees it as `~/storage/shared`),
   since that is the one place both apps can reach.

The rest of this file is about how the port is built and developed.

## What it has

- **Start screen.** With no folder open, the window says so and offers
  large rows, for touch or the arrow keys and Enter: open a folder on phone
  storage, open one from another app or cloud, the recent folders (the last
  six, newest first, in the `recent` preference; ones that are gone or lost
  their permission are dropped), the terminal and the shortcuts
  (`StartScreen.kt`). `am start ... --ez start true` shows it without
  forgetting the saved folder, for looking at it on a phone already set up.
- **File list.** One pane at a time rather than a sidebar: on a 576 by 640 dp
  screen there is no room for two. Folders are opened through the system
  document picker, so the editor needs no storage permission, and the choice
  is remembered between launches. Only the terminal asks for one (below).
  An empty folder says so and offers New file (also in the ⋮ menu), which
  asks for a name, creates the file and opens it. A folder the app can no
  longer read (moved, deleted, its permission taken back, or no "All files
  access" for a path) says which, with a row to fix it. Back in a subfolder
  goes up one. The list follows the disk: see "The file list follows the
  disk" below. A long press and a drag moves a file or folder into another
  folder: see "Moving files by dragging".
- **The title bar says where you are.** The first line names the pane (the
  folder, the file with a dot while unsaved, Terminal, Browser, Source
  Control, a diff); the second says where it is, as "Phone storage / mc-test
  / docs", or the app a picked folder comes from, plus Unsaved, Preview,
  Source or View only for a file.
- **No editor without a file.** Asking for the editor with nothing open (the
  leader alone, leader F) stays on the list and says no file is open, so
  nothing can be typed into a buffer with nowhere to be saved.
- **Editor.** The shared highlighter colours the file; autocorrect,
  suggestions, automatic capitals and the composing region are all off,
  because a keyboard that rewrites words is wrong for code (see
  CodeEditText, and "Pastiera" below).
- **Markdown preview.** The shared parser, rendered as the Mac and Linux
  render it: links that open on a tap, real tables whose cells wrap,
  pictures beside the file or at an https address shown inline (GIFs
  playing), and a double tap on a block to edit its Markdown. The leader's P flips between the preview
  and the source, keeping the place both ways. See "The Markdown preview"
  below.
- **Task lists.** A task's box in the preview ticks on a tap, leader L
  makes or toggles tasks in the source, Enter continues a list, the title
  counts what is done, and leader W lists every TODO comment and open task
  in the project. See "Task lists and the TODO list" below.
- **Images and PDFs.** Decoded by Android, scaled down but never up, with the
  pixel size or page count in the title bar. A PDF shows its first page.
- **Video and audio.** mp4, mov, webm, mkv and the rest, and mp3, flac, ogg
  and the rest, played by Android's own MediaPlayer in the editor's place,
  paused on the first frame until Space or the play button. See "Video and
  audio" below.
- **Terminal.** Termux's bash when Termux is set up, otherwise the phone's
  own `/system/bin/sh` with the toybox utilities, either one parsed by the
  shared TerminalScreen: the same grid that runs vim and less on the Mac.
  It starts in the open folder and follows it when another is opened, as
  long as the folder has a path. ⋮ → Shell picks one. See "Termux's bash in
  the terminal" below.
- **Browser.** The system web view with a URL bar.
- **LaTeX preview.** Typeset by tectonic in Termux, every page in a
  scrolling list, and a double tap on the text opens the source behind it
  for editing, as on the Mac. See "The LaTeX preview" below.
- **Language servers.** clangd, pylsp and the rest, run in Termux (see
  below): squiggles under errors and warnings, the message for the one at
  the caret in a line under the editor, completion, hover and go to
  definition.
- **Source control.** The Mac and Linux panel, with git run in Termux, in
  the file list's place (leader V): staged and unstaged changes, diffs,
  staging, committing, and the commit graph with what is pushed and what is
  not. See "Source control" below.

## Shortcuts, and why they are unusual

A phone keyboard is not a desktop keyboard. On a Unihertz Titan 2 there is no
Ctrl, Esc or Tab; Alt is the symbol layer, so Alt+S types "4"; and Android
claims Sym with some letters before an app sees them, so Sym+H opens the
microphone. What is left is one unclaimed key, so that key is a leader:

    the key left of right Shift, then
      S  save            P  Markdown preview      O  open a folder
      F  files or editor T  terminal              H  the shortcut list
      B  browser         V  source control        Y  on-screen keyboard
      U  undo            R  redo
      L  toggle a task   W  TODOs in the project
    in the editor, with a language server:
      N  complete        K  what the symbol is    G  go to its definition
    in the terminal:
      C  Ctrl C          D  Ctrl D                E  Escape     I  Tab
    in source control:
      Enter  commit

The key alone switches panes, twice opens the menu, and the ⋮ button in the
title bar offers the same items for a device whose keyboard offers nothing.
G was taken by go to definition, so the leader's letter for source control
is V.

### A held Ctrl

A USB or Bluetooth keyboard has Ctrl, and so does a Titan 2 whose Fn key is
set to act as Ctrl (below, under Pastiera). Holding Ctrl and pressing a
letter does what the leader and that letter do (`ctrlAction` in
`MainActivity.kt`), with these exceptions:

- C, V, X, A, Z and Y are the text field's: copy, paste, cut, select all,
  undo and redo. Ctrl+Z and Ctrl+Shift+Z undo and redo in the editor and in
  the Markdown preview; Ctrl+U and Ctrl+R do too, as the leader's U and R.
  Leader Y, the on-screen keyboard, has no Ctrl form.
- C, D, E and I stand in for keys a real Ctrl already sends to a shell
  (Ctrl C, Ctrl D, Ctrl [ and Ctrl I), so they are not taken.
- Ctrl+B is the file list, as Cmd+B is on the Mac, and Ctrl+Shift+B the
  browser, as on Linux. Ctrl+G is go to definition and Ctrl+Shift+G source
  control, as on the Mac and Linux. Ctrl+Shift+Space plays or pauses a
  video or audio file, as on Linux.
- Ctrl+L toggles a task, as leader L does, and Ctrl+Shift+L is the TODO
  list. Leader W, the TODO list, has no Ctrl form, since Ctrl+W closes
  things everywhere else.
- While the terminal has the keyboard, Ctrl and a letter go to the shell,
  since bash needs Ctrl R, Ctrl U, Ctrl P and the rest. Only Ctrl+S, B, O and
  H are taken there, as they always were. With Shift added every shortcut
  works in the terminal too (Ctrl+Shift+T leaves it), the way a desktop
  terminal keeps Ctrl+Shift for itself; Ctrl+Shift+C and V are left alone.

In the editor the key may come from the hardware or from an input method
that passes a held Ctrl on through `InputConnection.sendKeyEvent`, as
Pastiera does; either way it reaches `dispatchKeyEvent` with the Ctrl meta
state set, before the text field sees it.

One trap worth keeping: while the editor has focus, a letter never arrives as
a key event, because the keyboard reaches the field through the input method.
The leader's letter is caught in CodeEditText as text is committed; in the
file list and the terminal, the same letter arrives as a key event instead.
Both paths run the same table (`leaderActions`).

B is the browser on every port (Shift+Cmd+B on the Mac), so the file list
moved to F.

### Pastiera

[Pastiera](https://github.com/palsoftware/pastiera) is an input method for
phones with a physical keyboard, the Titan 2 among them. It works with
MiniCode, with a few things worth knowing (checked against Pastiera 0.86's
source):

- **It sees keys only while a text field has focus.** In the editor, the
  commit box and the Markdown edit box it turns keys into text one
  character at a time. The terminal declares no text field (TYPE_NULL), and
  the file list and source control panel are not text fields, so there it
  passes keys straight to MiniCode except for its own Nav Mode and Sym
  shortcuts.
- **No capitals or autocorrect in code.** Pastiera capitalises the first
  letter and after a period, turns a double space into ". ", and corrects
  words, in every field except password, URI, email and filter ones. The
  editor, the commit box and the Markdown edit box tell the keyboard they
  are visible-password fields, so all of that stays off and `if` stays
  `if`. Nothing is hidden, and other keyboards show their usual layout.
- **Hold Ctrl, do not tap it.** A held Ctrl reaches MiniCode as Ctrl and the
  letter (see "A held Ctrl" above). A tapped or double-tapped Ctrl is
  Pastiera's Nav Mode instead: Ctrl Q is Escape, Ctrl T is Tab, ESDF and
  IJKL are arrows, and so on.
- **Nav Mode in the terminal takes over typing** until Ctrl is tapped again,
  because the terminal is not a text field and Nav Mode is what Pastiera
  does outside one. If letters stop reaching the shell, tap Ctrl.
- **Sym in the terminal.** Pastiera's Sym power shortcuts also act outside
  text fields. Turn them off in Pastiera's settings if you want Sym to reach
  the terminal.
- **The leader keys pass through.** The unlabelled key left of right Shift
  (keycode 403), Menu and Function produce no character, so Pastiera leaves
  them alone and they work as before. A letter after the leader is caught
  as committed text in the editor, as with any keyboard.
- **The Titan 2's Fn key can be Ctrl.** Unihertz's own keyboard settings
  can set Fn to act as Ctrl; it then arrives as a left Ctrl, and every
  Ctrl shortcut above works from the phone's own keyboard.

### Symbols the keyboard lacks

The Titan 2's Alt layer has no #, backtick, braces, pipe or backslash, so a
Markdown heading could not be typed. Two ways around it:

- **A row of symbols under the editor** (`EditorKeys.kt`): Tab, `#`, `*`,
  backtick, `_ - [ ] ( ) { } < > | \ / ~ = + " ' ! ? @ $ % ^ & ; :`,
  scrolling sideways. A tap inserts at the caret through the editor's text,
  so it is highlighted, marks the file unsaved, updates the preview and can
  be undone like typing; the keys never take focus. It shows only while a
  file's source is on screen, and "Hide the symbol row" in the ⋮ menu takes
  it away for good (the `symbolRow` preference). The Markdown preview's edit
  box has the same row, starting with a New line key.
- **Clear of the rounded corners.** The Titan 2's screen has corners of a
  100 pixel radius, and a row flush with the bottom edge lost the outer
  halves of its first and last keys to them. Both rows (this one and the
  terminal's) sit 4 dp up, and `CurvedEdges` in `EditorKeys.kt` pads their
  ends by as much as the corner cuts in at the height of the keys' text,
  from `WindowInsets.getRoundedCorner` (Android 12 and later; 12 dp
  otherwise). The padding is inside the scrolling row, so the end keys can
  still be scrolled to, and a row lifted above the keyboard gets none.
- **The on-screen keyboard**, from the keyboard button in the title bar or
  leader Y. While a hardware keyboard is attached Android keeps the
  on-screen one hidden, and no app can override that: asked to show, the
  keyboard app draws only a thin strip with a hide arrow and a picker
  button. The switch that allows it is "Use on-screen keyboard", at the top
  of Android's keyboard picker (also Settings, System, Keyboard, Physical
  keyboard; it is the secure setting `show_ime_with_hard_keyboard`). So with
  that switch off the button opens the picker, and when the picker closes
  with the switch on, the keyboard comes up. With the switch on, the button
  simply shows and hides it. Checked on the Titan 2, whose Kika keyboard
  then offers a ?!# panel. The strip counts as "visible" in the window
  insets, so a keyboard is taken as up only when it is over 120 dp tall.

### The terminal and the open folder

The editor reaches files through the document picker, which hands out
content URIs, and a shell can only use paths. So the terminal can follow the
open folder only when that folder is on the phone's own storage, and only
once the app has "All files access" (Android 11 and later), which it asks for
the first time the terminal opens there. The URI's document id
(`primary:Documents/project`) maps to `/storage/emulated/0/Documents/project`.
A folder from a cloud provider, such as Google Drive, has no path at all, and
a folder inside Termux (opened through Termux's own entry in the picker) has
one, `/data/data/com.termux/files/home`, that Android lets no other app
enter, whatever permissions it holds. In both cases the terminal starts in
MiniCode's private folder and prints why, in grey, above the prompt.
All of this is about Android's own shell; Termux's bash needs none of
MiniCode's permissions, only Termux's own storage access, and starts in
Termux's home when the folder is not in shared storage.

Android's picker is a poor way into that storage: many phones hide it behind
the picker's menu ("Show internal storage"), and since Android 11 it refuses
the top level and Download outright. So leader O first asks where to open
from. "Phone storage" browses `/storage/emulated/0` by path, inside MiniCode;
long-press a folder there to make it the project. "Another app or cloud" is
the picker, for Drive and Termux folders the editor can use but a shell
cannot. (A long press that moves on drags the folder instead; see "Moving
files by dragging".) A path folder is remembered as `folderPath`, a picked one as
`folder`, and opening one clears the other.

The place both apps can reach is shared storage. In Termux,
`termux-setup-storage` makes it `~/storage/shared`; in MiniCode it is the
phone's storage in the picker (`/storage/emulated/0`). A project kept there
can be edited in MiniCode and built or committed from either terminal.
Granting access while the terminal is open moves it into the folder as soon
as you return from Settings.

### Termux's bash in the terminal

The phone's own shell cannot reach anything installed in Termux, so a user
who ran `pkg install python` still got "python3: inaccessible or not found"
in MiniCode's terminal. Now, when Termux is installed and MiniCode may run
commands in it, the terminal is bash running in Termux, in the open folder,
with Termux's HOME, PATH, prompt, `~/.bashrc` and history. python, git,
clang and whatever else `pkg` installed run there, and so do the Python
prompt, less and vim, because bash has a real pseudo terminal.

- **How.** `Termux.start` (the language servers' bridge) could already run a
  program in Termux with its stdin and stdout on a loopback socket, but a
  socket is not a terminal: no line editing, no Ctrl C, no full-screen
  programs. Termux ships nothing that turns one into the other (`script` is
  in util-linux, python may not be there), so MiniCode brings its own:
  `app/src/main/cpp/termux_pty.c`, about 150 lines of C against bionic,
  which opens a pty inside Termux, starts `bash --rcfile ... -i` on it and
  relays between the pty and the socket. Input travels in frames (`d`, a
  two-byte length, the bytes; or `w` with the new columns and rows), so a
  change of size reaches the pty as TIOCSWINSZ and bash, less and vim get
  SIGWINCH. Output comes back raw. `terminal_jni.cpp` has a second kind of
  session for this (`nativeAttach`): the same screen, keys and snapshots,
  with writes framed and resizes sent as frames.
- **Getting the helper into Termux.** Android runs no program out of
  another app's storage, and MiniCode's native libraries are not even
  extracted (they stay in the APK, for 16 KB page alignment). Termux can run
  its own files, though, so the helper is built as an executable named
  `libminicode_pty.so` (the name is what makes the Android build carry it in
  the APK), read out of the APK by `TermuxShell.kt`, and sent over the same
  socket the first time: the script Termux runs answers "have" or "need"
  for `$TMPDIR/minicode-pty-<hash>`, and on "need" reads exactly the
  helper's bytes with `head -c`. A new build with a different helper gets a
  new hash, and older copies are removed. Nothing is needed from `pkg`.
- **The rc file** (`$TMPDIR/minicode-bashrc`, rewritten on every start)
  enters the open folder (passed in `MINICODE_DIR`, never written into a
  script), runs Termux's `bash.bashrc` and the user's `~/.bashrc`, then adds
  a PROMPT_COMMAND that sends OSC 7 (the directory) and OSC 133;D (a
  command finished). The core's TerminalScreen now records both
  (`directory()`, `commandsEnded()`): the title bar follows the directory,
  links resolve against it, and a finished command makes the file list
  look again.
- **Security** is the language servers': a one-shot listener on 127.0.0.1
  that takes one connection, which must send the random token first.
- **Which shell.** ⋮ → Shell: Automatic (the default: Termux's when Termux
  is installed and MiniCode has its permission), Termux, or Android. A
  change restarts the shell. On Android's shell in automatic mode a grey
  line says how to get Termux's. If Termux will not start a shell, Android's
  starts instead, with Termux's reason above the prompt, and the same
  happens if bash ends within moments of starting.
- **Where it starts.** A folder in shared storage opens there. Anywhere
  else (a cloud folder, MiniCode's own storage) bash starts in Termux's home
  and says why. If Termux has no storage permission yet, the rc file says
  to run `termux-setup-storage`.
- **Ending.** `exit` (or Ctrl D) ends bash, then the helper, then the pane,
  as before. Closing MiniCode closes the socket, and the helper hangs up
  bash, so nothing is left running in Termux; the window stops the shell in
  `onDestroy` too, since a Termux shell lives in Termux's process.
- **Size changes** used to recreate the window (a display size or layout
  change is not in `configChanges` by default), which lost the shell and
  any unsaved buffer. The activity now handles `smallestScreenSize` and
  `screenLayout` itself, like the others it already did.

Checked on the Titan 2 (2026-09-26, Termux 0.118.3, which had python3,
less, clang and git but not vim): bash in the open folder with the user's
prompt, `python3 -c 'print(1+1)'`, a Python prompt line, a script in the
folder with a traceback whose file link opened the file at the line,
`less` filling the grid, a change of height from 23 to 14 rows and back
reaching `stty size` and redrawing `less`, Ctrl C from the key row
interrupting `sleep` (status 130), `exit` and a new shell with no process
left behind in Termux, and ⋮ Shell switching to Android's sh and back.
The on-screen keyboard could not be used to change the height: the Titan's
Kika keyboard shows only its strip for a TYPE_NULL view, so `wm size` was
used instead, which changes the pane the same way.

### The file list follows the disk

The list used to be read only when a folder was opened, so a file made by
`touch` in the terminal never appeared. Now:

- **A folder with a path MiniCode may read** (Phone storage, or a picked
  folder on the phone's storage once "All files access" is granted) is
  watched with a `FileObserver`. Creating, deleting or moving an entry
  starts a 250 ms timer, and the folder is read once when it runs; the list
  is replaced only if an entry changed, keeping the scroll position and the
  focused row. A deleted subfolder gives way to the nearest folder above it
  that is still there; a deleted project folder says it is gone.
- **A folder with no path** (Drive, Termux's own folders, other apps) cannot
  be watched. It is read again on returning to the app, on showing the list
  (from the terminal, say), and when a command finishes in Termux's bash.
- **One observer per folder.** Android keeps one inotify watch per path, and
  a second FileObserver on the same folder replaces the first one's event
  mask instead of adding to it. Watching the open file's folder separately
  made the list stop seeing deletions as soon as a file in it was opened,
  so both uses share one observer with both masks.
- **The open file** is checked when its folder reports a write to it, on
  returning to the app and on showing the editor. A buffer with no unsaved
  edits takes the new text as one edit (so undo can go back to what was
  there) with the caret kept; an unsaved one is kept and the user is told
  once, and saving then asks whether to save over the other program's
  change or load the file from disk. Before this, nothing noticed, and a
  save silently wrote over whatever had changed.
- Nothing is watched while MiniCode is in the background; returning reads
  everything again.

Checked on the Titan 2 with a scratch folder: files made by `adb shell
touch`, by `touch` in Termux's bash and in Android's sh appeared within a
second, an `rm` and a `mkdir` from adb showed up while a file in the same
folder was open, deleting the listed subfolder moved the list up, an
outside append to the open file reloaded it with the caret kept, and an
outside change under an unsaved edit gave the toast and then the dialog on
save.

### Moving files by dragging

A new file used to need the terminal to get into a folder. Now a file or
folder in the list can be dragged there (`FileMove.kt`, and the drag code
in `MainActivity.kt` under "moving by dragging"):

- **A long press, then move the finger.** The long press arms the row (the
  phone gives its usual buzz); moving after it starts the drag, with the
  name as a small label under the finger. Lifting without moving does what
  a long press always did: a folder on Phone storage becomes the project,
  and anything else does nothing. So the old gesture still works, and no
  menu was added.
- **Where it can go.** Onto a folder row, which lights up; onto ↑ in the
  title bar, for the folder above; or anywhere else in the list, meaning
  the folder being listed, and then the whole list lights up. Held on a
  folder row or on ↑ for most of a second, the list goes there, so a drop
  can reach any folder in the project; held near the top or bottom edge,
  the list scrolls. A drag that moves nothing returns the list to the
  folder it started in.
- **Refused.** The folder it is already in and the dragged folder itself
  never light up, and a drop there simply lets go. The list never enters
  the dragged folder, so nothing can be moved inside itself, and
  `FileMove.refusal` checks that too, by path or by the provider's
  document id. A name already in the destination is never written over: it
  says "docs already has something called notes.md." On the phone's
  storage that check ignores case, as the storage does; for a picked folder
  any name differing only in case is refused, to be safe.
- **How.** A folder opened by path moves with `Files.move`, which, unlike
  `File.renameTo`, refuses to replace an existing file. A folder from the
  picker asks its provider (`DocumentsContract.moveDocument`), after
  checking the entry says it can be moved; a provider that cannot gets a
  message naming it, and nothing changes.
- **The open file follows**, whether it was the one moved or inside a
  moved folder: the buffer, unsaved edits included, now saves to the new
  place, and the title, the file watch, the language server (closed at the
  old path and opened at the new one), the LaTeX preview and the Markdown
  preview's relative pictures follow. A paused video or audio file opens
  from its new place when shown again.
- The list is read again at once (FileObserver would notice anyway for a
  path, but not for a picked folder), and source control refreshes.

Built and compiled only: the phone was not reachable when this was
written. Still to check at the phone: the long press arming without
scrolling the list, the drag starting and the label following the finger,
the row and list highlights, a held folder and a held ↑ opening, the edge
scroll, a cancelled drag returning to its folder, the old long press on a
folder still making it the project, moving the open file (and a folder
holding it) with unsaved edits and then saving, a name clash, and a move
in a folder picked from Drive and from Termux.

### Tapping links in the terminal

A file reference or URL printed in the terminal is underlined in the accent
colour, and tapping it opens it: `src/main.cpp:42:7` from a compiler,
`File "run.py", line 17` from Python, `app.ts(12,5)` from TypeScript, or a
plain path, in the editor with the caret on that line and column; a URL in
the browser pane. A tap anywhere else focuses the terminal as before.

Finding them is the core's `TermLinks` (`src/TermLinks.cpp`, tested in
`run_tests.cpp`), through `links_jni.cpp`, which takes a row as one code
point per cell so the answer comes back in cells. A file is underlined only
if it exists. Relative paths are resolved against the directory in the
prompt above them first: the phone's `/system/bin/sh` is mksh, which sends
no OSC 7 but prints its directory in the prompt (`:/storage/emulated/0/mc $ `).
Termux's bash sends OSC 7, so its directory comes next. After that come the
shell's folder, the open folder, and MiniCode's home, which `~/` means. A link that wraps onto the next row is not found.

### The key row under the terminal

The Titan 2's Alt layer has no pipe, backslash or backtick, and the keyboard
app's symbol picker cannot help: the terminal declares TYPE_NULL, so there is
no text field for it to type into. So the terminal has a row of keys under it,
as Termux does: Esc, Tab, a sticky Ctrl (tap it, then a letter; it lights up
while it waits), Up and Down for the shell's history (Android's
mksh keeps it for the session only: persistent history is compiled out of
it, so `HISTFILE` does nothing; Termux's bash keeps `~/.bash_history`), `| ~ / - \ ` & > < { } [ ]`,
and Left and Right. The row scrolls
sideways, and its keys never take focus, so typing stays with the shell
(`TerminalKeys.kt`).

### Alt, symbols and Meta in the terminal

On the Titan 2 the digits and most symbols (`| > & ~ -` and the rest) are
typed with Alt, so Alt cannot simply mean Meta the way it does on a desktop
terminal. An early build sent every Alt key as Meta: Alt+S reached the shell
as ESC then "4", and the line editor swallowed it, so no digit or symbol could
be typed in the terminal at all. `TerminalView.handleKey` now asks the key
map what the key makes without Alt. If Alt changes the character, the
character is sent as typed; only a key Alt leaves unchanged (Enter, the
arrows) gets the Meta prefix. So on this keyboard Alt+letter is always a
symbol, and there is no Meta for letters (the Alt+B and Alt+F word jumps of
a shell). A leader binding is the way to add one if it is missed.

If Alt misbehaves on another device, find out what the key actually sends
before changing code. On a debug build, `adb logcat -s MiniCodeKeys` prints
each key the app receives with its meta state (0x12 is left Alt, 0x22 right
Alt) and whether Alt, Shift or Sym were down. Three things are worth
checking: whether Alt arrives as held (in the meta state of the letter's own
event) or as a separate press first (sticky, as some phone keyboards do it);
whether the symbol arrives as a key event or as text committed by the
keyboard app, which never shows an Alt at all; and which Alt it is, since
desktop terminals, Termux among them, often treat only the left one as Meta.
Termux has its own answer for keyboards without Ctrl and Esc: Volume Down
acts as Ctrl and Volume Up plus a letter gives Esc, Tab and the arrows (its
wiki's "Touch Keyboard" page). MiniCode does not copy that yet.

## Source control

The leader's V puts the source control panel where the file list goes (V
again, or Back, brings the list back). It is the Mac's and the Linux port's
panel, and all of its deciding is their shared C++: the core's `GitStatus`
and `GitGraph` read what git prints, and the GTK port's
`linux/src/GitModel.cpp`, which has no GTK in it, turns that into rows,
argument vectors, the summary line, error text and colored diff text.
`git_jni.cpp` puts those behind JNI (`GitNative.kt`), and `GitPanel.kt`
runs git and draws. The commands, the wording, the letters and the colors
are the same as on the desktop.

- **Git runs in Termux** (`pkg install git`), which is why only a project in
  shared storage works: that is the one place both apps see at the same
  path. Anywhere else the panel says so, and without git in Termux it says
  "Install git in Termux: pkg install git".
- **One helper per panel, not one launch per command.** A refresh runs up to
  five commands, and starting each through RUN_COMMAND would cost a moment
  apiece, so `GitRunner.kt` starts one small bash loop through `Termux.start`
  (the language servers' bridge) and keeps it. Each request is the argument
  count, the directory and the arguments, NUL-separated; bash reads each
  field with `read -r -d ''` into an array and runs `git "${args[@]}"`, so a
  path or a commit message never passes through a shell's quoting, splitting
  or globbing. git writes stdout and stderr into two files in Termux's
  temporary folder (so neither pipe can fill and stall it), and the answer
  is the exit status, both lengths, then both outputs. The loop says "ok" or
  "nogit" when it starts. It closes two minutes after the panel is last
  shown, and a dead connection is reopened once, only when git cannot have
  run yet.
- **Dubious ownership.** Termux's user does not own anything in shared
  storage, so git refuses every repository there unless it is named in
  `safe.directory`. Every command gets `-c safe.directory=<the top level>`
  for that one repository; no config file is ever written. The first command
  cannot know the top level yet, so it names the open folder, and when git
  answers "detected dubious ownership in repository at '<path>'" (a
  subfolder of a repository was opened) it runs again naming that path.
- **The same rules as the desktop**: `GIT_OPTIONAL_LOCKS=0`,
  `GIT_TERMINAL_PROMPT=0`, `GIT_EDITOR=true`, `GIT_PAGER=cat`; stdin
  `/dev/null`; paths after `--` and `--literal-pathspecs` on add, restore and
  rm; only rev-parse, status, diff, add, restore --staged (rm --cached
  before the first commit), commit, log, rev-list, for-each-ref and show. One
  worker thread per panel keeps an add and the status after it in order;
  results are dropped when the folder changed or a newer diff or graph was
  asked for. Native handles are freed on that thread, after any job still
  using them.
- **Refreshing**: on showing the panel, after every action, on returning to
  the app (a commit made in Termux), after a save, and on a tap on the
  branch line. There is no file watching.
- **Layout on 576 by 640 dp**: the branch line; the message box with the
  Commit button beside it; git's error, if any (six lines, the whole text on
  a tap); the change lists, which take what their rows need up to half of
  what is left; then the graph heading with All branches, the summary line,
  and the graph, which scrolls. Rows are drawn by hand in RecyclerViews, the
  graph's lanes, dots, pills, tints and arrows as the Mac draws them.
- **Keys** (the panel holds the keyboard itself; its rows are not views
  that take focus): Up and Down move through the files, skipping headings,
  and on into the graph; Up from the first file goes to the message box, and
  Down from the message box's last line comes back, since the phone has no
  Tab (Tab and Shift+Tab work on a keyboard that has them). Space stages or
  unstages, Enter shows the diff or the commit. In the message box Enter is
  a new line; the leader then Enter commits from anywhere in the panel, as
  does Ctrl+Enter.
- **Touch**: a tap on a file shows its diff, a tap on the + or − at its right
  stages or unstages it, a tap on a commit shows it, and a long press shows
  what the desktop puts in a tooltip.
- **The diff or commit** takes the editor's place, after "Save changes?" if
  the buffer is unsaved, and Back returns to the panel. `DiffView.kt` is a
  RecyclerView with a row per line: one TextView holding a 500 KB diff took
  seconds to lay out, with the window frozen, while this shows the core's
  full 4 MB at once. Up and Down scroll it, Space and Page Down page, Home
  and End jump.
- **Debug log**: `adb shell setprop log.tag.MiniCodeGit DEBUG` turns on, in
  any build, a line per git command with its exit status, and each refresh's
  branch line, rows, graph rows and timings.

Measured on the Titan 2 (2026-09-26): a refresh's status in 250 to 350 ms
through Termux, the first 200 commits of the graph 300 ms after that, the
next 200 (Show more) in 520 ms, and an unchanged refresh running no log at
all.

## The Markdown preview

The Mac's and the Linux preview, on a phone (`MarkdownPreview.kt`). The
core's `MarkdownParser` does the reading, and `minicode_jni.cpp` now hands
Kotlin everything a run carries: its source line, link target, picture
source, and a table cell's table, row, column and alignment. GitHub's
anchor for a heading comes from the core too (`MarkdownParser::anchor`).

- **Layout.** A column of views in a ScrollView: the text between tables
  and pictures is one TextView styled with spans, a table is a grid of
  wrapping TextViews with equal columns (as Linux draws them), and a
  picture is an ImageView. Every stretch of text keeps the source line it
  came from, which links, editing and place keeping all go through. The
  text is not selectable, because a selectable TextView takes a double tap
  to select a word.
- **Links.** A single tap: `#heading` scrolls to it, `http(s)` opens the
  browser pane, a path relative to the file opens that file (`#L12` in the
  source at that line, another anchor at that heading), another scheme goes
  to whatever app handles it. A file opened through the picker has no path,
  so a relative link is followed through the picked tree from the file's
  folder (`resolveRelative`).
- **Pictures** relative to the file, or absolute, decoded by `ImageDecoder`
  no wider than the pane and never larger than their own size. A GIF comes
  back as an `AnimatedImageDrawable`, which plays while the preview is on
  screen. Files over 64 MB show their alt text. Decoded pictures are kept
  for the next render, since an edit re-renders the page. A picture inside
  a table cell shows its alt text, local or not.
- **Pictures from the web** (`WebImages.kt`). An `https://` picture shows
  its alt text at first and is fetched in the background: platform
  `HttpsURLConnection` on three threads, the system's certificate checks,
  15 seconds to connect and for each read (60 in all), no cookies, no HTTP
  cache, and `MiniCode/<version>` as the User-Agent. Plain `http://` is
  never fetched, and neither is a redirect from https to http. When pictures
  arrive the page is rendered again, several arrivals to one render, and
  the text at the top of the pane stays where it was: a picture that lands
  above it moves the scroll by its height. Arrivals wait while a finger is
  on the page or it scrolled in the last 0.3 s, so a fling or a jump to a
  heading is never cut short. The bytes are decoded with
  `ImageDecoder.createSource(ByteBuffer)`, so a GIF plays like a local one.
  Kept in memory only: the downloaded bytes in a 32 MB `LruCache` keyed by
  address (an edit, or opening the file again, costs no request), and a
  failure for a minute, so a missing picture is not asked for on every
  edit. A picture over 20 MB is cut off when it passes that (or refused
  from its Content-Length), and one over 64 megapixels is refused from its
  header before it is decoded, as on Linux. A 404, a timeout, a picture Android cannot
  decode and anything else that goes wrong leave the alt text as it was,
  without a message. SVG cannot be decoded by `ImageDecoder`, so it keeps
  its alt text; its Content-Type is enough to refuse it without reading
  the body, which is the usual case for shields.io badges. A linked badge,
  `[![alt](src)](url)`, opens its link on a tap whether it shows the
  picture or the alt text. A page closed or changed before its pictures
  arrive drops them.
- **Web images in Markdown** (⋮ menu, on by default, kept in the
  `webImages` preference) turns the fetching off. Off, web pictures show
  their alt text, nothing is asked for, and fetches still queued are
  dropped before they go out. Turning it on or off keeps the text at the
  top of the pane where it was. This is the desktop's `markdown.web-images`;
  Android does not read `settings.conf`. `adb shell setprop
  log.tag.MiniCodeWeb DEBUG` logs every request and its result, in any
  build.
- **Editing.** A double tap on a paragraph, heading, list item, quote,
  code block or table cell opens a box (`MarkdownEditDialog.kt`) holding
  that block's Markdown, found by the core's `MarkdownEdit::blockAt`.
  Enter saves; Shift+Enter, or the New line key that starts the box's
  symbol row, types a new line (a phone keyboard may not let an app see
  Shift). A list item offers Add item. Save hands the text to
  `MarkdownEdit::replace` or `addItem` through JNI, and the new source goes
  into the editor as the smallest splice, so it is highlighted, marked
  unsaved and seen by the language server like typing. An edit is dropped
  if the buffer changed while the box was open.
- **Undo.** Preview edits are undone from whole-source snapshots (50 kept),
  with the leader's U and R, or Ctrl+Z and Ctrl+Shift+Z on a keyboard that
  has Ctrl, as on Linux. In the source the same keys reach the text field's
  own undo, which a phone keyboard had no way to reach before. Switching to
  the source drops the preview's snapshots.
- **Place keeping.** Going to the preview puts the caret's part of the page
  a third of the way down. Coming back, the source opens at what the
  preview had at its top if the reader scrolled it, and otherwise with the
  caret where it was. An edit keeps the page where it was.

Checked on the Titan 2 (2026-09-26) with a scratch document: every kind of
block edited or opened, Add item, a table cell with a pipe and a new line
(saved as `\|` and a space), undo and redo, all four kinds of link
including an anchor in another file and `#L3`, the PNG at its own size,
the GIF playing at the pane's width, the web picture's alt text, and
place keeping both ways. Injected keys cannot show whether the Titan's own
Enter and Shift+Enter arrive as keys or as committed text in the box; both
paths are handled, and the New line key works either way.

Pictures from the web were checked on the Titan 2 (2026-09-29) with the
release build and the MiniCodeWeb log, against a scratch page of public
addresses: the page came up at once with every picture as alt text; the
repository's icon.png and tour.gif (from raw.githubusercontent.com)
arrived in about 0.2 s and showed, and the GIF played (its area differed
across three screenshots 2 s apart while a still area did not); the same
address twice on the page made one request; the re-render for the
arrivals asked for nothing again, the 404 and the SVG included; a 404, a
shields.io SVG (refused from its Content-Type), a 100 MB file (refused
from its Content-Length) and a plain http address kept their alt text,
and the http one made no request. The phone left the network before the
rest could be checked: a tap on a linked picture, the place kept when
pictures land above a scrolled page, the toggle off making no request,
and an edit from the preview making none.

## Task lists and the TODO list

GitHub's task lists (`- [ ] buy milk`, `- [x] done`), as the Mac and Linux
have them. Every decision is the core's `src/MarkdownTasks.cpp` and
`FolderSearch::findTodos`, reached through `minicode_jni.cpp`; Kotlin only
draws and applies the one replacement each of them returns. What counts as
a task is the parser's reading, so a `- [ ]` inside a code block never is.

- **A box in the preview.** A task item's ☐ or ☑ (the core's marker run,
  flag bit 19, with the task's state in bits 20 and 21) is drawn by a
  `ReplacementSpan` as a rounded square the size of a capital: outlined in
  grey while open, filled with the accent and a check mark when done. A
  checked item's own text is grey (#858585, the desktop's `markdown.done`
  default) and struck through. A single tap on the box (or up to 12 dp left
  of it, or on the space after it, but not on the item's text) ticks or
  clears it at once, without waiting to see if a
  second tap follows: `MarkdownTasks::toggleBox` changes the one character
  between the brackets, and the new source reaches the buffer as every
  preview edit does, as the smallest splice, so it is highlighted, seen by
  the language server, marked unsaved and undone by leader U (a snapshot).
  The file is not saved. A double tap anywhere but on a box still opens
  the edit box, and the preview's text is still not selectable.
- **Leader L, Ctrl+L, in the source** of a Markdown file: the task key on
  every line the caret or selection touches (`MarkdownTasks::toggle`). A
  line that is not a task becomes one (`text` and `- text` become
  `- [ ] text`, `1. text` becomes `1. [ ] text`); when all of them already
  are, they are all ticked, or all cleared if they all were. It is one
  replacement, so the text field's own undo (leader U in the source) takes
  it back, and the selection moves where the core says. In the preview
  the key says to tap the box instead; in any other file it says task
  lists are for Markdown.
- **Enter continues a list** in a Markdown file, whether the keyboard
  sends Enter as a key or as text: `CodeEditText` asks
  `MarkdownTasks::newline` first, which starts the next item with the same
  indentation, quote markers and marker (`- [ ] ` after a task, `4. ` after
  `3. `), or ends the list when the item is empty. Anywhere else the core
  answers "no change" and Enter keeps the line's indentation as before.
- **The count in the title.** While a Markdown file with at least one task
  is open, the title reads `notes.md — 3 of 7 done` (`MarkdownTasks::count`).
  It is counted on opening, 300 ms after typing stops, and at once after a
  tap on a box or leader L. The title is cut from the left when it is too
  long, so the count stays in view.
- **The TODO list** (leader W, Ctrl+Shift+L, or ⋮ TODOs) takes the file
  list's place, as source control does: every TODO, FIXME, HACK, XXX and BUG
  after a comment opener, and every open task in the project's Markdown
  files, in Find in Folder's order and limits (hidden folders,
  `node_modules`, `build` and the like skipped, files over 1 MB skipped,
  2,000 at most). Rows read `relative/path:line  text`. Up and Down move,
  Page Up, Page Down (or Space), Home and End jump, and Enter or a tap opens
  the file in the source with the caret at the tag or the box, asking about
  unsaved changes first unless it is the open file. W again or Back
  returns to the files. After opening a row, the leader alone or Back
  brings the TODO list back, read again.
- **Read afresh each time it is shown**, on one worker thread. A scan still
  running when the pane goes, or when another starts, is cancelled through
  the core's cancel flag, and its answer is dropped. The rows shown last
  stay until the new answer arrives. `adb shell setprop log.tag.MiniCodeTodos
  DEBUG` logs each scan's count and time.
- **Only folders with a path.** The core walks the folder with the file
  system, so the list works for Phone storage folders and folders picked
  on the phone's own storage once "All files access" is granted. A folder
  from the cloud or another app has no path, and the pane says so rather
  than showing nothing.

Built and compiled only (2026-10-07): the phone was not connected, so none
of this has been seen running. To try at the phone, with a scratch folder
in `/sdcard/mc-test` holding a Markdown file of tasks (nested, numbered, in
a quote, one inside a code fence) and a source file with TODO comments:
the boxes' look, open and checked, at both text sizes; a tap on a box
ticking and clearing it, the title's dot and count following, leader U and
R undoing and redoing it, and the file unchanged on disk until leader S; a
tap beside the box and on the item's text (the text must not toggle); a
double tap on the item's text opening the edit box; checked text grey and
struck through; leader L (and Ctrl+L with the Fn key set to Ctrl) on one
line, on a selection of mixed lines, on a blank line, and in a non-Markdown
file; Enter from the Titan's own keyboard after a task, after `3.`, on an
empty item, and in a plain paragraph; the TODO list from leader W and
Ctrl+Shift+L, a row opening at its line by Enter and by a tap, W and Back
returning to the files, and a Drive folder getting its message.

## The LaTeX preview

Opening a `.tex`, `.ltx` or `.latex` file shows it typeset, as Markdown opens
rendered, and the leader's P flips to the source and back. The design is the
Mac's (the "LaTeX preview" section of `../CLAUDE.md`); what differs is where
things run and live.

- **tectonic runs in Termux** (`pkg install tectonic`), through `Termux.kt`.
  Android will not run a binary out of another app's storage, so there is no
  way to ship or download one the way the Mac app does. ⋮ → Termux tools
  says whether it is installed. The first run downloads tectonic's bundle,
  and the status line shows tectonic's output as it goes.
- **Only documents on shared storage can be typeset**, because that is the
  one place both apps see at the same path. A document opened from a cloud
  folder or from inside Termux gets a message saying so.
- **The buffer is typeset from a hidden sibling**, `.<name>.minicode.tex` in
  the document's own folder, written by MiniCode and deleted after each run,
  so relative `\input` and `\includegraphics` resolve and the user's file is
  never written. It appears in the file list while a run is going.
- **Output goes to `/storage/emulated/0/.minicode/latex/<hash of the path>/`.**
  On the Mac it is the temporary folder, but Termux cannot write MiniCode's
  private storage and MiniCode cannot read Termux's, so the PDF and its
  `.synctex.gz` have to be in shared storage too. The folder is outside every
  project on purpose, so it never lands in a file list or a commit. The
  renderer works from a copy in MiniCode's cache, so the next run can rewrite
  the original while pages are on screen.
- **Runs are debounced and counted**: 0.8 s after typing stops, at once on
  open and on save, one run at a time, with a change during a run queueing
  one more. Opening another file drops whatever is still running for the old
  one. The scroll position survives a re-typeset.
- **A failed run shows the end of tectonic's log** in place of the pages.
- **Double tap to edit** uses the core's SyncTeX reader and `LatexDoc` through
  `latex_jni.cpp`, the same matcher the Mac app's `MCLatexSpanAtPoint` runs
  and `tests/latex/sweep.sh` measures. The word under the tap and 40
  characters either side come from `PdfRenderer`'s text selection
  (`selectContent`), which only exists from Android 15; on older versions the
  pages show but editing from them says it needs a newer Android. A tap that
  matches nothing is refused rather than guessed. The edit dialog holds the
  span's own LaTeX, and Done splices exactly those characters in the buffer,
  which is then unsaved until the leader's S. If the buffer changed while the
  dialog was up, the edit is not applied.

## Video and audio

Opening a video (mp4, m4v, mov, 3gp, webm, mkv) or an audio file (mp3, wav,
m4a, aac, flac, ogg, opus) puts a player in the editor's place, as the Mac
does with AVKit and Linux with GtkVideo (`PlayerPane.kt`). It is Android's
own MediaPlayer drawing into a TextureView, so what plays is whatever the
phone can decode, and nothing is added to the APK but the pane (about
19 KB).

- **Paused on the first frame.** The title says the size and length
  ("640 × 360  0:05"; audio only the length). A seek before the first
  play draws that frame. Audio shows its name over the controls, and the
  file's contents decide which it is: an .mp4 of sound alone shows as
  audio. Nothing is saved, and nothing marks the file unsaved.
- **Controls.** A bar along the bottom: play or pause, the place (drag
  it), the time and the length, kept clear of the rounded corners like
  the key rows. While a video plays the bar hides after 3 seconds, and a
  tap on the picture brings it back. With the keyboard: Space plays or
  pauses (a headset's button too), Left and Right go back or on 5
  seconds, and Ctrl+Shift+Space plays or pauses from anywhere the player
  is on screen, as on Linux. At the end it stops, and playing again starts
  over. No leader letter was added: the free ones say nothing about
  playing, and Space already does it.
- **No sound from a pane you left.** The player is released whenever its
  pane is hidden (the file list, the terminal, the browser, source
  control, Back, another file, a diff) and when the app goes to the
  background, and its place is kept. Coming back opens it again, paused
  there. Playing takes audio focus, so music in another app pauses, and
  a call or pulling out headphones pauses this.
- **Paused for 4 seconds, it is released too.** The Titan 2 plays a
  file's sound through the audio hardware ("offload"), and Android's
  player shuts that down after 10 seconds paused and restarts it with a
  seek to the key frame before the place, so the picture jumped back and
  playing resumed up to 8 seconds early. Releasing the player first avoids
  it; the last frame stays on screen, and play or a seek opens a new
  player at the right place, a fraction of a second later.
- **Changes on disk** reload it, keeping the place and whether it was
  playing, through the open file's FileObserver. A deleted file stops and
  says so, and comes back if the file does.
- **A file Android cannot decode** says "Cannot play" and why, in the
  player's place.
- **Debug log**: `adb shell setprop log.tag.MiniCodeMedia DEBUG` logs each
  open, prepare, seek, play, pause and release with its position, in any
  build.

Checked on the Titan 2 (2026-09-28) with clips made by ffmpeg: each of the
thirteen kinds opened and played (H.264, VP8, VP9, AAC, MP3, FLAC, Vorbis,
Opus, PCM), a 3gp at 176 × 144, a phone-style rotated .mov as 360 × 640
upright, an .mp4 of sound alone as audio, an hour-long .m4a as 1:02:05, and
random bytes as "Cannot play". Space, Left, Right, Ctrl+Shift+Space, taps on
the picture and the button, and a drag of the bar were injected, and `adb
shell dumpsys media.player` and `dumpsys audio` showed no MiniCode player
after Back, Home, opening a.cpp or a picture, and the terminal. A clip
replaced on disk while playing carried on playing at its place, and while
paused stayed paused there. Reading positions back after a pause of 15
seconds is what found the jump to the key frame described above. Real
sound, a call or pulled-out headphones pausing it, and the Titan's own
Space and leader key need a person.

## Language servers

A language server runs in Termux, since that is where `pkg` installs clangd
and pip installs pylsp, and Android lets no app run a program out of
another app's storage. `Termux.kt` asks Termux to start it with RUN_COMMAND,
and the program's stdin and stdout come back over a loopback socket (the
file's comment has the details). From there it is the same client as the
Mac's: `lsp_jni.cpp` exposes `src/LspClient.cpp` with no I/O, and
`LspSession.kt` owns the socket, the reader thread and the UI.

- **Setup** is the ⋮ menu's Termux tools item, which also lists what is
  installed. It needs `allow-external-apps=true` in
  `~/.termux/termux.properties` and MiniCode's "Run commands in Termux"
  permission.
- **Which files.** Termux sees only shared storage, so a server starts only
  for files under `/storage/...`, opened through Phone storage; the path is
  the same on both sides. The project root is the open folder. There is one
  server per language per project, started the first time a file of that
  language opens, and a new project stops the old one's servers.
- **Which server.** The core's defaults, the first one installed: clangd for
  C and C++, pyright or pylsp for Python, gopls, rust-analyzer,
  typescript-language-server. If none is, the bar says what to install.
- **Diagnostics** are `DiagnosticSpan` markers in the text (so they follow
  edits until the server publishes again), and `CodeEditText` draws the
  squiggles after its text, the way the Mac editor does. They never touch the
  highlighter's color spans. The bar under the editor shows the message for
  the diagnostic at the caret, otherwise the error and warning counts or what
  the server is doing, and disappears when there is nothing to say.
- **Completion** opens on `.`, `->` and `::` when the server lists them as
  triggers, and on the leader's N. The list is filtered by the core as you
  type; the arrows move through it, Enter or Tab accepts, Esc or Back closes.
- **Hover** (leader K) shows in a dialog; **definition** (leader G) moves the
  caret, opening the other file first when it is elsewhere.
- In a debug build the server's stderr goes to `~/.minicode-lsp.log` in
  Termux's home.

## Building

Needs the Android SDK and NDK, and JDK 17 or 21 (Gradle 8.14 does not run on
newer ones). No Android Studio.

    brew install openjdk@21
    brew install --cask android-commandlinetools
    export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
    export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
    sdkmanager "ndk;29.0.14206865" "platforms;android-36" "build-tools;36.1.0" "platform-tools"
    echo "sdk.dir=$ANDROID_HOME" > local.properties

    ./gradlew assembleDebug
    adb install -r app/build/outputs/apk/debug/app-debug.apk

Gradle is pinned to 8.14.3 through the wrapper: 9.7 drops an API the Android
plugin still uses.


### A release build

`scripts/release.sh` builds the signed APK along with the Mac zip. By hand:

    export MINICODE_KEYSTORE=~/.config/minicode/release.keystore
    export MINICODE_KEYSTORE_PASSWORD="$(security find-generic-password -a minicode -s minicode-android-keystore -w)"
    ./gradlew -PminicodeVersion=1.4.0 assembleRelease

The release build is shrunk by R8 (4 MB against 15 MB for a debug build),
with every class of MiniCode's own kept whole in `app/proguard-rules.pro`,
because JNI finds them by name. The version code is derived from the version
(1.4.0 is 10400). A debug build and a release build are signed with
different keys, so going from one to the other on a phone needs an uninstall
first, which clears the app's settings and permissions.

## Developing against a real phone

A device is the test machine; there is no emulator in this setup and none is
needed. Wireless debugging changes its port whenever it reconnects, so expect
to pair again:

    adb pair 192.168.1.x:PAIRING_PORT PAIRING_CODE
    adb connect 192.168.1.x:CONNECT_PORT

The app takes a folder path as an intent extra, which avoids tapping through
the document picker on every run, and a debug build can be handed files
without any storage permission:

    adb push ../demo /data/local/tmp/
    adb shell "run-as org.minicode.editor cp -r /data/local/tmp/demo files/demo"
    adb shell am start -n org.minicode.editor/.MainActivity \
        --es folder /data/user/0/org.minicode.editor/files/demo

What can and cannot be tested from a development machine:

- `adb shell input tap/text/keyevent` works, and `adb exec-out screencap -p`
  gives a screenshot worth looking at.
- Injected **letters** reach the editor as committed text, like a real
  keyboard's, but they do **not** arrive as key events, so a shortcut that
  depends on a modifier cannot be reproduced. Sym in particular cannot be
  injected at all. Those need a person at the phone.
- Injected keycodes above about 288 (the Titan's spare key is 403) arrive as
  KEYCODE_UNKNOWN, which is why Menu and the Function key are leaders too.
- `sendevent` is refused by SELinux, so raw driver events are out.
- Every key the app sees is logged: `adb logcat -s MiniCodeKeys`. That log is
  how each keyboard finding above was established, and it is the first place
  to look when a shortcut does nothing.
- Text typed with `adb shell input text` arrives shuffled in any of the
  app's text fields ("Second commit" became "Scond commeit" in the editor
  and "Scceond commit" in the commit message box): the injected key events
  race the keyboard app. It says nothing about real typing; check that at
  the phone.
- `adb push` leaves out empty folders, and a git repository without
  `.git/refs/heads` or `.git/refs/tags` is not a repository to git. Make
  them with `adb shell mkdir -p` after pushing a scratch repository.
- A release build is not debuggable, so `dumpsys activity top` shows none of
  its views. The source control panel's own log (above) is how its state
  was read in testing.

Back on Android 16: an app that targets API 36 never gets `onBackPressed`,
so Back went straight out of the app from any pane until it moved to an
`OnBackPressedCallback` (September 2026).

## Running the core's own tests on the phone

The 1,079 checks in `../tests/run_tests.cpp` are plain C++ and pass on the
device unchanged, at speeds close to an M3 Mac (a full lex of 100,000 lines in
29 ms, a keystroke in 0.13 ms).

    NDK=$ANDROID_HOME/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin
    $NDK/aarch64-linux-android35-clang++ -std=c++17 -O2 -Isrc -static-libstdc++ \
        tests/run_tests.cpp src/SyntaxHighlighter.cpp src/MarkdownParser.cpp \
        src/TerminalStream.cpp src/TerminalScreen.cpp src/Settings.cpp \
        src/LineComments.cpp src/LatexDoc.cpp src/SyncTex.cpp src/Json.cpp \
        src/LspClient.cpp -o /tmp/run_tests_android
    adb push /tmp/run_tests_android /data/local/tmp/mc/run_tests
    adb push demo src scripts tests /data/local/tmp/mc/
    adb shell "cd /data/local/tmp/mc && ./run_tests"

Run it from the repository root; the tests read a few files from `demo/`,
`src/` and `scripts/`.

## Files

- `app/src/main/cpp/minicode_jni.cpp` — highlighting and Markdown across the
  JNI boundary, including `MarkdownEdit` for editing from the preview, with
  block ranges converted from UTF-8 bytes to UTF-16 units, and
  `MarkdownTasks` and `FolderSearch::findTodos` for task lists and the
  TODO list. The editor uses the core's incremental highlighter over a
  native mirror of the text, so an edit crosses as its position and the
  inserted characters, and only the lines it can have changed are recolored.
- `app/src/main/cpp/jni_strings.h` — Java strings to real UTF-8 and back.
  JNI's own `GetStringUTFChars`/`NewStringUTF` use *modified* UTF-8, which
  splits an emoji into two surrogates, and CheckJNI (on in debug builds)
  aborts the app when `NewStringUTF` is given a 4-byte sequence. Use these
  helpers for any user text.
- `app/src/main/cpp/terminal_jni.cpp`: the pty (or the socket to a Termux
  shell), and the shared TerminalScreen reading it.
- `app/src/main/java/org/minicode/editor/MainActivity.kt` — the panes, the
  leader, the menu, the title bar, recent folders.
- `StartScreen.kt` — what shows while no folder is open.
- `FileMove.kt` — moving a file or folder for the list's drag and drop: by
  path or through the document provider, never over an existing name, and
  where the open file ends up.
- `CodeEditText.kt` — the editor field: no composing, and the leader's letter.
- `EditorKeys.kt` — the row of symbols under the editor, and `CurvedEdges`,
  which keeps both key rows clear of the screen's rounded corners.
- `TerminalView.kt` — draws the grid, sends keys. Declares TYPE_NULL so
  keyboards send keys rather than composing words. Also finds, underlines
  and opens tapped links.
- `app/src/main/cpp/links_jni.cpp` — the core's TermLinks over one row of
  cells.
- `Pty.kt`, `Core.kt` — the native declarations and the shared palette.
- `Highlighter.kt` — keeps the editor's color spans current, an edit at a
  time, fed from the editor's TextWatcher.
- `MarkdownPreview.kt`: the Markdown preview, with text, tables and
  pictures, taps on links, double taps to edit, and the lines behind place
  keeping.
- `MarkdownEditDialog.kt`: the box a double tap opens.
- `TodoPane.kt`: the TODO list in the file list's place, scanned by the
  core's `FolderSearch::findTodos` on a worker thread.
- `WebImages.kt`: fetching https pictures for the preview, with the
  in-memory cache and the memory of failures.
- `Termux.kt`: runs a program in Termux with its stdin and stdout on a
  loopback socket; how language servers, tectonic, git and the terminal's
  bash are reached.
- `TermuxShell.kt`: bash in Termux for the terminal: hands the pty helper
  to Termux, writes the rc file, and turns the socket into a `Pty`.
- `app/src/main/cpp/termux_pty.c`: the pty helper that runs inside Termux.
- `LatexPreview.kt` — the LaTeX preview: typesetting through Termux, the
  page list, and the double tap to edit (`PageText` finds the tapped word).
- `app/src/main/cpp/latex_jni.cpp` — SyncTeX and `LatexDoc` for the tap.
- `app/src/main/cpp/lsp_jni.cpp` — the shared LSP client, fed and drained by
  Kotlin; results come back as small JSON events.
- `LspSession.kt` — language servers for the editor: processes, document
  sync, squiggles, the status bar, completion, hover and definition.
- `app/src/main/cpp/git_jni.cpp`: the core's `GitStatus` and `GitGraph` and
  the GTK port's `GitModel` (compiled from `../linux/src` by CMake), behind
  `GitNative.kt`. Argument vectors cross as byte arrays, so a path goes back
  to git exactly as git printed it.
- `GitRunner.kt`: the bash loop in Termux that runs git for the panel.
- `GitPanel.kt`: the source control panel, with its refreshing, actions, keys and
  the hand-drawn change and graph rows.
- `DiffView.kt`: a diff or a commit in the editor's place, a row per line.
- `PlayerPane.kt`: video and audio in the editor's place: MediaPlayer on a
  TextureView, the control bar, keys, audio focus, and letting go of the
  player whenever it is hidden or paused for a few seconds.

## Not yet

- In the LaTeX preview: adding a list item from the preview, a separate undo
  for preview edits, zoom, and Export PDF, all of which the Mac has.
- Project search and comment toggling.
- PDFs opened from the file list beyond the first page (the LaTeX preview
  shows them all); zoom and scroll for large images; terminal
  scrollback.
