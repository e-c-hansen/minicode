package org.minicode.editor

import android.text.Editable
import android.text.Spanned
import android.text.style.ForegroundColorSpan

/**
 * Syntax colors for the editor, kept up to date one edit at a time.
 *
 * This is the macOS editor's scheme on Android: the core's
 * IncrementalHighlighter keeps every line's start and end state, and after an
 * edit only the lines it can have changed are lexed and repainted. Typing a
 * character costs a line, not the file. Opening a block comment at the top
 * of a file still recolors everything below, because everything below changed.
 *
 * The native side mirrors the text, so an edit crosses JNI as its position
 * and the inserted characters only. Bookkeeping happens in [edited], called
 * from TextWatcher.onTextChanged; painting waits for [paint], called from
 * afterTextChanged, since the text is still being changed before that.
 */
class Highlighter {
    private var handle = 0L
    private var pendingStart = -1
    private var pendingEnd = -1
    private var resync = false   // the native mirror fell out of step

    /** Starts over on a newly opened file and colors all of it. */
    fun open(editable: Editable, filename: String) {
        close()
        clearSpans(editable, 0, editable.length)
        handle = Core.hlOpen(editable.toString(), filename)
        pendingStart = -1
        if (handle != 0L) repaint(editable, 0, editable.length)
    }

    fun close() {
        if (handle != 0L) Core.hlClose(handle)
        handle = 0L
    }

    /**
     * `before` units at `start` were replaced by `count` units, and `text`
     * is the text afterwards (TextWatcher.onTextChanged's arguments).
     */
    fun edited(text: CharSequence, start: Int, before: Int, count: Int) {
        if (handle == 0L) return
        val inserted = text.subSequence(start, start + count).toString()
        val range = Core.hlEdit(handle, start, before, inserted)
        if (range.size < 2) {
            // The two sides disagree about the text. Should not happen; if it
            // does, start over from what the editor holds.
            resync = true
            return
        }
        // Carry an earlier pending range across this edit, then add its own,
        // as the macOS editor does.
        val delta = count - before
        if (pendingStart >= 0) {
            val editEnd = start + before
            if (pendingStart >= editEnd) {
                pendingStart += delta
                pendingEnd += delta
            } else if (pendingEnd > start) {
                pendingStart = minOf(pendingStart, start)
                pendingEnd = maxOf(pendingEnd, editEnd) + delta
            }
            pendingStart = minOf(pendingStart, range[0])
            pendingEnd = maxOf(pendingEnd, range[1])
        } else {
            pendingStart = range[0]
            pendingEnd = range[1]
        }
    }

    /** Recolors the lines edits have touched since the last paint. */
    fun paint(editable: Editable, filename: String) {
        if (resync) {
            resync = false
            open(editable, filename)
            return
        }
        if (handle == 0L || pendingStart < 0) return
        val start = pendingStart.coerceIn(0, editable.length)
        val end = pendingEnd.coerceIn(start, editable.length)
        pendingStart = -1
        pendingEnd = -1
        repaint(editable, start, end)
    }

    private fun repaint(editable: Editable, start: Int, end: Int) {
        val tokens = Core.hlTokens(handle, start, end)
        if (tokens.size < 2) return
        val from = tokens[0].coerceIn(0, editable.length)
        val to = tokens[1].coerceIn(from, editable.length)
        clearSpans(editable, from, to)
        // Adjacent pieces of one style are painted as one span: a comment
        // spanning lines arrives as one piece per line.
        var i = 2
        while (i + 2 < tokens.size) {
            val style = tokens[i + 2]
            val s = tokens[i]
            var e = s + tokens[i + 1]
            var j = i + 3
            while (j + 2 < tokens.size && tokens[j + 2] == style && tokens[j] == e) {
                e = tokens[j] + tokens[j + 1]
                j += 3
            }
            i = j
            if (style == Core.PLAIN || style !in Palette.styles.indices) continue
            if (s < from || e > to || e <= s) continue
            editable.setSpan(ForegroundColorSpan(Palette.styles[style]), s, e,
                             Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
    }

    /**
     * Removes the color spans inside [from, to). A span reaching past either
     * end keeps the part outside, so a neighbouring line's color survives.
     */
    private fun clearSpans(editable: Editable, from: Int, to: Int) {
        for (span in editable.getSpans(from, to, ForegroundColorSpan::class.java)) {
            val s = editable.getSpanStart(span)
            val e = editable.getSpanEnd(span)
            val inside = (s < to && e > from) || (s == e && s in from until to)
            if (!inside) continue
            editable.removeSpan(span)
            if (s < from) editable.setSpan(ForegroundColorSpan(span.foregroundColor), s, from,
                                           Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            if (e > to) editable.setSpan(ForegroundColorSpan(span.foregroundColor), to, e,
                                         Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
    }
}
