# The Android port

Area notes moved out of `CLAUDE.md` (see its "Area notes" list). The text
below is as it stood there; section names quoted in it ("see ...") are
found through that list.

## The Android port

`android/README.md` is its handoff. The phone is a Unihertz Titan 2 (Android
16, 576 by 640 dp, hardware keyboard, Termux and F-Droid); wireless debugging
changes port on every reconnect, so ask for it (restart the adb server if
connect says "No route"); test files are in `/sdcard/mc-test`. Gradle 8.14.3
via the wrapper, JDK 21, SDK and NDK from `sdkmanager`. CI builds the debug
APK (no key needed), a release APK signed with a throwaway key, and runs
`./gradlew kotlinChecks`: `android/checks` compiles the app's Kotlin files
with no Android in them (listed in its build file; `Pty.kt`,
`LeaderKey.kt`) with `app/src/test/kotlin` on the JVM, no test library, and
checks them against the C++ they mirror (TermKey, TermMod, the cell flags)
and the leader key's presses. `linux:build-lists` checks the CMakeLists names every
core file its files include. Traps paid for:

- The core compiles with the NDK and its tests pass on the phone. `TermKey`
  ordinals in `Pty.kt` mirror `src/TerminalScreen.h` (Enter numbered as Escape
  looked like a dead key). The terminal uses `TerminalScreen` with scrollback
  off; its `resize` now reflows wrapped lines there too.
- No Ctrl, Esc or Tab on the phone; Alt is the symbol layer; Android claims Sym
  with some letters; a spare key (403 on the Titan, Menu elsewhere) is the
  leader and repeats in bursts, so presses split on release. `adb logcat -s
  MiniCodeKeys` shows keys. Injection cannot send Sym, keycodes above ~288 or
  `sendevent`; `adb shell input keycombination KEYCODE_CTRL_LEFT KEYCODE_U`
  does carry Ctrl. `adb shell input text` shuffles letters in fields.
- Letters reach a focused field through the input method
  (`CodeEditText.commitText`), not key events; Pastiera needs the
  visible-password variation; a letter with Ctrl has no `unicodeChar`; the
  terminal declares `TYPE_NULL` (composition re-sent words).
- Termux: bash on a pty from a helper built as `libminicode_pty.so`, sent over
  the token socket and run from Termux's `$TMPDIR`; servers and git there too
  (listen on 127.0.0.1 by name: `getLoopbackAddress()` is ::1; git through one
  bash loop with NUL-separated fields and `-c safe.directory`, never written to
  a config); only shared storage.
- One FileObserver per path; `wm size` recreated the activity until
  `smallestScreenSize|screenLayout` joined `configChanges` (the phone's
  override is 1076x1200, never `wm size reset`); `adb shell` in a `while read`
  loop eats its input (`-n`); no app can force the on-screen keyboard up beside
  a hardware one (`show_ime_with_hard_keyboard`, put back as it was); Back on
  Android 16 for API 36 only through `OnBackPressedDispatcher`; a focusable
  ViewGroup gets a grey veil (`defaultFocusHighlightEnabled = false`); a long
  diff needs a RecyclerView; preview text must not be selectable; View's
  constructor reports visibility before a Kotlin subclass's fields exist.
- A paused MediaPlayer moves by itself (offload restarts with a seek after 10
  s), so `PlayerPane` releases it 4 s after pausing; MediaController is a
  focusable window, so the pane draws its own bar. Web pictures re-render
  keeping the place by run index, with arrived bytes passed straight to the
  next render (an LruCache loop otherwise); release builds trust only system
  CAs, so test against public https addresses.

## Traps

Each trap's first sentence, in the order it appears above; the whole
text is in place in its section.

- The Android port: Traps paid for: (the bullets that follow it)
