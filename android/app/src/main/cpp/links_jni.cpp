// links_jni.cpp — the core's TermLinks for the terminal pane: what in a row
// of the screen is a file reference or a URL that a tap can open.
//
// The row crosses as one code point per cell rather than as a string, so the
// answer can come back in cells, which is what the view draws and what a tap
// lands on. The core reads UTF-8 bytes; each byte remembers its cell.
#include <jni.h>

#include <string>
#include <vector>

#include "TermLinks.h"
#include "jni_strings.h"

extern "C" {

/**
 * The links in one row. `cells` holds a code point per column, -1 for the
 * right half of a wide character. Returns Object[2]: an int[] of
 * (kind, first column, end column, line, column) per link, kind 0 for a URL
 * and 1 for a file, and a String[] of the targets as written.
 */
JNIEXPORT jobjectArray JNICALL
Java_org_minicode_editor_Core_termLinks(JNIEnv *env, jclass, jintArray cells) {
    const jsize count = env->GetArrayLength(cells);
    std::vector<jint> cps(static_cast<size_t>(count));
    env->GetIntArrayRegion(cells, 0, count, cps.data());

    std::string utf8;
    std::vector<int> cellOf;   // per byte of utf8, the column it came from
    for (jsize col = 0; col < count; col++) {
        const jint cp = cps[static_cast<size_t>(col)];
        if (cp < 0) continue;   // the right half of a wide character
        const size_t before = utf8.size();
        jnistr::AppendUtf8(utf8, cp == 0 ? U' ' : static_cast<char32_t>(cp));
        for (size_t b = before; b < utf8.size(); b++) cellOf.push_back(col);
    }
    cellOf.push_back(count);

    const std::vector<TermLinks::Link> links = TermLinks::find(utf8);
    std::vector<jint> flat;
    jobjectArray targets = env->NewObjectArray(
        static_cast<jsize>(links.size()), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < links.size(); i++) {
        const TermLinks::Link &l = links[i];
        const int first = cellOf[l.start];
        // End after the last byte's cell, taking a wide character's second
        // column with it.
        int end = cellOf[l.start + l.length - 1] + 1;
        while (end < count && cps[static_cast<size_t>(end)] < 0) end++;
        flat.insert(flat.end(), {l.kind == TermLinks::Link::File ? 1 : 0,
                                 first, end, l.line, l.column});
        jstring t = jnistr::ToJava(env, l.target);
        env->SetObjectArrayElement(targets, static_cast<jsize>(i), t);
        env->DeleteLocalRef(t);
    }
    jintArray spans = env->NewIntArray(static_cast<jsize>(flat.size()));
    env->SetIntArrayRegion(spans, 0, static_cast<jsize>(flat.size()), flat.data());

    jobjectArray out = env->NewObjectArray(2, env->FindClass("java/lang/Object"), nullptr);
    env->SetObjectArrayElement(out, 0, spans);
    env->SetObjectArrayElement(out, 1, targets);
    return out;
}

}  // extern "C"
