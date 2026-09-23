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
        screen = session.snapshot()
        invalidate()
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
            else -> pty?.type(ch.code)
        }
    }

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
            session.press(special, mods)
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
            session.type(unicode, mods)
            return true
        }
        return false
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
