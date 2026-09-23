package org.minicode.editor

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.util.AttributeSet
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The terminal pane: a shell on a pty, drawn as a grid of character cells.
 *
 * The grid itself is the shared TerminalScreen from ../../../src, the same
 * code that runs vim and less in the macOS app, so this view only measures
 * the cell size, draws what the screen holds, and turns input into bytes.
 *
 * Typing arrives the way it does in the editor: on a phone keyboard the
 * letters come through the input method as committed text rather than as key
 * events, so both paths are handled.
 */
class TerminalView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.MONOSPACE
        color = Palette.TEXT
    }
    private val boldPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
        color = Palette.TEXT
    }
    private val fillPaint = Paint()
    private val cursorPaint = Paint().apply { color = Palette.ACCENT; alpha = 140 }
    private val linkPaint = Paint().apply { color = Palette.ACCENT; strokeWidth = 2f }
    private val ui = Handler(Looper.getMainLooper())

    private var pty: Pty? = null
    private var rows = 24
    private var cols = 80
    private var screen: Pty.Screen? = null
    private var message: String? = null
    // Set while a redraw is already on its way to the UI thread, so a burst
    // of output makes one snapshot rather than one per read.
    private val redrawQueued = AtomicBoolean(false)

    /** Called when the shell exits, so the pane can close itself. */
    var onExit: (() -> Unit)? = null

    var textSizeSp: Float = 11f
        set(value) {
            field = value
            paint.textSize = value * resources.displayMetrics.scaledDensity
            boldPaint.textSize = paint.textSize
            requestLayout()
            invalidate()
        }

    init {
        isFocusable = true
        isFocusableInTouchMode = true
        // With a hardware keyboard Android draws a grey focus highlight over
        // the whole focused view, which turned the terminal grey.
        defaultFocusHighlightEnabled = false
        setBackgroundColor(Palette.BACKGROUND)
        textSizeSp = 11f
    }

    /** Starts the shell, in `home`, once the view has a size. */
    fun start(home: String, cwd: String = home) {
        if (pty != null) return
        val session = Pty.start("/system/bin/sh", home, cwd, cols, rows) ?: run {
            message = "Could not start /system/bin/sh"
            invalidate()
            return
        }
        pty = session
        message = null
        session.startReading(
            onChange = {
                if (redrawQueued.compareAndSet(false, true)) ui.post { snapshot() }
            },
            onExit = {
                // The shell ended by itself. The check against `pty` keeps a
                // late exit from closing a terminal started since.
                ui.post { if (pty === session) { stop(); onExit?.invoke() } }
            })
    }

    fun stop() {
        pty?.close()
        pty = null
    }

    val isRunning: Boolean get() = pty?.isOpen == true

    private fun snapshot() {
        redrawQueued.set(false)
        val session = pty ?: return
        screen = session.snapshot().also { links = findLinks(it) }
        invalidate()
    }

    // --------------------------------------------------------------- links

    /**
     * A file reference or URL on screen that a tap opens. The core's
     * TermLinks finds them; a file counts only if it exists, resolved against
     * the directory of the prompt above it, then against `linkDirs`.
     */
    class Link(val row: Int, val first: Int, val end: Int,
               val file: java.io.File?, val url: String?,
               val line: Int, val column: Int)

    /** Where relative paths are looked for after the prompt's directory. */
    var linkDirs: () -> List<String> = { emptyList() }

    /** Called when a link is tapped. */
    var onLink: ((Link) -> Unit)? = null

    private var links: List<Link> = emptyList()

    /**
     * The phone's /system/bin/sh (mksh) prints its directory in the prompt,
     * ":/storage/emulated/0/mc $ ", and sends no OSC 7, so the prompt is the
     * best record of where each command ran.
     */
    private val promptPattern = Regex("""^:?(/[^ ]*) [$#] """)

    private fun rowCells(s: Pty.Screen, row: Int) = IntArray(s.cols) { col ->
        val cp = s.codePoint(row, col)
        // 0 after a wide character is its right half, otherwise a blank.
        if (cp == 0 && col > 0 && s.flags(row, col - 1) and Pty.CELL_WIDE != 0) -1 else cp
    }

    private fun rowText(cells: IntArray) = buildString {
        for (cp in cells) if (cp >= 0) appendCodePoint(if (cp == 0) ' '.code else cp)
    }

    private fun findLinks(s: Pty.Screen): List<Link> {
        val out = mutableListOf<Link>()
        var promptDir: String? = null
        val dirs = linkDirs()
        for (row in 0 until s.rows) {
            val cells = rowCells(s, row)
            promptPattern.find(rowText(cells))?.let { promptDir = it.groupValues[1] }
            for (l in Core.termLinksIn(cells)) {
                if (!l.isFile) {
                    out += Link(row, l.first, l.end, null, l.target, 0, 0)
                    continue
                }
                val file = resolve(l.target, listOfNotNull(promptDir) + dirs) ?: continue
                out += Link(row, l.first, l.end, file, null, l.line, l.column)
            }
        }
        return out
    }

    /** `~/` is the last of `dirs`, which MainActivity makes the shell's home. */
    private fun resolve(path: String, dirs: List<String>): java.io.File? {
        val expanded = if (path.startsWith("~/"))
            (dirs.lastOrNull() ?: return null) + path.substring(1) else path
        val candidates = if (expanded.startsWith("/")) listOf(java.io.File(expanded))
                         else dirs.map { java.io.File(it, expanded) }
        return candidates.firstOrNull { it.isFile }
    }

    private val gestures = android.view.GestureDetector(context,
        object : android.view.GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: android.view.MotionEvent) = true
            override fun onSingleTapUp(e: android.view.MotionEvent): Boolean {
                val link = linkAt(e.x, e.y)
                if (link != null) { onLink?.invoke(link); return true }
                // Anywhere else a tap is what it always was: focus and keys.
                requestFocus()
                (context.getSystemService(Context.INPUT_METHOD_SERVICE)
                        as android.view.inputmethod.InputMethodManager)
                    .showSoftInput(this@TerminalView, 0)
                return true
            }
        })

    @android.annotation.SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: android.view.MotionEvent): Boolean =
        gestures.onTouchEvent(event) || super.onTouchEvent(event)

    private fun linkAt(x: Float, y: Float): Link? {
        val cellWidth = paint.measureText("M")
        val lineHeight = paint.fontSpacing
        val row = ((y - paddingTop) / lineHeight).toInt()
        val col = ((x - paddingLeft) / cellWidth).toInt()
        return links.firstOrNull { it.row == row && col >= it.first && col < it.end }
    }

    // ------------------------------------------------------------ measuring

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val cellWidth = paint.measureText("M")
        val lineHeight = paint.fontSpacing
        if (cellWidth <= 0 || lineHeight <= 0) return
        cols = ((w - paddingLeft - paddingRight) / cellWidth).toInt().coerceAtLeast(20)
        rows = ((h - paddingTop - paddingBottom) / lineHeight).toInt().coerceAtLeast(5)
        pty?.resize(cols, rows)
    }

    /**
     * Each cell's background first, then its character. A colour of 0 is the
     * terminal's default, painted in the panel's own colours; inverse swaps
     * the two after that, which is how vim and less draw their status lines
     * and most selections. A background left at the default is not painted,
     * since the view's own background already is that colour.
     */
    override fun onDraw(canvas: Canvas) {
        val cellWidth = paint.measureText("M")
        val lineHeight = paint.fontSpacing
        message?.let {
            canvas.drawText(it, paddingLeft.toFloat(), paddingTop - paint.fontMetrics.top, paint)
            return
        }
        val s = screen ?: return
        val baseline = -paint.fontMetrics.top
        for (row in 0 until s.rows) {
            val top = paddingTop + row * lineHeight
            for (col in 0 until s.cols) {
                val flags = s.flags(row, col)
                val fg = s.foreground(row, col)
                val bg = s.background(row, col)
                val inverse = flags and Pty.CELL_INVERSE != 0
                val ink = if (inverse) (if (bg == 0) Palette.BACKGROUND else bg)
                          else (if (fg == 0) Palette.TEXT else fg)
                val fill = if (inverse) (if (fg == 0) Palette.TEXT else fg) else bg
                val left = paddingLeft + col * cellWidth
                val span = if (flags and Pty.CELL_WIDE != 0) 2 else 1
                if (fill != 0) {
                    fillPaint.color = fill
                    canvas.drawRect(left, top, left + span * cellWidth, top + lineHeight, fillPaint)
                }
                val cp = s.codePoint(row, col)
                if (cp == 0 || cp == ' '.code) continue
                val pen = if (flags and Pty.CELL_BOLD != 0) boldPaint else paint
                pen.color = ink
                canvas.drawText(String(Character.toChars(cp)), left, top + baseline, pen)
            }
        }
        // Links are underlined in the accent colour: on a touch screen there
        // is no hover to find them by.
        for (l in links) {
            val y = paddingTop + (l.row + 1) * lineHeight - 1.5f
            canvas.drawLine(paddingLeft + l.first * cellWidth, y,
                            paddingLeft + l.end * cellWidth, y, linkPaint)
        }
        // The cursor is a block, as it is in the other ports, and hidden
        // when the program hides it (vim does while it redraws).
        if (s.cursorVisible) {
            val cx = paddingLeft + s.cursorCol * cellWidth
            val cy = paddingTop + s.cursorRow * lineHeight
            canvas.drawRect(cx, cy, cx + cellWidth, cy + lineHeight, cursorPaint)
        }
    }

    // --------------------------------------------------------------- input

    override fun onCheckIsTextEditor() = true

    /**
     * The terminal takes keys, not text.
     *
     * TYPE_NULL tells the keyboard there is no text field here, so it sends
     * plain key events instead of composing words and committing them. Every
     * terminal app on Android does this, and the reason is visible without
     * it: the keyboard re-sends what it is composing on every keystroke, and
     * "ls demo" reaches the shell as "sso dlemodlemo".
     */
    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = android.text.InputType.TYPE_NULL
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or
                EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING or
                EditorInfo.IME_FLAG_NO_FULLSCREEN or
                EditorInfo.IME_ACTION_NONE
        return object : BaseInputConnection(this, false) {
            // Some keyboards still commit text even with TYPE_NULL.
            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                text?.forEach { ch ->
                    // A shortcut's letter belongs to the window, not the
                    // shell: the leader is how a keyboard without Ctrl sends
                    // Ctrl C or Escape.
                    val activity = context as? MainActivity
                    if (activity?.leaderLetter(ch) != true) send(ch)
                }
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                if (event.action == KeyEvent.ACTION_DOWN) handleKey(event, text = true)
                return true
            }
        }
    }

    private fun send(ch: Char) {
        when (ch) {
            '\n' -> pty?.press(Pty.KEY_ENTER)
            else -> pty?.type(ch.code, takeCtrl())
        }
    }

    /**
     * Ctrl from the key row: it applies to the next key typed, then lets go,
     * the way a sticky modifier does on a phone. `onCtrlChanged` lets the row
     * show whether it is waiting.
     */
    var ctrlArmed = false
        set(value) { field = value; onCtrlChanged?.invoke(value) }
    var onCtrlChanged: ((Boolean) -> Unit)? = null

    private fun takeCtrl(): Int {
        if (!ctrlArmed) return 0
        ctrlArmed = false
        return Pty.MOD_CTRL
    }

    /** Text from the key row, typed as if from the keyboard. */
    fun typeText(text: String) = text.forEach { send(it) }

    /** A special key from the key row (Pty.KEY_*). */
    fun pressKey(key: Int) { pty?.press(key, takeCtrl()) }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean =
        handleKey(event, text = true) || super.onKeyDown(keyCode, event)

    private fun handleKey(event: KeyEvent, text: Boolean): Boolean {
        val session = pty ?: return false
        var mods = 0
        if (event.isShiftPressed) mods = mods or Pty.MOD_SHIFT
        if (event.isAltPressed) mods = mods or Pty.MOD_ALT
        if (event.isCtrlPressed) mods = mods or Pty.MOD_CTRL
        val special = when (event.keyCode) {
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> Pty.KEY_ENTER
            KeyEvent.KEYCODE_DEL -> Pty.KEY_BACKSPACE
            KeyEvent.KEYCODE_TAB -> Pty.KEY_TAB
            KeyEvent.KEYCODE_ESCAPE -> Pty.KEY_ESCAPE
            KeyEvent.KEYCODE_DPAD_UP -> Pty.KEY_UP
            KeyEvent.KEYCODE_DPAD_DOWN -> Pty.KEY_DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> Pty.KEY_LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> Pty.KEY_RIGHT
            KeyEvent.KEYCODE_MOVE_HOME -> Pty.KEY_HOME
            KeyEvent.KEYCODE_MOVE_END -> Pty.KEY_END
            KeyEvent.KEYCODE_PAGE_UP -> Pty.KEY_PAGE_UP
            KeyEvent.KEYCODE_PAGE_DOWN -> Pty.KEY_PAGE_DOWN
            else -> null
        }
        if (special != null) {
            session.press(special, mods or takeCtrl())
            return true
        }
        val unicode = if (text) event.unicodeChar else 0
        if (unicode != 0) {
            // On a phone keyboard Alt is the symbol layer (Alt+S is "4" on a
            // Titan 2), so when Alt changes which character the key makes,
            // the character is what was typed. Sending it as Meta, ESC then
            // "4", made every digit and symbol vanish into the shell's line
            // editor. Alt stays Meta only for a key it leaves unchanged.
            if (mods and Pty.MOD_ALT != 0) {
                val withoutAlt = event.getUnicodeChar(
                    event.metaState and KeyEvent.META_ALT_MASK.inv())
                if (withoutAlt != unicode) mods = mods and Pty.MOD_ALT.inv()
            }
            session.type(unicode, mods or takeCtrl())
            return true
        }
        return false
    }

    /** A note from the app, printed in dim text above the shell's output. */
    fun notice(text: String) {
        val session = pty ?: return
        session.show("\u001b[90m$text\u001b[0m\n")
        snapshot()
    }

    /**
     * Moves a running shell to `path`, for when another folder is opened.
     * Typed as a command, so it lands wherever the shell is: at a prompt it
     * runs, and the new directory shows in the next one.
     */
    fun changeDirectory(path: String) {
        val quoted = "'" + path.replace("'", "'\\''") + "'"
        pty?.write("cd $quoted\n".toByteArray(Charsets.UTF_8))
    }

    /** Ctrl and Escape, for a keyboard that has neither. */
    fun sendControl(letter: Char) = pty?.type(letter.lowercaseChar().code, Pty.MOD_CTRL)
    fun sendEscape() = pty?.press(Pty.KEY_ESCAPE)
    fun sendTab() = pty?.press(Pty.KEY_TAB)
}
