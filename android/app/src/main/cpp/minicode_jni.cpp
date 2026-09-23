// minicode_jni.cpp — the only Android-specific C++ in the port: a thin bridge
// from Kotlin to the shared core in ../../../src. Everything it calls is
// unit-tested there, so keep logic on that side rather than in here.
//
// The one job with any substance is units. The core lexes UTF-8 and reports
// byte offsets; a Java string and a Spannable are indexed in UTF-16 code
// units. So the text is converted here and each byte offset is mapped back to
// the UTF-16 index it came from.
#include <jni.h>

#include <string>
#include <vector>

#include "MarkdownParser.h"
#include "SyntaxHighlighter.h"

namespace {

std::string Utf8(JNIEnv *env, jstring s) {
    if (!s) return {};
    const char *chars = env->GetStringUTFChars(s, nullptr);
    std::string out(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(s, chars);
    return out;
}

// The lowercase extension without the dot, which is what the core expects.
std::string ExtensionOf(const std::string &name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return {};
    std::string ext = name.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(tolower(c));
    return ext;
}

// UTF-16 in, UTF-8 out, plus one entry per UTF-8 byte holding the UTF-16 index
// that byte belongs to (with a final entry for the end, so a token that ends
// at the end of the text maps cleanly).
struct Converted {
    std::string utf8;
    std::vector<int> toUtf16;
};

Converted ToUtf8(const jchar *units, jsize count) {
    Converted c;
    c.utf8.reserve(static_cast<size_t>(count) * 2);
    c.toUtf16.reserve(static_cast<size_t>(count) * 2 + 1);
    for (jsize i = 0; i < count; i++) {
        char32_t cp = units[i];
        int width = 1;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < count &&
            units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            width = 2;   // a surrogate pair is two UTF-16 units, one character
        }
        const size_t before = c.utf8.size();
        if (cp < 0x80) {
            c.utf8 += static_cast<char>(cp);
        } else if (cp < 0x800) {
            c.utf8 += static_cast<char>(0xC0 | (cp >> 6));
            c.utf8 += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            c.utf8 += static_cast<char>(0xE0 | (cp >> 12));
            c.utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            c.utf8 += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            c.utf8 += static_cast<char>(0xF0 | (cp >> 18));
            c.utf8 += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            c.utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            c.utf8 += static_cast<char>(0x80 | (cp & 0x3F));
        }
        for (size_t b = before; b < c.utf8.size(); b++) c.toUtf16.push_back(i);
        i += width - 1;
    }
    c.toUtf16.push_back(count);
    return c;
}

}  // namespace

extern "C" {

// Tokens for one file, flattened into an int array of triples
// (start, length, style), so the whole file's highlighting crosses the JNI
// boundary in a single copy. Offsets are UTF-16 code units.
JNIEXPORT jintArray JNICALL
Java_org_minicode_editor_Core_highlight(JNIEnv *env, jclass, jstring text,
                                        jstring filename) {
    const jchar *units = env->GetStringChars(text, nullptr);
    const jsize count = env->GetStringLength(text);
    Converted c = ToUtf8(units, count);
    env->ReleaseStringChars(text, units);

    const std::vector<Token> tokens =
        SyntaxHighlighter::highlight(c.utf8, ExtensionOf(Utf8(env, filename)));

    std::vector<jint> flat;
    flat.reserve(tokens.size() * 3);
    const size_t bytes = c.utf8.size();
    for (const Token &t : tokens) {
        if (t.start > bytes || t.start + t.length > bytes) continue;
        const int start = c.toUtf16[t.start];
        const int end = c.toUtf16[t.start + t.length];
        flat.push_back(start);
        flat.push_back(end - start);
        flat.push_back(static_cast<jint>(t.style));
    }
    jintArray out = env->NewIntArray(static_cast<jsize>(flat.size()));
    env->SetIntArrayRegion(out, 0, static_cast<jsize>(flat.size()), flat.data());
    return out;
}

// Whether the core has a real grammar for this file name, as opposed to
// treating it as plain text.
JNIEXPORT jboolean JNICALL
Java_org_minicode_editor_Core_supports(JNIEnv *env, jclass, jstring filename) {
    return SyntaxHighlighter::supports(ExtensionOf(Utf8(env, filename)))
               ? JNI_TRUE
               : JNI_FALSE;
}

/**
 * Markdown as a flat list of styled runs, the same list the macOS app turns
 * into an attributed string: a String[] of the runs' text and an int[] of
 * their packed flags, returned together so the document is parsed once and
 * crosses the JNI boundary in two copies. What a heading or a quote looks
 * like belongs to the platform, so that stays in Kotlin.
 */
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_Core_markdown(JNIEnv *env, jclass, jstring source) {
    const std::vector<MdRun> runs = MarkdownParser::parse(Utf8(env, source));

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray texts = env->NewObjectArray(
        static_cast<jsize>(runs.size()), stringClass, nullptr);
    std::vector<jint> flags(runs.size());
    for (size_t i = 0; i < runs.size(); i++) {
        const MdRun &r = runs[i];
        jstring text = env->NewStringUTF(r.text.c_str());
        env->SetObjectArrayElement(texts, static_cast<jsize>(i), text);
        env->DeleteLocalRef(text);

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
        flags[static_cast<size_t>(i)] = f;
    }
    jintArray packed = env->NewIntArray(static_cast<jsize>(flags.size()));
    env->SetIntArrayRegion(packed, 0, static_cast<jsize>(flags.size()), flags.data());

    jobjectArray out = env->NewObjectArray(
        2, env->FindClass("java/lang/Object"), nullptr);
    env->SetObjectArrayElement(out, 0, texts);
    env->SetObjectArrayElement(out, 1, packed);
    return out;
}

}  // extern "C"
