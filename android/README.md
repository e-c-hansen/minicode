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
3. Open MiniCode, press ⋮ → Open a folder → Phone storage, and pick a folder.

To keep it updated without checking by hand, add
`https://github.com/e-c-hansen/minicode` to
[Obtainium](https://github.com/ImranR98/Obtainium), which installs from a
repository's releases. MiniCode is not on the Play Store: Google allows the
"All files access" the terminal uses only for some kinds of app, and new
developer accounts need a two-week closed test before publishing anything.

Every release is signed with the same key, so updates install over the old
version. The APK is built by `scripts/release.sh` from the tagged source.

### Language servers and LaTeX (optional)

These run in [Termux](https://f-droid.org/packages/com.termux/), installed
from F-Droid (the Play Store copy is years out of date):

1. In Termux:

       pkg install clang tectonic python
       pip install python-lsp-server
       echo allow-external-apps=true >> ~/.termux/termux.properties
       termux-reload-settings
       termux-setup-storage

2. In MiniCode, ⋮ → Termux tools → Allow. It then lists which language
   servers and tectonic it found.
3. Keep projects in phone storage (Termux sees it as `~/storage/shared`),
   since that is the one place both apps can reach.

The rest of this file is about how the port is built and developed.

## What it has

- **File list.** One pane at a time rather than a sidebar: on a 576 by 640 dp
  screen there is no room for two. Folders are opened through the system
  document picker, so the editor needs no storage permission, and the choice
  is remembered between launches. Only the terminal asks for one (below).
- **Editor.** The shared highlighter colours the file; autocorrect,
  suggestions and the composing region are all off, because a keyboard that
  rewrites words is wrong for code (see CodeEditText).
- **Markdown preview.** The shared parser, rendered as styled text. The
  leader's P flips between the preview and the source.
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

## Shortcuts, and why they are unusual

A phone keyboard is not a desktop keyboard. On a Unihertz Titan 2 there is no
Ctrl, Esc or Tab; Alt is the symbol layer, so Alt+S types "4"; and Android
claims Sym with some letters before an app sees them, so Sym+H opens the
microphone. What is left is one unclaimed key, so that key is a leader:

    the key left of right Shift, then
      S  save            P  Markdown preview      O  open a folder
      F  files or editor T  terminal              H  the shortcut list
      B  browser
    in the editor, with a language server:
      N  complete        K  what the symbol is    G  go to its definition
    in the terminal:
      C  Ctrl C          D  Ctrl D                E  Escape     I  Tab

The key alone switches panes, twice opens the menu, and the ⋮ button in the
title bar offers the same items for a device whose keyboard offers nothing.
Ctrl also works, for a USB or Bluetooth keyboard.

One trap worth keeping: while the editor has focus, a letter never arrives as
a key event, because the keyboard reaches the field through the input method.
The leader's letter is caught in CodeEditText as text is committed; in the
file list and the terminal, the same letter arrives as a key event instead.
Both paths run the same table (`leaderActions`).

B is the browser on every port (Shift+Cmd+B on the Mac), so the file list
moved to F. On a USB or Bluetooth keyboard Ctrl+B is the file list, as
Cmd+B is on the Mac.

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
  JNI boundary. The editor uses the core's incremental highlighter over a
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
  leader, the menu.
- `CodeEditText.kt` — the editor field: no composing, and the leader's letter.
- `TerminalView.kt` — draws the grid, sends keys. Declares TYPE_NULL so
  keyboards send keys rather than composing words. Also finds, underlines
  and opens tapped links.
- `app/src/main/cpp/links_jni.cpp` — the core's TermLinks over one row of
  cells.
- `Pty.kt`, `Core.kt` — the native declarations and the shared palette.
- `Highlighter.kt` — keeps the editor's color spans current, an edit at a
  time, fed from the editor's TextWatcher.
- `Markdown.kt` — the core's runs turned into styled text.
- `Termux.kt` — runs a program in Termux with its stdin and stdout on a
  loopback socket; how language servers and tectonic are reached.
- `LatexPreview.kt` — the LaTeX preview: typesetting through Termux, the
  page list, and the double tap to edit (`PageText` finds the tapped word).
- `app/src/main/cpp/latex_jni.cpp` — SyncTeX and `LatexDoc` for the tap.
- `app/src/main/cpp/lsp_jni.cpp` — the shared LSP client, fed and drained by
  Kotlin; results come back as small JSON events.
- `LspSession.kt` — language servers for the editor: processes, document
  sync, squiggles, the status bar, completion, hover and definition.

## Not yet

- In the LaTeX preview: adding a list item from the preview, a separate undo
  for preview edits, zoom, and Export PDF, all of which the Mac has.
- Project search and comment toggling.
- PDFs opened from the file list beyond the first page (the LaTeX preview
  shows them all); zoom and scroll for large images; terminal
  scrollback.
