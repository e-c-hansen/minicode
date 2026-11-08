package org.minicode.editor

/**
 * The leader key's presses, told apart (MainActivity.handleShortcut).
 *
 * One press of the leader key sends a burst of key-downs on a Titan 2 (a
 * hundred of them in the log). The release separates one press from the
 * next, so a down while the key is still held is part of the same press,
 * however fast the presses come. A press that starts within [window]
 * milliseconds of the one before opens the menu; any other press arms the
 * key for a letter.
 *
 * Plain Kotlin with no Android in it, so the JVM checks
 * (app/src/test/kotlin, `./gradlew kotlinChecks`) run it.
 */
class LeaderPresses(private val window: Long) {
    enum class Press {
        /** A down of a press already counted: nothing to do. */
        Repeat,
        /** A new press: wait for a letter. */
        Arm,
        /** A second press soon after the first: the menu. */
        Menu,
    }

    private var held = false
    private var lastPress = 0L

    /** A key-down of the leader key at `eventTime` (milliseconds). */
    fun down(eventTime: Long): Press {
        if (held) return Press.Repeat
        held = true
        val press = if (eventTime - lastPress < window) Press.Menu else Press.Arm
        lastPress = eventTime
        return press
    }

    /** The leader key was released: the next down is a new press. */
    fun up() {
        held = false
    }
}
