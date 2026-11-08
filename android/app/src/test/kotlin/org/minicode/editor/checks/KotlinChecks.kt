package org.minicode.editor.checks

import java.io.File
import kotlin.system.exitProcess
import org.minicode.editor.LeaderPresses
import org.minicode.editor.Pty

/*
 * The Android port's Kotlin checks: plain JVM code with no test library,
 * run by `./gradlew kotlinChecks` (android/checks/build.gradle.kts) in CI's
 * Android job. They compile only the app's Kotlin files that have no
 * Android in them, and read the C++ files those mirror from the repo
 * (the first argument is the repo's root).
 *
 * A failed check prints its group and what it expected; the run exits 1
 * if any failed.
 */

private var passed = 0
private var failed = 0
private var group = ""

private fun check(ok: Boolean, what: () -> String) {
    if (ok) {
        passed++
    } else {
        failed++
        println("  FAIL [$group] ${what()}")
    }
}

private fun <T> checkEqual(actual: T, expected: T, what: String) =
    check(actual == expected) { "$what: $actual, expected $expected" }

/** The names of `enum class <name> { ... };` in a C++ header, in order. */
private fun enumNames(header: String, name: String): List<String> {
    val body = Regex("""enum\s+class\s+$name\s*\{([^}]*)\}""").find(header)
        ?: return emptyList()
    return body.groupValues[1]
        .replace(Regex("""//[^\n]*"""), "")
        .split(',')
        .map { it.trim() }
        .filter { it.isNotEmpty() }
}

/*
 * Pty.kt's KEY_* constants are TermKey's ordinals in src/TerminalScreen.h
 * (the JNI passes the number through). An Enter once numbered as Escape
 * looked like a dead key on the phone, so the header is read here, not
 * copied.
 */
private fun checkTermKeys(root: File) {
    group = "pty:term-keys"
    val header = File(root, "src/TerminalScreen.h").readText()
    val names = enumNames(header, "TermKey")
    check(names.size > 20) { "TermKey not found in src/TerminalScreen.h ($names)" }
    val kotlin = linkedMapOf(
        "Up" to Pty.KEY_UP, "Down" to Pty.KEY_DOWN, "Right" to Pty.KEY_RIGHT,
        "Left" to Pty.KEY_LEFT, "Home" to Pty.KEY_HOME, "End" to Pty.KEY_END,
        "PageUp" to Pty.KEY_PAGE_UP, "PageDown" to Pty.KEY_PAGE_DOWN,
        "Insert" to Pty.KEY_INSERT, "Delete" to Pty.KEY_DELETE,
        "Enter" to Pty.KEY_ENTER, "KeypadEnter" to Pty.KEY_KEYPAD_ENTER,
        "Backspace" to Pty.KEY_BACKSPACE, "Tab" to Pty.KEY_TAB,
        "Escape" to Pty.KEY_ESCAPE,
    )
    for ((name, value) in kotlin) {
        val ordinal = names.indexOf(name)
        check(ordinal == value) { "TermKey::$name is $ordinal in the header, $value in Pty.kt" }
    }
    // Every key but the function keys has a constant, so a key added to
    // the header is either given one or knowingly left out here.
    for (name in names) {
        if (Regex("F\\d+").matches(name)) continue
        check(name in kotlin) { "TermKey::$name has no KEY_ constant in Pty.kt" }
    }

    group = "pty:term-mods"
    val mods = Regex("""TermMod(\w+)\s*=\s*(\d+)""").findAll(header)
        .associate { it.groupValues[1] to it.groupValues[2].toInt() }
    checkEqual(mods["Shift"], Pty.MOD_SHIFT, "TermModShift")
    checkEqual(mods["Alt"], Pty.MOD_ALT, "TermModAlt")
    checkEqual(mods["Ctrl"], Pty.MOD_CTRL, "TermModCtrl")
}

/* The CELL_* flags are the bits terminal_jni.cpp's nativeSnapshot sets. */
private fun checkCellFlags(root: File) {
    group = "pty:cell-flags"
    val jni = File(root, "android/app/src/main/cpp/terminal_jni.cpp").readText()
    fun bit(condition: String): Int? =
        Regex("""if \(${Regex.escape(condition)}\) flags \|= (\d+);""").find(jni)
            ?.groupValues?.get(1)?.toInt()
    checkEqual(bit("cell.style.bold"), Pty.CELL_BOLD, "bold")
    checkEqual(bit("cell.style.inverse"), Pty.CELL_INVERSE, "inverse")
    checkEqual(bit("cell.width == 2"), Pty.CELL_WIDE, "wide")
}

/* Pty.Screen reads nativeSnapshot's layout: five ints, then four per cell. */
private fun checkScreen() {
    group = "pty:screen"
    // Two rows of three columns, the cursor at row 1, column 2, shown.
    val data = IntArray(5 + 2 * 3 * 4)
    data[0] = 2; data[1] = 3; data[2] = 1; data[3] = 2; data[4] = 1
    fun put(row: Int, col: Int, cp: Int, fg: Int, bg: Int, flags: Int) {
        val at = 5 + (row * 3 + col) * 4
        data[at] = cp; data[at + 1] = fg; data[at + 2] = bg; data[at + 3] = flags
    }
    put(0, 0, 'a'.code, 0xFFFF0000.toInt(), 0, Pty.CELL_BOLD)
    put(1, 1, 0x65E5, 0, 0xFF00FF00.toInt(), Pty.CELL_WIDE)
    put(1, 2, 0, 0, 0, 0)   // the right half of the wide character
    val s = Pty.Screen(data)
    checkEqual(s.rows, 2, "rows")
    checkEqual(s.cols, 3, "cols")
    checkEqual(s.cursorRow, 1, "cursor row")
    checkEqual(s.cursorCol, 2, "cursor column")
    checkEqual(s.cursorVisible, true, "cursor shown")
    checkEqual(s.codePoint(0, 0), 'a'.code, "code point")
    checkEqual(s.foreground(0, 0), 0xFFFF0000.toInt(), "foreground")
    checkEqual(s.flags(0, 0), Pty.CELL_BOLD, "flags")
    checkEqual(s.codePoint(1, 1), 0x65E5, "wide code point")
    checkEqual(s.background(1, 1), 0xFF00FF00.toInt(), "background")
    checkEqual(s.flags(1, 1), Pty.CELL_WIDE, "wide flag")
    checkEqual(s.codePoint(1, 2), 0, "right half")
    // A closed pty's snapshot is empty: nothing to draw, nothing thrown.
    val empty = Pty.Screen(IntArray(0))
    checkEqual(empty.rows to empty.cols, 0 to 0, "empty screen")
    checkEqual(empty.cursorVisible, false, "empty screen's cursor")
}

/*
 * The leader key: a held key's burst of downs is one press, the release
 * splits presses, and a second press inside the window is the menu.
 */
private fun checkLeaderPresses() {
    group = "leader:presses"
    val window = 700L
    val p = LeaderPresses(window)
    val start = 10_000L
    checkEqual(p.down(start), LeaderPresses.Press.Arm, "first press")
    // The Titan 2's burst: a hundred downs while held, a few ms apart.
    var repeats = 0
    for (i in 1..100) if (p.down(start + i * 3L) == LeaderPresses.Press.Repeat) repeats++
    checkEqual(repeats, 100, "downs while held")
    p.up()
    // Released and pressed again within the window: the menu.
    checkEqual(p.down(start + 400), LeaderPresses.Press.Menu, "second press soon after")
    checkEqual(p.down(start + 450), LeaderPresses.Press.Repeat, "held after the second")
    p.up()
    // The window counts from the last press's start, not the first's.
    checkEqual(p.down(start + 400 + window - 1), LeaderPresses.Press.Menu, "just inside")
    p.up()
    checkEqual(p.down(start + 400 + 2 * window), LeaderPresses.Press.Arm, "after the window")
    p.up()
    // However fast the presses come, a release always separates them.
    val q = LeaderPresses(window)
    checkEqual(q.down(start), LeaderPresses.Press.Arm, "fresh press")
    q.up()
    checkEqual(q.down(start + 1), LeaderPresses.Press.Menu, "press a millisecond later")
    // Releases with no press between change nothing.
    q.up(); q.up()
    checkEqual(q.down(start + 2 * window), LeaderPresses.Press.Arm, "press after two releases")
}

fun main(args: Array<String>) {
    val root = File(args.firstOrNull() ?: "..").canonicalFile
    if (!File(root, "src/TerminalScreen.h").isFile) {
        println("kotlinChecks: give the repo's root as the first argument ($root has no src/)")
        exitProcess(2)
    }
    println("Running the Android port's Kotlin checks...")
    checkTermKeys(root)
    checkCellFlags(root)
    checkScreen()
    checkLeaderPresses()
    println("\n$passed passed, $failed failed")
    exitProcess(if (failed == 0) 0 else 1)
}
