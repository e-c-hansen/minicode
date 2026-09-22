# android/ — the Android port

MiniCode for Android, built on the same portable C++ core as the macOS app
(`../src`) and the GTK port (`../linux`). The core is compiled straight from
`../src` by CMake through the NDK; nothing is copied here, so highlighting,
Markdown, the settings file and comment toggling behave the same everywhere
and stay covered by `../tests/run_tests.cpp`.

It is early: a file list, an editor with syntax highlighting, and save. No
terminal, no language servers, no previews yet.

## Building

Needs the Android SDK and NDK, and a JDK 17 or 21 (Gradle 8.14 does not run on
newer ones). No Android Studio.

    brew install openjdk@21
    brew install --cask android-commandlinetools
    export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
    export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
    sdkmanager "ndk;29.0.14206865" "platforms;android-36" "build-tools;36.1.0" "platform-tools"
    echo "sdk.dir=$ANDROID_HOME" > local.properties

    ./gradlew assembleDebug
    adb install -r app/build/outputs/apk/debug/app-debug.apk

## Running the core's own tests on a device

The 1,079 checks in `../tests/run_tests.cpp` are plain C++ and run on the
phone. They pass there unchanged, and the timings are close to a Mac's.

    NDK=$ANDROID_HOME/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin
    $NDK/aarch64-linux-android35-clang++ -std=c++17 -O2 -Isrc -static-libstdc++ \
        tests/run_tests.cpp src/SyntaxHighlighter.cpp src/MarkdownParser.cpp \
        src/TerminalStream.cpp src/TerminalScreen.cpp src/Settings.cpp \
        src/LineComments.cpp src/LatexDoc.cpp src/SyncTex.cpp src/Json.cpp \
        src/LspClient.cpp -o /tmp/run_tests_android
    adb push /tmp/run_tests_android /data/local/tmp/mc/run_tests
    adb shell "cd /data/local/tmp/mc && ./run_tests"

The test reads a few files from the repo (demo/, src/, scripts/), so push
those beside the binary.

## Driving it from a development machine

The app takes a plain folder path as an intent extra, which avoids tapping
through the document picker on every run:

    adb shell am start -n org.minicode.editor/.MainActivity \
        --es folder /data/user/0/org.minicode.editor/files/demo

A debug build can also be given files without any storage permission:

    adb push demo /data/local/tmp/
    adb shell "run-as org.minicode.editor cp -r /data/local/tmp/demo files/demo"

Everyday use goes through the Open button and the system folder picker, which
needs no permission and is remembered between launches.

## Layout

The first target is a Unihertz Titan 2: 1080x1200 px at 300 dpi, so 576 by
640 dp, with a hardware keyboard. That is wider and shorter than an ordinary
phone, and it leaves no room for side-by-side panes, so the file list covers
the editor and closes when a file opens. A screen this size fits roughly 43
columns of monospaced text at 13sp with the system font scale at 1.3.

## Files

- `app/src/main/cpp/minicode_jni.cpp` — the only Android-specific C++: the
  bridge from Kotlin to the core, including the UTF-16 to UTF-8 conversion,
  since the core counts bytes and a Spannable counts UTF-16 units.
- `app/src/main/java/org/minicode/editor/Core.kt` — the native declarations
  and the shared palette.
- `app/src/main/java/org/minicode/editor/MainActivity.kt` — the file list, the
  editor, and the hardware keyboard shortcuts.
