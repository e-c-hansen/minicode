# android/ — the Android port

MiniCode for Android, built on the same portable C++ core as the macOS app
(`../src`) and the GTK port (`../linux`). CMake compiles that core straight
from `../src` through the NDK; nothing is copied here, so highlighting,
Markdown, the terminal screen and the rest behave the same everywhere and
stay covered by `../tests/run_tests.cpp`.

It is young but real: a file list, an editor with syntax highlighting, save,
the Markdown preview, images and PDFs, a terminal on a genuine shell, and a
browser panel.

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

## Shortcuts, and why they are unusual

A phone keyboard is not a desktop keyboard. On a Unihertz Titan 2 there is no
Ctrl, Esc or Tab; Alt is the symbol layer, so Alt+S types "4"; and Android
claims Sym with some letters before an app sees them, so Sym+H opens the
microphone. What is left is one unclaimed key, so that key is a leader:

    the key left of right Shift, then
      S  save            P  Markdown preview      O  open a folder
      F  files or editor T  terminal              H  the shortcut list
      B  browser
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
A folder from a cloud provider, such as Google Drive, has no path at all; the
terminal says so and starts in the app's own folder. To use git or a build
on a project, keep it in local storage (Termux's `~/storage/shared` is the
same place).

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
  keyboards send keys rather than composing words.
- `Pty.kt`, `Core.kt` — the native declarations and the shared palette.
- `Highlighter.kt` — keeps the editor's color spans current, an edit at a
  time, fed from the editor's TextWatcher.
- `Markdown.kt` — the core's runs turned into styled text.

## Not yet

- Language servers. The client is portable C++ and already compiles here, but
  clangd and pyright live in Termux, whose files this app cannot read.
- The LaTeX preview. The reader is portable; tectonic is not built for
  Android, so this needs Termux or a machine on the network.
- Project search and comment toggling.
- PDFs beyond the first page; zoom and scroll for large images; terminal
  scrollback.
