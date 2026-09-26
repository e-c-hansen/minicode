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

### Language servers, LaTeX and git (optional)

These run in [Termux](https://f-droid.org/packages/com.termux/), installed
from F-Droid (the Play Store copy is years out of date):

1. In Termux:

       pkg install git clang tectonic python
       pip install python-lsp-server
       echo allow-external-apps=true >> ~/.termux/termux.properties
       termux-reload-settings
       termux-setup-storage

2. In MiniCode, ⋮ → Termux tools → Allow. It then lists which language
   servers, tectonic and git it found.
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
  goes up one.
- **The title bar says where you are.** The first line names the pane (the
  folder, the file with a dot while unsaved, Terminal, Browser, Source
  Control, a diff); the second says where it is, as "Phone storage / mc-test
  / docs", or the app a picked folder comes from, plus Unsaved, Preview,
  Source or View only for a file.
- **No editor without a file.** Asking for the editor with nothing open (the
  leader alone, leader F) stays on the list and says no file is open, so
  nothing can be typed into a buffer with nowhere to be saved.
- **Editor.** The shared highlighter colours the file; autocorrect,
  suggestions and the composing region are all off, because a keyboard that
  rewrites words is wrong for code (see CodeEditText).
- **Markdown preview.** The shared parser, rendered as the Mac and Linux
  render it: links that open on a tap, real tables whose cells wrap,
  pictures beside the file shown inline (GIFs playing), and a double tap on
  a block to edit its Markdown. The leader's P flips between the preview
  and the source, keeping the place both ways. See "The Markdown preview"
  below.
- **Images and PDFs.** Decoded by Android, scaled down but never up, with the
  pixel size or page count in the title bar. A PDF shows its first page.
- **Terminal.** `/system/bin/sh` on a pty, parsed by the shared
  TerminalScreen: the same grid that runs vim and less on the Mac. No Termux
  needed, though only the toybox utilities are reachable until there is.
  It starts in the open folder and follows it when another is opened, as
  long as the folder has a path (see below).
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
    in the editor, with a language server:
      N  complete        K  what the symbol is    G  go to its definition
    in the terminal:
      C  Ctrl C          D  Ctrl D                E  Escape     I  Tab
    in source control:
      Enter  commit

The key alone switches panes, twice opens the menu, and the ⋮ button in the
title bar offers the same items for a device whose keyboard offers nothing.
Ctrl also works, for a USB or Bluetooth keyboard, and Ctrl+Shift+G is
source control there, as on the Mac and Linux. G was taken by go to
definition, so the leader's letter for source control is V.

One trap worth keeping: while the editor has focus, a letter never arrives as
a key event, because the keyboard reaches the field through the input method.
The leader's letter is caught in CodeEditText as text is committed; in the
file list and the terminal, the same letter arrives as a key event instead.
Both paths run the same table (`leaderActions`).

B is the browser on every port (Shift+Cmd+B on the Mac), so the file list
moved to F. On a USB or Bluetooth keyboard Ctrl+B is the file list, as
Cmd+B is on the Mac.

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

Android's picker is a poor way into that storage: many phones hide it behind
the picker's menu ("Show internal storage"), and since Android 11 it refuses
the top level and Download outright. So leader O first asks where to open
from. "Phone storage" browses `/storage/emulated/0` by path, inside MiniCode;
long-press a folder there to make it the project. "Another app or cloud" is
the picker, for Drive and Termux folders the editor can use but a shell
cannot. A path folder is remembered as `folderPath`, a picked one as
`folder`, and opening one clears the other.

The place both apps can reach is shared storage. In Termux,
`termux-setup-storage` makes it `~/storage/shared`; in MiniCode it is the
phone's storage in the picker (`/storage/emulated/0`). A project kept there
can be edited in MiniCode and built or committed from either terminal.
Granting access while the terminal is open moves it into the folder as soon
as you return from Settings.

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
After that come the shell's folder, the open folder, and the shell's home,
which `~/` means. A link that wraps onto the next row is not found.

### The key row under the terminal

The Titan 2's Alt layer has no pipe, backslash or backtick, and the keyboard
app's symbol picker cannot help: the terminal declares TYPE_NULL, so there is
no text field for it to type into. So the terminal has a row of keys under it,
as Termux does: Esc, Tab, a sticky Ctrl (tap it, then a letter; it lights up
while it waits), Up and Down for the shell's history (Android's
mksh keeps it for the session only: persistent history is compiled out of
it, so `HISTFILE` does nothing), `| ~ / - \ ` & > < { } [ ]`,
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
  screen. Web pictures are never fetched and show their alt text, as do
  files over 64 MB. Decoded pictures are kept for the next render, since an
  edit re-renders the page.
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
  block ranges converted from UTF-8 bytes to UTF-16 units. The editor uses the core's incremental highlighter over a
  native mirror of the text, so an edit crosses as its position and the
  inserted characters, and only the lines it can have changed are recolored.
- `app/src/main/cpp/jni_strings.h` — Java strings to real UTF-8 and back.
  JNI's own `GetStringUTFChars`/`NewStringUTF` use *modified* UTF-8, which
  splits an emoji into two surrogates, and CheckJNI (on in debug builds)
  aborts the app when `NewStringUTF` is given a 4-byte sequence. Use these
  helpers for any user text.
- `app/src/main/cpp/terminal_jni.cpp` — the pty, and the shared
  TerminalScreen reading it.
- `app/src/main/java/org/minicode/editor/MainActivity.kt` — the panes, the
  leader, the menu, the title bar, recent folders.
- `StartScreen.kt` — what shows while no folder is open.
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
- `Termux.kt` — runs a program in Termux with its stdin and stdout on a
  loopback socket; how language servers, tectonic and git are reached.
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

## Not yet

- In the LaTeX preview: adding a list item from the preview, a separate undo
  for preview edits, zoom, and Export PDF, all of which the Mac has.
- Project search and comment toggling.
- PDFs opened from the file list beyond the first page (the LaTeX preview
  shows them all); zoom and scroll for large images; terminal
  scrollback.
