package org.minicode.editor

import android.graphics.Typeface
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.AbsoluteSizeSpan
import android.text.style.ForegroundColorSpan
import android.text.style.LeadingMarginSpan
import android.text.style.RelativeSizeSpan
import android.text.style.StyleSpan
import android.text.style.TypefaceSpan

/**
 * Turns the core's Markdown runs into styled text for a TextView.
 *
 * The parsing is the shared MarkdownParser from ../../../src, as on the Mac
 * and on Linux; what a heading or a quote looks like is decided here, since
 * that is a platform's business.
 */
object Markdown {

    fun render(source: String, density: Float): CharSequence {
        val runs = Core.markdownRuns(source)
        val out = SpannableStringBuilder()

        for (i in runs.text.indices) {
            val text = runs.text[i]
            val flags = runs.flags[i]
            val heading = flags and Core.MD_HEADING
            val depth = Core.mdListDepth(flags)
            val start = out.length

            if (flags and Core.MD_RULE != 0) {
                out.append("————————\n")
                out.setSpan(ForegroundColorSpan(Palette.DIVIDER), start, out.length,
                            Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                continue
            }

            out.append(text)
            val end = out.length
            if (end == start) continue

            fun span(what: Any) =
                out.setSpan(what, start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)

            if (heading > 0) {
                // 1.6x down to 1.0x, so a phone keeps its lines readable.
                span(RelativeSizeSpan(1f + (0.6f - 0.1f * (heading - 1)).coerceAtLeast(0f)))
                span(StyleSpan(Typeface.BOLD))
                span(ForegroundColorSpan(Palette.MD_HEADING))
            }
            if (flags and Core.MD_BOLD != 0) span(StyleSpan(Typeface.BOLD))
            if (flags and Core.MD_ITALIC != 0) span(StyleSpan(Typeface.ITALIC))
            if (flags and (Core.MD_CODE or Core.MD_CODE_BLOCK or Core.MD_TABLE) != 0) {
                span(TypefaceSpan("monospace"))
                span(ForegroundColorSpan(Palette.MD_CODE))
                span(AbsoluteSizeSpan((13 * density).toInt()))
            }
            if (flags and Core.MD_QUOTE != 0) {
                span(ForegroundColorSpan(Palette.MD_QUOTE))
                span(StyleSpan(Typeface.ITALIC))
            }
            if (flags and Core.MD_LINK != 0) span(ForegroundColorSpan(Palette.MD_LINK))
            if (depth > 0) span(LeadingMarginSpan.Standard((depth * 16 * density).toInt()))
        }
        return out
    }
}
