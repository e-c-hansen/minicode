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
  document picker, so the app needs no storage permission, and the choice is
  remembered between launches.
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
- **Browser.** The system web view with a URL bar.

## Shortcuts, and why they are unusual

A phone keyboard is not a desktop keyboard. On a Unihertz Titan 2 there is no
Ctrl, Esc or Tab; Alt is the symbol layer, so Alt+S types "4"; and Android
claims Sym with some letters before an app sees them, so Sym+H opens the
microphone. What is left is one unclaimed key, so that key is a leader:

    the key left of right Shift, then
      S  save            P  Markdown preview      O  open a folder
      B  files or editor T  terminal              H  the shortcut list
                         W  browser
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
  JNI boundary, including the UTF-16 to UTF-8 conversion the core needs.
- `app/src/main/cpp/terminal_jni.cpp` — the pty, and the shared
  TerminalScreen reading it.
- `app/src/main/java/org/minicode/editor/MainActivity.kt` — the panes, the
  leader, the menu.
- `CodeEditText.kt` — the editor field: no composing, and the leader's letter.
- `TerminalView.kt` — draws the grid, sends keys. Declares TYPE_NULL so
  keyboards send keys rather than composing words.
- `Pty.kt`, `Core.kt` — the native declarations and the shared palette.
- `Markdown.kt` — the core's runs turned into styled text.

## Not yet

- Language servers. The client is portable C++ and already compiles here, but
  clangd and pyright live in Termux, whose files this app cannot read.
- The LaTeX preview. The reader is portable; tectonic is not built for
  Android, so this needs Termux or a machine on the network.
- Project search, comment toggling, and incremental highlighting (the editor
  re-colours the whole file on each edit).
- PDFs beyond the first page; zoom and scroll for large images; terminal
  scrollback.
