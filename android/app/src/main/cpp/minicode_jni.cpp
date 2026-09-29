// minicode_jni.cpp — the only Android-specific C++ in the port: a thin bridge
// from Kotlin to the shared core in ../../../src. Everything it calls is
// unit-tested there, so keep logic on that side rather than in here.
//
// The one job with any substance is units. A Java string and a Spannable are
// indexed in UTF-16 code units. The highlighter works in UTF-16 directly, so
// its offsets go straight onto spans; Markdown is parsed from UTF-8, and
// crosses through jni_strings.h rather than JNI's modified UTF-8.
#include <jni.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "MarkdownEdit.h"
#include "MarkdownParser.h"
#include "SyntaxHighlighter.h"
#include "jni_strings.h"

namespace {

using jnistr::FromJava;
using jnistr::ToJava;

// The lowercase extension without the dot, which is what the core expects.
std::string ExtensionOf(const std::string &name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return {};
    std::string ext = name.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(tolower(c));
    return ext;
}

// One open file's highlighting. The text is mirrored here, updated by each
// edit, so an edit costs one splice rather than copying the whole document
// across JNI on every keystroke.
struct Highlight {
    std::u16string text;
    std::unique_ptr<IncrementalHighlighter<char16_t>> hl;
};

Highlight *Get(jlong handle) { return reinterpret_cast<Highlight *>(handle); }

std::u16string Utf16(JNIEnv *env, jstring s) {
    if (!s) return {};
    const jsize count = env->GetStringLength(s);
    std::u16string out(static_cast<size_t>(count), u'\0');
    env->GetStringRegion(s, 0, count, reinterpret_cast<jchar *>(&out[0]));
    return out;
}

jintArray IntArray(JNIEnv *env, const std::vector<jint> &v) {
    jintArray out = env->NewIntArray(static_cast<jsize>(v.size()));
    env->SetIntArrayRegion(out, 0, static_cast<jsize>(v.size()), v.data());
    return out;
}

std::vector<jint> LineTokens(Highlight *h, size_t start, size_t end) {
    auto &hl = *h->hl;
    start = std::min(start, hl.length());
    end = std::min(std::max(end, start), hl.length());
    const size_t first = hl.lineOf(start);
    const size_t last = hl.lineOf(end > start ? end - 1 : start);
    std::vector<Token> tokens;
    hl.lineTokens(StringSource<char16_t>(h->text), first, last + 1, tokens);
    const size_t to = last + 1 < hl.lineCount() ? hl.lineStart(last + 1) : hl.length();

    std::vector<jint> flat;
    flat.reserve(2 + tokens.size() * 3);
    flat.push_back(static_cast<jint>(hl.lineStart(first)));
    flat.push_back(static_cast<jint>(to));
    for (const Token &t : tokens) {
        if (t.length == 0) continue;
        flat.push_back(static_cast<jint>(t.start));
        flat.push_back(static_cast<jint>(t.length));
        flat.push_back(static_cast<jint>(t.style));
    }
    return flat;
}

// How many UTF-16 units the first `bytes` bytes of UTF-8 `s` make: one per
// character, two for one outside the Basic Multilingual Plane. A malformed
// byte counts one, as jnistr::ToUtf16 turns it into one U+FFFD.
size_t Utf16Length(const std::string &s, size_t bytes) {
    bytes = std::min(bytes, s.size());
    size_t units = 0;
    for (size_t i = 0; i < bytes;) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        size_t len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3
                   : (b & 0xF8) == 0xF0 ? 4 : 1;
        if (i + len > s.size()) len = 1;
        units += len == 4 ? 2 : 1;
        i += len;
    }
    return units;
}

}  // namespace

extern "C" {

// Whether the core has a real grammar for this file name, as opposed to
// treating it as plain text.
JNIEXPORT jboolean JNICALL
Java_org_minicode_editor_Core_supports(JNIEnv *env, jclass, jstring filename) {
    return SyntaxHighlighter::supports(ExtensionOf(FromJava(env, filename)))
               ? JNI_TRUE
               : JNI_FALSE;
}

/**
 * Markdown as a flat list of styled runs, the same list the macOS app turns
 * into an attributed string. Returned together, so the document is parsed
 * once:
 *   [0] String[]  each run's text
 *   [1] int[]     its packed style flags (see Core.kt)
 *   [2] int[]     seven ints per run: the source line, table id, row,
 *                 column, column count, alignment, and 1 for a picture
 *   [3] String[]  two per run: the link's target and the picture's source,
 *                 null where there is none
 * What a heading or a quote looks like belongs to the platform, so that
 * stays in Kotlin.
 */
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_Core_markdown(JNIEnv *env, jclass, jstring source) {
    const std::vector<MdRun> runs = MarkdownParser::parse(FromJava(env, source));
    constexpr size_t kStride = 7;

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray texts = env->NewObjectArray(
        static_cast<jsize>(runs.size()), stringClass, nullptr);
    jobjectArray targets = env->NewObjectArray(
        static_cast<jsize>(runs.size() * 2), stringClass, nullptr);
    std::vector<jint> flags(runs.size());
    std::vector<jint> extra(runs.size() * kStride);
    for (size_t i = 0; i < runs.size(); i++) {
        const MdRun &r = runs[i];
        jstring text = ToJava(env, r.text);
        env->SetObjectArrayElement(texts, static_cast<jsize>(i), text);
        env->DeleteLocalRef(text);
        if (r.link && !r.url.empty()) {
            jstring url = ToJava(env, r.url);
            env->SetObjectArrayElement(targets, static_cast<jsize>(i * 2), url);
            env->DeleteLocalRef(url);
        }
        if (r.image && !r.src.empty()) {
            jstring src = ToJava(env, r.src);
            env->SetObjectArrayElement(targets, static_cast<jsize>(i * 2 + 1), src);
            env->DeleteLocalRef(src);
        }

        jint f = r.heading & 0x7;
        if (r.bold) f |= 1 << 3;
        if (r.italic) f |= 1 << 4;
        if (r.code) f |= 1 << 5;
        if (r.codeBlock) f |= 1 << 6;
        if (r.quote) f |= 1 << 7;
        if (r.rule) f |= 1 << 8;
        if (r.table) f |= 1 << 9;
        if (r.link) f |= 1 << 10;
        if (r.ordered) f |= 1 << 11;
        f |= (r.listDepth & 0xF) << 12;
        if (r.strike) f |= 1 << 16;
        flags[i] = f;

        jint *e = &extra[i * kStride];
        e[0] = r.line;
        e[1] = r.tableId;
        e[2] = r.tableRow;
        e[3] = r.tableCol;
        e[4] = r.tableCols;
        e[5] = r.tableAlign;
        e[6] = r.image ? 1 : 0;
    }

    jobjectArray out = env->NewObjectArray(
        4, env->FindClass("java/lang/Object"), nullptr);
    env->SetObjectArrayElement(out, 0, texts);
    env->SetObjectArrayElement(out, 1, IntArray(env, flags));
    env->SetObjectArrayElement(out, 2, IntArray(env, extra));
    env->SetObjectArrayElement(out, 3, targets);
    return out;
}

/** GitHub's anchor for a heading's text, what a "#section" link names. */
JNIEXPORT jstring JNICALL
Java_org_minicode_editor_Core_mdAnchor(JNIEnv *env, jclass, jstring heading) {
    return ToJava(env, MarkdownParser::anchor(FromJava(env, heading)));
}

// ---------------------------------------------------- editing from the preview
//
// MarkdownEdit works in the UTF-8 bytes of the source; Kotlin holds UTF-16.
// A block's range crosses back in UTF-16 units, and an edit comes back as
// the whole new source, which the caller splices into the editor as the
// smallest change that turns one into the other.

/**
 * The block on 0-based source `line` (`column` picks a table cell, -1 the
 * whole row): {kind, start, end, firstLine, lastLine}, with start and end in
 * UTF-16 units of `source` and kind in the order of MarkdownEdit::Block's
 * enum. Null where there is nothing to edit (a blank line, a rule, a fence).
 */
JNIEXPORT jintArray JNICALL
Java_org_minicode_editor_Core_mdBlockAt(JNIEnv *env, jclass, jstring source, jint line,
                                        jint column) {
    const std::string src = FromJava(env, source);
    const MarkdownEdit::Block b = MarkdownEdit::blockAt(src, line, column);
    if (b.kind == MarkdownEdit::Block::None) return nullptr;
    const jint start = static_cast<jint>(Utf16Length(src, b.start));
    const jint end = static_cast<jint>(Utf16Length(src, b.end));
    return IntArray(env, {static_cast<jint>(b.kind), start, end, b.firstLine, b.lastLine});
}

/**
 * The source once the block at (`line`, `column`) holds `text`, or, with
 * `adding`, once a new list item holding `text` follows that item. Null
 * when the block is gone or nothing would change.
 */
JNIEXPORT jstring JNICALL
Java_org_minicode_editor_Core_mdApply(JNIEnv *env, jclass, jstring source, jint line,
                                      jint column, jstring text, jboolean adding) {
    const std::string src = FromJava(env, source);
    const MarkdownEdit::Block b = MarkdownEdit::blockAt(src, line, column);
    if (b.kind == MarkdownEdit::Block::None) return nullptr;
    const std::string t = FromJava(env, text);
    std::string edited;
    if (adding) {
        if (b.kind != MarkdownEdit::Block::ListItem || t.empty()) return nullptr;
        edited = MarkdownEdit::addItem(src, b, t);
    } else {
        edited = MarkdownEdit::replace(src, b, t);
    }
    if (edited == src) return nullptr;
    return ToJava(env, edited);
}

// ---------------------------------------------------- incremental highlighting
//
// The same IncrementalHighlighter the macOS editor uses: every line's start
// and end state are kept, and an edit re-lexes from the edited line until a
// line ends in the state it used to. Offsets are UTF-16 code units throughout.

/** A highlighter for `text`, or 0 when the file has no grammar. */
JNIEXPORT jlong JNICALL
Java_org_minicode_editor_Core_hlOpen(JNIEnv *env, jclass, jstring text,
                                     jstring filename) {
    const std::string ext = ExtensionOf(FromJava(env, filename));
    if (!SyntaxHighlighter::supports(ext)) return 0;
    auto h = std::make_unique<Highlight>();
    h->text = Utf16(env, text);
    h->hl = std::make_unique<IncrementalHighlighter<char16_t>>(ext);
    h->hl->reset(StringSource<char16_t>(h->text), nullptr);
    return reinterpret_cast<jlong>(h.release());
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_Core_hlClose(JNIEnv *, jclass, jlong handle) {
    delete Get(handle);
}

/**
 * The units [pos, pos + oldLen) were replaced by `inserted`. Returns the
 * range the edit re-lexed, {start, end} in the new text, or an empty array
 * when the edit does not fit the text this side holds (the caller then opens
 * the file afresh).
 */
JNIEXPORT jintArray JNICALL
Java_org_minicode_editor_Core_hlEdit(JNIEnv *env, jclass, jlong handle, jint pos,
                                     jint oldLen, jstring inserted) {
    Highlight *h = Get(handle);
    if (!h || pos < 0 || oldLen < 0 ||
        static_cast<size_t>(pos) + static_cast<size_t>(oldLen) > h->text.size()) {
        return env->NewIntArray(0);
    }
    const std::u16string added = Utf16(env, inserted);
    h->text.replace(static_cast<size_t>(pos), static_cast<size_t>(oldLen), added);
    std::vector<Token> unused;   // the caller asks for colors once edits settle
    const auto r = h->hl->edit(StringSource<char16_t>(h->text),
                               static_cast<size_t>(pos), static_cast<size_t>(oldLen),
                               added.size(), unused);
    return IntArray(env, {static_cast<jint>(r.start), static_cast<jint>(r.end)});
}

/**
 * The tokens of the whole lines covering [start, end), as they stand. The
 * first two entries are the lines' range {from, to}, which is what the caller
 * repaints; triples (start, length, style) follow.
 */
JNIEXPORT jintArray JNICALL
Java_org_minicode_editor_Core_hlTokens(JNIEnv *env, jclass, jlong handle, jint start,
                                       jint end) {
    Highlight *h = Get(handle);
    if (!h) return env->NewIntArray(0);
    return IntArray(env, LineTokens(h, static_cast<size_t>(std::max(start, 0)),
                                    static_cast<size_t>(std::max(end, 0))));
}

}  // extern "C"
