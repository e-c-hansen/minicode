// latex_jni.cpp — the LaTeX preview's click-to-source, from the shared core.
//
// Typesetting happens in Termux (tectonic); what is left for native code is
// the question the Mac app answers in MCLatexSpanAtPoint: which bytes of the
// source produced the word under a tap? SyncTeX offers candidate lines, the
// page's own text names the word, and LatexDoc decides, refusing rather than
// guessing. Both readers are the same files the Mac app builds, and are
// covered by tests/run_tests.cpp.
//
// Offsets cross as UTF-16 units, since the editor is a Java string.
#include <jni.h>
#include <zlib.h>

#include <string>
#include <vector>

#include "LatexDoc.h"
#include "SyncTex.h"
#include "jni_strings.h"

namespace {

struct Session {
    SyncTexIndex sync;
};

Session *Get(jlong handle) { return reinterpret_cast<Session *>(handle); }

// The .synctex.gz tectonic writes beside the PDF, decompressed.
std::string ReadGzip(const std::string &path) {
    std::string out;
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) return out;
    char buffer[65536];
    int n;
    while ((n = gzread(f, buffer, sizeof buffer)) > 0)
        out.append(buffer, static_cast<size_t>(n));
    gzclose(f);
    return out;
}

// How many UTF-16 units the first `bytes` bytes of `s` make.
jint Utf16Length(const std::string &s, size_t bytes) {
    return static_cast<jint>(jnistr::ToUtf16(s.substr(0, bytes)).size());
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_org_minicode_editor_LatexNative_open(JNIEnv *, jclass) {
    return reinterpret_cast<jlong>(new Session());
}

JNIEXPORT void JNICALL
Java_org_minicode_editor_LatexNative_close(JNIEnv *, jclass, jlong handle) {
    delete Get(handle);
}

/** Reads a .synctex.gz; false when it is missing or holds nothing. */
JNIEXPORT jboolean JNICALL
Java_org_minicode_editor_LatexNative_loadSyncTex(JNIEnv *env, jclass,
                                                 jlong handle, jstring path) {
    Session *s = Get(handle);
    if (!s) return JNI_FALSE;
    s->sync = SyncTexIndex::parse(ReadGzip(jnistr::FromJava(env, path)));
    return s->sync.valid() ? JNI_TRUE : JNI_FALSE;
}

/** The SyncTeX tag of the file tectonic was given, or 0 for any file. */
JNIEXPORT jint JNICALL
Java_org_minicode_editor_LatexNative_tagForPath(JNIEnv *env, jclass,
                                                jlong handle, jstring path) {
    Session *s = Get(handle);
    return s ? s->sync.tagForPath(jnistr::FromJava(env, path)) : 0;
}

/**
 * The span a tap landed on, as {start, end, kind, itemIndex} in UTF-16 units
 * of `source`, or null when nothing matches. `page` is 1-based and x, y are
 * PDF points from the page's top-left. `word` is the word under the tap (or
 * the line, when the page gave no word), `before` and `after` the page text
 * around it. `source` is the buffer now, so the offsets are valid for it
 * even if it changed since the PDF was made; a word that has gone simply
 * matches nothing.
 */
JNIEXPORT jintArray JNICALL
Java_org_minicode_editor_LatexNative_spanAt(JNIEnv *env, jclass, jlong handle,
                                            jstring source, jint tag, jint page,
                                            jdouble x, jdouble y, jstring word,
                                            jstring before, jstring after) {
    Session *s = Get(handle);
    if (!s || !s->sync.valid()) return nullptr;
    const std::string src = jnistr::FromJava(env, source);
    const LatexDoc doc = LatexDoc::parse(src);

    std::vector<int> lines;
    for (const SyncTexHit &h : s->sync.textHitsAtPoint(page, x, y, 8))
        if (tag == 0 || h.tag == tag) lines.push_back(h.line);

    const LatexSpan *span = doc.spanForClick(
        lines, jnistr::FromJava(env, word), jnistr::FromJava(env, before),
        jnistr::FromJava(env, after));
    if (!span) return nullptr;

    const jint out[4] = {Utf16Length(src, span->start),
                         Utf16Length(src, span->end),
                         static_cast<jint>(span->kind), span->itemIndex};
    jintArray result = env->NewIntArray(4);
    env->SetIntArrayRegion(result, 0, 4, out);
    return result;
}

/** LatexDoc::matchKey, so Kotlin can tell a word from a bullet or a logo. */
JNIEXPORT jboolean JNICALL
Java_org_minicode_editor_LatexNative_hasKey(JNIEnv *env, jclass, jstring text) {
    return LatexDoc::matchKey(jnistr::FromJava(env, text)).empty() ? JNI_FALSE
                                                                    : JNI_TRUE;
}

}  // extern "C"
